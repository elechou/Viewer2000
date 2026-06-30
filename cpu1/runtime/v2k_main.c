//#############################################################################
// v2k_main.c - CPU1 platform entrypoint and background runtime
//
// Responsibilities of this phase (AGENTS.md roadmap Phase 1):
//   1. Ownership assignment: CPU2 shared RAM and CPU2 LED data register -> CPU2
//   2. Publish shared-interface entities: CPU1 plane + MSGRAM heartbeat
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
#include "../../common/v2k_planes.h"
#include "v2k_timebase.h"
#include "v2k_fault.h"
#include "v2k_profile.h"
#include "v2k_registry.h"
#include "v2k_scope_runtime.h"
#include "v2k_user_runtime.h"
#include "../board/v2k_board.h"

//-----------------------------------------------------------------------------
// Shared-memory entities (section → physical-region mapping in 28p65x_generic_*_lnk_cpu1.cmd)
//-----------------------------------------------------------------------------
#pragma DATA_SECTION(g_v2k_cpu1_plane, V2K_SECT_CPU1_PLANE)
v2k_cpu1_plane_t g_v2k_cpu1_plane;

#pragma DATA_SECTION(g_v2k_msg_1to2, V2K_SECT_MSG_1TO2)
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
// Link-placement self-check: an entity address != its memmap base is a build
// error (.cmd out of sync with v2k_memmap.h); halt immediately and check the
// linker script.
//-----------------------------------------------------------------------------
static void v2k_assert_layout(void)
{
    if (((uint32_t)&g_v2k_cpu1_plane != V2K_CPU1_PLANE_BASE) ||
        ((uint32_t)&g_v2k_msg_1to2 != V2K_MSGRAM_1TO2_BASE))
    {
        v2k_board_panic_halt();
    }
}

void main(void)
{
    v2k_tick_t heartbeat_tick = 0u;
    v2k_tick_t led_tick = 0u;
    v2k_tick_t monitor_tick = 0u;
    uint32_t cpu2_hb_last = 0u;
    uint16_t cpu2_hb_stale = 0u;

    v2k_board_boot_init_device();
    v2k_assert_layout();

    //
    // Ownership assignment (boot-master responsibility, before booting CPU2):
    // CPU1 owns Flash Banks 0-2; CPU2 owns Banks 3-4 and boots from Bank 3.
    // CPU2 shared RAM ownership is assigned here. Loading CPU2's .out via the
    // debugger also writes that RAM, so CPU1 must run past this line before CPU2
    // is loaded (see the debug-session order in docs/phase1-sysconfig.md).
    //
    v2k_board_assign_boot_resources();

    //
    // Shared-interface publish: zero the whole plane, fill the contents, then
    // write magic last (publish barrier, see the publish protocol in
    // v2k_descriptor.h). The memset is on-chip owned-region init, not an on-wire
    // serialization path, so it is not bound by "no memcpy on the wire".
    //
    memset(&g_v2k_cpu1_plane, 0, sizeof(g_v2k_cpu1_plane));
    memset(&g_v2k_msg_1to2, 0, sizeof(g_v2k_msg_1to2));
    g_v2k_msg_1to2.cpu1_status.contract_ver = V2K_CONTRACT_VER;
    g_v2k_msg_1to2.cpu1_status.sys_state    = V2K_STATE_INIT;
    g_v2k_msg_1to2.cpu1_status.tick_hz      = V2K_ISR_HZ;
    v2k_profile_init();
    v2k_registry_init();
    v2k_scope_init();
    v2k_user_runtime_init();

    //
    // Phase 2 protection-first (1): lock out pre-emptively before any release.
    // Do not rely on whether device init has ever enabled TBCLKSYNC (the template
    // device.c does; the syscfg-generated version depends on config) — always
    // disable it explicitly first; then pre-emptively latch OST — so throughout
    // the following Board_init landing of the syscfg config
    // (motor ePWM/ADC/X-BAR/probe and trip pins), PWM can never reach the pins.
    //
    v2k_board_freeze_timebase_clock();
    v2k_fault_arm();

    //
    // GPIO: pad config (incl. LED_CPU2's Core Select→CPU2) is done by the
    // sysconfig-generated Board_init; here we set CSEL once more explicitly as a
    // backstop (setting it again is harmless). From Phase 2 on Board_init also
    // lands the motor ePWM/ADC/GPIO/I2C/SPI configuration.
    //
    v2k_board_init_generated_peripherals();

    //
    // Board-owned interrupt and NMI setup must be in place before releasing CPU2.
    //
    v2k_board_boot_init_interrupts();

    //
    // Phase 2 protection-first (2): contract self-check (reconcile syscfg config
    // against read-back, incl. the EPWMCLKDIV errata item) + ISR registration +
    // state machine to IDLE, and only then release TBCLKSYNC.
    //
    v2k_tb_init();
    v2k_fault_init();
    v2k_tb_start();

    v2k_board_boot_cpu2_and_sync();

    // sys_state is synced by v2k_fault_poll from here on (fault_init already set IDLE)

    // Send the first ping (thereafter the main loop re-sends on each ack)
    (void)v2k_board_ipc_ping_try_send();

    for (;;)
    {
        v2k_tick_t now = g_v2k_tick;

        if (v2k_tick_due(now, &heartbeat_tick, V2K_BG_1MS_TICKS))
        {
            // The shared-plane services are all bounded, preemptible
            // run-to-completion work units. With no new seq/request they return
            // immediately, never waiting on the comms core or a peripheral.
            // Grouped at a ~1 ms poll point to avoid an idle control core
            // continuously reading CPU2-plane/MSGRAM data.
            v2k_catalog_service();
            v2k_param_service();
            v2k_param_read_service();
            v2k_profile_service();
            v2k_profile_publish_status(&g_v2k_msg_1to2.cpu1_status);
            v2k_board_background_service();
            v2k_scope_service();
            v2k_scope_apply_ready();
            v2k_scope_ccs_view_service();
            v2k_fault_poll(&g_v2k_msg_1to2.cpu1_status);

            g_v2k_msg_1to2.cpu1_status.heartbeat++;
            g_v2k_msg_1to2.cpu1_status.tick = now;
            // ping is a 1 ms periodic diagnostic, not a control task; skip outright if one is still unacked.
            if (v2k_board_ipc_ping_try_send() != 0u)
            {
                g_ping_cnt++;
            }
        }

        // Check CPU2's heartbeat every 256 ms; loss of contact only sets a status bit, the control ISR keeps running.
        if (v2k_tick_due(now, &monitor_tick, V2K_BG_MONITOR_TICKS))
        {
            uint32_t hb = v2k_board_cpu2_heartbeat_read();
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
            v2k_board_status_led_toggle();
        }
    }
}
