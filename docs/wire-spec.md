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
- **变量寻址的通用键 = `(addr, type)`**（CPU1 数据空间 word 地址 + 类型码）。
  地址有两个来源，固件不区分：① ENUM 枚举的描述符表（**只含 L1 自动注册的
  平台量**，作为开箱即用面与默认绑定来源）；② viewer 解析与固件同构建的 .out
  （ELF/DWARF）得到的完整符号树（一切应用变量，含任意 struct 成员/数组元素；
  配对正确性由 build_hash 校验）。用户/学生不写注册代码、不手打名字字符串。
- **线上值就是真实值**：协议不承载 `min/max/scale/offset` 这类显示或护栏元数据。
  DAQ block 与 CAL value 都按变量原生类型解释位模式；固件不量化、不换算、不做
  参数范围裁剪或范围拒绝。

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
| 0x10 | CAL_WRITE | H→F | 2+12k | 0x90 ACK |
| 0x11 | CAL_COMMIT | H→F | 0 | 0x91 ACK（data=commit_seq） |
| 0x12 | CAL_READ | H→F | 4 | 0x92 CAL_READ_RESP |
| 0x20 | DAQ_CTRL | H→F | 12（兼容）或 14 | 0xA0 ACK |
| 0x21 | BLOCK_REQ | H→F | 2 | 0xA1 BLOCK_DATA |
| 0x22 | DAQ_BIND | H→F | 2+8k | 0xA2 ACK（data=bind_seq） |
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

请求 payload 空。响应基础前缀 28 octets，wire v1 当前响应为 36 octets；
新增字段只允许追加在尾部：

```
0   2  proto_ver     = V2K_WIRE_VER（host 不符即断开，提示固件/上位机版本错配）
2   2  contract_ver  = V2K_CONTRACT_VER
4   4  build_hash    固件 git 短哈希（§5.1 重枚举依据）
8   2  desc_count    描述符总数
10  2  reserved
12  16 fw_name       ASCII，如 "viewer2000"
28  4  tick_hz       CPU1 ISR tick 频率；block tick 换算为秒的唯一依据
32  4  capabilities  设备能力位
```

`capabilities`（只追加，不复用）：

| bit | 名称 | 语义 |
|---:|---|---|
| 0 | ENUM | 描述符枚举 |
| 1 | CAL | 参数读写与原子提交 |
| 2 | DAQ_LIVE | Live 连续流 |
| 3 | DAQ_SNAPSHOT | 触发 Snapshot |
| 4 | PRE_TRIGGER | Snapshot pre-trigger |
| 5 | SYSTEM_CMD | Start / Stop / Clear Fault |
| 6 | NATIVE_BLOCK | 原生位宽 `ScopeBlock` |

旧解析器可只读 28-octet 前缀；新解析器若收到较短响应，尾部能力按 0 处理。

### 4.2 STATUS（0x02 / 0x82）

请求 payload 空。响应 42 octets；兼任链路心跳（host 周期轮询）：

```
0   2  sys_state      V2K_STATE_*（v2k_command.h）
2   2  fault_code
4   2  status_flags   V2K_SF_*（含 CPU2 视角的 CPU1 心跳停走位，运行时扩展）
6   4  tick           CPU1 当前 ISR tick
10  4  cpu1_heartbeat
14  4  cpu2_heartbeat
18  4  applied_seq    参数平面对账（§5.2）
22  2  cal_result     V2K_CAL_*
24  2  cal_fail_idx
26  4  build_hash     会话中检测固件热更换
30  4  scope_mode     4 组各 1 octet：V2K_SCOPE_*（组 id = octet 下标）
34  4  cmd_ack_seq    CPU1 已执行的最大系统命令序号
38  2  cmd_result     V2K_CMDR_*（对应 cmd_ack_seq）
40  2  reserved
```

### 4.3 ENUM（0x03 / 0x83）

枚举对象 = 描述符表 = L1 自动注册的平台量（物理量/占空比/状态/平台参数），
用于开箱即用和默认绑定。应用变量不在此表，经 DWARF 路径发现（§2 约定）。

请求（4 octets）：`{0:2 start_idx, 2:1 max_count(≤8), 3:1 reserved}`
响应（6 + 28×count octets）：

```
0  2  total_count    （= desc_count）
2  2  start_idx      回显
4  1  count          本页实际条数
5  1  reserved
6  …  count × 描述符条目（28 octets，逐字段镜像当前 v2k_desc_entry_t）:
      0:16 name | 16:2 type | 18:2 kind | 20:4 addr | 24:2 prescaler | 26:2 group
      prescaler/group 为开机默认绑定提示，运行时以 DAQ_BIND/CTRL 为准
```

`start_idx ≥ total_count` → `count=0`（合法的"读完了"信号）。

### 4.4 CAL_WRITE / CAL_COMMIT / CAL_READ（0x10/0x11/0x12）

`CAL_WRITE` 请求（2 + 12k octets）：

```
0  1  count   本帧条数 k（累计暂存不得超 V2K_PARAM_BATCH_MAX=16）
1  1  reserved
2  …  k × {0:4 addr, 4:4 value_bits, 8:2 type, 10:2 reserved}（镜像 v2k_param_write_t）
```

语义：CPU2 写入参数平面 shadow 暂存区**但不发布**；多帧累计，**同 addr
覆盖已暂存条目**（重发幂等的依据）；超上限回 ACK(BAD_PARAM)。
`CAL_COMMIT`（payload 空）→ CPU2 填 count 后最后写 `commit_seq+1`（发布），
回 ACK(OK, data=commit_seq)。应用结果经 STATUS 的 `applied_seq/cal_result`
对账（§5.2）。

提交语义：CPU1 对每条写入只做机械一致性检查——type 合法、地址位于允许写入的
CPU1 数据区、32-bit 类型地址对齐；addr 命中描述符表时还要求 `kind&PARAM` 且
type 一致。**不做 min/max 范围检查，不做 clamp，不做 scale/offset 反算**。
批内任一条机械检查失败则整批拒绝；检查通过后在 ISR 安全点整批同拍写入。

`CAL_READ` 请求（4 octets）：`{0:2 start_idx, 2:1 count(≤32), 3:1 reserved}`
响应（8 + 4×count）：

```
0  4  mirror_seq   值镜像刷新计数（判新旧）
4  2  start_idx
6  1  count
7  1  reserved
8  …  count × value_bits(4)    （读自参数平面 value_mirror，≈10Hz 新鲜度）
```

### 4.5 DAQ_CTRL / DAQ_BIND（0x20 / 0x22）

`DAQ_CTRL` 请求保留原 12-octet 前缀，并在新格式尾部追加
`block_n_ticks`，共 14 octets：

```
0  1  group
1  1  mode_req      V2K_SCOPE_OFF / LIVE / SNAP_ARMED
2  2  trig_ch_slot  触发源 = 本组绑定的通道槽位 0..n_ch-1
4  4  trig_level    f32，**源值域**（f32 变量=值本身，ADC 计数=计数值；
                    固件无物理换算知识，host 按显示元数据折算后下发）
8  1  trig_edge     V2K_TRIG_*
9  1  pre_trig_pct  0..100
10 2  prescaler     0 = 维持当前值
12 2  block_n_ticks 0 = 维持当前值；旧 12-octet 请求按默认 N=10 解释
```

固件必须继续接受旧 12-octet 请求；该向后兼容扩展不提升 wire version。
CPU2 写组 cfg 并发布 `cfg_seq`，回 ACK(OK)=已受理；
模式实际跃迁经 STATUS 的 `scope_mode[group]` 确认。

`DAQ_BIND` 请求（2 + 8k octets）——**运行时选通道，不重烧**：

```
0  1  group
1  1  n_ch          1..8
2  …  k × 通道绑定（8 octets，逐字段镜像 v2k_scope_ch_bind_t）:
      0:4 addr | 4:2 type | 6:2 reserved
```

语义：addr 来源任意（描述符表或 DWARF）；样本按**原生宽度无损直拷**
（I16/U16→2 octets，I32/U32/F32→4 octets，位模式原样，固件零转换零量化
——准确性优先）。物理量换算（如 ADC 计数→安培）是纯 host 侧显示元数据，
不上线、不进固件。**仅组 mode==OFF 时可绑**。
CPU2 写组 bind 区并发布 `bind_seq`，然后短暂等待（≤1 ms）CPU1 的
`bind_ack_seq/bind_result`，把最终结果放进 ACK：OK / BAD_STATE（非 OFF，
先 DAQ_CTRL(OFF)）/ BAD_PARAM（n_ch 或 type 非法），data=bind_seq；
超时回 INTERNAL（CPU1 ISR 未运行）。

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
12 2  bind_seq      产生本块的绑定代号（host 换绑后丢弃不匹配旧块）
14 2  stride_octets 每 tick 样本区宽度 = Σ 通道原生宽度（块自描述定界）
16 …  样本区 n_ticks × stride_octets：tick-major，每 tick 内按绑定顺序，
      各通道按原生宽度连续排列（I16/U16=2，I32/U32/F32=4，LE 位模式无损）
```

host 凭组内 `block_seq` 跳变检测丢块 → 画断口，**不存在重传**（基本规则 1）。

带宽参考（ISR 周期 20–100 kHz 待定）：20kHz×8ch×f32 = 640 KB/s；
100kHz×8ch×f32 = 3.2 MB/s——均在 EtherCAT 实用吞吐内，物理层结论不变。
EtherCAT 档 N 由单帧过程数据上限（~1486 octets）在 Phase 6 定
（f32 8ch：N=20×2 块或 N=40×1 块量级）。

### 4.7 CMD（0x30）

请求（8 octets）：`{0:2 cmd_code(V2K_CMD_*), 2:2 arg0, 4:4 arg1}`
CPU2 检查 mailbox 空闲（`cmd_seq == ack_seq`）→ 写命令 mailbox，
回 ACK(OK, data=cmd_seq)；mailbox 忙 → ACK(BUSY)，host 稍后重试。
执行结果经 STATUS 的 `cmd_ack_seq/cmd_result/sys_state` 确认。

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

应用变量发现（DWARF 路径，viewer 侧 Phase 3.5+）：viewer 加载用户构建出的
.out（ELF+DWARF，cl2000 EABI 标准产物），解析符号树供 GUI 浏览/勾选；
加载时校验 .out 内嵌的 build_hash 与固件 HELLO 报告值一致，不一致即拒载
（杜绝拿旧符号表算新固件的地址）。固件对此路径零感知。

### 5.2 参数事务（两阶段 + 异步对账）

```
host                     CPU2                      CPU1 ISR 安全点
 │ ─ CAL_WRITE ×m ─────→ │ 暂存 shadow              │
 │ ←─ ACK(OK) ×m ─────── │                          │
 │ ─ CAL_COMMIT ───────→ │ 发布 commit_seq=s ──────→ │ 见 s≠applied_seq:
 │ ←─ ACK(OK,data=s) ─── │                          │ 机械校验→整组应用→
 │ ─ STATUS 轮询 ───────→ │ 读参数状态块              │ 写 applied_seq=s
 │ ←─ applied_seq==s? ── │  ←──────────────────────  │
```

要点：批内**全有效或全拒绝**（同一拍生效，cal_result/cal_fail_idx 报因；
所有成功写入均是原生位模式写入，不做范围或单位换算）；
host 在 applied_seq 追上 commit_seq 前不得发起下一批 COMMIT。

### 5.3 示波流

通道选择（任何模式开始前）：`DAQ_CTRL(OFF)` → `DAQ_BIND(group, 通道列表)`
→ ACK(OK) 后方可启动。开机时 L1 已写入默认绑定（组 0 = 平台经典 8 通道），
host 不发 BIND 也能直接看波形。

**Live**：`DAQ_CTRL(mode=LIVE)` → host 持续 `BLOCK_REQ` 轮询（SCI 阶段
即"软 PDO"；频率按 `remain_hint` 自适应）。环满生产者丢新块 + overrun_cnt++，
流不停，host 画断口。

**Snapshot**：`DAQ_CTRL(mode=SNAP_ARMED, trig…)` → CPU1 全速覆盖写环 +
逐拍触发判定 → 命中后补采 post 段（深度 = 环深×(100-pre_trig_pct)%）→
FROZEN（STATUS.scope_mode 可见）→ host `BLOCK_REQ` 慢速排空（remain_hint
递减到 0）→ host 重新 ARM。pre-trigger 历史由环形结构天然保存；
block 顺序由 host 按 `start_tick` 重建。

应用变量的"watch 窗口"= 把变量绑到慢速组（prescaler 大，如 1 kHz/10 Hz）
跑 Live——自带 tick 时间戳且原生位模式无损（f32 看到的就是精确值），
取代独立轮询式监控。

### 5.4 错误处理与重同步

- 损坏帧静默丢弃（§3.1）。host 对每请求设超时（建议 100 ms）+ 重发。
- CPU2 缓存上一条已编码响应；host 以相同 `(msg_type, seq)` 超时重发时直接
  重放，不再次执行 CAL_COMMIT、CMD、DAQ_BIND 或消费 BLOCK。缓存只复用当前
  TX 缓冲，不增加第二份 1 KB 级缓冲。
- 服务本身仍保持可安全重试：重复 CAL_WRITE 覆盖同 addr 暂存条目；
  DAQ_BIND 整区覆盖。host 只有在放弃旧请求并使用新 seq 后，才视为新操作。
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
| MAVLink | 参数微服务是扁平 float 表，装不下 type/kind/prescaler/group 等运行时枚举元数据→描述符表仍需自定义消息；payload ≤255 octets→800B block 要 4 片重组；官方 C 生成代码 = packed struct + memcpy，C28x 等于重写生成器后端。保住的只有心跳与 XML 格式。 |
| nanopb / protobuf | 官方明确不支持 CHAR_BIT≠8 平台（自维护私有分支）；varint 让 golden vectors 人工不可核；.proto 表达不了共享 RAM 布局，共享 struct 仍需手写对齐——第二数据模型成本照付。 |
| CBOR（控制面） | 有 Zephyr MCUmgr 先例，字段演进友好；但固件需自写 ~400 行 16-bit-char 安全编解码子集（造了个更通用的轮子），一协议两编码，vectors 需钉确定性编码。演进需求已由"追加字段+长度判别+重枚举机制"低成本覆盖。 |

**业界对照**：成帧层（COBS/CRC）全标准；定长小端布局是本领域（XCP/
MAVLink payload/ST MCP）主流流派；运行时枚举跟随 ODrive(Fibre)/Klipper
(data dictionary) 开源先例；RCP 细分行业（dSPACE/Imperix 等）本就
全部自定义私有协议——本方案 = 该行业惯例 + 一套防腐化流程（spec 为唯一基准 +
golden vectors 双端 conformance + 版本字段），针对的正是既有私有协议
"魔法偏移考古"的教训。

**自定义路线的最大风险与对策**：风险不是写不出来，而是十年后腐化。对策
已写进流程：本文档变更流程（文首）、以 vectors 为准的规则、消息目录新增不改旧、
版本字段三层（wire/contract/build_hash）各管一段。

### ADR-2：变量发现架构（2026-06-11 定稿）

**决策**：描述符表只承载 L1 自动注册的平台量（开箱即用面 + 默认
绑定来源）；应用变量一律走"viewer 解析 .out(DWARF) → 按 (addr,type) 下发"
路径，示波经 DAQ_BIND、参数写经 CAL_WRITE。

**否决的中间形态**：① L2 组件 init 自注册（`pi_init(&pi, "vel")`）与
② 用户侧字符串化注册宏——两者都要求用户为变量维护第二个名字字符串
（FreeRTOS 式双命名反模式）或手动逐个注册；C 符号本身是唯一可接受的
命名来源，而 DWARF 恰好免费提供它。

**代价与对策**：viewer 必须实现 ELF/DWARF 解析（Rust `gimli`/`object`，
Phase 3.5+）；.out 与固件的配对靠 build_hash 双向校验。协议不承载
`min/max/scale/offset`：值本身必须已经是要显示、记录、写回的真实量。
换得：学生零注册代码、零命名负担、任意 struct 成员/数组元素可观测，且
通道选择完全运行时化（不重烧）。

## 附录 B：Scope2000 `DataSource` 边界（Phase 3.5）

Scope2000 的原生实现是 `V2kSource`，内部严格分成服务语义、消息 codec 与
byte-stream transport 三层。Phase 3.5 transport = SCI；Phase 6 增加
EtherCAT transport 时，不改变 GUI 数据模型和服务命令。

```rust
pub enum SourceCommand {
    Connect(TransportEndpoint),
    Disconnect,
    WriteParams(Vec<ParamWrite>),
    CommitParams,
    ReadValues { start: u16, count: u8 },
    BindChannels { group: u8, channels: Vec<VarRef> },
    ConfigureScope(ScopeConfig),
    SystemCommand(SystemCommand),
}

pub enum SourceEvent {
    Connected(DeviceInfo),
    Descriptors(Vec<VarDescriptor>),
    Status(DeviceStatus),
    Blocks(Vec<ScopeBlock>),
    StreamGap { group: u8, expected: u16, received: u16 },
    DeviceChanged { old_hash: u32, new_hash: u32 },
    // 参数、绑定、模式、错误与日志事件略
}
```

`ScopeBlock` 保留 `start_tick/block_seq/bind_seq/stride_octets` 与原生
`samples: Vec<u8>`；只有绘图或 CSV 导出边界才按绑定类型展开为数值，不应用
任何 scale/offset。

旧设备兼容不实现为 Scope2000 内部专用数据源。未来独立 `LegacyBridge`
进程负责旧协议，另一侧通过通用本地 byte-stream transport 暴露规范化的
Viewer2000 消息语义，并用 capability 位声明缺失能力。桥接器可合成
tick/序号并显式报告精度限制，但不得定义、削减或拖慢 `V2kSource` 原生路径。
