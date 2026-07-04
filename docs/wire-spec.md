# Viewer2000 wire protocol specification (wire spec) v10

> **Document status**: this document together with the `contracts/` headers forms the single source of truth for the protocol; the golden test vectors under `contracts/vectors/` are the executable form of this document. Both the firmware C serializer and the host Rust parser must pass the conformance test against the same set of vectors. When the three disagree, **the vectors are authoritative**, and the document is fixed immediately.
>
> **Change flow**: any message-layout change → edit this document → edit `tools/gen_vectors.py` then regenerate vectors → both ends' codecs follow → an incompatible change must bump `V2K_WIRE_VER`.

---

## 1. Layered model

```
┌─────────────────────────────────────────────────────────────────────────────┐
│ Service-semantics layer  session/enum/param txn/DAQ stream/command (§5)     │ transport-agnostic
├─────────────────────────────────────────────────────────────────────────────┤
│ Message layer  message catalog v10, fixed/var-length LE layout (§4)         │ transport-agnostic
├─────────────────────────────────────────────────────────────────────────────┤
│ Frame adapter  per-transport:                 (§3)                          │ transport-dependent
│             SCI = COBS + frame header + CRC-32C                             │
│             EtherCAT = mailbox/PDO self-delimited, the frame layer vanishes │
├─────────────────────────────────────────────────────────────────────────────┤
│ Physical layer  SCI(XDS110 VCP) → EtherCAT 100M                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

Design invariant: **everything above the message layer is transport-agnostic**. Swapping the physical layer only swaps the frame adapter; the messages and shared interfaces (`contracts/*.h`) don't move a bit.

The service semantics keep the generic vocabulary CAL/DAQ/prescaler from XCP (ASAM MCD-1), but Viewer2000 exposes two Scope entries: Stream = continuous block flow, Capture = device-side trigger-freeze window. The two reuse the same channel binding, ring, and block format; the firmware protocol no longer exposes the XCP event-channel / group concept.

## 2. Common conventions

- **octet** = on-wire 8-bit byte; **word** = C28x 16-bit unit. This file uses octet throughout.
- Multi-octet integers are always **little-endian (LE)**; `f32` = the LE bit pattern of IEEE-754 single precision.
- Strings are printable ASCII. Unless a field explicitly carries `name_len`,
  strings are fixed-length and NUL-padded (NUL termination not guaranteed).
- In the offset tables, `off:sz` is in octets.
- SCI model: control-plane messages remain request-response, while STATUS and
  scope data are firmware-initiated push frames. The host must keep receiving
  and demuxing push frames while command responses are in flight.
- The `value_bits` (parameter value bit pattern) convention is in `v2k_common.h`: F32 native bit pattern, I16 sign-extended / U16 zero-extended to 32 bit.
- **The universal key for variable addressing = `(addr, type)`** (CPU1 data-space word address + type code). Names live only in the CPU1-owned catalog: platform/system names are firmware-owned, and user names are baked into the CPU1 Flash catalog from DWARF. CPU2 proxies ENUM requests through shared RAM but does not own or parse the dictionary. Realtime services (`CAL`, `DAQ_BIND`, Stream/Capture blocks) stay name-free.
- **The on-wire value is the real value**: the protocol carries no `min/max/scale/offset` display or guard-rail metadata. Both the DAQ block and the CAL value interpret the bit pattern by the variable's native type; the firmware does no quantization, no conversion, no parameter range clamping or range rejection.

## 3. Frame adapter

### 3.1 SCI frame

```
Pre-encoding frame ("raw frame", CRC coverage = offset 0 .. 7+n-1):

off  sz  field
0    1   ver_magic   = 0x5A (high nibble 0x5 fixed magic, low nibble = V2K_WIRE_VER)
1    1   msg_type    (§4 catalog; response = request | 0x80, or a push type)
2    1   flags       = 0x00, reserved
3    2   seq         request-response pairing seq, or push_frame_seq for push frames
5    2   payload_len = n (octet count, ≤ V2K_MAX_PAYLOAD = 1024)
7    n   payload
7+n  4   crc32c      (LE)

On-wire form: COBS(raw frame) + 0x00 delimiter
```

- **CRC-32C** (Castagnoli, polynomial 0x1EDC6F41 reflected 0x82F63B78, init=0xFFFFFFFF, xorout=0xFFFFFFFF, i.e. the iSCSI/RFC 3720 parameter set). Rationale: the frame can be up to ~1KB, and CRC-32C at this length still keeps HD≥4 with a random miss rate of 2⁻³²; SSE4.2 hardware instructions on the PC; the C28x VCRC extended instruction set has a ready CRC-32C routine (C2000Ware `CRC_run32BitPoly2Reflected`), and a software table-lookup fallback also keeps up with the SCI rate (CPU2 background loop, not the ISR path).
- **COBS** (Consistent Overhead Byte Stuffing): the encoded frame contains no 0x00, and 0x00 is the delimiter only. After the receiver loses sync, discarding to the next 0x00 resyncs (deterministic). Rationale (vs candidates):
  - SLIP/HDLC escape style: worst-case 2× inflation, and this protocol's main payload is an int16 sample stream, with **0x00 octets frequent near signal zero-crossings**, so the inflation rate floats with the waveform content — unacceptable on the bandwidth-tightest SCI link. COBS has a constant overhead ≤ ⌈len/254⌉+1 octet.
  - Pure length-prefix + magic scan: zero TX transform (saves one octet-processing pass on the C28x), but sync recovery is heuristic (a magic collision needs CRC to exclude). COBS's encoding cost happens in the CPU2 background loop (not the control ISR), trading for deterministic resync — worth it.
  - Having the in-frame `payload_len` and COBS coexist is intentional redundancy: after decoding, check the length before the CRC, discarding a corrupt frame early.
- **Decoder discard rule**: COBS decode failure / `ver_magic` mismatch / length mismatch / CRC mismatch -> silently drop the whole frame (a corrupt frame's content is untrustworthy, **no NAK**). Request reliability is ensured by the host's timeout resend (all requests idempotent, see §5.4). Stream push loss is detected and shown as a gap; capture push loss is recovered with selective replay.
- The frame `seq`, push-frame sequence, and the block header's `block_seq` (v2k_scope.h) have different jobs and **must not be merged**: request seq handles request-response pairing and resend dedup; push_frame_seq detects lost SCI push frames; block_seq detects dropped producer blocks. A dropped stream block is not refilled by the link layer (rule 1), the host draws a gap.

### 3.2 EtherCAT mapping plan

| This protocol's element | EtherCAT carrier | Note |
|---|---|---|
| The whole frame-adapter layer | (vanishes) | mailbox/PDO is self-delimited, Ethernet FCS carries its own check |
| Control-plane messages (HELLO/ENUM/CAL/DAQ_CTRL/CMD) | mailbox (minimal CoE or vendor mailbox) | the raw frame drops COBS and CRC, `ver_magic..payload` loaded verbatim |
| SCOPE_BLOCK_PUSH payload | TxPDO fixed layout | `count + blocks`, the master grabs more when there's data each 2 kHz cycle |
| CAPTURE_BATCH_PUSH payload | mailbox/PDO TBD | EtherCAT can carry capture batches directly or map the frozen ring to mailbox replay |
| CAPTURE_REPLAY_REQ | mailbox | only needed when capture batch loss is detected |
| STATUS_RESP core fields | TxPDO header region | state/fault/heartbeat visible with the stream |

The timestamp is always the ISR tick in the block header, unrelated to the EtherCAT DC clock system (no DC).

## 4. Message catalog v10

### 4.0 Master table

| code | name | dir | payload | response |
|---|---|---|---|---|
| 0x01 | HELLO_REQ | H→F | 0 | 0x81 HELLO_RESP |
| 0x02 | STATUS_REQ | H→F | 0 | 0x82 STATUS_RESP |
| 0x03 | ENUM_REQ | H→F | 4 | 0x83 ENUM_RESP |
| 0x10 | CAL_WRITE | H→F | 2+12k | 0x90 ACK |
| 0x11 | CAL_COMMIT | H→F | 0 | 0x91 ACK (data=commit_seq) |
| 0x12 | CAL_READ | H→F | 2+8k | 0x92 CAL_READ_RESP |
| 0x20 | DAQ_CTRL | H→F | 24 | 0xA0 ACK |
| 0x21 | CAPTURE_REPLAY_REQ | H→F | 8 | 0xA1 ACK |
| 0x22 | DAQ_BIND | H→F | 2+8k | 0xA2 ACK (data=bind_seq) |
| 0x30 | CMD | H→F | 8 | 0xB0 ACK (data=ack_seq) |
| 0x41 | STATUS_PUSH | F→H | 96 | none |
| 0x42 | SCOPE_BLOCK_PUSH | F→H | 8+Σblock | none |
| 0x45 | CAPTURE_BATCH_PUSH | F→H | 16+Σblock | none |
| 0x60–0x6F | (reserved) firmware update | — | — | finalized with the mature high-bandwidth transport |
| 0x70 | (reserved) LOG pull | — | — | CPU2 diagnostic log |

Convention: response code = request code | 0x80. A request with no typed response uniformly returns the **generic ACK**:

```
ACK payload (8 octets):
off sz field
0   1  status      0=OK 1=BAD_PARAM 2=BUSY 3=BAD_STATE 4=UNSUPPORTED 5=INTERNAL
1   1  echo_type   the acknowledged request's msg_type
2   2  reserved
4   4  data        meaning varies by message (CAL_COMMIT→commit_seq; CMD→the forwarded cmd_seq; others 0)
```

### 4.1 HELLO (0x01 / 0x81)

Request payload empty. The wire v10 response is 84 octets:

```
0   2  proto_ver     = V2K_WIRE_VER (=10; host disconnects on mismatch)
2   2  contract_ver  = V2K_CONTRACT_VER
4   4  build_hash    baker-generated final-image hash (§5.1 re-enumeration basis)
8   2  catalog_count total variable-catalog entry count
10  2  reserved
12  16 fw_name       ASCII, e.g. "viewer2000"
28  4  tick_hz       CPU1 ISR tick frequency; the sole basis for converting block tick to seconds
32  4  capabilities  device capability bits
36  32 project_name  CPU1-baked project name, printable ASCII, NUL-padded; max 32 visible chars
68  4  build_time_utc CPU1 firmware build time as Unix UTC seconds; human-readable only, not a safety hash
72  2  mcu_model     public MCU family identifier: 0=unknown, 1=F28P65x, 2=F28379D
74  2  scope_max_ch  maximum accepted DAQ_BIND channel count for this firmware/profile
76  2  scope_block_ticks nominal producer block tick count used by Scope capacity calculations
78  2  reserved      set to 0
80  4  scope_ring_words C28x 16-bit words in the Stream/Capture scope ring
```

`capabilities` (append-only, no reuse):

| bit | name | semantics |
|---:|---|---|
| 0 | ENUM | descriptor enumeration |
| 1 | CAL | parameter read/write and atomic commit |
| 2 | SCOPE_STREAM | Scope continuous block flow |
| 3 | SCOPE_CAPTURE | Scope device-side trigger-freeze window |
| 4 | PRE_TRIGGER | the trigger-freeze pre-trigger ring history |
| 5 | SYSTEM_CMD | Start / Stop / Clear Fault |
| 6 | NATIVE_BLOCK | native-bit-width `ScopeBlock` |

The host rejects shorter HELLO responses because scope capacity and channel-count
limits are part of the v10 connection contract.

`project_name` is copied from CPU1's CCS/Eclipse `cpu1/.project` `<name>` field as-is, after only the wire-level printable-ASCII/32-character check. If the name is empty it becomes `untitled`. If it is `untitled`, the CPU1 post-link baker emits a build warning but does not fail the build. These HELLO fields are for human identification only. Machine safety and host cache invalidation still use `build_hash`.

### 4.2 STATUS (0x02 / 0x82, push 0x41)

`STATUS_REQ` request payload is empty and returns `STATUS_RESP`. Firmware also
pushes `STATUS_PUSH` at about 4 Hz after a valid v10 host frame is observed.
Both response and push carry the same 96-octet payload:

```
0   2  sys_state      V2K_STATE_* (v2k_command.h)
2   2  fault_code
4   2  status_flags   V2K_SF_* (`CPU2_LOST` from CPU1; `CPU1_STALE` may be ORed by CPU2 while serializing STATUS)
6   4  tick           CPU1 current ISR tick
10  4  cpu1_heartbeat
14  4  cpu2_heartbeat
18  4  applied_seq    parameter-plane reconcile (§5.2)
22  2  cal_result     V2K_CAL_*
24  2  cal_fail_idx
26  4  build_hash     detect a firmware hot-swap during a session
30  1  scope_mode     current V2K_SCOPE_* (OFF/STREAM/CAPTURE_ARMED/CAPTURE_POST/CAPTURE_FROZEN)
31  1  scope_flags    reserved currently, set 0
32  2  reserved
34  4  cmd_ack_seq    the max system-command seq CPU1 has executed
38  2  cmd_result     V2K_CMDR_* (corresponding to cmd_ack_seq)
40  2  reserved
42  4  prof_seq       runtime-load snapshot sequence, sampled before the profiler fields
46  4  cycle_budget   V2K_CPUTIMER_HZ / V2K_ISR_HZ
50  4  load_avg       mean ADC/EOC latency + ISR cycles in the completed one-second window
54  4  load_peak      peak ADC/EOC latency + ISR cycles in the completed one-second window
58  4  ctrl_at_peak   user control() body cycles on the peak tick
62  4  scope_at_peak  scope epilogue cycles on the peak tick
66  2  lat_at_peak    ADC/EOC entry latency on the peak tick, in CPUTIMER cycles
68  4  peak_tick      hidden bring-up correlation tick for the peak record
72  4  budget_violations  lifetime ISR budget violations
76  4  isr_overflows  lifetime ADC interrupt overflows
80  4  prof_seq_end   same source field as prof_seq, sampled after the profiler fields
84  2  scope_state_seq current scope producer state sequence
86  2  scope_frozen_count frozen block count for the current capture, valid in CAPTURE_FROZEN
88  4  scope_trigger_tick trigger-hit ISR tick for the current frozen capture
92  2  scope_bind_seq active binding sequence acknowledged by CPU1
94  2  reserved
```

The host accepts profiler fields only when `prof_seq != 0` and
`prof_seq == prof_seq_end`. A mismatch means CPU1 was publishing a new snapshot
while CPU2 serialized STATUS; the regular status fields remain valid, but that
profiler sample is discarded. Runtime cycles are host-derived as
`load_peak - lat_at_peak - ctrl_at_peak - scope_at_peak`.

System-command codes are append-only for platform commands:
`0=NOP`, `1=APP_START`, `2=APP_STOP`, and `3=CLEAR_FAULT`.

System-command results are append-only: `0=OK`, `1=BAD_CMD`,
`2=BAD_STATE`, `3=NOT_READY` (power-stage readiness failed), and
`4=START_FAILED` (user-state restore or setup preparation failed). Detailed
diagnostics are enumerated Variables: `start_block` for board-owned
power-stage preconditions and `user_reset_err` for user-state restoration.

Fault codes are append-only: `0=NONE`, `1=TZ1_EXT` (external trip-zone source),
and `2=OVERCURRENT` (CMPSS or ADC PPB current-window hardware trip).
`start_block` bit `0x0020` means the current
protection route failed register read-back, a current sample was outside its
startup window, or DCAEVT1 asserted while START attempted to release OST.
Detailed current-source bits are available through the enumerated
`curr_trip_last` Variable.

### 4.3 ENUM (0x03 / 0x83)

The enumeration object is CPU1's catalog: platform quantities registered by L1/L0 plus user application variables baked into CPU1 Flash from the firmware DWARF. CPU2 is only a stable proxy. After the initial v10 CPU2 upgrade, user catalog changes require rebuilding/flashing CPU1 only.

Request (4 octets): `{0:2 start_idx, 2:1 max_count(≤8), 3:1 reserved}`
Response (6 + variable entries, up to `V2K_WIRE_MAX_PAYLOAD` octets):

```
0  2  total_count    (= catalog_count)
2  2  start_idx      echoed
4  1  count          actual entries this page
5  1  reserved
6  …  count × variable-length catalog entry:
      0:4 addr | 4:2 type | 6:2 kind | 8:2 prescaler | 10:1 name_len | 11:1 reserved | 12:name_len name_octets
      kind bit0=PARAM, bit1=SCOPE, bit2=USER (build-time-baked user origin;
      clear for platform/system descriptors); bits3..15 reserved
      prescaler is the default-sampling-division suggestion; the runtime actual rate is per DAQ_CTRL
```

`start_idx ≥ total_count` → `count=0` (a legal "done reading" signal).
Because names are variable length, `count` may be less than `max_count` before
the end of the catalog when the next complete entry would exceed the payload
limit. The host must advance by `count` and stop only when
`start_idx + count >= total_count` or when it explicitly requests
`start_idx ≥ total_count`.

Scope2000 presents USER entries in its main `All Variables` tree and keeps
platform/system entries in a separate diagnostics tree; both still use the
same `(addr,type)` CAL/DAQ services.

### 4.4 CAL_WRITE / CAL_COMMIT / CAL_READ (0x10/0x11/0x12)

`CAL_WRITE` request (2 + 12k octets):

```
0  1  count   entries this frame k (the accumulated stage must not exceed V2K_PARAM_BATCH_MAX=16)
1  1  reserved
2  …  k × {0:4 addr, 4:4 value_bits, 8:2 type, 10:2 reserved} (mirrors v2k_param_write_t)
```

Semantics: CPU2 writes into the parameter-plane shadow stage **but doesn't publish**; multi-frame accumulation, **the same addr overwrites the staged entry** (the basis for resend idempotency); over the limit returns ACK(BAD_PARAM). `CAL_COMMIT` (payload empty) → CPU2 fills count then writes `commit_seq+1` last (publish), returns ACK(OK, data=commit_seq). The applied result is reconciled via STATUS's `applied_seq/cal_result` (§5.2).

Commit semantics: CPU1 does only a mechanical-consistency check per write — type legal, address in a CPU1 data region allowed for writing, 32-bit-type address aligned; when addr hits the CPU1 catalog it additionally requires `kind&PARAM` and a matching type. **No min/max range check, no clamp, no scale/offset back-calculation.** If any item in the batch fails the mechanical check the whole batch is rejected; after the check passes, the whole batch is written on the same tick at the ISR safe point.

`CAL_READ` request (2 + 8k octets):

```
0  1  count   read entries this frame k (1..V2K_CAL_READ_MAX=32)
1  1  reserved
2  …  k × {0:4 addr, 4:2 type, 6:2 reserved} (mirrors v2k_param_read_ref_t)
```

Semantics: CPU2 writes the GS4 read request and publishes `read_seq+1`; CPU1, at a background ~1 ms poll point, reads the CPU1 data-region address on demand once, writes the GS0 read response, and writes `ack_seq` last. CPU2 waits for `ack_seq==read_seq` then returns the response; an illegal address/type returns ACK(BAD_PARAM), a wait timeout returns ACK(INTERNAL).

Response (8 + 4×count):

```
0  4  read_seq
4  1  count
5  1  reserved
6  2  reserved
8  …  count × value_bits(4)
```

### 4.5 DAQ_CTRL / DAQ_BIND (0x20 / 0x22)

`DAQ_CTRL` request 24 octets. It selects and configures the Scope entry: `STREAM` is the continuous block flow; `CAPTURE_ARMED` is the device-side trigger-freeze window. The two use the same binding and block format, but to the host they are two entries.

```
0  2  mode_req      V2K_SCOPE_OFF / STREAM / CAPTURE_ARMED
2  2  trig_ch_slot  trigger source = the currently-bound channel slot 0..n_ch-1 (ignored in STREAM)
4  4  trig_level    f32, **source value domain** (f32 variable = the value itself, ADC count = the count value;
                    the firmware has no physical-conversion knowledge, the host scales by display metadata then sends)
8  4  trig_hysteresis f32, trigger hysteresis half-width, absolute value in the source domain; 0 = most sensitive
12 2  trig_edge     V2K_TRIG_* (ignored in STREAM)
14 2  pre_trig_pct  0..100 (ignored in STREAM)
16 2  prescaler     0 = keep the current value
18 2  record_points Capture target sample count; set 0 and ignored in STREAM/OFF
20 2  ack_capture_id completed capture id being released while re-arming; set 0xFFFF for manual fresh starts
22 2  flags         reserved, set 0
```

CPU2 writes cfg and publishes `cfg_seq`, waits for CPU1's `cfg_ack_seq/cfg_result` then returns ACK: OK=config applied; BAD_PARAM=field illegal or record_points exceeds the exact Capture capacity under the current binding; BAD_STATE=the current state doesn't accept this config. The mode can also be re-checked via STATUS's `scope_mode`.

`DAQ_BIND` request (2 + 8k octets) — **select channels at runtime, no reflash**:

```
0  1  n_ch          1..16
1  1  reserved      set 0
2  …  k × channel binding (8 octets, field-mirroring v2k_scope_ch_bind_t):
      0:4 addr | 4:2 type | 6:2 reserved
```

Semantics: addr comes from the CPU1 catalog (platform quantities + build-time-baked user variables, §2); samples are losslessly direct-copied at **native width** (I16/U16→2 octets, I32/U32/F32→4 octets, bit pattern verbatim, firmware zero-conversion zero-quantization — accuracy first). Physical-quantity conversion (e.g. ADC count→ampere) is pure host-side display metadata, not on the wire, not in the firmware. **Bindable only when scope mode==OFF.** CPU2 writes the bind region and publishes `bind_seq`, then briefly waits (≤1 ms) for CPU1's `bind_ack_seq/bind_result`, putting the final result into the ACK: OK / BAD_STATE (not OFF, DAQ_CTRL(OFF) first) / BAD_PARAM (n_ch or type illegal), data=bind_seq; a timeout returns INTERNAL (CPU1 ISR not running).

### 4.6 Stream and Capture Push (0x42 / 0x45 / 0x21)

CPU2 pushes stream and capture data whenever no command response is pending.
`SCOPE_BLOCK_PUSH` is stream-only. Capture uses `CAPTURE_BATCH_PUSH`, where the
host assembles a bitmap over `0..total_blocks-1` and requests replay only for
missing capture indices. The normal no-loss capture path has no host pull round
trip and no drain marker.

`SCOPE_BLOCK_PUSH` uses frame `seq=push_frame_seq`, incremented by one for each
push frame. The payload is `8 + Σblock` octets:

```
0  1  count        number of complete blocks in this push frame
1  1  reserved
2  2  overrun_cnt  producer-side cumulative dropped stream blocks
4  2  remain_hint  blocks still takeable in the stream ring after this frame
6  2  reserved
8  …  count × block
```

block = the scope-plane memory layout on the wire verbatim (**zero re-encoding on the hot path**):

```
0  4  start_tick | 4:2 block_seq | 6:2 flags | 8:2 n_ticks | 10:2 n_ch
12 2  bind_seq      the binding id that produced this block (host discards mismatched old blocks after rebinding)
14 2  stride_octets per-tick sample-region width = Σ channel native widths (block self-describing delimiter)
16 …  sample region n_ticks × stride_octets: tick-major, within each tick in binding order,
      each channel laid out contiguously at its native width (I16/U16=2, I32/U32/F32=4, LE bit pattern lossless)
```

The host detects dropped blocks by `block_seq` jumps → draws a gap, **there is no retransmission** (rule 1).

When Capture reaches `CAPTURE_FROZEN` with a new `state_seq`, CPU2 sends
`CAPTURE_BATCH_PUSH` frames sequentially once, using `capture_id = state_seq`.
The payload is `16 + Σblock` octets:

```
0   2  capture_id        current scope state_seq
2   2  total_blocks      frozen block count for this capture
4   2  first_block_index capture index of the first block in this batch
6   1  count             number of contiguous blocks in this batch
7   1  flags             bit0=replay, other bits reserved
8   2  remaining_hint    blocks left in this initial/replay cursor after this frame
10  2  reserved
12  4  trigger_tick      ISR tick of the trigger hit
16  …  count × block
```

Blocks in one batch are contiguous capture indices:
`first_block_index + i`. The host validates `capture_id`, `total_blocks`, block
count, binding sequence, and CRC-decoded frame validity, inserts each block by
capture index, and ignores duplicate replay blocks. Completion is exactly "all
bitmap bits are present"; then the host sorts/renders by capture index, trims
around `trigger_tick`, and sends `DAQ_CTRL(CAPTURE_ARMED,
ack_capture_id=capture_id)` to release/re-arm. CPU2 keeps the frozen ring valid
until CPU1 leaves `CAPTURE_FROZEN` through `DAQ_CTRL(OFF/STREAM/CAPTURE_ARMED)`.

`CAPTURE_REPLAY_REQ` uses message id `0x21` in v10:

```
0  2  capture_id
2  2  first_block_index
4  1  max_blocks
5  3  reserved
```

Firmware ACKs the request immediately, then enqueues replay
`CAPTURE_BATCH_PUSH` frames from the frozen ring with `flags bit0` set. Invalid
or stale capture id returns `BAD_STATE`; an out-of-range block index or zero
`max_blocks` returns `BAD_PARAM`. Replay uses a separate replay cursor/range and
does not mutate the frozen initial-push cursor or the stream consumer cursor.

Bandwidth reference (ISR period 20-100 kHz TBD): 20 kHz x 8 ch x f32 = 640 KB/s; 100 kHz x 8 ch x f32 = 3.2 MB/s. Both are within EtherCAT practical throughput, so the physical-layer conclusion is unchanged. The EtherCAT-tier N is fixed by the single-frame process-data ceiling, about 1486 octets.

### 4.7 CMD (0x30)

Request (8 octets): `{0:2 cmd_code(V2K_CMD_*), 2:2 arg0, 4:4 arg1}`
CPU2 checks the mailbox is free (`cmd_seq == ack_seq`) → writes the command mailbox, returns ACK(OK, data=cmd_seq); mailbox busy → ACK(BUSY), host retries later. The execution result is confirmed via STATUS's `cmd_ack_seq/cmd_result/sys_state`.

`APP_START` is asynchronous from CPU1's point of view. CPU2 ACKs only mailbox
acceptance immediately; the command result becomes final only when STATUS
reports the matching `cmd_ack_seq`.

## 5. Service semantics

### 5.1 Session establishment and forced re-enumeration

```
host                                                     firmware(CPU2)
 │ ─────────────────────── HELLO_REQ ──────────────────────→ │
 │ ←────────────────────── HELLO_RESP ────────────────────── │  proto_ver mismatch → host aborts and reports an error
 │ ─────────────────────── ENUM_REQ(0,8) ──────────────────→ │
 │ ←────────────────────── ENUM_RESP ─────────────────────── │  … page until count < max or start≥total
 │ (then receive STATUS_PUSH, SCOPE_BLOCK_PUSH, CAPTURE_BATCH_PUSH) │
```

The host caches the variable catalog, keyed by `build_hash`. The descriptor baker computes this value from the final ELF with the patch section normalized, plus the generated catalog records. It therefore changes when linked code, addresses, or the baked variable set changes, including dirty-tree builds. At any time (in HELLO or STATUS), detecting a `build_hash` change means **invalidate the entire cache and re-enumerate**. This prevents reading new firmware with an old catalog.

User application-variable discovery: a build tool reads the CPU1 firmware `.out` DWARF and bakes each user variable's `name→addr→type` into the CPU1 Flash catalog (struct members / array elements expanded into named scalar entries). The host enumerates them over ENUM like any platform quantity — no `.out` on the host, no stale-ELF risk. CPU2 remains unchanged across CPU1-only user catalog rebuilds as long as the shared contract version is unchanged.

### 5.2 Parameter transaction (two-stage + async reconcile)

```
host                         CPU2                       CPU1 ISR safe point
 │ ─────── CAL_WRITE ×m ─────→ │ stage shadow                    │
 │ ←────── ACK(OK) ×m ──────── │                                 │
 │ ─────── CAL_COMMIT ───────→ │ publish commit_seq=s ─────────→ │ sees s≠applied_seq:
 │ ←────── ACK(OK,data=s) ──── │                                 │ mechanical check→apply whole set→
 │ ←────── STATUS_PUSH ─────── │ read the parameter-status block │ write applied_seq=s
```

Key points: within a batch **all valid or all rejected** (takes effect on the same tick, cal_result/cal_fail_idx report the cause; all successful writes are native-bit-pattern writes, with no range or unit conversion); the host must not issue the next COMMIT batch before applied_seq catches up to commit_seq.

`CAL_READ` is an on-demand single read, not the periodic mirror: the host sends an `(addr,type)` list, CPU2 publishes the read request, CPU1's background reads and acks. It serves the Variable Map / on-demand watch; high-speed real-time waveforms use the DAQ_BIND + SCOPE_BLOCK_PUSH/CAPTURE_BATCH_PUSH scope path.

### 5.3 Scope flow

Channel selection (before any mode starts): `DAQ_CTRL(OFF)` -> `DAQ_BIND(channel list)` -> only after ACK(OK) may you start. Binding is on-demand; the device does not rely on a boot default binding.

Scope UX is transport-tiered. On the SCI/XDS110 path, Scope2000 exposes the
firmware-triggered finite Capture entry as the normal user path because SCI
cannot sustain full-rate native-width Stream for typical multi-channel
bindings. Stream remains a protocol capability, but the host keeps it behind an
explicit advanced setting until a high-bandwidth transport is selected. On the
future EtherCAT path, the host may enable continuous Stream as the primary
workflow: firmware pushes live blocks, Scope2000 maintains the long host-side
ring, performs host-side trigger/search over that ring, and uses gaps only to
mark true transport or producer loss. On SCI, trigger-before-transfer remains a
firmware job, so Capture uses a bounded firmware ring plus bitmap completion
and selective replay.

**Stream entry**: `DAQ_CTRL(mode=STREAM, prescaler, record_points=0)` -> CPU2 pushes `SCOPE_BLOCK_PUSH` frames whenever complete blocks are available. When the ring fills the producer drops new blocks + overrun_cnt++, the flow doesn't stop, and the host draws a gap by block_seq.

**Capture entry**: under the same channel binding, send `DAQ_CTRL(mode=CAPTURE_ARMED, trig…, pre_trig_pct, prescaler, record_points, ack_capture_id=0xFFFF)` -> CPU1 overwrites the ring in the same block format and evaluates the trigger each tick -> after a hit it enters CAPTURE_POST to capture the post segment (depth = record_points×(100-pre_trig_pct)%) -> CAPTURE_FROZEN (visible in STATUS.scope_mode and STATUS.scope_state_seq) -> CPU2 pushes all frozen blocks once via `CAPTURE_BATCH_PUSH` -> the host fills a capture bitmap, selectively requests missing ranges with `CAPTURE_REPLAY_REQ`, renders when the bitmap is complete, then re-arms with `DAQ_CTRL(CAPTURE_ARMED, ack_capture_id=capture_id)` or switches to STREAM/OFF. The pre-trigger history is naturally preserved by the ring structure; the host reconstructs block order by capture index.

Capture freezes complete block slots plus one guard block so a trigger that
lands in the middle of a block still contains enough samples for the host to
trim an exact `record_points` window around `trigger_tick`. Therefore the host's
maximum submitted record count is `(capacity_blocks - 1) * scope_block_ticks`
for the active binding, where `capacity_blocks =
floor(scope_ring_words / aligned_slot_words)` (exact fit; the former
power-of-two rounding was removed together with the free-running ring
indices -- firmware ring indices now wrap at `2 * capacity_blocks`, which
stays invisible on the wire).

The "watch window" of an application variable = after selecting variables, run STREAM with a larger `prescaler` — it carries tick timestamps and is lossless native bit pattern, replacing independent poll-style monitoring. When you need a per-tick waveform set prescaler to 1; when you need to reduce link pressure increase prescaler, reduce channels, or lower the block frequency.

### 5.4 Error handling and resync

- Corrupt frames silently dropped (§3.1). The host sets a timeout per request (suggested 100 ms) + resend.
- CPU2 caches the last encoded response in the high-priority response slot; when the host resends the same `(msg_type, seq)` on timeout, it replays directly, without re-executing CAL_COMMIT, CMD, or DAQ_BIND.
- The service itself stays safely retryable: a repeated CAL_WRITE overwrites the same-addr staged entry; DAQ_BIND overwrites the whole region. Only after the host abandons the old request and uses a new seq is it treated as a new operation.
- The host discards late/mismatched responses by the frame seq echo, and continues to process push frames while waiting for a response.
- Sync recovery: the host sends any request, and the firmware decoder auto-resyncs at the 0x00 boundary. Normal streaming does not require periodic `STATUS_REQ`; capture replay requests are sent only when the host's bitmap has holes.

## 6. Versioning and evolution strategy

| Change | Mechanism |
|---|---|
| Variable add/remove/modify | doesn't touch the protocol — build_hash changes → re-enumerate (most common, zero cost) |
| New message | takes a new code; the old end returns ACK(UNSUPPORTED), backward-compatible |
| Message field append | appended at the payload tail + length discrimination; the parser ignores the extra tail |
| Incompatible layout change | `V2K_WIRE_VER` +1 (HELLO refuses on mismatch) |
| Shared-struct change | `V2K_CONTRACT_VER` +1 (intercepted by the dual-core startup handshake, see v2k_command.h) |

---

## Appendix A: protocol-selection ADR (finalized 2026-06-11)

**Decision**: a custom message catalog + standard framing primitives (COBS + CRC-32C), service semantics aligned with XCP vocabulary, variable description via runtime enumeration.

**Background**: four "off-the-shelf routes" were evaluated — XCP / MAVLink / nanopb(protobuf) / CBOR. Two platform facts run through all evaluations: one, C28x CHAR_BIT=16, so every off-the-shelf library that assumes an 8-bit byte + packed struct + memcpy needs its serialization core rewritten, and "save serialization code" — the biggest advantage — doesn't hold for any external solution; two, CLAUDE.md mandates that the memory layout and the wire format are the same data model, so any independent schema language (.proto / a dialect XML) would create a second definition.

| Candidate | Reason not chosen |
|---|---|
| **XCP** (the industry standard in this field) | the domain model fits perfectly (CAL/DAQ are the parameter/scope planes), but its variable description relies on offline A2L (ELF-generated), the opposite of the runtime-enumeration idea — after adding private enum extensions an off-the-shelf master (CANape etc.) can't use it, shrinking most of the ecosystem advantage; a minimal-subset implementation is still ~2–3k lines of hand-written 16-bit-char; DAQ's one DTO header per event is 20–30% overhead at 100 kHz, worse than a block amortizing one header over 50 ticks; the EtherCAT phase has no standard XCP binding. **Kept**: the semantic-vocabulary alignment + the fallback of later adding an XCP facade on CPU2. |
| MAVLink | the parameter microservice is a flat float table, can't hold runtime-enum metadata like type/kind/prescaler → the variable catalog still needs custom messages; payload ≤255 octets → an 800B block needs 4-piece reassembly; the official C generated code = packed struct + memcpy, on C28x that means rewriting the generator backend. All that's kept is the heartbeat and the XML format. |
| nanopb / protobuf | the official explicitly doesn't support CHAR_BIT≠8 platforms (self-maintained private fork); varint makes golden vectors not human-checkable; .proto can't express the shared-RAM layout, so the shared struct still needs hand-written alignment — the second-data-model cost is paid anyway. |
| CBOR (control plane) | there's the Zephyr MCUmgr precedent, field-evolution friendly; but the firmware needs ~400 lines of self-written 16-bit-char-safe codec subset (building a more general wheel), one protocol with two encodings, and the vectors need pinned deterministic encoding. The evolution need is already covered cheaply by "field append + length discrimination + re-enumeration mechanism". |

**Industry comparison**: the framing layer (COBS/CRC) is all standard; fixed-length little-endian layout is the mainstream school in this field (XCP/MAVLink payload/ST MCP); runtime enumeration follows the ODrive(Fibre)/Klipper(data dictionary) open-source precedents; the RCP sub-industry (dSPACE/Imperix etc.) all use custom private protocols anyway — this scheme = that industry's convention + an anti-rot process (the spec as the single source of truth + golden-vector dual-end conformance + version fields), aimed precisely at the lesson of "magic-offset archaeology" in existing private protocols.

**The biggest risk of the custom route and the countermeasure**: the risk isn't being unable to write it, but rot ten years later. The countermeasure is written into the process: this document's change flow (top of the doc), the vectors-are-authoritative rule, message-catalog additions not modifying the old, and the three-layer version fields (wire/contract/build_hash) each owning a segment.

### ADR-2: variable-discovery architecture (finalized 2026-06-11)

**Decision (revised 2026-06-30)**: CPU1 owns the Flash catalog carrying platform quantities plus user application variables baked at build time from DWARF. Names travel with the device, but not in realtime traffic and not as a persistent CPU2-owned dictionary. Discovery is over ENUM; scoping via DAQ_BIND and parameter writing via CAL_WRITE are both keyed by `(addr,type)`.

**Rejected forms**: ① L2-component init self-registration (`pi_init(&pi, "vel")`) and ② user-side stringified registration macros — both force a second name string or a mandated declaration style; the C symbol is the only acceptable name source. ③ **Host-side runtime `.out`/DWARF parsing** (the original 2026-06-11 plan) — rejected 2026-06-19 because it ties Scope2000 to the project directory and risks a stale/wrong ELF writing to the wrong address. The C symbol is still the only name source, but it is harvested **at build time** and baked into the device.

**Cost and countermeasure**: the descriptor build tool runs TI `ofd2000 --xml --dwarf` on the firmware `.out` and bakes a compact `name→addr→type` table into the reserved image blob; the `.out`-to-device pairing is automatic (same build) and build_hash guards the host cache. The protocol carries no `min/max/scale/offset`: the value itself must already be the real quantity to display, log, and write back. In exchange: zero registration code and zero naming burden for students, any supported struct member / array element observable, fully runtime channel selection (no reflash), and **names that travel with the device — no `.out` on the host**.

## Appendix B: Scope2000 `DataSource` Boundary

Scope2000's native implementation is `V2kSource`, internally split strictly into three layers: service semantics, message codec, and byte-stream transport. SCI is the initial byte-stream transport; when adding the EtherCAT transport, the GUI data model and service commands do not change.

```rust
pub enum SourceCommand {
    Connect(TransportEndpoint),
    Disconnect,
    WriteParams(Vec<ParamWrite>),
    CommitParams,
    ReadValues(Vec<ValueRead>),
    BindChannels { channels: Vec<VarRef> },
    ConfigureScope(ScopeConfig),
    SystemCommand(SystemCommand),
}

pub enum SourceEvent {
    Connected(DeviceInfo),
    Descriptors(Vec<VarDescriptor>),
    Status(DeviceStatus),
    Blocks(Vec<ScopeBlock>),
    StreamGap { expected: u16, received: u16 },
    DeviceChanged { old_hash: u32, new_hash: u32 },
    // parameter, binding, mode, error, and log events omitted
}
```

`ScopeBlock` keeps `start_tick/block_seq/bind_seq/stride_octets` and the native `samples: Vec<u8>`; only the plotting or CSV-export boundary expands them into numeric values by binding type, applying no scale/offset.

Legacy-device compatibility is not implemented as a dedicated data source inside Scope2000. A future standalone `LegacyBridge` process handles the old protocol, and the other side exposes normalized Viewer2000 message semantics over a generic local byte-stream transport, declaring missing capabilities with capability bits. The bridge may synthesize tick/sequence numbers and explicitly report precision limits, but must not define, trim, or slow down the `V2kSource` native path.
