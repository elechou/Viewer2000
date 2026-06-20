#!/usr/bin/env python3
"""gen_vectors.py — Viewer2000 线上协议 golden test vectors 生成器

本脚本是 docs/wire-spec.md 的可执行伪码：COBS / CRC-32C / 帧构造 /
全部 v6 消息的参考实现。生成的 contracts/vectors/*.txt 是协议的
规范字节样本——固件 C 序列化器与上位机 Rust 解析器各自的 conformance
test 必须对同一组样本通过。三方不一致时以 vectors 为准（见 spec 文首）。

用法:
    python gen_vectors.py            # （再）生成 contracts/vectors/
    python gen_vectors.py --check    # 校验现存样本与本脚本逐字节一致
"""

import struct
import sys
from pathlib import Path

VECTORS_DIR = Path(__file__).resolve().parent.parent / "contracts" / "vectors"

# ---------------------------------------------------------------------------
# CRC-32C (Castagnoli)：反射式，poly=0x82F63B78, init=0xFFFFFFFF, xorout=0xFFFFFFFF
# 即 iSCSI/RFC3720 参数集；与 SSE4.2 crc32 指令、C2000Ware VCRC
# CRC_run32BitPoly2Reflected 一致。
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


# 公认校验值（Check value, "123456789" → 0xE3069283）
assert crc32c(b"123456789") == 0xE3069283, "CRC-32C 实现错误"

# ---------------------------------------------------------------------------
# COBS (Consistent Overhead Byte Stuffing)
# 编码后数据不含 0x00；线上形态 = cobs_encode(frame) + b"\x00"
# ---------------------------------------------------------------------------
def cobs_encode(data: bytes) -> bytes:
    out = bytearray()
    idx = 0
    while True:
        # 找下一个 0x00（或数据尾），段长上限 254
        end = min(idx + 254, len(data))
        zero = data.find(0, idx, end)
        seg_end = zero if zero != -1 else end
        out.append(seg_end - idx + 1)           # code = 段长 + 1
        out += data[idx:seg_end]
        if zero != -1:
            idx = zero + 1                      # 跳过这个 0x00
            if idx == len(data):                # 数据以 0x00 结尾 → 末尾空段
                out.append(0x01)
                break
        else:
            if end == len(data):
                break
            idx = end                           # 254 满段，无 0 被消耗
    return bytes(out)


def cobs_decode(data: bytes) -> bytes:
    out = bytearray()
    idx = 0
    while idx < len(data):
        code = data[idx]
        assert code != 0, "COBS 编码区内不得出现 0x00"
        out += data[idx + 1 : idx + code]
        idx += code
        if code < 0xFF and idx < len(data):
            out.append(0)
    return bytes(out)


# 自检：经典样例 + 含零块往返
assert cobs_encode(b"\x00") == b"\x01\x01"
assert cobs_encode(b"\x11\x22\x00\x33") == b"\x03\x11\x22\x02\x33"
for _case in (b"", b"\x00\x00", b"\x01" * 300, bytes(range(256))):
    assert cobs_decode(cobs_encode(_case)) == _case

# ---------------------------------------------------------------------------
# 帧构造（wire-spec §3.1）
# ---------------------------------------------------------------------------
VER_MAGIC = 0x56


def raw_frame(msg_type: int, seq: int, payload: bytes) -> bytes:
    hdr = struct.pack("<BBBHH", VER_MAGIC, msg_type, 0x00, seq, len(payload))
    body = hdr + payload
    return body + struct.pack("<I", crc32c(body))


def wire_frame(raw: bytes) -> bytes:
    return cobs_encode(raw) + b"\x00"


# ---------------------------------------------------------------------------
# 消息 payload 构造（wire-spec §4；字段顺序与偏移以 spec 为准）
# ---------------------------------------------------------------------------
def desc_entry(name, type_, kind, addr, presc, reserved=0):
    return struct.pack("<16sHHIHH", name.encode("ascii"),
                       type_, kind, addr, presc, reserved)


# V2K_TYPE_* → (struct 格式码, 样本 octet 宽度)；样本按原生宽度无损直拷
_TYPE_FMT = {0: ("h", 2), 1: ("H", 2), 2: ("i", 4), 3: ("I", 4), 4: ("f", 4)}


def block(start_tick, block_seq, flags, bind_seq, ch_types, samples_2d):
    """block = 16 octet 头 + 样本区（tick-major，每 tick 内按绑定顺序原生宽度排列）"""
    n_ticks, n_ch = len(samples_2d), len(ch_types)
    stride = sum(_TYPE_FMT[t][1] for t in ch_types)
    hdr = struct.pack("<IHHHHHH", start_tick, block_seq, flags,
                      n_ticks, n_ch, bind_seq, stride)
    body = b"".join(struct.pack("<" + _TYPE_FMT[t][0], v)
                    for row in samples_2d for t, v in zip(ch_types, row))
    return hdr + body


BUILD_HASH = 0x08CCB6EB  # 样本固定值（取自本 repo initial commit 短哈希）
BUILD_TIME_UTC = 1_781_913_600  # 2026-06-20 00:00:00 UTC, fixed for deterministic vectors

# ---------------------------------------------------------------------------
# vector 用例表
# 每项: (文件名, 描述, raw 帧)。负例（CRC 损坏）单独处理。
# ---------------------------------------------------------------------------
def build_cases():
    cases = []

    def add(name, desc, msg_type, seq, payload):
        cases.append((name, desc, raw_frame(msg_type, seq, payload)))

    # ---- 4.1 HELLO ----
    add("hello_req", "HELLO_REQ：空 payload", 0x01, 0x0001, b"")
    add("hello_resp", "HELLO_RESP：版本、build_hash、固件名、tick_hz、能力位、项目名与构建时间",
        0x81, 0x0001,
        struct.pack("<HHIHH16sII32sI", 6, 12, BUILD_HASH, 10, 0,
                    b"viewer2000", 20000, 0x7F,
                    b"phase4-demo", BUILD_TIME_UTC))

    # ---- 4.2 STATUS ----
    add("status_req", "STATUS_REQ：空 payload（兼任链路心跳）", 0x02, 0x0002, b"")
    add("status_resp",
        "STATUS_RESP：RUNNING 态，Scope mode=STREAM",
        0x82, 0x0002,
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
                    3))           # prof_seq end

    # ---- 4.3 ENUM ----
    add("enum_req", "ENUM_REQ：从 0 开始要 8 条", 0x03, 0x0003,
        struct.pack("<HBB", 0, 8, 0))
    add("enum_resp_2entries",
        "ENUM_RESP：总数 10，本页 2 条（用户 vel_kp 参数/示波 + 系统 iq_meas 示波）",
        0x83, 0x0003,
        struct.pack("<HHBB", 10, 0, 2, 0)
        + desc_entry("vel_kp", 4, 0x0007, 0x0000A012, 1, 0)  # F32, USER|PARAM|SCOPE
        + desc_entry("iq_meas", 0, 0x0002, 0x0000A044, 1))  # I16, system SCOPE
    add("enum_resp_empty",
        "ENUM_RESP 边界：start_idx 越过总数 → count=0（合法的读完信号）",
        0x83, 0x0004, struct.pack("<HHBB", 10, 10, 0, 0))

    # ---- 4.4 CAL ----
    add("cal_write",
        "CAL_WRITE：2 条暂存，按 (addr,type) 寻址"
        "（0xA012=F32 写 3.5；0xA044=I16 写 -7 符号扩展）",
        0x10, 0x0005,
        struct.pack("<BB", 2, 0)
        + struct.pack("<IIHH", 0x0000A012,
                      struct.unpack("<I", struct.pack("<f", 3.5))[0], 4, 0)
        + struct.pack("<IIHH", 0x0000A044, 0xFFFFFFF9, 0, 0))
    add("cal_commit", "CAL_COMMIT：空 payload", 0x11, 0x0006, b"")
    add("ack_cal_commit", "ACK(CAL_COMMIT)：OK，data=commit_seq=8",
        0x91, 0x0006, struct.pack("<BBHI", 0, 0x11, 0, 8))
    add("cal_read", "CAL_READ：按 (addr,type) 读 3 条", 0x12, 0x0007,
        struct.pack("<BB", 3, 0)
        + struct.pack("<IHH", 0x0000A012, 4, 0)
        + struct.pack("<IHH", 0x0000A044, 1, 0)
        + struct.pack("<IHH", 0x0000A046, 0, 0))
    add("cal_read_resp", "CAL_READ_RESP：read_seq=42 的 3 个 value_bits",
        0x92, 0x0007,
        struct.pack("<IBBH", 42, 3, 0, 0)
        + struct.pack("<3I",
                      struct.unpack("<I", struct.pack("<f", 3.5))[0],
                      0x00000064, 0xFFFFFFF9))

    # ---- 4.5 DAQ_CTRL / DAQ_BIND ----
    add("daq_ctrl_capture",
        "DAQ_CTRL：Capture 入口进入 CAPTURE_ARMED，触发源=通道槽位 1 上升沿过 2.5，"
        "hysteresis=0.05，pre-trigger 30%，prescaler=1，record=1000 pts",
        0x20, 0x0008,
        struct.pack("<HHffHHHH", 2, 1, 2.5, 0.05, 0, 30, 1, 1000))
    add("daq_ctrl_stream",
        "DAQ_CTRL：Stream 入口连续流，trigger 与 record_points 字段被忽略，prescaler=1",
        0x20, 0x000D,
        struct.pack("<HHffHHHH", 1, 0, 0.0, 0.0, 0, 0, 1, 0))
    add("daq_bind_2ch",
        "DAQ_BIND：绑定 2 通道（0xA044=I16 原生 2 octets；"
        "0xC120=F32 原生 4 octets 无损——地址来自枚举后的描述符表）",
        0x22, 0x000C,
        struct.pack("<BB", 2, 0)
        + struct.pack("<IHH", 0x0000A044, 0, 0)    # I16 源
        + struct.pack("<IHH", 0x0000C120, 4, 0))   # F32 source from ENUM
    add("ack_daq_bind", "ACK(DAQ_BIND)：OK，data=bind_seq=3",
        0xA2, 0x000C, struct.pack("<BBHI", 0, 0x22, 0, 3))
    add("ack_daq_bind_badstate",
        "ACK(DAQ_BIND) 负例语义：scope 非 OFF → BAD_STATE（须先 DAQ_CTRL(OFF)）",
        0xA2, 0x000D, struct.pack("<BBHI", 3, 0x22, 0, 4))

    # ---- 4.6 BLOCK ----
    add("block_req", "BLOCK_REQ：最多取 2 块", 0x21, 0x0009,
        struct.pack("<BB", 2, 0))
    add("block_data_1blk",
        "BLOCK_DATA：1 块（N=4×M=2 全 I16，stride=4，样本含 0 与负值"
        "——COBS 含零路径覆盖）",
        0xA1, 0x0009,
        struct.pack("<BBBBHHI", 1, 1, 0, 0, 0, 5, 0)
        # count1, STREAM, overrun0, remain5, trigger_tick0
        + block(1000, 17, 0, 3, (0, 0),
                [[0, 100], [-100, 0], [200, -200], [0, 300]]))
    add("block_data_mixed",
        "BLOCK_DATA：1 块混合宽度（N=2，通道=[F32, I16]，stride=6——"
        "钉死原生宽度交错布局：每 tick 内 F32 4 octets 后接 I16 2 octets）",
        0xA1, 0x000E,
        struct.pack("<BBBBHHI", 1, 1, 0, 0, 0, 0, 0)
        # count1, STREAM, trigger_tick0
        + block(2000, 5, 0, 2, (4, 0),
                [[1.5, -7], [-0.25, 32767]]))
    add("block_data_capture_frozen",
        "BLOCK_DATA：Capture Frozen 前缀带 trigger_tick，block 仍为原生布局",
        0xA1, 0x000F,
        struct.pack("<BBBBHHI", 1, 4, 0, 0, 0, 0, 1234)
        + block(1200, 9, 0, 2, (4,),
                [[-0.5], [0.0], [0.5]]))
    add("block_data_empty",
        "BLOCK_DATA 边界：count=0（环空，合法响应）",
        0xA1, 0x000A,
        struct.pack("<BBBBHHI", 0, 1, 0, 0, 0, 0, 0))

    # ---- 4.7 CMD ----
    add("cmd_app_start", "CMD：APP_START", 0x30, 0x000B,
        struct.pack("<HHI", 1, 0, 0))
    add("ack_cmd_busy", "ACK(CMD)：mailbox 忙 → BUSY", 0xB0, 0x000B,
        struct.pack("<BBHI", 2, 0x30, 0, 0))

    return cases


# ---------------------------------------------------------------------------
# 文件渲染
# ---------------------------------------------------------------------------
def hexstr(b: bytes) -> str:
    return b.hex()


def render(name: str, desc: str, raw: bytes, corrupt_note: str = "") -> str:
    wire = wire_frame(raw)
    lines = [
        f"# vector: {name}",
        f"# {desc}",
        "# raw  = COBS 编码前帧 (ver|type|flags|seq|len|payload|crc32c, 全 LE)",
        "# wire = COBS(raw) + 0x00 定界符（SCI 链路实际字节）",
    ]
    if corrupt_note:
        lines.append(f"# {corrupt_note}")
    lines += [f"raw: {hexstr(raw)}", f"wire: {hexstr(wire)}", ""]
    return "\n".join(lines)


def generate() -> dict:
    files = {}
    for name, desc, raw in build_cases():
        # 自检：每个用例 wire→raw 往返 + CRC 复核
        assert cobs_decode(wire_frame(raw)[:-1]) == raw
        body, crc = raw[:-4], struct.unpack("<I", raw[-4:])[0]
        assert crc32c(body) == crc
        files[f"{name}.txt"] = render(name, desc, raw)

    # 负例：CRC 末 octet 翻转（解码器必须静默丢弃，spec §3.1）
    good = raw_frame(0x02, 0x00FF, b"")
    bad = good[:-1] + bytes([good[-1] ^ 0xFF])
    files["neg_bad_crc.txt"] = render(
        "neg_bad_crc",
        "负例：STATUS_REQ 帧 CRC 末 octet 翻转——解码器必须静默丢弃，不回 NAK",
        bad, corrupt_note="本帧 CRC 故意损坏，wire 行仍按损坏后内容编码")
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
    print(f"已生成 {len(files)} 个 vectors → {VECTORS_DIR}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
