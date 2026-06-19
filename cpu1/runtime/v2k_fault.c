//=============================================================================
// v2k_fault.c — Phase 2 protection (implementation)
//=============================================================================

#include "v2k_fault.h"
#include "../wire/wire.h"
#include "v2k_user_runtime.h"

volatile uint16_t g_v2k_sm_state = V2K_STATE_INIT;
volatile uint16_t g_v2k_fault_code = V2K_FAULT_NONE;
volatile uint32_t g_v2k_tz_int_cnt;

static uint32_t s_cmd_handled;   // cmd_seq already accepted

//-----------------------------------------------------------------------------
// TZ interrupt (OST source only; the EPWM-level TZEINT.OST tracks the PIE-level
// interrupt, both enabled only in RUNNING).
// A real trip → latch FAULT. On exit from this ISR, disable both interrupt
// levels and leave only the OST latch:
//   · Disable PIE level (Interrupt_disable): stops the interrupt storm —
//     latched-OST keeps re-setting TZFLG.INT, and with PIE enabled it gets stuck
//     precisely in this ISR (2026-06-13: even CPU2 could not be booted);
//   · Disable EPWM-level TZEINT.OST: the root-cause fix for this debug round —
//     otherwise during FAULT latched-OST keeps forwarding TZFLG.INT into PIEIFR
//     and latching it (PIE disabled so it does not fire, but the flag piles up,
//     and EPWM_clearTripZoneFlag cannot clear PIEIFR), so the moment the next
//     APP_START enables PIE it immediately re-fires a spurious trip. Phase 2
//     reproduced exactly this fault: every START was first spuriously triggered
//     by a stale PIEIFR into this ISR (setting FAULT/fault_code=1 and disabling
//     interrupts), then START overwrote sys_state back to RUNNING, leaving
//     fault_code=1 stale and the TZ interrupt dead → afterwards actually fitting
//     the jumper left only the hardware OST gating the waveform, while the state
//     machine stayed stuck in RUNNING and every command returned BAD_STATE.
// Keeping the OST latch = output still gated; the re-evaluation that exits FAULT
// happens on CLEAR_FAULT→IDLE, then APP_START.
//-----------------------------------------------------------------------------
static __interrupt void v2k_tz_isr(void)
{
    g_v2k_tz_int_cnt++;
    v2k_user_disable();
    g_v2k_sm_state   = V2K_STATE_FAULT;
    g_v2k_fault_code = V2K_FAULT_TZ1_EXT;     // Phase 2's only interrupt trip source is TZ1
    wire_fault_disable_irq();
    wire_fault_ack_isr();
}

void v2k_fault_arm(void)
{
    //
    // Pre-emptively latch OST before Board_init (protection-first, first line).
    // ⚠ Key: EPWM1's peripheral clock is not yet on at this point — v2k_main.c
    // only calls Device_init, and Device_init does not enable peripheral clocks
    // (that lives in the never-called Device_enableAllPeripherals); EPWM1's clock
    // comes on only at Board_init→SYSCTL_init. Writing TZFRC directly with no
    // clock makes the write get dropped and OST not latch — exactly the root
    // cause of "outputs waveform at power-on, IDLE is a no-op" (confirmed on
    // hardware 2026-06-13). So we must explicitly enable EPWM1's clock here, wait
    // a few cycles for it to take effect, then force OST. At this point TZCTL is
    // still at its reset value (Hi-Z) and TZSEL is unconfigured, so this pass only
    // aims to "gate as early as possible"; the authoritative latch — with the
    // force-low action and via TZSEL — is completed in v2k_fault_init plus a
    // read-back assertion.
    //
    wire_fault_pre_board_lock_outputs();
}

void v2k_fault_init(void)
{
    //
    // Authoritative lockout point (protection-first, second line, guaranteed to
    // take): Board_init has now landed — EPWM1's clock is on, TZSEL=OSHT1,
    // TZA/TZB=force-low are all configured — so forcing OST here is certain to
    // latch and take effect as force-low. The arm() pass was a best-effort "gate
    // as early as possible"; this pass is the guarantee (independent of whether
    // arm() caught the clock in time).
    //
    wire_fault_init_trip_isr(&v2k_tz_isr);
    //
    // ⚠ Both TZ interrupt levels stay disabled in IDLE and are enabled only in
    // RUNNING (after APP_START releases):
    //   · PIE level: OST is already latched (output gated); enabling the PIE-
    //     level TZ interrupt at the same time would let latched-OST re-set
    //     TZFLG.INT repeatedly → interrupt storm (observed hang, CPU2 unbootable);
    //   · EPWM-level TZEINT.OST: the root cause of this debug round — left enabled
    //     during IDLE/FAULT, latched-OST keeps forwarding TZFLG.INT into PIEIFR
    //     and latching it (PIE disabled so it does not fire, but the flag piles
    //     up), and the moment the next START enables PIE it re-fires a spurious
    //     trip (see the note above v2k_tz_isr). So TZEINT.OST is likewise enabled
    //     only in RUNNING: enabled on START / disabled on STOP / disabled when the
    //     ISR enters FAULT.
    // At this point OST is latched + both interrupt levels disabled → output
    // gated, with neither a storm nor a stale PIEIFR.
    //
    wire_fault_disable_irq();

    //
    // Read-back assertion: turn "protection first" into an invariant verified at
    // power-on. OST must actually latch (output gated); otherwise = lockout
    // failed, and once tb_start releases it would output energized — halt
    // immediately, never silently release. This "outputs waveform at power-on"
    // bug was exactly the missing gate here (same reconciliation philosophy as
    // v2k_tb_check / v2k_assert_layout: do not trust "I think I gated it", trust
    // only the register read-back).
    //
    if (wire_fault_output_is_locked() == 0u)
    {
        wire_panic_halt();
    }

    // OST is latched → output stays gated; the only release path is APP_START clearing OST
    g_v2k_sm_state = V2K_STATE_IDLE;
}

//-----------------------------------------------------------------------------
// Slow-loop poll: accept commands (cmd_seq advanced = new command) + sync state
// / fault code to MSGRAM. Commands come from CPU2-side MSGRAM (in Phase 2 poked
// in via Expressions in a CPU2 debug session: fill cmd_code/arg first, then
// write cmd_seq = old+1 last — the contract's publish protocol).
//-----------------------------------------------------------------------------
void v2k_fault_poll(volatile v2k_cpu1_status_t *st)
{
    const volatile v2k_cmd_req_t *req = &V2K_MSG_2TO1_RO->cmd_req;
    uint32_t seq = req->cmd_seq;

    if (seq != s_cmd_handled)
    {
        uint16_t code   = req->cmd_code;
        uint16_t result = V2K_CMDR_OK;

        switch (code)
        {
            case V2K_CMD_NOP:
                break;

            case V2K_CMD_APP_START:
                if (g_v2k_sm_state == V2K_STATE_IDLE)
                {
                    if (v2k_user_prepare_start() == 0u)
                    {
                        result = V2K_CMDR_BAD_STATE;
                        break;
                    }
                    wire_apply(&v2k_io.out);
                    // Release + arm the TZ interrupt. The ordering fixes two
                    // Phase 2 debug root causes:
                    //   ① clear OST to release the output + clear INT first;
                    //   ② set state to RUNNING before enabling the interrupt — if
                    //      the trip source is still present (GPIO3 low), enabling
                    //      TZEINT.OST/PIE synchronously enters the ISR and re-
                    //      enters FAULT, and FAULT is safe from being overwritten
                    //      only if written after RUNNING. The old code wrote
                    //      RUNNING after enable, clobbering the FAULT the ISR had
                    //      just set → a START with a live trip could never reach
                    //      FAULT (a spurious trip was likewise clobbered into a
                    //      stuck RUNNING);
                    //   ③ clear OST before enabling TZEINT.OST: with no trip
                    //      source, after clearing OST there is no assertion and
                    //      PIEIFR is clean → enabling PIE will not spuriously fire.
                    // Outcome: with a trip → the ISR has already set state to
                    // FAULT; without a trip → it stays RUNNING. Both are correct,
                    // and no further check is needed after enable.
                    wire_fault_release_output_lock();
                    // Defensive fault-code clear: a normal IDLE must be NONE, but
                    // an APP_STOP race may leave a stale TZ1_EXT (see the note
                    // there). Clear it before enabling the interrupt — if a real
                    // trip then pre-empts into the ISR, it resets the code to this
                    // run's value and is not clobbered by this clear.
                    g_v2k_fault_code = V2K_FAULT_NONE;
                    g_v2k_sm_state = V2K_STATE_RUNNING;
                    wire_fault_enable_irq();
                }
                else { result = V2K_CMDR_BAD_STATE; }
                break;

            case V2K_CMD_APP_STOP:
                if (g_v2k_sm_state == V2K_STATE_RUNNING)
                {
                    v2k_user_disable();
                    wire_apply(&v2k_io.out);
                    // Disable both TZ interrupt levels (PIE level + EPWM-level
                    // TZEINT.OST) first, then force OST to gate the output: force
                    // therefore does not trigger the ISR (no “expected trip” flag
                    // needed), and with TZEINT.OST disabled this latched OST will
                    // not pile a stale flag into PIEIFR to pollute the next START.
                    // After entering IDLE both interrupt levels stay disabled.
                    wire_fault_disable_irq();
                    wire_fault_force_output_lock();
                    wire_fault_clear_interrupt_flag();
                    // Race guard: between the g_v2k_sm_state==RUNNING test above and
                    // disabling both interrupt levels here, a real trip can still
                    // pre-empt into v2k_tz_isr and set state to FAULT. Both
                    // interrupt levels are now disabled and the ISR will not change
                    // state again, so we drop to IDLE only if “no trip beat us to
                    // it”; if one did, keep FAULT — do not silently swallow a real
                    // protection event (rule 7). This is symmetric with
                    // APP_START's “set state before enabling the interrupt”.
                    if (g_v2k_sm_state == V2K_STATE_RUNNING)
                    {
                        g_v2k_sm_state = V2K_STATE_IDLE;
                    }
                }
                else { result = V2K_CMDR_BAD_STATE; }
                break;

            case V2K_CMD_CLEAR_FAULT:
                if (g_v2k_sm_state == V2K_STATE_FAULT)
                {
                    // Leave the OST latch alone (IDLE gates the output just the
                    // same); the PIE-level TZ interrupt stays disabled (the ISR
                    // disabled it). Only adjudicate whether the trip source is
                    // gone: source still present (GPIO3 still low) → keep FAULT
                    // (accepted but not released).
                    if (wire_fault_source_is_released() != 0u)
                    {
                        wire_fault_clear_interrupt_flag();
                        g_v2k_fault_code = V2K_FAULT_NONE;
                        g_v2k_sm_state   = V2K_STATE_IDLE;
                    }
                }
                else { result = V2K_CMDR_BAD_STATE; }
                break;

            default:
                result = V2K_CMDR_BAD_CMD;
                break;
        }

        s_cmd_handled  = seq;
        st->ack_seq    = seq;
        st->cmd_result = result;
    }

    st->sys_state  = g_v2k_sm_state;
    st->fault_code = g_v2k_fault_code;
}
