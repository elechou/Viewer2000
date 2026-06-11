# Viewer2000 线上协议规范（wire spec）v1

> **文档地位**：本文档与 `contracts/` 头文件共同构成协议的唯一基准；
> `contracts/vectors/` 的 golden test vectors 是本文档的可执行形态。
> 固件 C 序列化器与上位机 Rust 解析器都必须通过同一组 vectors 的
> conformance test。三者不一致时以 **vectors 为准**，并立即修文档。
>
> **变更流程**：任何消息布局变更 → 改本文档 → 改 `tools/gen_vectors.py`
> 再生成 vectors → 两端 codec 跟进 → 不兼容变更必须 bump `V2K_WIRE_VER`。

---

## 1. 分层模型

```
┌──────────────────────────────────────────────────────┐
│ 服务语义层   会话/枚举/参数事务/DAQ 流/命令   （§5）     │  传输无关
├──────────────────────────────────────────────────────┤
│ 消息层      消息目录 v1，定长小端布局        （§4）     │  传输无关
├──────────────────────────────────────────────────────┤
│ 帧适配器    per-transport：                 （§3）     │  传输相关
│             SCI = COBS + 帧头 + CRC-32C               │
│             EtherCAT = mailbox/PDO 自带定界，帧层消失   │
├──────────────────────────────────────────────────────┤
│ 物理层      SCI(XDS110 VCP) → EtherCAT 100M           │
└──────────────────────────────────────────────────────┘
```

设计不变量：**消息层以上与传输无关**。换物理层只换帧适配器（Phase 3.5 → 6
的全部迁移量），消息与共享接口（`contracts/*.h`）一个 bit 不动。

服务语义对齐 XCP（ASAM MCD-1）概念词汇：参数事务 ≈ CAL，示波流 ≈ DAQ，
通道组 ≈ event channel，降采样比 ≈ prescaler。语义对齐保留"将来在 CPU2
加 XCP 门面对接 CANape 类工具"的退路；编码本身不是 XCP。

## 2. 通用约定

- **octet** = 线上 8-bit 字节；**word** = C28x 16-bit 单元。本文件一律 octet。
- 多 octet 整数一律**小端（LE）**；`f32` = IEEE-754 单精度的 LE 位模式。
- 字符串 = ASCII 定长，NUL 填充（不保证 NUL 结尾）。
- 偏移表中 `off:sz` 单位均为 octet。
- 主从模型：**host 是唯一发起方**（请求-响应），固件无自发帧。
  这与 EtherCAT 的 master 轮询模型同构——Phase 3.5 验证的就是 Phase 6 的语义。
- `value_bits`（参数值位模式）约定见 `v2k_common.h`：F32 原位模式，
  I16 符号扩展 / U16 零扩展到 32 bit。

## 3. 帧适配器

### 3.1 SCI 帧

```
编码前帧（"裸帧"，CRC 覆盖区 = offset 0 .. 7+n-1）:

off  sz  字段
0    1   ver_magic   = 0x51（高 nibble 0x5 固定魔数，低 nibble = V2K_WIRE_VER）
1    1   msg_type    （§4 目录；响应 = 请求 | 0x80）
2    1   flags       = 0x00，保留
3    2   seq         host 每请求 +1（回绕）；响应原样回显，host 据此配对
5    2   payload_len = n（octet 数，≤ V2K_MAX_PAYLOAD = 1024）
7    n   payload
7+n  4   crc32c      （LE）

线上形态: COBS(裸帧) + 0x00 定界符
```

- **CRC-32C**（Castagnoli，多项式 0x1EDC6F41 反射式 0x82F63B78，
  init=0xFFFFFFFF，xorout=0xFFFFFFFF，即 iSCSI/RFC 3720 参数集）。
  选型依据：帧长可达 ~1KB，CRC-32C 在此长度仍保有 HD≥4 且随机漏检 2⁻³²；
  PC 端 SSE4.2 硬件指令；C28x 端 VCRC 扩展指令集有现成 CRC-32C 例程
  （C2000Ware `CRC_run32BitPoly2Reflected`），软件查表实现兜底亦满足
  SCI 速率（CPU2 背景循环，非 ISR 路径）。
- **COBS**（Consistent Overhead Byte Stuffing）：编码后帧内无 0x00，
  0x00 仅作定界符。接收端失步后丢弃至下一个 0x00 即重同步（确定性）。
  选型依据（对比候选）：
  - SLIP/HDLC 转义式：最坏膨胀 2×，且本协议主载荷是 int16 采样流，
    **信号过零附近 0x00 octet 高发**，膨胀率随波形内容浮动——在带宽
    最紧的 SCI 链路上不可接受。COBS 恒定开销 ≤ ⌈len/254⌉+1 octet。
  - 纯 length-prefix + 魔数扫描：TX 零变换（C28x 上省一遍 octet 处理），
    但失步恢复是启发式的（魔数撞车需 CRC 排除）。COBS 的编码代价发生在
    CPU2 背景循环（非控制 ISR），换确定性重同步，值得。
  - 帧内 `payload_len` 与 COBS 并存是有意冗余：解码后先验长度再验 CRC，
    提早丢弃损坏帧。
- **解码端丢弃规则**：COBS 解码失败 / `ver_magic` 不符 / 长度不符 /
  CRC 不符 → 静默丢整帧（损坏帧的内容不可信，**不回 NAK**）。
  可靠性由 host 端超时重发保证（全部请求幂等，见 §5.4）。
- 帧 `seq` 与 block 头内 `block_seq`（v2k_scope.h）职责不同，**禁止合并**：
  前者管链路层请求-响应配对与重发去重；后者管数据流丢块检测，
  block 丢了链路层不补（基本规则 1），host 画断口。

### 3.2 EtherCAT 映射预案（Phase 6 执行，此处定原则）

| 本协议元素 | EtherCAT 载体 | 说明 |
|---|---|---|
| 帧适配器整层 | （消失） | mailbox/PDO 自带定界，Ethernet FCS 自带校验 |
| 控制面消息（HELLO/ENUM/CAL/DAQ_CTRL/CMD） | mailbox（最简 CoE 或厂商 mailbox） | 裸帧去掉 COBS 与 CRC，`ver_magic..payload` 原样装入 |
| BLOCK_DATA payload | TxPDO 固定布局 | `count + 0–2 block`，master 每 2 kHz 周期有数据就多拿 |
| BLOCK_REQ | （消失） | PDO 周期读取即隐式请求 |
| STATUS_RESP 核心字段 | TxPDO 头部区 | state/fault/heartbeat 随流可见 |

时间戳始终是 block 头里的 ISR tick，与 EtherCAT DC 时钟体系无关（不用 DC）。

## 4. 消息目录 v1

### 4.0 总表

| code | 名称 | 方向 | payload | 响应 |
|---|---|---|---|---|
| 0x01 | HELLO_REQ | H→F | 0 | 0x81 HELLO_RESP |
| 0x02 | STATUS_REQ | H→F | 0 | 0x82 STATUS_RESP |
| 0x03 | ENUM_REQ | H→F | 4 | 0x83 ENUM_RESP |
| 0x10 | CAL_WRITE | H→F | 2+8k | 0x90 ACK |
| 0x11 | CAL_COMMIT | H→F | 0 | 0x91 ACK（data=commit_seq） |
| 0x12 | CAL_READ | H→F | 4 | 0x92 CAL_READ_RESP |
| 0x20 | DAQ_CTRL | H→F | 12 | 0xA0 ACK |
| 0x21 | BLOCK_REQ | H→F | 2 | 0xA1 BLOCK_DATA |
| 0x30 | CMD | H→F | 8 | 0xB0 ACK（data=ack_seq） |
| 0x60–0x6F | （预留）固件升级 | — | — | Phase 6+ 定稿 |
| 0x70 | （预留）LOG 拉取 | — | — | CPU2 诊断日志 |

约定：响应 code = 请求 code | 0x80。无类型化响应的请求统一回**通用 ACK**：

```
ACK payload（8 octets）:
off sz 字段
0   1  status      0=OK 1=BAD_PARAM 2=BUSY 3=BAD_STATE 4=UNSUPPORTED 5=INTERNAL
1   1  echo_type   被确认的请求 msg_type
2   2  reserved
4   4  data        含义随消息（CAL_COMMIT→commit_seq；CMD→已转发的 cmd_seq；其余 0）
```

### 4.1 HELLO（0x01 / 0x81）

请求 payload 空。响应（28 octets）：

```
0   2  proto_ver     = V2K_WIRE_VER（host 不符即断开，提示固件/上位机版本错配）
2   2  contract_ver  = V2K_CONTRACT_VER
4   4  build_hash    固件 git 短哈希（§5.1 重枚举依据）
8   2  desc_count    描述符总数
10  2  reserved
12  16 fw_name       ASCII，如 "v2k-foc-demo"
```

### 4.2 STATUS（0x02 / 0x82）

请求 payload 空。响应（36 octets）——兼任链路心跳（host 周期轮询）：

```
0   2  sys_state      V2K_STATE_*（v2k_command.h）
2   2  fault_code
4   2  status_flags   V2K_SF_*（含 CPU2 视角的 CPU1 心跳停走位，运行时扩展）
6   2  reserved
8   4  tick           CPU1 当前 ISR tick
12  4  cpu1_heartbeat
16  4  cpu2_heartbeat
20  4  applied_seq    参数平面对账（§5.2）
24  2  cal_result     V2K_CAL_*
26  2  cal_fail_idx
28  4  build_hash     会话中检测固件热更换
32  4  scope_mode     4 组各 1 octet：V2K_SCOPE_*（组 id = octet 下标）
```

### 4.3 ENUM（0x03 / 0x83）

请求（4 octets）：`{0:2 start_idx, 2:1 max_count(≤8), 3:1 reserved}`
响应（6 + 44×count octets）：

```
0  2  total_count    （= desc_count）
2  2  start_idx      回显
4  1  count          本页实际条数
5  1  reserved
6  …  count × 描述符条目（44 octets，逐字段镜像 v2k_desc_entry_t）:
      0:16 name | 16:2 type | 18:2 kind | 20:4 addr | 24:4 min(f32)
      28:4 max | 32:4 scale | 36:4 offset | 40:2 prescaler | 42:2 group
```

`start_idx ≥ total_count` → `count=0`（合法的"读完了"信号）。

### 4.4 CAL_WRITE / CAL_COMMIT / CAL_READ（0x10/0x11/0x12）

`CAL_WRITE` 请求（2 + 8k octets）：

```
0  1  count   本帧条数 k（累计暂存不得超 V2K_PARAM_BATCH_MAX=16）
1  1  reserved
2  …  k × {0:2 desc_idx, 2:2 reserved, 4:4 value_bits}   （镜像 v2k_param_write_t）
```

语义：CPU2 写入参数平面 shadow 暂存区**但不置 commit**；多帧累计；
超上限回 ACK(BAD_PARAM)。`CAL_COMMIT`（payload 空）→ CPU2 置
`commit_seq+1`、`commit_flag=1`，回 ACK(OK, data=commit_seq)。
应用结果经 STATUS 的 `applied_seq/cal_result` 对账（§5.2）。

`CAL_READ` 请求（4 octets）：`{0:2 start_idx, 2:1 count(≤32), 3:1 reserved}`
响应（8 + 4×count）：

```
0  4  mirror_seq   值镜像刷新计数（判新旧）
4  2  start_idx
6  1  count
7  1  reserved
8  …  count × value_bits(4)    （读自参数平面 value_mirror，≈10Hz 新鲜度）
```

### 4.5 DAQ_CTRL（0x20）

请求（12 octets），逐字段镜像 `v2k_scope_cfg_t`：

```
0  1  group
1  1  mode_req      V2K_SCOPE_OFF / LIVE / SNAP_ARMED
2  2  trig_desc_idx
4  4  trig_level    f32，物理量纲
8  1  trig_edge     V2K_TRIG_*
9  1  pre_trig_pct  0..100
10 2  prescaler     0 = 维持注册值
```

CPU2 写组 cfg + 置 `commit_flag`，回 ACK(OK)=已受理；
模式实际跃迁经 STATUS 的 `scope_mode[group]` 确认。

### 4.6 BLOCK_REQ / BLOCK_DATA（0x21 / 0xA1）

请求（2 octets）：`{0:1 group, 1:1 max_blocks(1..2)}`
响应（8 + Σblock octets）：

```
0  1  group
1  1  count        0..max_blocks（0 = 当前无数据，合法）
2  1  mode         该组当前 V2K_SCOPE_*
3  1  reserved
4  2  overrun_cnt  生产侧累计丢块数（host 据此报"采集端过载"）
6  2  remain_hint  环内尚可取的 block 数（host 调轮询节奏；snapshot 排空进度）
8  …  count × block
```

block = 示波平面内存布局原样上线（**热路径零重编码**）：

```
0  4  start_tick | 4:2 block_seq | 6:2 group_id | 8:2 n_ticks | 10:2 n_ch
12 …  int16 × n_ticks × n_ch（tick-major 交错：t0ch0 t0ch1 … t1ch0 …）
```

host 凭组内 `block_seq` 跳变检测丢块 → 画断口，**不存在重传**（基本规则 1）。

### 4.7 CMD（0x30）

请求（8 octets）：`{0:2 cmd_code(V2K_CMD_*), 2:2 arg0, 4:4 arg1}`
CPU2 检查 mailbox 空闲（`cmd_seq == ack_seq`）→ 写命令 mailbox，
回 ACK(OK, data=cmd_seq)；mailbox 忙 → ACK(BUSY)，host 稍后重试。
执行结果经 STATUS 的 `sys_state/cmd_result` 确认。

## 5. 服务语义

### 5.1 会话建立与强制重枚举

```
host                          firmware(CPU2)
 │ ──── HELLO_REQ ────────────→ │
 │ ←─── HELLO_RESP ──────────── │  proto_ver 不符 → host 终止并报错
 │ ──── ENUM_REQ(0,8) ────────→ │
 │ ←─── ENUM_RESP ───────────── │  … 分页直到 count < max 或 start≥total
 │ （此后周期 STATUS 轮询，建议 2–10 Hz） │
```

host 缓存描述符表，键 = `build_hash`。任何时刻（HELLO 或 STATUS 中）
发现 `build_hash` 变化 → **作废全部缓存并重新枚举**。杜绝拿旧表读新固件。

### 5.2 参数事务（两阶段 + 异步对账）

```
host                     CPU2                      CPU1 ISR 安全点
 │ ─ CAL_WRITE ×m ─────→ │ 暂存 shadow              │
 │ ←─ ACK(OK) ×m ─────── │                          │
 │ ─ CAL_COMMIT ───────→ │ commit_seq=s, flag=1 ──→ │ 校验→整组应用→
 │ ←─ ACK(OK,data=s) ─── │                          │ applied_seq=s, flag=0
 │ ─ STATUS 轮询 ───────→ │ 读参数状态块              │
 │ ←─ applied_seq==s? ── │  ←──────────────────────  │
```

要点：批内**全有效或全拒绝**（同一拍生效，cal_result/cal_fail_idx 报因）；
host 在 applied_seq 追上 commit_seq 前不得发起下一批 COMMIT。

### 5.3 示波流

**Live**：`DAQ_CTRL(mode=LIVE)` → host 持续 `BLOCK_REQ` 轮询（SCI 阶段
即"软 PDO"；频率按 `remain_hint` 自适应）。环满生产者丢新块 + overrun_cnt++，
流不停，host 画断口。

**Snapshot**：`DAQ_CTRL(mode=SNAP_ARMED, trig…)` → CPU1 全速覆盖写环 +
逐拍触发判定 → 命中后补采 post 段（深度 = 环深×(100-pre_trig_pct)%）→
FROZEN（STATUS.scope_mode 可见）→ host `BLOCK_REQ` 慢速排空（remain_hint
递减到 0）→ host 重新 ARM。pre-trigger 历史由环形结构天然保存；
block 顺序由 host 按 `start_tick` 重建。

### 5.4 错误处理与重同步

- 损坏帧静默丢弃（§3.1）。host 对每请求设超时（建议 100 ms）+ 重发。
- **全部请求幂等**：重复 CAL_WRITE 覆盖同 desc_idx 暂存条目；重复 COMMIT
  被 `commit_seq` 对账吸收；重复 BLOCK_REQ 返回新数据（丢响应 = 丢块，
  由 block_seq 断口机制兜底，符合基本规则 1 的"丢了就丢了"）。
- host 凭帧 seq 回显丢弃迟到/错配响应。
- 失步恢复：host 发任意请求，固件解码器自动在 0x00 边界重同步。

## 6. 版本与演进策略

| 变化 | 机制 |
|---|---|
| 变量增删改 | 不动协议——build_hash 变化 → 重枚举（最常见，零成本） |
| 新增消息 | 占用新 code；旧端回 ACK(UNSUPPORTED)，向后兼容 |
| 消息追加字段 | 追加在 payload 尾部 + 长度判别；解析器忽略多余尾部 |
| 不兼容布局变更 | `V2K_WIRE_VER` +1（HELLO 即拒绝错配） |
| 共享 struct 变更 | `V2K_CONTRACT_VER` +1（双核启动握手拦截，见 v2k_command.h） |

---

## 附录 A：协议选型 ADR（2026-06-11 定稿）

**决策**：自定义消息目录 + 标准成帧原语（COBS + CRC-32C），服务语义对齐
XCP 词汇，变量描述用运行时枚举。

**背景**：评估了 XCP / MAVLink / nanopb(protobuf) / CBOR 四条"现成路线"。
两个平台事实贯穿所有评估：一是 C28x CHAR_BIT=16，一切假定 8-bit byte +
packed struct + memcpy 的现成库都需要重写其序列化核心，"省序列化代码"
这一最大优势对所有外部方案不成立；二是 CLAUDE.md 规定内存布局与线上格式是
同一数据模型，任何独立 schema 语言（.proto / 方言 XML）都会制造第二份定义。

| 候选 | 不选的原因 |
|---|---|
| **XCP**（本领域行业标准） | 领域模型完美契合（CAL/DAQ 即参数/示波平面），但其变量描述靠离线 A2L（ELF 生成），与运行时枚举的思路相反——加私有枚举扩展后现成 master（CANape 等）即用不了，生态优势缩水大半；最小子集实现量 ~2–3k 行仍要 16-bit char 手写；DAQ 每事件一个 DTO 头在 100 kHz 下开销 20–30%，不如 50-tick 摊一头的 block；EtherCAT 阶段无标准 XCP 绑定。**保留**：语义词汇对齐 + 将来 CPU2 加 XCP 门面的退路。 |
| MAVLink | 参数微服务是扁平 float 表，装不下 min/max/scale/prescaler 元数据→描述符表仍需自定义消息；payload ≤255 octets→800B block 要 4 片重组；官方 C 生成代码 = packed struct + memcpy，C28x 等于重写生成器后端。保住的只有心跳与 XML 格式。 |
| nanopb / protobuf | 官方明确不支持 CHAR_BIT≠8 平台（自维护私有分支）；varint 让 golden vectors 人工不可核；.proto 表达不了共享 RAM 布局，共享 struct 仍需手写对齐——第二数据模型成本照付。 |
| CBOR（控制面） | 有 Zephyr MCUmgr 先例，字段演进友好；但固件需自写 ~400 行 16-bit-char 安全编解码子集（造了个更通用的轮子），一协议两编码，vectors 需钉确定性编码。演进需求已由"追加字段+长度判别+重枚举机制"低成本覆盖。 |

**业界对照**：成帧层（COBS/CRC）全标准；定长小端布局是本领域（XCP/
MAVLink payload/ST MCP）主流流派；运行时枚举跟随 ODrive(Fibre)/Klipper
(data dictionary) 开源先例；RCP 细分行业（dSPACE/Imperix/Myway）本就
全部自定义私有协议——本方案 = 该行业惯例 + 一套防腐化流程（spec 为唯一基准 +
golden vectors 双端 conformance + 版本字段），针对的正是 myway 协议
"魔法偏移考古"的教训。

**自定义路线的最大风险与对策**：风险不是写不出来，而是十年后腐化。对策
已写进流程：本文档变更流程（文首）、以 vectors 为准的规则、消息目录新增不改旧、
版本字段三层（wire/contract/build_hash）各管一段。

## 附录 B：上位机 `DataSource` trait 草案（Phase 3.5 落地）

目标：myway_viewer 前端与中立数据模型复用，通讯层拆为三个数据源。
现有 `connection/mod.rs` 的 `Command`/`HardwareEvent` 已是事实边界，
拆分 = 中立化命名 + 把 myway 专有概念降级为 `MywaySource` 内部实现。

```rust
/// 中立命令（GUI → 数据源）
pub enum SourceCommand {
    Connect(String), Disconnect,
    Enumerate,                                  // V2k: ENUM; Myway: 解析 .def
    WriteParams(Vec<(VarId, f64)>),             // 暂存
    CommitParams,                               // V2k: CAL_COMMIT; Myway: inspector_write 逐条
    ReadValues { ids: Vec<VarId> },             // V2k: CAL_READ; Myway: inspector_read
    ScopeConfig(ScopeConfig),                   // V2k: DAQ_CTRL; Myway: wave_start/end
    RequestBlocks { group: u8, max: u8 },       // V2k 专属拉流（Myway 内部自驱）
    SystemCmd(SysCmd),                          // Start/Stop/ClearFault ↔ execute/stop
}

/// 中立事件（数据源 → GUI）
pub enum SourceEvent {
    Connected(DeviceInfo),                      // ← HELLO（build_hash, fw_name）
    Disconnected, Error(String),
    Descriptors(Vec<VarDescriptor>),            // ← ENUM / .def 解析结果
    Status(DeviceStatus),                       // ← STATUS / status_poll(28)
    ParamsApplied { seq: u32, result: CalResult },
    Values { seq: u32, vals: Vec<(VarId, f64)> },
    Blocks(Vec<ScopeBlock>),                    // ← BLOCK_DATA / WaveRound 切块
    ScopeStateChanged { group: u8, mode: ScopeMode },
}

pub trait DataSource: Send {
    fn spawn(self: Box<Self>, rt: &tokio::runtime::Runtime)
        -> (mpsc::UnboundedSender<SourceCommand>,
            mpsc::UnboundedReceiver<SourceEvent>);
}
// 实现: SimSource(L2 FFI) / MywaySource(现有 protocol/ 模块内迁) / V2kSource(本 spec)
```

与 myway 现状对照：`StartInspector`→`ReadValues` 周期化由源内部处理；
`WaveRound(Vec<Vec<f32>>)` 整轮交付改为 `Blocks` 增量交付，Myway 源把
整轮切成单 block（group=0, start_tick 合成）适配；`Download/Verify`
（固件烧写）暂留 myway 专属扩展，V2k 对应 0x60 预留号段实现后再中立化。
