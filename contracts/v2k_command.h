//=============================================================================
// v2k_command.h - shared-memory command/status plane and heartbeats
//
// Transport: IPC MSGRAM (1K words in each CPU1TOCPU2/CPU2TOCPU1 direction,
// with hardware-enforced single-writer ownership). State-machine requests flow
// CPU2 to CPU1; status and heartbeats flow in the opposite direction. IPC
// interrupts are optional doorbells only; all data uses the structs below.
//
// ---- Heartbeat semantics (monitoring side of basic rule 1) ----
// Each core increments its own heartbeat in its foreground loop; the peer
// checks for progress. CPU2 loss only sets CPU2_LOST on CPU1 and never stops
// control. CPU1 loss is reported to the host by CPU2. PWM shutdown never
// depends on this path; hardware trip and independent watchdogs are authoritative.
//
// ---- Startup handshake ----
// After CPU1 boots CPU2, CPU2 checks cpu1_status.contract_ver against its own
// V2K_CONTRACT_VER and waits for the descriptor-table magic before advancing
// its heartbeat. A version mismatch leaves CPU2 in a failure state reported by
// STATUS, preventing operation with mismatched CPU1/CPU2 images.
//=============================================================================
#ifndef V2K_COMMAND_H
#define V2K_COMMAND_H

#include "v2k_common.h"

//-----------------------------------------------------------------------------
// Command codes (cmd_req.cmd_code; 0x8000 and above are L3 application-defined)
//-----------------------------------------------------------------------------
#define V2K_CMD_NOP         0u
#define V2K_CMD_APP_START   1u   // Enter RUNNING; releases PWM only after readiness checks.
#define V2K_CMD_APP_STOP    2u   // Return to IDLE and lock PWM outputs.
#define V2K_CMD_CLEAR_FAULT 3u   // Clear the FAULT latch when the trip source is gone.
#define V2K_CMD_APP_BASE    0x8000u

//-----------------------------------------------------------------------------
// Platform state machine (cpu1_status.sys_state; L1-owned)
//-----------------------------------------------------------------------------
#define V2K_STATE_INIT    0u   // Power-on initialization; table not published
#define V2K_STATE_IDLE    1u   // Ready with PWM outputs locked
#define V2K_STATE_RUNNING 2u   // Control is running
#define V2K_STATE_FAULT   3u   // Hardware trip latched; awaiting CLEAR_FAULT

// Command results (cpu1_status.cmd_result)
#define V2K_CMDR_OK        0u
#define V2K_CMDR_BAD_CMD   1u
#define V2K_CMDR_BAD_STATE 2u   // Command is not accepted in the current state
#define V2K_CMDR_NOT_READY 3u   // Power-stage preconditions did not pass
#define V2K_CMDR_START_FAILED 4u // User-state restore/setup preparation failed

// Status flags. CPU1 owns cpu1_status.status_flags; CPU2 may OR CPU1_STALE
// into the serialized STATUS payload without writing back to CPU1 state.
#define V2K_SF_CPU2_LOST   0x0001u  // CPU2 heartbeat stopped; informational only
#define V2K_SF_CPU1_STALE  0x0002u  // CPU1 heartbeat/tick stopped from CPU2's view

// Fault codes (cpu1_status.fault_code; 0=none, 1..255 platform-reserved,
// 256+ L3-defined). Append new values; never reuse them because the host
// renders the numeric source.
#define V2K_FAULT_NONE     0u
#define V2K_FAULT_TZ1_EXT  1u   // External TZ1 source, board-profile defined
#define V2K_FAULT_OVERCURRENT 2u // Hardware current-window trip (CMPSS or ADC PPB).

//-----------------------------------------------------------------------------
// Command request (MSGRAM CPU2 to CPU1; CCS may also write this structure)
//
// Publish protocol: write cmd_code/arguments first, then write cmd_seq=old+1
// last. CPU1 accepts a request when cmd_seq differs from the handled value and
// writes ack_seq to status only after processing completes.
//-----------------------------------------------------------------------------
typedef struct {
    uint32_t cmd_seq;    // Increment per command; written last to publish
    uint16_t cmd_code;   // V2K_CMD_*
    uint16_t arg0;
    uint32_t arg1;
} v2k_cmd_req_t;

V2K_ASSERT_SIZE_BITS(v2k_cmd_req_t, 96u);

//-----------------------------------------------------------------------------
// CPU1 status block (MSGRAM CPU1 to CPU2)
//-----------------------------------------------------------------------------
typedef struct {
    uint16_t contract_ver;  // V2K_CONTRACT_VER startup check
    uint16_t sys_state;     // V2K_STATE_*
    uint32_t ack_seq;       // Highest processed cmd_seq
    uint16_t cmd_result;    // V2K_CMDR_* result corresponding to ack_seq
    uint16_t fault_code;    // 0=none; 1..255 platform; 256+ L3-defined
    uint16_t status_flags;  // V2K_SF_*
    uint16_t reserved;
    uint32_t heartbeat;     // Incremented by the CPU1 foreground loop
    v2k_tick_t tick;        // Current ISR tick snapshot
    uint32_t tick_hz;       // ISR tick rate used by the host for time conversion
    uint32_t prof_seq;      // Runtime-load snapshot sequence; written last
    uint32_t cycle_budget;  // V2K_CPUTIMER_HZ / V2K_ISR_HZ
    uint32_t load_avg;      // Mean ADC/EOC latency + ISR cycles in the completed window
    uint32_t load_peak;     // Peak ADC/EOC latency + ISR cycles in the completed window
    uint32_t ctrl_at_peak;  // User control() body cycles on the peak tick
    uint32_t scope_at_peak; // Scope epilogue cycles on the peak tick
    uint16_t lat_at_peak;   // ADC/EOC entry latency on the peak tick, in CPUTIMER cycles
    uint16_t prof_reserved;
    v2k_tick_t peak_tick;   // Hidden bring-up correlation tick for the peak record
    uint32_t budget_violations; // Lifetime ISR budget violations
    uint32_t isr_overflows;     // Lifetime ADC interrupt overflows
} v2k_cpu1_status_t;

V2K_ASSERT_SIZE_BITS(v2k_cpu1_status_t, 544u);

//-----------------------------------------------------------------------------
// CPU2 status block (MSGRAM CPU2 to CPU1)
//-----------------------------------------------------------------------------
typedef struct {
    uint32_t heartbeat;     // Incremented by the CPU2 foreground loop
    uint16_t link_state;    // 0=no host, 1=SCI online, 2=EtherCAT OP
    uint16_t reserved;
} v2k_cpu2_status_t;

V2K_ASSERT_SIZE_BITS(v2k_cpu2_status_t, 64u);

#endif // V2K_COMMAND_H
