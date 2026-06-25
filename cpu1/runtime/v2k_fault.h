//=============================================================================
// v2k_fault.h — protection fault-latch state machine (CPU1 only)
//
// Protection is a pure-hardware chain (basic rule 2): trip source → TZ → PWM
// shutoff, through no CPU at all; the software part of this module does only the
// "after the fact" work — latch state, report fault_code, accept commands.
// The TZ static config (action/pins) starts in SysConfig; the selected GPIO to
// INPUT X-BAR binding and all read-back reconciliation live in v2k_board_f28p65x.c.
//
// Phase 5.0 trip sources:
//   TZ1  ← INPUT X-BAR INPUT1 ← DRV nFAULT on GPIO82: active-low gate-driver trip
//   TZ6  ← emulation stop (CBC): forces the output low while the debugger halts, auto-restored on resume
//   software ← EPWM_forceTripZoneEvent(OST): reused by the STOP command and the initial lockout
// CMPSS current-threshold trips are a Phase 5.0 hardware-verification item once
// the final current-sense pins and thresholds are measured.
//
// State machine (sys_state, value range = contract V2K_STATE_*, reaches MSGRAM via v2k_fault_poll):
//   IDLE/FAULT both have outputs locked off by a one-shot TZ latch (OST); they
//   differ only in semantics and which commands they accept;
//   only APP_START clears OST to release → there is never a window where the
//   output is "briefly opened";
//   while the trip source persists, START immediately re-enters FAULT and
//   CLEAR_FAULT stays put (contract semantics).
//
//   IDLE  --APP_START (clear OST)--> RUNNING --TZ event--> FAULT
//   RUNNING --APP_STOP (force OST, expected)--> IDLE
//   FAULT --CLEAR_FAULT (source gone)--> IDLE (OST stays latched)
//=============================================================================
#ifndef V2K_FAULT_H
#define V2K_FAULT_H

#include "../../common/v2k_planes.h"

//-----------------------------------------------------------------------------
// Observables (CCS Expressions)
//-----------------------------------------------------------------------------
extern volatile uint16_t g_v2k_sm_state;    // V2K_STATE_* (internal source of MSGRAM sys_state)
extern volatile uint16_t g_v2k_fault_code;  // V2K_FAULT_*
extern volatile uint32_t g_v2k_tz_int_cnt;  // Cumulative TZ OST interrupts (real trips while running only;
                                            // a command STOP / initial lockout disables the interrupt before force, so it is not counted)

void v2k_fault_arm(void);    // Call before Board_init: latch OST pre-emptively; while the syscfg
                             // config lands the output cannot open (TZ interrupt off, no interrupt generated)
void v2k_fault_init(void);   // TZ interrupt register/enable + state machine to IDLE (after Board_init)
void v2k_fault_poll(volatile v2k_cpu1_status_t *st);
                             // Called from the slow loop: accept cmd_req commands + sync state to MSGRAM

#endif // V2K_FAULT_H
