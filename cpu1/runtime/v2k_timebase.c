//=============================================================================
// v2k_timebase.c — Phase 2 time-base proof (runtime part)
//
// Signal chain (static config in SysConfig, reconciled in wire_f28p65x.c):
// ePWM1 (up-down, FREE_SOFT=free-run, master sync) -> SOCA @ CTR=ZERO -> all
// selected ADC SOCs -> ADCA EOC3 -> INT_ADCA1 into v2k_tb_isr. Phase 5.0 uses
// ePWM1/ePWM2/ePWM8 as the three motor PWM modules, with ePWM1 kept as the
// master time base and ADC trigger source.
//
// FREE_SOFT decision (AGENTS.md "C2000-specific traps", first item) = FREE_RUN,
// with three layers of meaning. "Phase" below means TBCTR's position within the
// carrier period 0→PRD→0 and its derived timing (SOC trigger point, dead-band
// edges, the future relative alignment of ePWMs within a sync group); it is
// unrelated to the motor rotor's electrical angle:
//   ① Carrier timing is continuous: in STOP mode a halt freezes the counter at
//      an arbitrary position and pins the output at its then-current level, so
//      the first cycle after resume is a "partial tick" (SOC instant / duty /
//      dead-band recover from an arbitrary mid-state); under FREE_RUN the counter
//      keeps running, the hardware chain advances consistently, and the first
//      tick after resume is already a full tick with no transient. This layer
//      matters little in Phase 2 as it stands;
//   ② Hardware observability: SOC is an ePWM→ADC hardware trigger that bypasses
//      the CPU, so during a halt conversions still happen and the result
//      registers still update — but the ISR does not run and the tick does not
//      advance, so there is no software-meaningful data flow;
//   ③ The truly weighty reason: in CCS real-time mode (the platform's primary
//      interaction, enabled from Phase 3 on) a time-critical ISR keeps computing
//      while the background is halted, which requires TBCTR not to stop —
//      FREE_RUN is essentially a prerequisite for that mode, and ① is an
//      incidental gain.
// The motor is outside FREE_SOFT's jurisdiction: the rotor keeps turning during a
// halt, so a plain halt followed by control-loop resume necessarily has a
// transient — energized debugging relies on real-time mode (the loop never stops,
// only the background halts), not on making a breakpoint "harmless to the motor".
// Output safety during a halt is likewise unrelated to FREE_SOFT (a stopped
// counter just pins the output at an arbitrary level); it is carried by TZ6
// (emulation stop) cycle-by-cycle trip forcing the output low (syscfg TZ config,
// reconciled by v2k_tb_check).
//=============================================================================

#include "v2k_timebase.h"
#include "v2k_executor.h"
#include "../wire/wire.h"

void v2k_tb_init(void)
{
    wire_timebase_init(&v2k_executor_isr);
}

void v2k_tb_start(void)
{
    wire_timebase_start();
}
