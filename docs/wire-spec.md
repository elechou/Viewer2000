# Viewer2000 wire protocol specification (wire spec) v6

> **Document status**: this document together with the `contracts/` headers forms the single source of truth for the protocol; the golden test vectors under `contracts/vectors/` are the executable form of this document. Both the firmware C serializer and the host Rust parser must pass the conformance test against the same set of vectors. When the three disagree, **the vectors are authoritative**, and the document is fixed immediately.
>
> **Change flow**: any message-layout change → edit this document → edit `tools/gen_vectors.py` then regenerate vectors → both ends' codecs follow → an incompatible change must bump `V2K_WIRE_VER`.

---

## 1. Layered model

```
┌─────────────────────────────────────────────────────────────────────────────┐
│ Service-semantics layer  session/enum/param txn/DAQ stream/command (§5)     │ transport-agnostic
├─────────────────────────────────────────────────────────────────────────────┤
│ Message layer  message catalog v6, fixed-length little-endian layout (§4)   │ transport-agnostic
├─────────────────────────────────────────────────────────────────────────────┤
│ Frame adapter  per-transport:                 (§3)                          │ transport-dependent
│             SCI = COBS + frame header + CRC-32C                             │
│             EtherCAT = mailbox/PDO self-delimited, the frame layer vanishes │
├─────────────────────────────────────────────────────────────────────────────┤
│ Physical layer  SCI(XDS110 VCP) → EtherCAT 100M                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

Design invariant: **everything above the message layer is transport-agnostic**. Swapping the physical layer only swaps the frame adapter (the entire migration of Phase 3.5 → 6); the messages and shared interfaces (`contracts/*.h`) don't move a bit.

The service semantics keep the generic vocabulary CAL/DAQ/prescaler from XCP (ASAM MCD-1), but Viewer2000 exposes two Scope entries: Stream = continuous block flow, Capture = device-side trigger-freeze window. The two reuse the same channel binding, ring, and block format; the firmware protocol no longer exposes the XCP event-channel / group concept.

## 2. Common conventions

- **octet** = on-wire 8-bit byte; **word** = C28x 16-bit unit. This file uses octet throughout.
- Multi-octet integers are always **little-endian (LE)**; `f32` = the LE bit pattern of IEEE-754 single precision.
- Strings = ASCII fixed-length, NUL-padded (NUL termination not guaranteed).
- In the offset tables, `off:sz` is in octets.
- Master/slave model: **the host is the sole initiator** (request-response), the firmware has no spontaneous frames. This is isomorphic to EtherCAT's master polling model — what Phase 3.5 validates is the semantics of Phase 6.
- The `value_bits` (parameter value bit pattern) convention is in `v2k_common.h`: F32 native bit pattern, I16 sign-extended / U16 zero-extended to 32 bit.
- **The universal key for variable addressing = `(addr, type)`** (CPU1 data-space word address + type code). All addresses come from one source the host enumerates over the wire: **the descriptor table**, which holds the platform quantities (registered by L0/L1) plus the user's application variables (**baked into the table at build time from the firmware DWARF — Phase 4.5**; struct members / array elements expanded into named scalar entries; pairing guarded by build_hash). The names therefore travel with the device — the host needs no `.out`. Users/students write plain C: no registration code, no name strings, no mandated declaration style.
- **The on-wire value is the real value**: the protocol carries no `min/max/scale/offset` display or guard-rail metadata. Both the DAQ block and the CAL value interpret the bit pattern by the variable's native type; the firmware does no quantization, no conversion, no parameter range clamping or range rejection.

## 3. Frame adapter

### 3.1 SCI frame

```
Pre-encoding frame ("raw frame", CRC coverage = offset 0 .. 7+n-1):

off  sz  field
0    1   ver_magic   = 0x56 (high nibble 0x5 fixed magic, low nibble = V2K_WIRE_VER)
1    1   msg_type    (§4 catalog; response = request | 0x80)
2    1   flags       = 0x00, reserved
3    2   seq         host +1 per request (wraps); the response echoes it verbatim, the host pairs by it
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
- **Decoder discard rule**: COBS decode failure / `ver_magic` mismatch / length mismatch / CRC mismatch → silently drop the whole frame (a corrupt frame's content is untrustworthy, **no NAK**). Reliability is ensured by the host's timeout resend (all requests idempotent, see §5.4).
- The frame `seq` and the block header's `block_seq` (v2k_scope.h) have different jobs and **must not be merged**: the former handles link-layer request-response pairing and resend dedup; the latter handles data-stream dropped-block detection, and a dropped block isn't refilled by the link layer (rule 1), the host draws a gap.

### 3.2 EtherCAT mapping plan (executed in Phase 6, principle fixed here)

| This protocol's element | EtherCAT carrier | Note |
|---|---|---|
| The whole frame-adapter layer | (vanishes) | mailbox/PDO is self-delimited, Ethernet FCS carries its own check |
| Control-plane messages (HELLO/ENUM/CAL/DAQ_CTRL/CMD) | mailbox (minimal CoE or vendor mailbox) | the raw frame drops COBS and CRC, `ver_magic..payload` loaded verbatim |
| BLOCK_DATA payload | TxPDO fixed layout | `count + 0–2 blocks`, the master grabs more when there's data each 2 kHz cycle |
| BLOCK_REQ | (vanishes) | reading the PDO each cycle is an implicit request |
| STATUS_RESP core fields | TxPDO header region | state/fault/heartbeat visible with the stream |

The timestamp is always the ISR tick in the block header, unrelated to the EtherCAT DC clock system (no DC).

## 4. Message catalog v6

### 4.0 Master table

| code | name | dir | payload | response |
|---|---|---|---|---|
| 0x01 | HELLO_REQ | H→F | 0 | 0x81 HELLO_RESP |
| 0x02 | STATUS_REQ | H→F | 0 | 0x82 STATUS_RESP |
| 0x03 | ENUM_REQ | H→F | 4 | 0x83 ENUM_RESP |
| 0x10 | CAL_WRITE | H→F | 2+12k | 0x90 ACK |
| 0x11 | CAL_COMMIT | H→F | 0 | 0x91 ACK (data=commit_seq) |
| 0x12 | CAL_READ | H→F | 2+8k | 0x92 CAL_READ_RESP |
| 0x20 | DAQ_CTRL | H→F | 20 | 0xA0 ACK |
| 0x21 | BLOCK_REQ | H→F | 2 | 0xA1 BLOCK_DATA |
| 0x22 | DAQ_BIND | H→F | 2+8k | 0xA2 ACK (data=bind_seq) |
| 0x30 | CMD | H→F | 8 | 0xB0 ACK (data=ack_seq) |
| 0x60–0x6F | (reserved) firmware update | — | — | finalized in Phase 6+ |
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

Request payload empty. Response base prefix 28 octets, the current wire v6 response is 72 octets; new fields are only allowed appended at the tail:

```
0   2  proto_ver     = V2K_WIRE_VER (host disconnects on mismatch, hinting a firmware/host version mismatch)
2   2  contract_ver  = V2K_CONTRACT_VER
4   4  build_hash    baker-generated final-image hash (§5.1 re-enumeration basis)
8   2  desc_count    total descriptor count
10  2  reserved
12  16 fw_name       ASCII, e.g. "viewer2000"
28  4  tick_hz       CPU1 ISR tick frequency; the sole basis for converting block tick to seconds
32  4  capabilities  device capability bits
36  32 project_name  CPU1-baked project name, printable ASCII, NUL-padded; max 32 visible chars
68  4  build_time_utc CPU1 firmware build time as Unix UTC seconds; human-readable only, not a safety hash
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

An old parser can read only the 28-octet prefix; if a new parser receives a shorter response, the absent tail fields are treated as 0/empty.

`project_name` is copied from CPU1's CCS/Eclipse `cpu1/.project` `<name>` field as-is, after only the wire-level printable-ASCII/32-character check. If the name is empty it becomes `untitled`. If it is `untitled`, the CPU1 post-link baker emits a build warning but does not fail the build. These HELLO fields are for human identification only. Machine safety and host cache invalidation still use `build_hash`.

### 4.2 STATUS (0x02 / 0x82)

Request payload empty. Response 42 octets; doubles as the link heartbeat (host periodic polling):

```
0   2  sys_state      V2K_STATE_* (v2k_command.h)
2   2  fault_code
4   2  status_flags   V2K_SF_* (includes the CPU1-heartbeat-stopped bit from CPU2's view, runtime-extended)
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
```

### 4.3 ENUM (0x03 / 0x83)

The enumeration object = the descriptor table = the platform quantities L1/L0 register (physical quantities/duty/state/platform parameters) plus the user's application variables baked in at build time from the firmware DWARF (Phase 4.5, §2 convention) — so a host with no `.out` still enumerates everything by name.

Request (4 octets): `{0:2 start_idx, 2:1 max_count(≤8), 3:1 reserved}`
Response (6 + 28×count octets):

```
0  2  total_count    (= desc_count)
2  2  start_idx      echoed
4  1  count          actual entries this page
5  1  reserved
6  …  count × descriptor entry (28 octets, field-mirroring the current v2k_desc_entry_t):
      0:16 name | 16:2 type | 18:2 kind | 20:4 addr | 24:2 prescaler | 26:2 reserved
      kind bit0=PARAM, bit1=SCOPE, bit2=USER (build-time-baked user origin;
      clear for platform/system descriptors); bits3..15 reserved
      prescaler is the default-sampling-division suggestion; the runtime actual rate is per DAQ_CTRL
```

`start_idx ≥ total_count` → `count=0` (a legal "done reading" signal).

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

Commit semantics: CPU1 does only a mechanical-consistency check per write — type legal, address in a CPU1 data region allowed for writing, 32-bit-type address aligned; when addr hits the descriptor table it additionally requires `kind&PARAM` and a matching type. **No min/max range check, no clamp, no scale/offset back-calculation.** If any item in the batch fails the mechanical check the whole batch is rejected; after the check passes, the whole batch is written on the same tick at the ISR safe point.

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

`DAQ_CTRL` request 20 octets. It selects and configures the Scope entry: `STREAM` is the continuous block flow; `CAPTURE_ARMED` is the device-side trigger-freeze window. The two use the same binding and block format, but to the host they are two entries.

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
```

CPU2 writes cfg and publishes `cfg_seq`, waits for CPU1's `cfg_ack_seq/cfg_result` then returns ACK: OK=config applied; BAD_PARAM=field illegal or record_points exceeds the ring capacity under the current binding; BAD_STATE=the current state doesn't accept this config. The mode can also be re-checked via STATUS's `scope_mode`.

`DAQ_BIND` request (2 + 8k octets) — **select channels at runtime, no reflash**:

```
0  1  n_ch          1..16
1  1  reserved      set 0
2  …  k × channel binding (8 octets, field-mirroring v2k_scope_ch_bind_t):
      0:4 addr | 4:2 type | 6:2 reserved
```

Semantics: addr comes from the descriptor table (platform quantities + build-time-baked user variables, §2); samples are losslessly direct-copied at **native width** (I16/U16→2 octets, I32/U32/F32→4 octets, bit pattern verbatim, firmware zero-conversion zero-quantization — accuracy first). Physical-quantity conversion (e.g. ADC count→ampere) is pure host-side display metadata, not on the wire, not in the firmware. **Bindable only when scope mode==OFF.** CPU2 writes the bind region and publishes `bind_seq`, then briefly waits (≤1 ms) for CPU1's `bind_ack_seq/bind_result`, putting the final result into the ACK: OK / BAD_STATE (not OFF, DAQ_CTRL(OFF) first) / BAD_PARAM (n_ch or type illegal), data=bind_seq; a timeout returns INTERNAL (CPU1 ISR not running).

### 4.6 BLOCK_REQ / BLOCK_DATA (0x21 / 0xA1)

Request (2 octets): `{0:1 max_blocks(1..2), 1:1 reserved}`
Response (12 + Σblock octets):

```
0  1  count        0..max_blocks (0 = no data currently, legal)
1  1  mode         current V2K_SCOPE_*
2  2  reserved
4  2  overrun_cnt  producer-side cumulative dropped blocks (host reports "capture-side overload" by this)
6  2  remain_hint  blocks still takeable in the ring (host tunes the poll cadence; freeze-drain progress)
8  4  trigger_tick the ISR tick of the trigger hit when CAPTURE_FROZEN; set 0 in other modes
12 …  count × block
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

Bandwidth reference (ISR period 20–100 kHz TBD): 20kHz×8ch×f32 = 640 KB/s; 100kHz×8ch×f32 = 3.2 MB/s — both within EtherCAT practical throughput, the physical-layer conclusion unchanged. The EtherCAT-tier N is fixed in Phase 6 by the single-frame process-data ceiling (~1486 octets) (f32 8ch: on the order of N=20×2 blocks or N=40×1 block).

### 4.7 CMD (0x30)

Request (8 octets): `{0:2 cmd_code(V2K_CMD_*), 2:2 arg0, 4:4 arg1}`
CPU2 checks the mailbox is free (`cmd_seq == ack_seq`) → writes the command mailbox, returns ACK(OK, data=cmd_seq); mailbox busy → ACK(BUSY), host retries later. The execution result is confirmed via STATUS's `cmd_ack_seq/cmd_result/sys_state`.

## 5. Service semantics

### 5.1 Session establishment and forced re-enumeration

```
host                                                     firmware(CPU2)
 │ ─────────────────────── HELLO_REQ ──────────────────────→ │
 │ ←────────────────────── HELLO_RESP ────────────────────── │  proto_ver mismatch → host aborts and reports an error
 │ ─────────────────────── ENUM_REQ(0,8) ──────────────────→ │
 │ ←────────────────────── ENUM_RESP ─────────────────────── │  … page until count < max or start≥total
 │ (then periodic STATUS polling, suggested 2–10 Hz)         │
```

The host caches the descriptor table, keyed by `build_hash`. The Phase 4.5 baker computes this value from the final ELF with the patch section normalized, plus the generated descriptor records. It therefore changes when linked code, addresses, or the baked variable set changes, including dirty-tree builds. At any time (in HELLO or STATUS), detecting a `build_hash` change → **invalidate the entire cache and re-enumerate**. Prevents reading new firmware with an old table.

User application-variable discovery (build-time baking, Phase 4.5): a build tool reads the firmware `.out` DWARF and bakes each user variable's `name→addr→type` into the descriptor table (struct members / array elements expanded into named scalar entries). The host enumerates them over ENUM like any platform quantity — no `.out` on the host, no stale-ELF risk (the addresses come from the same build that is flashed; build_hash still guards the host cache). The student writes plain C.

### 5.2 Parameter transaction (two-stage + async reconcile)

```
host                         CPU2                       CPU1 ISR safe point
 │ ─────── CAL_WRITE ×m ─────→ │ stage shadow                    │
 │ ←────── ACK(OK) ×m ──────── │                                 │
 │ ─────── CAL_COMMIT ───────→ │ publish commit_seq=s ─────────→ │ sees s≠applied_seq:
 │ ←────── ACK(OK,data=s) ──── │                                 │ mechanical check→apply whole set→
 │ ─────── STATUS poll ──────→ │ read the parameter-status block │ write applied_seq=s
 │ ←────── applied_seq==s? ─── │  ←────────────────────────────  │
```

Key points: within a batch **all valid or all rejected** (takes effect on the same tick, cal_result/cal_fail_idx report the cause; all successful writes are native-bit-pattern writes, with no range or unit conversion); the host must not issue the next COMMIT batch before applied_seq catches up to commit_seq.

`CAL_READ` is an on-demand single read, not the periodic mirror: the host sends an `(addr,type)` list, CPU2 publishes the read request, CPU1's background reads and acks. It serves the Variable Map / on-demand watch; high-speed real-time waveforms still use the DAQ_BIND + BLOCK_REQ scope ring.

### 5.3 Scope flow

Channel selection (before any mode starts): `DAQ_CTRL(OFF)` → `DAQ_BIND(channel list)` → only after ACK(OK) may you start. (Phase 3 wrote a boot default binding of the first 8 platform observables; Phase 4 removes it — binding is on-demand.)

**Stream entry**: `DAQ_CTRL(mode=STREAM, prescaler, record_points=0)` → the host continuously `BLOCK_REQ` polls (a "soft PDO" in the SCI phase; frequency adapts by `remain_hint`). When the ring fills the producer drops new blocks + overrun_cnt++, the flow doesn't stop, and the host draws a gap by block_seq.

**Capture entry**: under the same channel binding, send `DAQ_CTRL(mode=CAPTURE_ARMED, trig…, pre_trig_pct, prescaler, record_points)` → CPU1 overwrites the ring in the same block format and evaluates the trigger each tick → after a hit it enters CAPTURE_POST to capture the post segment (depth = record_points×(100-pre_trig_pct)%) → CAPTURE_FROZEN (visible in STATUS.scope_mode) → the host `BLOCK_REQ` drains slowly (remain_hint decrements to 0) → the host re-ARMs or switches back to STREAM. The pre-trigger history is naturally preserved by the ring structure; the host reconstructs block order by `start_tick`.

The "watch window" of an application variable = after selecting variables, run STREAM with a larger `prescaler` — it carries tick timestamps and is lossless native bit pattern, replacing independent poll-style monitoring. When you need a per-tick waveform set prescaler to 1; when you need to reduce link pressure increase prescaler, reduce channels, or lower the block frequency.

### 5.4 Error handling and resync

- Corrupt frames silently dropped (§3.1). The host sets a timeout per request (suggested 100 ms) + resend.
- CPU2 caches the last encoded response; when the host resends the same `(msg_type, seq)` on timeout, it replays directly, without re-executing CAL_COMMIT, CMD, DAQ_BIND, or consuming a BLOCK. The cache reuses only the current TX buffer, adding no second 1 KB-class buffer.
- The service itself stays safely retryable: a repeated CAL_WRITE overwrites the same-addr staged entry; DAQ_BIND overwrites the whole region. Only after the host abandons the old request and uses a new seq is it treated as a new operation.
- The host discards late/mismatched responses by the frame seq echo.
- Sync recovery: the host sends any request, and the firmware decoder auto-resyncs at the 0x00 boundary.

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
| MAVLink | the parameter microservice is a flat float table, can't hold runtime-enum metadata like type/kind/prescaler → the descriptor table still needs custom messages; payload ≤255 octets → an 800B block needs 4-piece reassembly; the official C generated code = packed struct + memcpy, on C28x that means rewriting the generator backend. All that's kept is the heartbeat and the XML format. |
| nanopb / protobuf | the official explicitly doesn't support CHAR_BIT≠8 platforms (self-maintained private fork); varint makes golden vectors not human-checkable; .proto can't express the shared-RAM layout, so the shared struct still needs hand-written alignment — the second-data-model cost is paid anyway. |
| CBOR (control plane) | there's the Zephyr MCUmgr precedent, field-evolution friendly; but the firmware needs ~400 lines of self-written 16-bit-char-safe codec subset (building a more general wheel), one protocol with two encodings, and the vectors need pinned deterministic encoding. The evolution need is already covered cheaply by "field append + length discrimination + re-enumeration mechanism". |

**Industry comparison**: the framing layer (COBS/CRC) is all standard; fixed-length little-endian layout is the mainstream school in this field (XCP/MAVLink payload/ST MCP); runtime enumeration follows the ODrive(Fibre)/Klipper(data dictionary) open-source precedents; the RCP sub-industry (dSPACE/Imperix etc.) all use custom private protocols anyway — this scheme = that industry's convention + an anti-rot process (the spec as the single source of truth + golden-vector dual-end conformance + version fields), aimed precisely at the lesson of "magic-offset archaeology" in existing private protocols.

**The biggest risk of the custom route and the countermeasure**: the risk isn't being unable to write it, but rot ten years later. The countermeasure is written into the process: this document's change flow (top of the doc), the vectors-are-authoritative rule, message-catalog additions not modifying the old, and the three-layer version fields (wire/contract/build_hash) each owning a segment.

### ADR-2: variable-discovery architecture (finalized 2026-06-11)

**Decision (revised 2026-06-19, see [Phase 4.5](../docs/phase4.5-symbol-baking.md))**: the descriptor table carries platform quantities (registered by L0/L1) **plus user application variables baked in at build time from the firmware DWARF** — so the names travel with the device and the host needs no `.out`. Discovery is over ENUM; scoping via DAQ_BIND, parameter writing via CAL_WRITE, both keyed by (addr,type).

**Rejected forms**: ① L2-component init self-registration (`pi_init(&pi, "vel")`) and ② user-side stringified registration macros — both force a second name string or a mandated declaration style; the C symbol is the only acceptable name source. ③ **Host-side runtime `.out`/DWARF parsing** (the original 2026-06-11 plan) — rejected 2026-06-19 because it ties Scope2000 to the project directory and risks a stale/wrong ELF writing to the wrong address. The C symbol is still the only name source, but it is harvested **at build time** and baked into the device.

**Cost and countermeasure**: the Phase 4.5 build tool runs TI `ofd2000 --xml --dwarf` on the firmware `.out` and bakes a compact `name→addr→type` table into the reserved image blob; the `.out`-to-device pairing is automatic (same build) and build_hash guards the host cache. The protocol carries no `min/max/scale/offset`: the value itself must already be the real quantity to display, log, and write back. In exchange: zero registration code and zero naming burden for students, any supported struct member / array element observable, fully runtime channel selection (no reflash), and **names that travel with the device — no `.out` on the host**.

## Appendix B: Scope2000 `DataSource` boundary (Phase 3.5)

Scope2000's native implementation is `V2kSource`, internally split strictly into three layers: service semantics, message codec, and byte-stream transport. The Phase 3.5 transport = SCI; when adding the EtherCAT transport in Phase 6, the GUI data model and service commands don't change.

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
