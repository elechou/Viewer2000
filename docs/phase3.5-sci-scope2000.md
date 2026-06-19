# Phase 3.5 — SCI data pump and Scope2000: operating and verification checklist

> This document is the **Phase 3.5 bring-up procedure to be executed**, not a measurement record. Firmware commit, Scope2000 commit, baud rate, continuous run time, error counts, and waveform screenshots all go into the Phase 3.5 area of [BRINGUP.md](../BRINGUP.md).
>
> Phase 3 already proved CPU1 can produce descriptors, parameter status, and native scope blocks; Phase 3.5 must prove CPU2 and a real PC program can consume these interfaces **without disturbing the control core**.

Phase 3's CCS Graph reads CPU1 memory directly, bypassing CPU2, the GS4 consumer index, IPC command forwarding, and the wire protocol. Phase 3.5 closes the full path for the first time:

```text
CPU1 ISR
  -> descriptor table / parameter plane / Scope SPSC ring / command status
  -> CPU2 shared-interface consumer
  -> Viewer2000 wire v6 (explicit serialization)
  -> COBS + CRC-32C
  -> SCIA / GPIO42,43 / XDS110 VCP
  -> Scope2000 V2kSource
  -> variable panel / parameter transaction / waveform / CSV / console
```

This phase's acceptance target is the **interfaces, the isolation boundary, and the protocol semantics**, not SCI's final throughput. 115200 baud cannot carry the platform's 100 kHz × 8ch performance anchor; the final continuous high-speed link is still Phase 6 EtherCAT. SCI's only job is to expose dual-core and host-side problems early over a low-cost physical link.

> **2026-06-17 Scope contract update**: the scope exposes two entries, Stream and Capture. `DAQ_BIND` no longer carries a group, `DAQ_CTRL(STREAM)` is a continuous flow, and `DAQ_CTRL(CAPTURE_ARMED)` starts the device-side trigger freeze; after freezing it still drains the same kind of block via `BLOCK_REQ`. All of `scope_prod` / `scope_cfg` / `scope_bind` / `scope_cons` are single objects, no longer `[group]` arrays. Wherever this document later shows an old single-entry or group phrasing, go by this paragraph and [wire-spec.md](wire-spec.md) v6.

## The parts I've already done (for reference)

| Artifact | Content |
|---|---|
| `contracts/v2k_common.h`, `v2k_command.h` | contract v8; HELLO's `tick_hz/capabilities`; STATUS's `cmd_ack_seq/cmd_result`; native capability bits |
| `docs/wire-spec.md` | wire v6 messages, Stream/Capture shared Scope, retry idempotency, build-hash re-enumeration, independent compatibility-bridge boundary |
| `contracts/vectors/`, `tools/gen_vectors.py` | HELLO/STATUS/ENUM/CAL/DAQ/CMD/BLOCK golden vectors and negative cases |
| `cpu2/v2k_sci_service.c/.h` | SCIA send/receive, COBS, CRC-32C, request dispatch, response replay, shared-plane service and diagnostic counts |
| `cpu2/cpu2.c` | CPU2 super-loop wires in the SCI service; the local heartbeat does not enter control time |
| `cpu1/cpu1.c` | publishes `tick_hz`, for HELLO and the host time axis |
| sibling `Scope2000` repo | Rust 2024 + egui; codec/transport/service layering; SCI transport; variables, parameters, Stream/Capture shared Scope, CSV, console |

Hardware-config rework not yet done and that **must not be skipped**:

- the current Phase 3.5 temporary code calls GPIO/CPUSEL driverlib directly in `cpu1/cpu1.c`. This violates the project principle "SysConfig owns static hardware".
- Before final acceptance, this must be migrated into CPU1/CPU2 SysConfig per §1, and these hand-written calls removed.
- `.syscfg` can only be modified through the CCS SysConfig tool; manually editing the text or the generated `board.c/board.h` is forbidden.

## Key decisions (finalized)

- **CPU1 is still the boot master, but it does not run SCI.** CPU1 SysConfig is only responsible for switching the SCIA peripheral ownership to CPU2 before releasing CPU2.
- **CPU2 owns SCIA.** CPU2 SysConfig is responsible for the SCIA instance, GPIO42/43 pinmux, pad/qualification, 115200 8N1, and the static FIFO config; CPU2 C is responsible for the RX ISR, software ring, codec, shared-plane access, and TX scheduling.
- **CPU1 source must not contain Phase 3.5's pinmux/CPUSEL patch.** The final generated result must come from `.syscfg`, otherwise the SysConfig GUI and the running code are two sources of truth.
- **The RX ISR only moves octets.** The ISR does no COBS, CRC, message dispatch, shared-RAM walk, block copy, or blocking send.
- **A single request in flight.** Scope2000 waits for only one response at a time; a request times out at 150 ms, and the same `(msg_type, seq)` is retried at most 2 times.
- **A retry must not repeat side effects.** CPU2 keeps the last encoded response; the same request replays the response, and does not re-execute COMMIT, CMD, or BIND, nor re-advance the Scope consumer index.
- **On-wire data keeps its native form.** `ScopeBlock` keeps sample bit width, tick, block/bind sequence numbers, and the interleaved layout; plotting or CSV decodes only by native type, with no `scale/offset` conversion.
- **Capabilities are defined by the native platform.** Scope2000 is designed against Viewer2000's full capability model; a future compatibility bridge can only declare missing capabilities, it cannot reverse-trim the native protocol or hot path.
- **A comms failure must not pollute the control domain.** Scope2000 stopping, the serial port unplugged, CPU2 stalling, or a ring overflow all manifest only as lost contact, timeout, overrun, or a gap; CPU1's tick, ISR budget, and protection state must proceed as usual.

## 1. SysConfig and code-responsibility rework

The TI dual-core SCI SysConfig example uses the following division:

```text
CPU1 syscfg: sysctl.cpuSel_SCIA = SYSCTL_CPUSEL_CPU2
CPU2 syscfg: SCI instance = SCIA + RX/TX pinmux + SCI configuration
```

Viewer2000 lands the same pattern.

### 1.1 CPU1 SysConfig

In the SysConfig GUI corresponding to `cpu1/sysconfig_cpu1.syscfg`, set:

| Item | Value |
|---|---|
| Peripheral CPU Select | SCIA → CPU2 |
| Timing | CPU1 `Board_init()`, and necessarily before `Device_bootCPU2()` |

The generated CPU1 `board.c` must contain code equivalent to the following semantics:

```c
SysCtl_selectCPUForPeripheralInstance(SYSCTL_CPUSEL_SCIA,
                                      SYSCTL_CPUSEL_CPU2);
```

CPU1 **creates no SCI runtime instance**, and sets no baud/FIFO/SCI interrupt.

### 1.2 CPU2 SysConfig

Add a SCI instance to `cpu2/sysconfig_cpu2.syscfg`, preferably selecting `XDS110 UART` hardware in Board View; if using manual pinmux, pin down the table below:

> **F28P65x dual-syscfg division**: the SCI instance in CPU2's syscfg **does not** generate `GPIO_setPinConfig` in CPU2's board.c (`pinmux.csv` hints "PinMux is done on CPU1"); sysconfig, through dual-context cooperation, places the SCIA pinmux + pad/qual back into **CPU1's** board.c `PINMUX_init`. CPU1 doesn't need to and isn't allowed to hang the SCI module again (otherwise the cross-context reports a Resource conflict); just set `cpuSel_SCIA → CPU2` in §1.1.

| Field | Value |
|---|---|
| Instance | SCIA |
| TX | GPIO42 |
| RX | GPIO43 |
| Baud | 115200 |
| Word Length | 8 |
| Stop Bits | 1 |
| Parity | None |
| Loopback | Disabled |
| FIFO | Enabled |
| RX qualification | Async |

The pad config must also be generated by SysConfig. If the GUI uses a default pull-up on the SCI pins, go by the generated result and verify it on hardware; any intentional adjustment goes back to SysConfig to change, not an appended GPIO override in `cpu1.c` or `cpu2.c`.

Interrupt responsibility uses the following boundary:

- SysConfig generates pinmux, SCI frame/baud, FIFO, and module enable;
- CPU2 C registers `INT_SCIA_RX`, sets the project-required RX FIFO level, and enables the RXFF interrupt;
- TX continues to be the super-loop polling the FIFO free space, with the TX ISR not enabled.

CPU2's startup order should be:

```text
Device_init
  -> SysCtl_enablePeripheral(SYSCTL_PERIPH_CLK_SCIA)
  -> SCIA_BASE_init (sysconfig-generated; called directly, bypassing the Board_init aggregate entry)
  -> NMI fallback
  -> dual-core shared-contract handshake
  -> SCI software buffer and RX ISR init
  -> super-loop
```

> **Why not call `Board_init()` directly**: CPU2 owns only RAMGS4 (0x2000 words, ~0x1E00 for .text+.bss+.const+.data after deducting the v2k planes). The `SYSCTL_init()` that `Board_init()` incidentally triggers contains hundreds of boot-master-only `SysCtl_setPeripheralAccessControl`/`CPUSEL` writes — dead code for CPU2, but cl2000 `-Ooff` links by translation unit by default, pulling the entire SYSCTL_init into CPU2's .text and blowing out RAMGS4 (the linker reports `error #10099-D: .bss size 0xb42 won't fit`). However `SYSCTL_init()` also enables CPU2's local `SYSCTL_PERIPH_CLK_SCIA`; when bypassing `Board_init()` you must manually keep this one clock gate, otherwise `SCIA_BASE_init()`'s writes to `SCICCR/SCICTL1/BAUD` are swallowed by the peripheral clock gate, manifesting as a host HELLO timeout with `rx_octets` constantly 0. Calling `SCIA_BASE_init()` directly together with the §6.3-mandated `--gen_func_subsections=on` lets the linker bring in only the board.obj functions actually called, so board.obj ultimately contributes only ~150 words to CPU2's .text.

### 1.3 Hand-written static config that must be deleted

After the SysConfig migration, `cpu1/cpu1.c` must no longer contain:

```text
GPIO_setPinConfig(GPIO_42_SCIA_TX / GPIO_43_SCIA_RX)
GPIO_setPadConfig(42 / 43, ...)
GPIO_setQualificationMode(43, ...)
GPIO_setControllerCore(42 / 43, ...)
SysCtl_selectCPUForPeripheralInstance(SCIA, CPU2)
```

CPU2 SCI init must also not re-execute the `SCI_setConfig`, pinmux, and FIFO/module static init already done by the generated code. `SysCtl_enablePeripheral(SCIA)` is keeping the local clock gate from `SYSCTL_init()`, not a duplicate SCI config. Although a duplicate write might "run", it masks `.syscfg` errors and causes config drift when later changing pins, changing LSPCLK, or upgrading C2000Ware. `v2k_sci_init()` keeps only the RX FIFO level (overriding the generated RX0=empty to RX1), `SCI_clearOverflowStatus()`, the `INT_SCIA_RX` registration, and the `SCI_INT_RXFF` enable.

### 1.4 Generated-result reconciliation

After each SysConfig change, check both the RAM and FLASH generated results:

| Check item | Pass condition |
|---|---|
| CPU1 SCIA CPUSEL | the generated value is CPU2, no longer the default CPU1 |
| CPU2 SCIA base | `SCIA_BASE` |
| pinmux | TX=GPIO42, RX=GPIO43 |
| RX qualification | Async |
| serial format | 115200 8N1 |
| FIFO | Enabled |
| hand-written override | no static pinmux/CPUSEL/duplicate SCI config in CPU1/CPU2 business source |

Never directly edit `cpu*/RAM/syscfg/board.c` or `cpu*/FLASH/syscfg/board.c`: these are regenerable files, not config sources.

## 2. Protocol and version prerequisites

Phase 3.5 fixes:

| Item | Value |
|---|---|
| `V2K_WIRE_VER` | 6 |
| `V2K_CONTRACT_VER` | 8 |
| max payload | 1024 octets |
| framing | COBS, `0x00` delimiter |
| integrity | CRC-32C |
| endian | little-endian |
| host request | single request in flight |
| timeout/retry | 150 ms; at most 2 retries |

Each version field owns one layer:

| Field | The problem it owns |
|---|---|
| wire version | an incompatible layout change in the frame or a message |
| contract version | whether the CPU1/CPU2 shared structs are the same generation |
| build hash | whether the variable table, addresses, and firmware build changed under the same protocol |

HELLO must report the following native capabilities:

```text
ENUM | CAL | SCOPE_STREAM | SCOPE_CAPTURE |
PRE_TRIGGER | SYSTEM_CMD | NATIVE_BLOCK
```

Scope2000 must refuse to connect on a wire or contract mismatch, never guessing the parse. A change of STATUS's `build_hash` at runtime must clear the descriptors, bindings, and waveform caches, then re-enumerate.

## 3. CPU2 data-pump behavior

### 3.1 RX path

```text
SCIA RX FIFO
  -> INT_SCIA_RX
  -> 512-word software ring (each C28x word uses only the low 8 bits)
  -> super-loop looks for 0x00
  -> COBS decode
  -> header/length/version check
  -> CRC-32C
  -> message dispatch
```

Constraints:

- the RX ISR doesn't wait on TX, doesn't access the CPU1 producer ring, doesn't walk the descriptor table;
- when the software ring is full, `g_v2k_sci_rx_overflow++`, the current octet is dropped, the control core is unaffected;
- after an encoded frame exceeds the receive scratch capacity it enters discard, recovering only at the next `0x00`;
- COBS, length, version, or CRC errors are silently dropped, no NAK is returned, the host times out and retries;
- C28x `char` is 16 bit, so on-wire octets must be explicitly packed/unpacked field by field; struct memcpy is forbidden.

### 3.2 TX and response replay

CPU2 first encodes the complete response into the TX buffer, then the super-loop sends it out as the FIFO has free space. While TX is unfinished, that buffer is not overwritten, because it simultaneously serves as the replay cache of the last response.

On receiving the same `(msg_type, seq)`:

- the TX buffer is resent from the start;
- the message handler is not re-executed;
- BLOCK_REQ does not `release` again;
- CAL_COMMIT/CMD does not increment the shared sequence again;
- DAQ_BIND does not publish a new `bind_seq` again.

Only after the host abandons the old request and uses a new seq is it a new service operation.

### 3.3 Message-to-shared-plane mapping

| wire message | CPU2 behavior | CPU1 reconcile |
|---|---|---|
| HELLO | read version, build hash, descriptor count, tick_hz, capability bits | no side effect |
| STATUS | summarize state, heartbeat, parameter result, scope mode, command result | no side effect |
| ENUM | paged read of the descriptor table, up to 8 entries per page | `desc_count/build_hash` |
| CAL_WRITE | stage into GS4 shadow; same address overwrites | not yet published |
| CAL_COMMIT | write `commit_seq+1` last | `applied_seq/result/fail_idx` |
| CAL_READ | publish a one-shot `(addr,type)` read request | `read_seq/ack_seq` |
| DAQ_CTRL | publish one scope cfg | `cfg_ack_seq/cfg_result/mode` |
| DAQ_BIND | publish a bind only in the OFF state | `bind_ack_seq/bind_result` |
| BLOCK_REQ | take 0–2 blocks each time, release after the copy is done | `rd_idx/remain_hint` |
| CMD | write `cmd_seq+1` last | `cmd_ack_seq/cmd_result/sys_state` |

During the CAPTURE_ARMED/CAPTURE_POST capture window there is no consume semantics. Only after the producer enters `FROZEN` does CPU2 initialize the consumer from `frozen_end_idx - frozen_count` and drain in time order.

### 3.4 CPU2 diagnostics

Keep resident in CPU2 CCS Expressions:

| Expression | Meaning |
|---|---|
| `g_handshake_state` | 0/1/2/3: not started, waiting for table, contract failed, running |
| `g_v2k_sci_rx_octets` | octets the RX ISR has received |
| `g_v2k_sci_tx_octets` | octets written to the TX FIFO |
| `g_v2k_sci_rx_overflow` | hardware FIFO or software ring overflow |
| `g_v2k_sci_bad_frames` | COBS/length/version/CRC/over-length-frame errors |
| `g_v2k_sci_good_frames` | requests that passed the full check |
| `g_v2k_msg_2to1.cpu2_status.link_state` | whether a legal frame was received in roughly the last 2 s |
| `g_v2k_msg_2to1.cpu2_status.heartbeat` | CPU2's local diagnostic heartbeat |
| `g_v2k_gs4.scope_cons.rd_idx` | the Scope consumer position |

These counters diagnose only the comms core; they must not become a source of control time or block timestamps.

## 4. Scope2000 behavior and operating entry

Scope2000's `V2kSource` has three layers:

```text
service semantics
  -> message codec
  -> ByteTransport
       -> SCI transport (this phase)
       -> local byte stream (boundary reserved only)
       -> EtherCAT transport (Phase 6)
```

The GUI first cut provides:

- serial-port enumeration, 115200/higher experimental baud, and connection status;
- HELLO version, build hash, tick_hz, capability;
- runtime variable enumeration and binding up to 16 channels;
- parameter Stage/Commit and value-mirror refresh;
- Start/Stop/Clear Fault;
- Scope Stream/Capture, trigger edge, threshold, pre-trigger, prescaler, block N;
- waveform tiles, gaps, CSV export, and a protocol console.

`V2kSource`'s current schedule:

| Behavior | Period |
|---|---:|
| STATUS | 250 ms |
| BLOCK_REQ with no backlog | 8 ms |
| `remain_hint != 0` | immediately continue taking blocks |
| worker idle sleep | 1 ms |

Scope2000 toggles the UI by capability. An undeclared feature only disables the corresponding operation, without changing Viewer2000's native data model.

## 5. SCI bandwidth budget

In 8N1 each on-wire octet takes about 10 bit, so the theoretical ceiling at 115200 baud is only:

```text
115200 / 10 = 11520 octets/s
```

Deduct COBS, the wire header, CRC, STATUS/control requests, and USB/VCP jitter as well. During bring-up, keep the sustained-stream budget at about 70% of the theoretical value, i.e. ~8 k octets/s.

A single native block:

```text
block_octets = 16 + n_ticks * stride_octets
```

BLOCK_DATA additionally has a 12-octet batch prefix, the wire frame additionally an 11-octet header+CRC, plus a little COBS inflation. A stable STREAM must satisfy:

```text
produced-block rate × per-block on-wire overhead < available serial throughput
```

Therefore:

- at 115200 STREAM must raise the `prescaler`, reduce channels, or use a more suitable block N;
- CAPTURE_ARMED trigger freeze can capture at full speed, because it drains slowly after freezing and doesn't require the serial port to keep up with the instantaneous sample rate;
- when deliberately using a config exceeding the SCI bandwidth, the correct result is producer overrun + block gap, not CPU1 slowing down or blocking;
- stepping the baud up is only for finding the XDS110 VCP's stable boundary, not for making SCI the final link.

## 6. Software and build checks

### 6.1 Viewer2000 protocol check

```bash
python3 tools/gen_vectors.py --check
cc -std=c99 -Wall -Wextra -Werror \
  -c tools/check_contracts.c -o /tmp/v2k-contracts.o
```

Confirm the golden vectors have no uncommitted drift, and the PC-compiler-side static assertions pass.

### 6.2 Scope2000 check

In the sibling Scope2000 repo:

```bash
cargo fmt --check
cargo clippy --all-targets -- -D warnings
cargo test --all-targets
python3 tools/check-brand.py
```

The tests cover at least:

- all golden vectors;
- bad CRC;
- frame split and coalesce;
- resync at `0x00` after over-length garbage;
- response seq mismatch;
- wire/contract mismatch;
- request timeout and retry.

### 6.3 Target project build

Must build separately via CCS `buildProject`:

| Core | Config |
|---|---|
| CPU1 | RAM, FLASH |
| CPU2 | RAM, FLASH |

Do not use `make`/`gmake` in place of the CCS project build. Only after the generated SCIA CPUSEL/pinmux/config is reconciled per §1.4 may you proceed to the hardware steps.

#### 6.3.1 Compiler option: `--gen_func_subsections=on`

Both core projects' ticlang/cl2000 compiler options must enable `--gen_func_subsections=on` (CCS GUI: Project Properties → Build → C2000 Compiler → Advanced Options → Assembler Options or "Add new flag"; persisted in `.cproject`). Effect: it makes the compiler put each function into its own `.text:funcname` subsection, letting the linker dead-strip at function granularity. From Phase 3.5, CPU2's direct call to `SCIA_BASE_init()` depends on this option to strip the several-KB `SYSCTL_init` dead code in the same obj. Side effect of disabling it: the CPU2 RAM link fails immediately (`error #10099-D: .bss won't fit`).

After changing a compiler option, you must `Clean` then `buildProject` (the subsection-naming change makes an incremental build produce "zombie objs").

## 7. Debug session and baseline

The main acceptance uses a RAM dual-core session first, with FLASH doing a boot smoke test. The load order follows [Phase 1 SysConfig](phase1-sysconfig.md) and [Phase 3 executor](phase3-executor-observability.md):

```text
Connect CPU1 -> Load/Resume
Connect CPU2 -> Load/Resume
```

Before connecting Scope2000, confirm:

| Observable | Baseline |
|---|---|
| `g_v2k_tick` | keeps incrementing |
| `g_v2k_isr_ovf_cnt` | 0 |
| `g_v2k_isr_budget_violation_cnt` | 0 |
| `g_handshake_state` | 3 |
| Phase 2 TZ | still holds the hardware-inhibit/state-machine semantics |
| CPU2 LED | toggles at ~2 Hz |

Record a set of `g_v2k_isr_cycles_max/control_cycles_max/scope_cycles_max` **before connecting Scope2000**, as a later performance-isolation reference.

## 8. Verification A — serial port and HELLO

1. On macOS confirm the XDS110 VCP has appeared; click `Refresh Ports` in Scope2000.
2. Select the corresponding port and 115200, click Connect.
3. CPU2 `rx_octets/good_frames/tx_octets` should increment, `link_state` becomes 1.
4. Reconcile in the Scope2000 console:

| Field | Expected |
|---|---|
| firmware | Viewer2000 firmware name |
| wire | 6 |
| contract | 8 |
| build hash | consistent with the CPU1 descriptor table |
| descriptor count | consistent with `entry_count` |
| tick_hz | consistent with `V2K_ISR_HZ` |
| capabilities | full native capability bits |

5. Unplug the VCP or stop requests for more than ~2 s, `link_state` should return to 0; CPU1 tick is unaffected.
6. After restoring the connection, re-HELLO without resetting either CPU.

## 9. Verification B — ENUM and build-hash re-enumeration

1. After connecting, Scope2000 requests 8 descriptors per page until `count=0` or it reaches total.
2. Reconcile name, type, kind, address, prescaler; the descriptor entry contains no `min/max/scale/offset` field.
3. Scope2000's total enumeration count must equal HELLO's `desc_count`.
4. Select several parameters and scope quantities, confirming the UI allows at most 16 scope channels.
5. Flash firmware with a different build hash but the same wire/contract.
6. After STATUS detects the hash change, it must:
   - clear the old descriptors and parameter values;
   - clear the old binding sequence numbers;
   - clear the waveforms;
   - auto re-ENUM;
   - log old/new hash in the console.

Blocks with old addresses or an old `bind_seq` must not continue into the plot.

## 10. Verification C — parameter transaction

### 10.1 Legal batch

1. In the variable panel select two or more PARAM descriptors and fill values.
2. Stage sends one or more CAL_WRITE; the target variable must not take effect yet.
3. Commit sends CAL_COMMIT, record the returned `commit_seq=s`.
4. Poll STATUS until `applied_seq==s`.
5. Reconcile `cal_result=OK`, the target value takes effect on the same control tick.

### 10.2 Rejection and atomicity

| Case | Expected |
|---|---|
| type mismatch | whole batch rejected |
| count over 16 | BAD_PARAM |
| commit again before the previous commit finished | BUSY |
| one item legal, one mechanically illegal (wrong type/wrong address etc.) in a batch | the legal item must not be written either |
| unregistered but allowed RAM address | writable |

When staging the same address across multiple frames, the last staged value overwrites the previous. A timed-out resend of the same CAL_COMMIT must not produce a second `commit_seq`.

## 11. Verification D — system commands

Execute in order:

```text
START -> STOP -> START -> manufacture a TZ fault -> CLEAR_FAULT
```

Each command is confirmed in two stages:

1. the CMD ACK's `data` returns the published `cmd_seq`;
2. STATUS's `cmd_ack_seq` catches up, and gives the final result with `cmd_result/sys_state/fault_code`.

Cover the following negatives:

- sending the next command before the previous one finished executing → BUSY;
- CLEAR_FAULT while the FAULT condition still exists → the state machine rejects or stays FAULT;
- retrying the same `(CMD, seq)` → must not re-execute.

System commands only change the application state; they must not reset `g_v2k_tick`, block sequence numbers, or the CPU1 heartbeat.

## 12. Verification E — Scope Stream

First use a config that fits within the §5 bandwidth budget:

1. `DAQ_CTRL(OFF)`, wait for STATUS to confirm scope mode is OFF.
2. Select 2–4 channels, send DAQ_BIND, record `bind_seq`.
3. Set a suitable `prescaler`, send `DAQ_CTRL(STREAM)`.
4. Scope2000 continuously BLOCK_REQ, taking at most 2 blocks each time.
5. Reconcile each block:

| Field | Pass condition |
|---|---|
| flags | currently 0 |
| bind_seq | consistent with the current binding |
| block_seq | normally contiguous, 16-bit wrap handled as unsigned |
| start_tick | monotonically advancing, from CPU1 |
| n_ticks | a partial block smaller than the configured N is allowed |
| n_ch/stride | consistent with the bound native types |
| samples | not uniformly converted to f64 at the codec/source layer |

6. Scope2000 builds the x-axis with `tick_hz` and prescaler; the y-axis value is decoded directly from the sample's native type.
7. Export CSV, check that time, channel columns, gaps, and displayed values match the GUI.

### 12.1 Gaps and overload

1. Keep CPU1/CPU2 running, pause Scope2000 consumption or deliberately raise the data rate.
2. After the ring fills, recover.
3. Expected:
   - `scope_prod.overrun_cnt` increases;
   - block_seq jumps;
   - Scope2000 inserts a NaN gap and records expected/received;
   - CPU1 `tick`, ISR overflow, budget violation, and the state machine are unaffected.

Re-BIND during STREAM must return BAD_STATE; OFF first before rebinding is allowed.

## 13. Verification F — Scope Capture and pre-trigger

1. `DAQ_CTRL(OFF)`, complete the channel binding.
2. Set the trigger channel slot, threshold, rising/falling edge, pre-trigger percentage, prescaler, and block N.
3. Send `DAQ_CTRL(CAPTURE_ARMED)`.
4. Manufacture a definite parameter or state transition.
5. Observe in STATUS:

```text
CAPTURE_ARMED -> CAPTURE_POST -> CAPTURE_FROZEN
```

6. Only after FROZEN start the BLOCK_REQ drain, until `remain_hint=0`.
7. Reconcile:

| Case | Pass condition |
|---|---|
| rising/falling edge | `trig_tick` lands near the transition |
| pre-trigger 0/30/50/100% | the pre/post ratio matches the config |
| partial block | the last block preserves the true `n_ticks` |
| drain order | from the oldest block of the frozen window to the newest |
| repeated ARM | a new `state_seq`, the old window doesn't pollute the new window |
| illegal trigger slot/percentage | BAD_PARAM, the original state isn't disturbed |

A trigger freeze can capture at full speed on CPU1 and then drain slowly over 115200; the serial speed must not reverse-limit the sampling time base.

## 14. Verification G — frame errors, split, and retry

Inject with a host test tool or a temporary transport:

| Injection | Firmware/Scope2000 expected |
|---|---|
| CRC flip | `bad_frames++`, no response, the host retries the same seq |
| illegal COBS | discard to the next delimiter then recover |
| over-length undelimited data | enter discard; recover at the next `0x00` |
| one frame split across multiple serial reads | spliced normally |
| multiple frames coalesced into one read | parse by delimiter |
| wrong-seq response | Scope2000 discards it and keeps waiting for the correct response |
| resend BLOCK_REQ with the same seq | returns the same batch of blocks, no second release |
| unknown message code | ACK(UNSUPPORTED) |

During error injection, check CPU1's ISR counts and protection state — there must be no related change.

## 15. Verification H — performance isolation and stable baud rate

### 15.1 CPU1 native-path regression

Record separately:

1. Scope OFF, Scope2000 not connected;
2. Scope OFF, Scope2000 periodic STATUS;
3. STREAM normal consumption;
4. STREAM host stops consuming and an overrun occurs;
5. ARMED full-speed capture and frozen drain.

For each tier record:

```text
g_v2k_isr_cycles_max
g_v2k_control_cycles_max
g_v2k_scope_cycles_max
g_v2k_isr_ovf_cnt
g_v2k_isr_budget_violation_cnt
g_v2k_tick
```

Pass conditions:

- with Scope OFF, whether the host is connected must not change the CPU1 hot path;
- the CPU1 cost added by STREAM/CAPTURE_ARMED can come only from the scope producer defined in Phase 3;
- the CPU2 codec, serial port, retry, or disconnect must not add CPU1 block encoding, copying, or polling;
- when the host stops consuming, CPU1 doesn't wait, at most adding producer overrun.

### 15.2 Baud-rate ladder

Test the following candidates tier by tier, not presuming all are stable:

```text
115200 -> 230400 -> 460800 -> 921600 -> 1500000
```

For each tier:

1. firmware and Scope2000 use the same baud;
2. run HELLO/ENUM/CAL/CMD first;
3. then run a RUN within the bandwidth budget;
4. continuously for at least 30 minutes;
5. record good/bad frame, RX overflow, retry, block gap, producer overrun.

Finally choose the highest tier that "runs continuously without anomaly and with margin", not the highest number that connects briefly. When changing the firmware's fixed baud, go back to SysConfig, not a separate register override in the C source.

## 16. FLASH and disconnect smoke test

After RAM fully passes, verify CPU1/CPU2 FLASH:

1. power-cycle, without connecting CCS;
2. confirm dual-core boot, protection inhibit, the CPU2 LED, and VCP enumeration;
3. Scope2000 completes HELLO/ENUM;
4. run one parameter Commit, START/STOP, a short STREAM, and a CAPTURE_ARMED freeze;
5. unplug/replug USB/VCP while running;
6. confirm CPU1 control and protection state are unaffected, and a session can be re-established after recovery.

## 17. Acceptance and exit

| Acceptance item | Pass condition |
|---|---|
| SysConfig responsibility | CPU1 only generates the SCIA→CPU2 ownership; CPU2 generates SCIA/pinmux; the business C has no static override |
| four-config build | CPU1/CPU2 RAM/FLASH all succeed via CCS `buildProject` |
| protocol conformance | Viewer vectors and Scope2000 tests all pass |
| HELLO/ENUM/STATUS | version, capability, tick, hash, enumeration and re-enumeration correct |
| CAL/CMD | two-stage async reconcile, rejection semantics, and retry idempotency correct |
| Scope Stream | native block, partial block, contiguous sequence, and gaps correct |
| Scope Capture | trigger, pre-trigger, freeze order, and slow drain correct |
| error recovery | CRC/COBS/split/coalesce/timeout/seq-mismatch all recoverable |
| isolation | host/CPU2 anomalies don't affect CPU1 tick, ISR budget, and protection |
| baud rate | the highest long-term-stable tier found and recorded |
| brand scan | Scope2000 public tracked content passes `check-brand.py` |

Write into `BRINGUP.md`:

- date, board, CCS/C2000Ware version;
- Viewer2000 and Scope2000 commits;
- CPU1/CPU2 RAM/FLASH build conclusion;
- `V2K_ISR_HZ`, wire/contract, build hash;
- the SysConfig-generated SCIA CPUSEL, GPIO42/43, baud, and FIFO config;
- per-tier baud, duration, and payload config;
- good/bad frame, RX overflow, host retry;
- block gap, producer overrun, frozen count;
- the five performance-isolation scenarios' CPU1 cycle/overflow/budget data;
- parameter, command, STREAM, CAPTURE_ARMED, disconnect, and FLASH smoke-test conclusions.

Only after this whole section passes and forms a measurement record is Phase 3.5 considered done.
