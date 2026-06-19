//#############################################################################
// v2k_main.c - CPU1 platform entrypoint and background runtime
//
// Responsibilities of this phase (AGENTS.md roadmap Phase 1):
//   1. Ownership assignment: GS4 RAM → CPU2; CPU2 LED (GPIO13) data-register ownership → CPU2
//   2. Publish shared-interface entities: GS0 plane (descriptor-table magic publish barrier) + MSGRAM heartbeat
//   3. Boot CPU2 → IPC_sync rendezvous → IPC ping-pong
//   4. Blink at 1 Hz (LED4 red = GPIO12, lit on low level)
//
// Phase 2 additions (roadmap "time-base proof + protection"):
//   5. Time base: ePWM1→ADC SOC→EOC ISR (v2k_timebase.c); g_v2k_tick takes over time ownership
//   6. Protection: TZ trip + fault-latch state machine (v2k_fault.c); protection in place before PWM reaches the pins
// Phase 3 background uses a plain foreground/background loop: the main loop
// services shared-plane requests by g_v2k_tick deadline. The tick only publishes
// time; it runs no background work inside the ISR.
//
// Note: the IPC_sync in the startup sequence is a one-time init-phase rendezvous,
// not a runtime path bound by "the control core must never block waiting on the
// comms core" (basic rule 1).
//#############################################################################

#include <string.h>
#include "driverlib.h"
#include "device.h"
#include "board.h"   // sysconfig-generated: LED_CPU1_GPIO / LED_CPU2_GPIO pin macros
                     // (the board-components LED module appends a _GPIO suffix to the nested GPIO instance)
#include "../../common/v2k_planes.h"
#include "v2k_timebase.h"
#include "v2k_fault.h"
#include "v2k_registry.h"
#include "v2k_scope_runtime.h"
#include "v2k_user_runtime.h"
#include "tools/v2k_build_hash.h"

extern void SetDBGIER(uint16_t dbgier);

//-----------------------------------------------------------------------------
// Shared-memory entities (section → physical-region mapping in 28p65x_generic_*_lnk_cpu1.cmd)
//-----------------------------------------------------------------------------
#pragma DATA_SECTION(g_v2k_gs0, "v2k_gs0_cpu1")
v2k_gs0_plane_t g_v2k_gs0;

#pragma DATA_SECTION(g_v2k_msg_1to2, "v2k_msg_1to2")
v2k_msg_1to2_t g_v2k_msg_1to2;

//-----------------------------------------------------------------------------
// Observables (CCS Expressions)
//-----------------------------------------------------------------------------
uint32_t g_ping_cnt;    // Completed IPC ping-pong rounds (keeps rising = the inter-core interrupt link is alive)
uint16_t g_cpu2_alive;  // 1 = CPU2 heartbeat is advancing (from CPU1's view; 0 just sets a flag, no halt)

#define V2K_BG_1MS_TICKS       (V2K_ISR_HZ / 1000u)
#define V2K_BG_MONITOR_TICKS   ((V2K_ISR_HZ * 256uL) / 1000uL)
#define V2K_BG_LED_TICKS       (V2K_ISR_HZ / 2u)

V2K_STATIC_ASSERT((V2K_ISR_HZ % 1000u) == 0u);
V2K_STATIC_ASSERT(V2K_BG_MONITOR_TICKS > 0u);

// Unsigned subtraction keeps tick wrap-around correct. When the background falls
// behind by several periods it runs once and re-phases on now, avoiding a burst
// of "catch-up" low-priority work after recovery.
static uint16_t v2k_tick_due(v2k_tick_t now,
                             v2k_tick_t *last,
                             v2k_tick_t period)
{
    if ((v2k_tick_t)(now - *last) < period)
    {
        return 0u;
    }
    *last = now;
    return 1u;
}

//-----------------------------------------------------------------------------
// NMI backstop (boot-master responsibility, AGENTS.md dual-core split).
// An unhandled NMI gets escalated by the NMI watchdog into a whole-chip reset
// (Phase 1, BRINGUP.md 2026-06-12: after CPU2 was released from reset but before
// its .out loaded, it ran garbage instructions in M0 → CPU2 watchdog reset event
// → CPU1 NMI → NMIWD whole-chip reset → boot old firmware from flash, symptom =
// "runaway"). The handler does just three things: count, leave a trace, clear
// flags — clearing the flags stops the NMIWD count; a mirror of the flags stays
// in variables so the event is not masked (rule 7).
//-----------------------------------------------------------------------------
volatile uint32_t g_nmi_cnt;         // Cumulative NMI count
volatile uint32_t g_nmi_flags_last;  // Most recent NMIFLG (SYSCTL_NMI_* bits)
volatile uint32_t g_nmi_shadow_last; // Most recent NMI shadow flags (historical union)

static __interrupt void v2k_nmi_isr(void)
{
    g_nmi_flags_last  = SysCtl_getNMIFlagStatus();
    g_nmi_shadow_last = SysCtl_getNMIShadowFlagStatus();
    g_nmi_cnt++;
    SysCtl_clearAllNMIFlags();
}

//-----------------------------------------------------------------------------
// Link-placement self-check: an entity address != its memmap base is a build
// error (.cmd out of sync with v2k_memmap.h); halt immediately and check the
// linker script. ESTOP0 acts as a breakpoint under the debugger.
//-----------------------------------------------------------------------------
static void v2k_assert_layout(void)
{
    if (((uint32_t)&g_v2k_gs0 != V2K_GS0_PLANE_BASE) ||
        ((uint32_t)&g_v2k_msg_1to2 != V2K_MSGRAM_1TO2_BASE))
    {
        for (;;) { ESTOP0; }
    }
}

void main(void)
{
    v2k_tick_t heartbeat_tick = 0u;
    v2k_tick_t led_tick = 0u;
    v2k_tick_t monitor_tick = 0u;
    uint32_t cpu2_hb_last = 0u;
    uint16_t cpu2_hb_stale = 0u;

    Device_init();
    v2k_assert_layout();

    //
    // Ownership assignment (boot-master responsibility, before booting CPU2):
    // GS4 goes to CPU2 (v2k_memmap.h: parameter shadow + scope cfg/cons + CPU2 code).
    // Loading CPU2's .out via the debugger also writes GS4, so CPU1 must run past
    // this line before CPU2 is loaded (see the debug-session order in
    // docs/phase1-sysconfig.md).
    //
    MemCfg_setGSRAMControllerSel(MEMCFG_SECT_GS4, MEMCFG_GSRAMCONTROLLER_CPU2);

    //
    // Shared-interface publish: zero the whole plane, fill the contents, then
    // write magic last (publish barrier, see the publish protocol in
    // v2k_descriptor.h). The memset is on-chip owned-region init, not an on-wire
    // serialization path, so it is not bound by "no memcpy on the wire".
    //
    memset(&g_v2k_gs0, 0, sizeof(g_v2k_gs0));
    memset(&g_v2k_msg_1to2, 0, sizeof(g_v2k_msg_1to2));
    g_v2k_msg_1to2.cpu1_status.contract_ver = V2K_CONTRACT_VER;
    g_v2k_msg_1to2.cpu1_status.sys_state    = V2K_STATE_INIT;
    g_v2k_msg_1to2.cpu1_status.tick_hz      = V2K_ISR_HZ;
    v2k_registry_init(V2K_BUILD_HASH);
    v2k_scope_init();
    v2k_user_runtime_init();

    //
    // Phase 2 protection-first (1): lock out pre-emptively before any release.
    // Do not rely on whether device init has ever enabled TBCLKSYNC (the template
    // device.c does; the syscfg-generated version depends on config) — always
    // disable it explicitly first; then pre-emptively latch OST — so throughout
    // the following Board_init landing of the syscfg config
    // (EPWM1/ADCA/DACA/X-BAR/probe and trip pins), PWM can never reach the pins.
    //
    SysCtl_disablePeripheral(SYSCTL_PERIPH_CLK_TBCLKSYNC);
    v2k_fault_arm();

    //
    // GPIO: pad config (incl. LED_CPU2's Core Select→CPU2) is done by the
    // sysconfig-generated Board_init; here we set CSEL once more explicitly as a
    // backstop (setting it again is harmless). From Phase 2 on Board_init also
    // lands EPWM1/ADCA/DACA/INPUTXBAR/GPIO2/3.
    //
    Device_initGPIO();
    Board_init();
    GPIO_setControllerCore(LED_CPU2_GPIO, GPIO_CORE_CPU2);

    //
    // Phase 3.5 SCIA backchannel: the pinmux (GPIO42 TX / GPIO43 RX, PULLUP,
    // ASYNC) and CPUSEL_SCIA→CPU2 are both generated into CPU1 board.c by the two
    // sysconfig contexts working together:
    //   - the SCI instance in CPU2's syscfg reverse-triggers the
    //     GPIO_setPinConfig(SCIA_SCIRX_PIN_CONFIG)/SCITX_PIN_CONFIG etc. in CPU1
    //     PINMUX_init
    //   - CPU1's sysctl.cpuSel_SCIA triggers the
    //     SysCtl_selectCPUForPeripheralInstance(SYSCTL_CPUSEL_SCIA, ...CPU2) in
    //     SYSCTL_init
    // This core's application source adds no further SCIA static config (see
    // phase3.5-sci-scope2000.md §1.3).
    //

    //
    // The NMI backstop must be in place before booting CPU2 — an NMI can arrive
    // at any time in the window from CPU2's release from reset until its .out
    // finishes loading (flow aligned with the TI example nmi_ex1_cpu1handling)
    //
    Interrupt_initModule();
    Interrupt_initVectorTable();
    SysCtl_clearAllNMIFlags();
    Interrupt_register(INT_NMI, &v2k_nmi_isr);
    SysCtl_enableNMIGlobalInterrupt();
    Interrupt_enable(INT_NMI);
    SetDBGIER(INTERRUPT_CPU_INT1); // ADCA1 is in PIE Group 1 = time-critical
    EINT;
    ERTM;

    //
    // Phase 2 protection-first (2): contract self-check (reconcile syscfg config
    // against read-back, incl. the EPWMCLKDIV errata item) + ISR registration +
    // state machine to IDLE, and only then release TBCLKSYNC.
    //
    v2k_tb_init();
    v2k_fault_init();
    v2k_tb_start();

    //
    // Boot CPU2. The flash-bank partitioning is not finalized (AGENTS.md open
    // decision); Phase 1 supports RAM builds only — the _FLASH branch keeps the
    // TI template default as a placeholder, to be fixed once finalized.
    //
#ifdef _FLASH
    Device_bootCPU2(BOOTMODE_BOOT_TO_FLASH_BANK3_SECTOR0);
#else
    Device_bootCPU2(BOOTMODE_BOOT_TO_M0RAM);
#endif

    //
    // Rendezvous with CPU2 (one-time, blocks until both cores reach the sync point)
    //
    IPC_clearFlagLtoR(IPC_CPU1_L_CPU2_R, IPC_FLAG_ALL);
    IPC_sync(IPC_CPU1_L_CPU2_R, IPC_FLAG31);

    // sys_state is synced by v2k_fault_poll from here on (fault_init already set IDLE)

    // Send the first ping (thereafter the main loop re-sends on each ack)
    IPC_setFlagLtoR(IPC_CPU1_L_CPU2_R, IPC_FLAG0);

    for (;;)
    {
        v2k_tick_t now = g_v2k_tick;

        if (v2k_tick_due(now, &heartbeat_tick, V2K_BG_1MS_TICKS))
        {
            // The shared-plane services are all bounded, preemptible
            // run-to-completion work units. With no new seq/request they return
            // immediately, never waiting on the comms core or a peripheral.
            // Grouped at a ~1 ms poll point to avoid an idle control core
            // continuously reading GS4/MSGRAM.
            v2k_param_service();
            v2k_param_read_service();
            v2k_scope_service();
            v2k_scope_apply_ready();
            v2k_scope_ccs_view_service();
            v2k_fault_poll(&g_v2k_msg_1to2.cpu1_status);

            g_v2k_msg_1to2.cpu1_status.heartbeat++;
            g_v2k_msg_1to2.cpu1_status.tick = now;
            // ping is a 1 ms periodic diagnostic, not a control task; skip outright if one is still unacked.
            if (!IPC_isFlagBusyLtoR(IPC_CPU1_L_CPU2_R, IPC_FLAG0))
            {
                g_ping_cnt++;
                IPC_setFlagLtoR(IPC_CPU1_L_CPU2_R, IPC_FLAG0);
            }
        }

        // Check CPU2's heartbeat every 256 ms; loss of contact only sets a status bit, the control ISR keeps running.
        if (v2k_tick_due(now, &monitor_tick, V2K_BG_MONITOR_TICKS))
        {
            uint32_t hb = V2K_MSG_2TO1_RO->cpu2_status.heartbeat;
            if (hb == cpu2_hb_last)
            {
                if (cpu2_hb_stale < 4u) { cpu2_hb_stale++; }
            }
            else
            {
                cpu2_hb_stale = 0u;
                cpu2_hb_last  = hb;
            }
            g_cpu2_alive = (cpu2_hb_stale < 4u) ? 1u : 0u;
            if (g_cpu2_alive)
            {
                g_v2k_msg_1to2.cpu1_status.status_flags &= (uint16_t)~V2K_SF_CPU2_LOST;
            }
            else
            {
                g_v2k_msg_1to2.cpu1_status.status_flags |= V2K_SF_CPU2_LOST;
            }
        }

        if (v2k_tick_due(now, &led_tick, V2K_BG_LED_TICKS))
        {
            GPIO_togglePin(LED_CPU1_GPIO);
        }
    }
}
