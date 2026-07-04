#!/usr/bin/env python3
"""Generate Viewer2000 golden test vectors for the wire protocol.

This script is executable pseudocode for docs/wire-spec.md: COBS, CRC-32C,
frame construction, and every v10 message reference sample. The generated
contracts/vectors/*.txt files are normative byte samples for firmware C
serializer tests and host Rust parser conformance tests. When implementations
disagree, the vectors are authoritative and the spec must be corrected.

Usage:
    python gen_vectors.py            # regenerate contracts/vectors/
    python gen_vectors.py --check    # verify existing vectors match this script
"""

import struct
import sys
from pathlib import Path

VECTORS_DIR = Path(__file__).resolve().parent.parent / "contracts" / "vectors"

# ---------------------------------------------------------------------------
# CRC-32C (Castagnoli): reflected form, poly=0x82F63B78, init=0xFFFFFFFF,
# xorout=0xFFFFFFFF. This is the iSCSI/RFC3720 parameter set and matches
# SSE4.2 crc32 instructions and C2000Ware VCRC CRC_run32BitPoly2Reflected.
# ---------------------------------------------------------------------------
_CRC32C_TABLE = []
for _i in range(256):
    _c = _i
    for _ in range(8):
        _c = (_c >> 1) ^ 0x82F63B78 if (_c & 1) else (_c >> 1)
    _CRC32C_TABLE.append(_c)


def crc32c(data: bytes) -> int:
    c = 0xFFFFFFFF
    for b in data:
        c = _CRC32C_TABLE[(c ^ b) & 0xFF] ^ (c >> 8)
    return c ^ 0xFFFFFFFF


# Standard check value: "123456789" -> 0xE3069283.
assert crc32c(b"123456789") == 0xE3069283, "CRC-32C implementation error"

# ---------------------------------------------------------------------------
# COBS (Consistent Overhead Byte Stuffing).
# Encoded data contains no 0x00; wire form is cobs_encode(frame) + b"\x00".
# ---------------------------------------------------------------------------
def cobs_encode(data: bytes) -> bytes:
    out = bytearray()
    idx = 0
    while True:
        # Find the next 0x00 or end of data; each segment is capped at 254.
        end = min(idx + 254, len(data))
        zero = data.find(0, idx, end)
        seg_end = zero if zero != -1 else end
        out.append(seg_end - idx + 1)           # code = segment length + 1
        out += data[idx:seg_end]
        if zero != -1:
            idx = zero + 1                      # skip this 0x00
            if idx == len(data):                # data ended with 0x00
                out.append(0x01)
                break
        else:
            if end == len(data):
                break
            idx = end                           # full 254-octet segment consumed no zero
    return bytes(out)


def cobs_decode(data: bytes) -> bytes:
    out = bytearray()
    idx = 0
    while idx < len(data):
        code = data[idx]
        assert code != 0, "0x00 is forbidden inside a COBS-encoded region"
        out += data[idx + 1 : idx + code]
        idx += code
        if code < 0xFF and idx < len(data):
            out.append(0)
    return bytes(out)


# Self-check: classic examples plus zero-containing round trips.
assert cobs_encode(b"\x00") == b"\x01\x01"
assert cobs_encode(b"\x11\x22\x00\x33") == b"\x03\x11\x22\x02\x33"
for _case in (b"", b"\x00\x00", b"\x01" * 300, bytes(range(256))):
    assert cobs_decode(cobs_encode(_case)) == _case

# ---------------------------------------------------------------------------
# Frame construction, docs/wire-spec.md section 3.1.
# ---------------------------------------------------------------------------
VER_MAGIC = 0x5A


def raw_frame(msg_type: int, seq: int, payload: bytes) -> bytes:
    hdr = struct.pack("<BBBHH", VER_MAGIC, msg_type, 0x00, seq, len(payload))
    body = hdr + payload
    return body + struct.pack("<I", crc32c(body))


def wire_frame(raw: bytes) -> bytes:
    return cobs_encode(raw) + b"\x00"


# ---------------------------------------------------------------------------
# Message payload construction, docs/wire-spec.md section 4.
# Field order and offsets follow the spec.
# ---------------------------------------------------------------------------
def enum_entry(name, type_, kind, addr, presc):
    encoded = name.encode("ascii")
    assert 0 < len(encoded) <= 128
    return struct.pack("<IHHHBB", addr, type_, kind, presc, len(encoded), 0) + encoded


# V2K_TYPE_* -> (struct format code, native sample width in octets).
# Samples are copied losslessly at their native width.
_TYPE_FMT = {0: ("h", 2), 1: ("H", 2), 2: ("i", 4), 3: ("I", 4), 4: ("f", 4)}


def block(start_tick, block_seq, flags, bind_seq, ch_types, samples_2d):
    """Return a block: 16-octet header + tick-major native-width sample area."""
    n_ticks, n_ch = len(samples_2d), len(ch_types)
    stride = sum(_TYPE_FMT[t][1] for t in ch_types)
    hdr = struct.pack("<IHHHHHH", start_tick, block_seq, flags,
                      n_ticks, n_ch, bind_seq, stride)
    body = b"".join(struct.pack("<" + _TYPE_FMT[t][0], v)
                    for row in samples_2d for t, v in zip(ch_types, row))
    return hdr + body


BUILD_HASH = 0x08CCB6EB  # Fixed sample value from the repository baseline hash.
BUILD_TIME_UTC = 1_781_913_600  # 2026-06-20 00:00:00 UTC, fixed for deterministic vectors.

# ---------------------------------------------------------------------------
# Vector case table. Each item is (file name, description, raw frame).
# Negative cases such as corrupted CRC are handled separately.
# ---------------------------------------------------------------------------
def build_cases():
    cases = []

    def add(name, desc, msg_type, seq, payload):
        cases.append((name, desc, raw_frame(msg_type, seq, payload)))

    # ---- 4.1 HELLO ----
    add("hello_req", "HELLO_REQ: empty payload", 0x01, 0x0001, b"")
    add("hello_resp", "HELLO_RESP: versions, build_hash, firmware name, tick_hz, capabilities, project metadata, MCU model, and Scope resources",
        0x81, 0x0001,
        struct.pack("<HHIHH16sII32sIHHHHI", 10, 16, BUILD_HASH, 10, 0,
                    b"viewer2000", 20000, 0x7F,
                    b"phase4-demo", BUILD_TIME_UTC,
                    1, 16, 10, 0, 0x7000))

    # ---- 4.2 STATUS ----
    status_payload = (
        struct.pack("<HHHIIIIHHI4BIHHIIIIIIHIIII",
                    2,            # sys_state = RUNNING
                    0,            # fault_code
                    0,            # status_flags
                    123456789,    # tick
                    4567,         # cpu1_heartbeat
                    4566,         # cpu2_heartbeat
                    7,            # applied_seq
                    0,            # cal_result = OK
                    0,            # cal_fail_idx
                    BUILD_HASH,
                    1, 0, 0, 0,   # scope mode/flags/reserved
                    9,            # cmd_ack_seq
                    0,            # cmd_result = OK
                    0,            # reserved
                    3,            # prof_seq begin
                    10000,        # cycle_budget
                    4200,         # load_avg
                    7300,         # load_peak
                    1600,         # ctrl_at_peak = user control() body
                    900,          # scope_at_peak
                    40,           # lat_at_peak
                    123456700,    # peak_tick
                    0,            # budget violations
                    0,            # ISR overflows
                    3)            # prof_seq end
        + struct.pack("<HHIHH",
                      22,          # scope_state_seq
                      4,           # scope_frozen_count
                      1234,        # scope_trigger_tick
                      3,           # scope_bind_seq
                      0))          # reserved
    add("status_req", "STATUS_REQ: empty payload, optional explicit status read", 0x02, 0x0002, b"")
    add("status_resp",
        "STATUS_RESP: RUNNING state, Scope mode STREAM",
        0x82, 0x0002, status_payload)
    add("status_push",
        "STATUS_PUSH: firmware-initiated 4 Hz status frame with STATUS_RESP payload layout",
        0x41, 0x0000, status_payload)

    # ---- 4.3 ENUM ----
    add("enum_req", "ENUM_REQ: request 8 entries starting at index 0", 0x03, 0x0003,
        struct.pack("<HBB", 0, 8, 0))
    add("enum_resp_2entries",
        "ENUM_RESP: total 10, page contains long user PI state and system iq_meas scope",
        0x83, 0x0003,
        struct.pack("<HHBB", 10, 0, 2, 0)
        + enum_entry("controller.current_loop.pi_q.integrator_state", 4, 0x0007, 0x0000A012, 1)
        + enum_entry("iq_meas", 0, 0x0002, 0x0000A044, 1))
    add("enum_resp_empty",
        "ENUM_RESP boundary: start_idx past total gives count=0, a legal done signal",
        0x83, 0x0004, struct.pack("<HHBB", 10, 10, 0, 0))

    # ---- 4.4 CAL ----
    add("cal_write",
        "CAL_WRITE: stage 2 entries addressed by (addr,type): 0xA012 F32 writes 3.5; 0xA044 I16 writes -7 sign-extended",
        0x10, 0x0005,
        struct.pack("<BB", 2, 0)
        + struct.pack("<IIHH", 0x0000A012,
                      struct.unpack("<I", struct.pack("<f", 3.5))[0], 4, 0)
        + struct.pack("<IIHH", 0x0000A044, 0xFFFFFFF9, 0, 0))
    add("cal_commit", "CAL_COMMIT: empty payload", 0x11, 0x0006, b"")
    add("ack_cal_commit", "ACK(CAL_COMMIT): OK, data=commit_seq=8",
        0x91, 0x0006, struct.pack("<BBHI", 0, 0x11, 0, 8))
    add("cal_read", "CAL_READ: read 3 entries by (addr,type)", 0x12, 0x0007,
        struct.pack("<BB", 3, 0)
        + struct.pack("<IHH", 0x0000A012, 4, 0)
        + struct.pack("<IHH", 0x0000A044, 1, 0)
        + struct.pack("<IHH", 0x0000A046, 0, 0))
    add("cal_read_resp", "CAL_READ_RESP: 3 value_bits for read_seq=42",
        0x92, 0x0007,
        struct.pack("<IBBH", 42, 3, 0, 0)
        + struct.pack("<3I",
                      struct.unpack("<I", struct.pack("<f", 3.5))[0],
                      0x00000064, 0xFFFFFFF9))

    # ---- 4.5 DAQ_CTRL / DAQ_BIND ----
    add("daq_ctrl_capture",
        "DAQ_CTRL: enter CAPTURE_ARMED, trigger source slot 1 rising across 2.5, hysteresis 0.05, pre-trigger 30%, prescaler 1, record 1000 pts",
        0x20, 0x0008,
        struct.pack("<HHffHHHHHH", 2, 1, 2.5, 0.05, 0, 30, 1, 1000,
                    0xFFFF, 0))
    add("daq_ctrl_stream",
        "DAQ_CTRL: enter STREAM, trigger and record_points ignored, prescaler 1",
        0x20, 0x000D,
        struct.pack("<HHffHHHHHH", 1, 0, 0.0, 0.0, 0, 0, 1, 0,
                    0xFFFF, 0))
    add("daq_bind_2ch",
        "DAQ_BIND: bind 2 channels: 0xA044 I16 native 2 octets, 0xC120 F32 native 4 octets; addresses came from ENUM",
        0x22, 0x000C,
        struct.pack("<BB", 2, 0)
        + struct.pack("<IHH", 0x0000A044, 0, 0)    # I16 source
        + struct.pack("<IHH", 0x0000C120, 4, 0))   # F32 source from ENUM
    add("ack_daq_bind", "ACK(DAQ_BIND): OK, data=bind_seq=3",
        0xA2, 0x000C, struct.pack("<BBHI", 0, 0x22, 0, 3))
    add("ack_daq_bind_badstate",
        "ACK(DAQ_BIND) negative semantics: scope not OFF gives BAD_STATE; send DAQ_CTRL(OFF) first",
        0xA2, 0x000D, struct.pack("<BBHI", 3, 0x22, 0, 4))

    # ---- 4.6 SCOPE stream push / capture batch push / capture replay ----
    add("capture_replay_req",
        "CAPTURE_REPLAY_REQ: request up to 4 frozen blocks from capture_id=22 starting at capture index 2",
        0x21, 0x0009,
        struct.pack("<HHBBBB", 22, 2, 4, 0, 0, 0))
    add("ack_capture_replay_req",
        "ACK(CAPTURE_REPLAY_REQ): OK, data=capture_id=22; replay CAPTURE_BATCH_PUSH frames follow asynchronously",
        0xA1, 0x0009, struct.pack("<BBHI", 0, 0x21, 0, 22))
    add("scope_block_push_1blk",
        "SCOPE_BLOCK_PUSH: 1 block, N=4, M=2, all I16, stride=4, samples include zero and negative values to cover COBS zero paths",
        0x42, 0x0001,
        struct.pack("<BBHHH", 1, 0, 0, 5, 0)
        # count1, reserved0, overrun0, remaining5, reserved0
        + block(1000, 17, 0, 3, (0, 0),
                [[0, 100], [-100, 0], [200, -200], [0, 300]]))
    add("scope_block_push_mixed",
        "SCOPE_BLOCK_PUSH: 1 mixed-width block, N=2, channels=[F32,I16], stride=6, tick-major native layout F32 then I16",
        0x42, 0x0002,
        struct.pack("<BBHHH", 1, 0, 0, 0, 0)
        # count1, reserved0, overrun0, remaining0, reserved0
        + block(2000, 5, 0, 2, (4, 0),
                [[1.5, -7], [-0.25, 32767]]))
    add("capture_batch_push",
        "CAPTURE_BATCH_PUSH: initial optimistic capture batch, capture_id=22, total=4, first_index=0, remaining=3",
        0x45, 0x0003,
        struct.pack("<HHHBBHHI", 22, 4, 0, 1, 0, 3, 0, 1234)
        + block(1200, 9, 0, 2, (4,),
                [[-0.5], [0.0], [0.5]]))
    add("capture_batch_replay",
        "CAPTURE_BATCH_PUSH: replay batch, capture_id=22, total=4, first_index=2, replay flag set",
        0x45, 0x0004,
        struct.pack("<HHHBBHHI", 22, 4, 2, 1, 1, 0, 0, 1234)
        + block(1230, 11, 0, 2, (4,),
                [[1.0], [1.5], [2.0]]))

    # ---- 4.7 CMD ----
    add("cmd_app_start", "CMD: APP_START", 0x30, 0x000B,
        struct.pack("<HHI", 1, 0, 0))
    add("ack_cmd_busy", "ACK(CMD): mailbox busy gives BUSY", 0xB0, 0x000B,
        struct.pack("<BBHI", 2, 0x30, 0, 0))

    return cases


# ---------------------------------------------------------------------------
# File rendering.
# ---------------------------------------------------------------------------
def hexstr(b: bytes) -> str:
    return b.hex()


def render(name: str, desc: str, raw: bytes, corrupt_note: str = "") -> str:
    wire = wire_frame(raw)
    lines = [
        f"# vector: {name}",
        f"# {desc}",
        "# raw  = frame before COBS (ver|type|flags|seq|len|payload|crc32c, all LE)",
        "# wire = COBS(raw) + 0x00 delimiter, the actual SCI-link octets",
    ]
    if corrupt_note:
        lines.append(f"# {corrupt_note}")
    lines += [f"raw: {hexstr(raw)}", f"wire: {hexstr(wire)}", ""]
    return "\n".join(lines)


def generate() -> dict:
    files = {}
    for name, desc, raw in build_cases():
        # Self-check: each case round-trips wire->raw and CRC verifies.
        assert cobs_decode(wire_frame(raw)[:-1]) == raw
        body, crc = raw[:-4], struct.unpack("<I", raw[-4:])[0]
        assert crc32c(body) == crc
        files[f"{name}.txt"] = render(name, desc, raw)

    # Negative case: flip the last CRC octet. Decoders must silently discard it.
    good = raw_frame(0x02, 0x00FF, b"")
    bad = good[:-1] + bytes([good[-1] ^ 0xFF])
    files["neg_bad_crc.txt"] = render(
        "neg_bad_crc",
        "Negative case: STATUS_REQ frame has its last CRC octet flipped; decoder must silently discard and send no NAK",
        bad, corrupt_note="CRC is intentionally corrupted; wire is encoded from the corrupted raw frame")
    return files


def main() -> int:
    files = generate()
    check = "--check" in sys.argv
    if check:
        ok = True
        for fname, content in files.items():
            p = VECTORS_DIR / fname
            if not p.exists():
                print(f"MISSING {fname}")
                ok = False
            elif p.read_text(encoding="utf-8").replace("\r\n", "\n") != content:
                print(f"MISMATCH {fname}")
                ok = False
        extra = {p.name for p in VECTORS_DIR.glob("*.txt")} - set(files)
        for e in sorted(extra):
            print(f"EXTRA {e}")
            ok = False
        print("CHECK OK" if ok else "CHECK FAILED")
        return 0 if ok else 1

    VECTORS_DIR.mkdir(parents=True, exist_ok=True)
    for fname, content in files.items():
        (VECTORS_DIR / fname).write_text(content, encoding="utf-8", newline="\n")
    print(f"Generated {len(files)} vectors -> {VECTORS_DIR}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
