# Phase 3.5 SCI Async TX Pump Plan

> Status: planning note. This file is intentionally in the public firmware repo
> because the fix belongs to the board-neutral SCI transport architecture first;
> private downstream targets should merge the validated result afterward.

## Problem

Wire v8 correctly moves scope data from host-polled `BLOCK_REQ` to
firmware-initiated `SCOPE_BLOCK_PUSH`, but the current CPU2 SCI implementation
still depends on the foreground loop periodically refilling the SCI TX FIFO.
That makes throughput sensitive to arbitrary service-loop delay and to unrelated
diagnostic cadence decisions.

Raising `STATUS_PUSH` cadence is not a throughput fix. Status cadence controls
operator-visible freshness only. It should not be used to keep the SCI line busy,
to mask FIFO refill gaps, or to justify a high-performance transport claim.

The transport layer needs an asynchronous TX pump that keeps the peripheral fed
when the SCI TX FIFO has space. Scope is only one producer of encoded frames;
the same pipe must serve command responses, status, drain markers, and future
message types.

## Current Hardware Reading

On the F28P65x driverlib snapshot in this repo, the DMA trigger table contains
UARTA/UARTB and SPI triggers, but no `DMA_TRIGGER_SCIA*` entry. Therefore SCI DMA
must not be assumed. The first implementation target is the SCI TX FIFO level
interrupt. DMA remains a research item until the exact target peripheral and pin
route prove it is supported.

## Design Direction

1. Split protocol serialization from physical TX service.
   `v2k_sci_service.c` should build COBS/CRC encoded frames and submit them to a
   CPU2 board-pipe TX API. It should not poll `SCI_getTxFIFOStatus()` as the
   primary mechanism for line-rate progress.

2. Add a board-pipe TX queue owned by CPU2.
   The queue stores encoded octets or frame descriptors. It must preserve ACK
   priority over scope push frames and retain the resend cache semantics for
   request-response retries.

3. Implement SCI TX FIFO interrupt refill in the board layer.
   Configure TX FIFO interrupt level low enough to provide refill headroom, enable
   the TX FIFO interrupt only while queued data exists, and have the ISR copy as
   many octets as possible into the 16-entry FIFO before clearing the interrupt.
   Disable the TX interrupt when the queue is empty.

4. Keep RX recovery separate.
   RX FIFO interrupt/error recovery remains responsible for physical disconnect,
   break, framing, overflow, partial COBS frame discard, and replay-cache reset.
   TX refill must not weaken RX recovery behavior.

5. Make status cadence a product requirement, not a throughput knob.
   Default status push should return to the lowest rate that gives acceptable UI
   freshness, currently 4 Hz unless a separate UX requirement says otherwise. The
   cadence source must be CPU2-local so CPU2 can still report that CPU1 has
   stopped advancing.

6. Add CPU1-loss reporting from CPU2.
   CPU2 should monitor CPU1 heartbeat/tick progress and serialize a CPU1-stale
   status bit while continuing to push status at the CPU2-local cadence. The
   control-time tick is still CPU1-owned and must not drive CPU2 diagnostic time.

7. Re-evaluate DMA only after the interrupt pump is proven.
   If a supported UART/DMA route can use the same physical debug connection and
   meet the board-seam rules, add it as an optional board-pipe backend behind the
   same API. If the active SCI peripheral has no DMA trigger, record that and do
   not carry DMA wording in acceptance claims.

## Acceptance Criteria

- CPU2 foreground loop delay can be varied without creating sustained TX FIFO
  empty gaps during active `SCOPE_BLOCK_PUSH`.
- Stream at the accepted high-baud operating point has zero push-frame gaps, zero
  block gaps, zero producer overrun growth, and no sustained `remaining_hint`
  growth over the recorded run.
- Full-ring Capture drain remains within the existing target time without host
  polling.
- Command ACK latency during active stream remains within the existing command
  interleave limit.
- STATUS push rate is validated separately from throughput and remains stable
  when no scope stream is active.
- With CPU1 heartbeat/tick intentionally stopped and CPU2 still running, CPU2
  continues to transmit status and reports CPU1-stale state.
- The implementation does not add any CPU1 control ISR dependency on CPU2, SCI,
  DMA, host traffic, or queue state.

## Work Sequence

1. Add board-pipe TX API shape and host/firmware contract notes.
2. Build the SCI TX FIFO interrupt backend for the existing SCIA route.
3. Refactor `v2k_sci_service.c` to submit encoded frames to the async pipe.
4. Restore status cadence semantics and add CPU1-stale reporting.
5. Extend diagnostics to measure FIFO-empty/refill behavior separately from frame
   gaps and payload throughput.
6. Run protocol vectors and host parser tests.
7. Build CPU1/CPU2 FLASH through CCS project tooling and collect hardware
   evidence on the public target.
8. Only after public-target acceptance, merge/cherry-pick the transport fix into
   private downstream repositories and redo target-specific evidence there.

## Downstream Policy

Do not tune private downstream SCI throughput by changing status cadence or CPU2
foreground sleep values. Downstream repositories should keep pinmux, baud, memory
placement, and hardware evidence private, but the async TX pump should originate
here so Scope2000 and firmware share one public transport model.
