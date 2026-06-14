# Phase 3.5 — SCI 数据泵与 Scope2000：操作与验证清单

> 本文是 **Phase 3.5 待执行的 bring-up 流程**，不是实测记录。固件 commit、
> Scope2000 commit、波特率、连续运行时间、错误计数和波形截图统一写入
> [BRINGUP.md](../BRINGUP.md) 的 Phase 3.5 区。
>
> Phase 3 已经证明 CPU1 能生产描述符、参数状态和原生示波 block；Phase 3.5
> 要证明 CPU2 与真实 PC 程序能在**不干扰控制核**的前提下消费这些接口。

Phase 3 的 CCS Graph 直接读 CPU1 内存，绕过了 CPU2、GS4 消费者索引、IPC
命令转发和线上协议。Phase 3.5 第一次把完整路径闭合：

```text
CPU1 ISR
  -> 描述符表 / 参数平面 / Scope SPSC 环 / 命令状态
  -> CPU2 共享接口消费者
  -> Viewer2000 wire v1（显式序列化）
  -> COBS + CRC-32C
  -> SCIA / GPIO42,43 / XDS110 VCP
  -> Scope2000 V2kSource
  -> 变量面板 / 参数事务 / 波形 / CSV / 控制台
```

本阶段的验收对象是**接口、隔离边界和协议语义**，不是 SCI 的最终吞吐量。
115200 baud 不可能承载平台的 100 kHz × 8ch 性能锚点；最终连续高速链路仍是
Phase 6 EtherCAT。SCI 只负责在低成本物理链路上提前暴露双核和 host 端问题。

## 我已完成的部分（仅供对照）

| 产物 | 内容 |
|---|---|
| `contracts/v2k_common.h`、`v2k_command.h` | contract v3；HELLO 的 `tick_hz/capabilities`；STATUS 的 `cmd_ack_seq/cmd_result`；原生能力位 |
| `docs/wire-spec.md` | wire v1 消息、重试幂等、build-hash 重枚举、独立兼容桥边界 |
| `contracts/vectors/`、`tools/gen_vectors.py` | HELLO/STATUS/ENUM/CAL/DAQ/CMD/BLOCK golden vectors 与负例 |
| `cpu2/v2k_sci_service.c/.h` | SCIA 收发、COBS、CRC-32C、请求分派、响应重放、共享平面服务和诊断计数 |
| `cpu2/cpu2.c` | CPU2 超级循环接入 SCI 服务；本地心跳不进入控制时间 |
| `cpu1/cpu1.c` | 发布 `tick_hz`，供 HELLO 与 host 时间轴使用 |
| 同级 `Scope2000` 仓库 | Rust 2024 + egui；codec/transport/service 分层；SCI transport；变量、参数、Live、Snapshot、CSV、控制台 |

尚未完成且**不能跳过**的硬件配置整改：

- 当前 Phase 3.5 临时代码在 `cpu1/cpu1.c` 直接调用 GPIO/CPUSEL driverlib。
  这违反“SysConfig 管静态硬件”的项目原则。
- 最终验收前必须按 §1 迁移到 CPU1/CPU2 SysConfig，并删除这些手写调用。
- `.syscfg` 只能通过 CCS SysConfig 工具修改，禁止手改文本或生成的
  `board.c/board.h`。

## 关键决策（定稿）

- **CPU1 仍是 boot master，但不运行 SCI。** CPU1 SysConfig 只负责在放出
  CPU2 前把 SCIA 外设归属切到 CPU2。
- **CPU2 拥有 SCIA。** CPU2 SysConfig 负责 SCIA 实例、GPIO42/43 pinmux、
  pad/qualification、115200 8N1 和 FIFO 静态配置；CPU2 C 负责 RX ISR、软件环、
  codec、共享平面访问和 TX 调度。
- **CPU1 源码不得出现 Phase 3.5 的 pinmux/CPUSEL 补丁。** 最终生成结果必须
  来自 `.syscfg`，否则 SysConfig GUI 与运行代码存在两个真相。
- **RX ISR 只搬 octet。** ISR 不做 COBS、CRC、消息分派、共享 RAM 遍历、
  block 复制或阻塞发送。
- **单请求在途。** Scope2000 同时只等待一个响应；请求超时 150 ms，同一
  `(msg_type, seq)` 最多重试 2 次。
- **重试不得重复副作用。** CPU2 保留上一条已编码响应；相同请求重放响应，
  不再次执行 COMMIT、CMD、BIND，也不再次推进 Scope 消费索引。
- **线上数据保留原生形态。** `ScopeBlock` 保留样本位宽、tick、block/bind
  序号和交错布局；只在绘图或 CSV 边界换算为显示值。
- **能力由原生平台定义。** Scope2000 按 Viewer2000 完整 capability 模型设计；
  未来兼容桥只能声明缺失能力，不能反向削减原生协议或热路径。
- **通信失败不能污染控制域。** Scope2000 停止、串口拔出、CPU2 堵塞或环溢出
  都只表现为失联、超时、overrun 或断口；CPU1 tick、ISR 预算和保护状态必须照常。

## 1. SysConfig 与代码职责整改

TI 双核 SCI SysConfig 示例采用以下分工：

```text
CPU1 syscfg: sysctl.cpuSel_SCIA = SYSCTL_CPUSEL_CPU2
CPU2 syscfg: SCI instance = SCIA + RX/TX pinmux + SCI configuration
```

Viewer2000 按同一模式落地。

### 1.1 CPU1 SysConfig

在 `cpu1/sysconfig_cpu1.syscfg` 对应的 SysConfig GUI 中设置：

| 项 | 值 |
|---|---|
| Peripheral CPU Select | SCIA → CPU2 |
| 配置时机 | CPU1 `Board_init()`，且必须早于 `Device_bootCPU2()` |

生成的 CPU1 `board.c` 必须包含等价于下列语义的代码：

```c
SysCtl_selectCPUForPeripheralInstance(SYSCTL_CPUSEL_SCIA,
                                      SYSCTL_CPUSEL_CPU2);
```

CPU1 **不创建 SCI 运行实例**，也不设置 baud/FIFO/SCI 中断。

### 1.2 CPU2 SysConfig

在 `cpu2/sysconfig_cpu2.syscfg` 增加 SCI 实例，优先在 Board View 选择
`XDS110 UART` 硬件；若用手动 pinmux，则固定下表：

> **F28P65x 双 syscfg 分工**：CPU2 syscfg 的 SCI 实例**不会**在 CPU2 board.c
> 生成 `GPIO_setPinConfig`（`pinmux.csv` 提示 "PinMux is done on CPU1"），
> sysconfig 通过双 context 协同把 SCIA pinmux + pad/qual 反向落到 **CPU1**
> board.c 的 `PINMUX_init`。CPU1 不需要也不允许重复挂 SCI 模块（否则跨
> context 报 Resource conflict）；仅在 §1.1 设 `cpuSel_SCIA → CPU2` 即可。

| 字段 | 值 |
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

pad 配置也必须由 SysConfig 生成。若 GUI 对 SCI pin 使用默认 pull-up，应以
生成结果为准并在实物上验证；任何有意调整都回到 SysConfig 修改，不能在
`cpu1.c` 或 `cpu2.c` 追加 GPIO 覆盖。

中断职责采用以下边界：

- SysConfig 生成 pinmux、SCI frame/baud、FIFO 和 module enable；
- CPU2 C 注册 `INT_SCIA_RX`，设置项目所需 RX FIFO level，开启 RXFF 中断；
- TX 继续由超级循环轮询 FIFO 空位，不启用 TX ISR。

CPU2 启动顺序应为：

```text
Device_init
  -> SCIA_BASE_init（sysconfig 生成；直接调用，绕开 Board_init 聚合入口）
  -> NMI 兜底
  -> 双核共享契约握手
  -> SCI 软件缓冲与 RX ISR 初始化
  -> 超级循环
```

> **为何不直接调用 `Board_init()`**：CPU2 仅拥有 RAMGS4（0x2000 words，
> 扣掉 v2k 平面后约 0x1E00 给 .text+.bss+.const+.data）。`Board_init()`
> 顺带触发的 `SYSCTL_init()` 内含数百条 boot-master 专属
> `SysCtl_setPeripheralAccessControl`/`CPUSEL` 写入——对 CPU2 是死代码，
> 但 cl2000 `-Ooff` 默认按 translation unit 链接，会把整个 SYSCTL_init 拉
> 进 CPU2 .text 把 RAMGS4 撑爆（链接器报 `error #10099-D: .bss size 0xb42
> won't fit`）。直调 `SCIA_BASE_init()` 配合 §6.3 强制的
> `--gen_func_subsections=on`，可让链接器只带入实际被调用的 board.obj 函
> 数，board.obj 最终只贡献 ~150 words 给 CPU2 .text。

### 1.3 必须删除的手写静态配置

SysConfig 迁移完成后，`cpu1/cpu1.c` 不得再出现：

```text
GPIO_setPinConfig(GPIO_42_SCIA_TX / GPIO_43_SCIA_RX)
GPIO_setPadConfig(42 / 43, ...)
GPIO_setQualificationMode(43, ...)
GPIO_setControllerCore(42 / 43, ...)
SysCtl_selectCPUForPeripheralInstance(SCIA, CPU2)
```

CPU2 SCI 初始化也不得重复执行已经由生成代码完成的 `SCI_setConfig`、
pinmux、FIFO/module 静态初始化。重复写虽然可能“能跑”，但会掩盖 `.syscfg`
错误，并使后续换 pin、换 LSPCLK 或升级 C2000Ware 时出现配置漂移。
`v2k_sci_init()` 仅保留 RX FIFO level（覆盖生成的 RX0=empty 为 RX1）、
`SCI_clearOverflowStatus()`、`INT_SCIA_RX` 注册与 `SCI_INT_RXFF` 使能。

### 1.4 生成结果对账

每次 SysConfig 修改后检查 RAM 和 FLASH 两套生成结果：

| 检查项 | 通过条件 |
|---|---|
| CPU1 SCIA CPUSEL | 生成值为 CPU2，不再是默认 CPU1 |
| CPU2 SCIA base | `SCIA_BASE` |
| pinmux | TX=GPIO42，RX=GPIO43 |
| RX qualification | Async |
| 串口格式 | 115200 8N1 |
| FIFO | Enabled |
| 手写覆盖 | CPU1/CPU2 业务源码中无静态 pinmux/CPUSEL/重复 SCI 配置 |

切勿直接编辑 `cpu*/RAM/syscfg/board.c` 或 `cpu*/FLASH/syscfg/board.c`：
这些都是可再生文件，不是配置源。

## 2. 协议与版本前置

Phase 3.5 固定使用：

| 项 | 值 |
|---|---|
| `V2K_WIRE_VER` | 1 |
| `V2K_CONTRACT_VER` | 3 |
| 最大 payload | 1024 octets |
| framing | COBS，`0x00` 定界 |
| integrity | CRC-32C |
| endian | little-endian |
| host request | 单请求在途 |
| timeout/retry | 150 ms；最多重试 2 次 |

版本字段各管一层：

| 字段 | 负责的问题 |
|---|---|
| wire version | 帧或消息出现不兼容布局变化 |
| contract version | CPU1/CPU2 共享 struct 是否同代 |
| build hash | 同一协议下变量表、地址和固件构建是否变化 |

HELLO 必须报告以下原生能力：

```text
ENUM | CAL | DAQ_LIVE | DAQ_SNAPSHOT |
PRE_TRIGGER | SYSTEM_CMD | NATIVE_BLOCK
```

Scope2000 对 wire 或 contract 不匹配必须拒绝连接，不能猜测解析。运行中
STATUS 的 `build_hash` 变化必须清空描述符、绑定和波形缓存，再重新枚举。

## 3. CPU2 数据泵行为

### 3.1 RX 路径

```text
SCIA RX FIFO
  -> INT_SCIA_RX
  -> 512-word 软件环（每个 C28x word 只用低 8 bit）
  -> 超级循环寻找 0x00
  -> COBS decode
  -> header/length/version 检查
  -> CRC-32C
  -> message dispatch
```

约束：

- RX ISR 不等待 TX、不访问 CPU1 producer 环、不遍历描述符表；
- 软件环满时 `g_v2k_sci_rx_overflow++`，丢当前 octet，控制核不受影响；
- 编码帧超过接收暂存容量后进入 discard，直到下一个 `0x00` 才恢复；
- COBS、长度、版本或 CRC 错误静默丢弃，不回 NAK，由 host 超时重试；
- C28x `char` 为 16 bit，线上 octet 必须逐字段显式装拆，禁止 struct memcpy。

### 3.2 TX 与响应重放

CPU2 先把完整响应编码进 TX buffer，再由超级循环按 FIFO 空位送出。TX 未完成
期间不覆盖该 buffer，因为它同时承担上一响应的重放缓存。

收到相同 `(msg_type, seq)`：

- TX buffer 从头重发；
- 不重新执行消息 handler；
- BLOCK_REQ 不再次 `release`；
- CAL_COMMIT/CMD 不再次递增共享序号；
- DAQ_BIND 不再次发布新 `bind_seq`。

host 放弃旧请求并使用新 seq 后，才算新的服务操作。

### 3.3 消息到共享平面的映射

| wire 消息 | CPU2 行为 | CPU1 对账 |
|---|---|---|
| HELLO | 读版本、build hash、描述符数、tick_hz、能力位 | 无副作用 |
| STATUS | 汇总状态、心跳、参数结果、scope mode、命令结果 | 无副作用 |
| ENUM | 分页读描述符表，每页最多 8 项 | `desc_count/build_hash` |
| CAL_WRITE | 暂存 GS4 shadow；同地址覆盖 | 尚未发布 |
| CAL_COMMIT | 最后写 `commit_seq+1` | `applied_seq/result/fail_idx` |
| CAL_READ | 读 10 Hz value mirror | `mirror_seq` |
| DAQ_CTRL | 发布每组 scope cfg | `cfg_ack_seq/cfg_result/mode` |
| DAQ_BIND | 仅 OFF 状态发布绑定 | `bind_ack_seq/bind_result` |
| BLOCK_REQ | 每次取 0–2 block，复制完成后 release | `rd_idx/remain_hint` |
| CMD | 最后写 `cmd_seq+1` | `cmd_ack_seq/cmd_result/sys_state` |

Snapshot 在覆盖采集期间没有消费语义。只有生产者进入 `SNAP_FROZEN` 后，
CPU2 才从 `frozen_end_idx - frozen_count` 初始化消费者并按时间顺序排空。

### 3.4 CPU2 诊断量

在 CPU2 CCS Expressions 中常驻：

| 表达式 | 含义 |
|---|---|
| `g_handshake_state` | 0/1/2/3：未开始、等表、契约失败、运行 |
| `g_v2k_sci_rx_octets` | RX ISR 已接收的 octet |
| `g_v2k_sci_tx_octets` | 已写入 TX FIFO 的 octet |
| `g_v2k_sci_rx_overflow` | 硬件 FIFO 或软件环溢出 |
| `g_v2k_sci_bad_frames` | COBS/长度/版本/CRC/超长帧错误 |
| `g_v2k_sci_good_frames` | 通过完整校验的请求 |
| `g_v2k_msg_2to1.cpu2_status.link_state` | 最近约 2 s 内是否收到合法帧 |
| `g_v2k_msg_2to1.cpu2_status.heartbeat` | CPU2 本地诊断心跳 |
| `g_v2k_gs4.scope_cons[g].rd_idx` | 每组消费者位置 |

这些计数只诊断通信核，不得成为控制时间或 block 时间戳来源。

## 4. Scope2000 行为与操作入口

Scope2000 的 `V2kSource` 分三层：

```text
service semantics
  -> message codec
  -> ByteTransport
       -> SCI transport（本阶段）
       -> local byte stream（只预留边界）
       -> EtherCAT transport（Phase 6）
```

GUI 首版提供：

- 串口枚举、115200/更高试验波特率和连接状态；
- HELLO 版本、build hash、tick_hz、capability；
- 运行时变量枚举与最多 8 通道绑定；
- 参数 Stage/Commit 与 value mirror 刷新；
- Start/Stop/Clear Fault；
- Live、Snapshot、触发边沿、阈值、pre-trigger、prescaler、block N；
- 波形 tiles、断口、CSV 导出和协议控制台。

`V2kSource` 当前调度：

| 行为 | 周期 |
|---|---:|
| STATUS | 250 ms |
| 无 backlog 的 BLOCK_REQ | 8 ms |
| `remain_hint != 0` | 立即继续取块 |
| worker idle sleep | 1 ms |

Scope2000 按 capability 开关 UI。未声明的功能只禁用对应操作，不改变
Viewer2000 原生数据模型。

## 5. SCI 带宽预算

8N1 每个线上 octet 约占 10 bit，115200 baud 的理论上限只有：

```text
115200 / 10 = 11520 octets/s
```

还要扣除 COBS、wire header、CRC、STATUS/控制请求和 USB/VCP 抖动。bring-up
阶段建议把持续流预算控制在理论值的约 70%，即约 8 k octets/s。

单个原生 block：

```text
block_octets = 16 + n_ticks * stride_octets
```

BLOCK_DATA 另有 8-octet batch 前缀，wire 帧另有 11-octet header+CRC，并有
少量 COBS 膨胀。稳定 Live 必须满足：

```text
生产 block 速率 × 每 block 线上开销 < 可用串口吞吐
```

因此：

- 115200 下 Live 必须提高 `prescaler`、减少通道或使用较合适的 block N；
- Snapshot 可以全速采集，因为冻结后慢速排空，不要求串口跟上采样瞬时速率；
- 故意使用超过 SCI 带宽的配置时，正确结果是 producer overrun + block gap，
  不是 CPU1 降速或阻塞；
- 逐档提高 baud 只用于寻找 XDS110 VCP 的稳定边界，不能把 SCI 变成最终链路。

## 6. 软件与构建检查

### 6.1 Viewer2000 协议检查

```bash
python3 tools/gen_vectors.py --check
cc -std=c99 -Wall -Wextra -Werror \
  -c tools/check_contracts.c -o /tmp/v2k-contracts.o
```

确认 golden vectors 没有未提交漂移，PC 编译器侧静态断言通过。

### 6.2 Scope2000 检查

在同级 Scope2000 仓库执行：

```bash
cargo fmt --check
cargo clippy --all-targets -- -D warnings
cargo test --all-targets
python3 tools/check-brand.py
```

测试至少覆盖：

- 全部 golden vectors；
- 坏 CRC；
- 拆包与粘包；
- 超长垃圾后在 `0x00` 重新同步；
- 响应 seq 错配；
- wire/contract 不匹配；
- 请求超时与重试。

### 6.3 目标工程构建

必须通过 CCS `buildProject` 分别构建：

| 核 | 配置 |
|---|---|
| CPU1 | RAM、FLASH |
| CPU2 | RAM、FLASH |

不得用 `make`/`gmake` 代替 CCS 工程构建。生成代码中 SCIA CPUSEL/pinmux/配置
按 §1.4 对账后，才允许进入实物步骤。

#### 6.3.1 编译器选项：`--gen_func_subsections=on`

两核工程的 ticlang/cl2000 编译选项中必须开启 `--gen_func_subsections=on`
（CCS GUI：Project Properties → Build → C2000 Compiler → Advanced Options
→ Assembler Options 或 "Add new flag"；持久化在 `.cproject`）。
作用：让编译器把每个函数放进独立 `.text:funcname` subsection，使链接器能
按函数粒度 dead-strip。Phase 3.5 起 CPU2 的 `SCIA_BASE_init()` 直调依赖
此选项才能把同 obj 中数 KB 的 `SYSCTL_init` 死代码剥掉。关闭此选项的副
作用：CPU2 RAM 链接会立即失败（`error #10099-D: .bss won't fit`）。

变更编译器选项后必须 `Clean` 再 `buildProject`（subsection 命名变化会让
增量构建产生“僵尸 obj”）。

## 7. 调试会话与基线

主验收先用 RAM 双核会话，FLASH 做启动 smoke test。装载顺序沿用
[Phase 1 SysConfig](phase1-sysconfig.md) 与
[Phase 3 执行器](phase3-executor-observability.md)：

```text
Connect CPU1 -> Load/Resume
Connect CPU2 -> Load/Resume
```

连接 Scope2000 前先确认：

| 观测项 | 基线 |
|---|---|
| `g_v2k_tick` | 持续递增 |
| `g_v2k_isr_ovf_cnt` | 0 |
| `g_v2k_isr_budget_violation_cnt` | 0 |
| `g_handshake_state` | 3 |
| Phase 2 TZ | 仍保持硬件封锁/状态机语义 |
| CPU2 LED | 约 2 Hz 翻转 |

记录一组**尚未连接 Scope2000**时的
`g_v2k_isr_cycles_max/control_cycles_max/scope_cycles_max`，后续作为性能隔离
对照。

## 8. 验证 A — 串口与 HELLO

1. 在 macOS 确认 XDS110 VCP 已出现；Scope2000 点击 `Refresh Ports`。
2. 选择对应端口和 115200，点击 Connect。
3. CPU2 `rx_octets/good_frames/tx_octets` 应递增，`link_state` 变为 1。
4. Scope2000 控制台核对：

| 字段 | 预期 |
|---|---|
| firmware | Viewer2000 固件名 |
| wire | 1 |
| contract | 3 |
| build hash | 与 CPU1 描述符表一致 |
| descriptor count | 与 `entry_count` 一致 |
| tick_hz | 与 `V2K_ISR_HZ` 一致 |
| capabilities | 原生能力位完整 |

5. 拔掉 VCP 或停止请求超过约 2 s，`link_state` 应回 0；CPU1 tick 不受影响。
6. 恢复连接后重新 HELLO，不需要复位任一 CPU。

## 9. 验证 B — ENUM 与 build-hash 重枚举

1. 连接后由 Scope2000 每页请求 8 条描述符，直到 `count=0` 或达到 total。
2. 核对名称、type、kind、地址、min/max、scale/offset、prescaler/group。
3. Scope2000 枚举总数必须等于 HELLO 的 `desc_count`。
4. 选择若干参数与示波量，确认 UI 只允许最多 8 个 scope 通道。
5. 刷入 build hash 不同但 wire/contract 相同的固件。
6. STATUS 检出 hash 变化后必须：
   - 清空旧描述符和参数值；
   - 清空旧绑定序号；
   - 清空波形；
   - 自动重新 ENUM；
   - 控制台记录旧/新 hash。

旧地址或旧 `bind_seq` 的 block 不得继续进入绘图。

## 10. 验证 C — 参数事务

### 10.1 合法批次

1. 在变量面板选择两个以上 PARAM 描述符并填值。
2. Stage 发送一个或多个 CAL_WRITE；此时目标变量不得生效。
3. Commit 发送 CAL_COMMIT，记录返回的 `commit_seq=s`。
4. STATUS 轮询直到 `applied_seq==s`。
5. 核对 `cal_result=OK`、目标值同一控制拍生效。

### 10.2 拒绝与原子性

| 用例 | 预期 |
|---|---|
| 越界注册参数 | 整批拒绝，`fail_idx` 指向首个非法项 |
| 类型不符 | 整批拒绝 |
| 数量超过 16 | BAD_PARAM |
| 上一 commit 未完成又提交 | BUSY |
| 一批中一项合法、一项非法 | 合法项也不得写入 |
| 未注册但允许的 RAM 地址 | 可写，`cal_unguarded` 增加 |

同一地址分多帧 Stage 时，最后一次暂存值覆盖前值。超时重发相同 CAL_COMMIT
不得产生第二个 `commit_seq`。

## 11. 验证 D — 系统命令

依次执行：

```text
START -> STOP -> START -> 制造 TZ fault -> CLEAR_FAULT
```

每条命令分两阶段确认：

1. CMD ACK 的 `data` 返回已发布 `cmd_seq`；
2. STATUS 的 `cmd_ack_seq` 追上，并用 `cmd_result/sys_state/fault_code` 给出最终结果。

覆盖以下负例：

- 前一条命令尚未执行完成时发送下一条 → BUSY；
- FAULT 条件仍存在时 CLEAR_FAULT → 状态机拒绝或保持 FAULT；
- 相同 `(CMD, seq)` 重试 → 不得重复执行。

系统命令只改变应用状态，不得重置 `g_v2k_tick`、block 序号或 CPU1 心跳。

## 12. 验证 E — Live

先使用能落入 §5 带宽预算的配置：

1. `DAQ_CTRL(OFF)`，等 STATUS 确认该组为 OFF。
2. 选择 2–4 个通道，发送 DAQ_BIND，记录 `bind_seq`。
3. 设置合适 `prescaler` 和 `block_n_ticks`，发送 `DAQ_CTRL(LIVE)`。
4. Scope2000 持续 BLOCK_REQ，每次最多取 2 块。
5. 核对每个 block：

| 字段 | 通过条件 |
|---|---|
| group | 与请求组一致 |
| bind_seq | 与当前绑定一致 |
| block_seq | 正常连续，16-bit 回绕按无符号处理 |
| start_tick | 单调前进，来自 CPU1 |
| n_ticks | 允许小于配置 N 的 partial block |
| n_ch/stride | 与绑定的原生类型一致 |
| samples | 不在 codec/source 层统一转成 f64 |

6. Scope2000 用 `tick_hz` 和 prescaler 建立横轴，scale/offset 只在显示边界应用。
7. 导出 CSV，检查时间、通道列、断口和显示值与 GUI 一致。

### 12.1 断口与过载

1. 保持 CPU1/CPU2 运行，暂停 Scope2000 消费或故意提高数据率。
2. 等环满后恢复。
3. 预期：
   - `scope_prod.overrun_cnt` 增加；
   - block_seq 跳变；
   - Scope2000 插入 NaN 断口并记录 expected/received；
   - CPU1 `tick`、ISR overflow、预算违规和状态机不受影响。

LIVE 中重新 BIND 必须返回 BAD_STATE；先 OFF 才允许换绑定。

## 13. 验证 F — Snapshot 与 pre-trigger

1. `DAQ_CTRL(OFF)`，完成通道绑定。
2. 设置触发通道槽位、阈值、上升/下降沿、pre-trigger 百分比、prescaler 和 block N。
3. 发送 `DAQ_CTRL(SNAP_ARMED)`。
4. 制造确定的参数或状态跃迁。
5. STATUS 观察：

```text
SNAP_ARMED -> SNAP_TRIGGERED -> SNAP_FROZEN
```

6. 只有 FROZEN 后才开始 BLOCK_REQ 排空，直到 `remain_hint=0`。
7. 核对：

| 用例 | 通过条件 |
|---|---|
| 上升沿/下降沿 | `trig_tick` 落在跃迁附近 |
| pre-trigger 0/30/50/100% | 触发前后比例符合配置 |
| partial block | 末块保留真实 `n_ticks` |
| 排空顺序 | 从冻结窗口最旧块到最新块 |
| 重复 ARM | 新 `state_seq`，旧窗口不污染新窗口 |
| 非法 trigger slot/百分比 | BAD_PARAM，原状态不被破坏 |

Snapshot 可在 CPU1 全速采集后经 115200 慢速排空；串口速度不得反向限制
采样时基。

## 14. 验证 G — 帧错误、拆包和重试

用 host 测试工具或临时 transport 注入：

| 注入 | 固件/Scope2000 预期 |
|---|---|
| CRC 翻转 | `bad_frames++`，无响应，host 同 seq 重试 |
| COBS 非法 | 丢弃到下一定界符后恢复 |
| 超长无定界数据 | 进入 discard；下一 `0x00` 恢复 |
| 一帧拆成多次串口读取 | 正常拼接 |
| 多帧粘在一次读取 | 逐定界符解析 |
| 错 seq 响应 | Scope2000 丢弃并继续等正确响应 |
| 同 seq 重发 BLOCK_REQ | 返回同一批 block，不二次 release |
| 未知消息码 | ACK(UNSUPPORTED) |

错误注入期间检查 CPU1 的 ISR 计数和保护状态，不能出现相关变化。

## 15. 验证 H — 性能隔离与稳定波特率

### 15.1 CPU1 原生路径回归

分别记录：

1. Scope OFF、Scope2000 未连接；
2. Scope OFF、Scope2000 周期 STATUS；
3. Live 正常消费；
4. Live host 停止消费并发生 overrun；
5. Snapshot 全速采集和冻结排空。

每档记录：

```text
g_v2k_isr_cycles_max
g_v2k_control_cycles_max
g_v2k_scope_cycles_max
g_v2k_isr_ovf_cnt
g_v2k_isr_budget_violation_cnt
g_v2k_tick
```

通过条件：

- Scope OFF 时，是否连接 host 不得改变 CPU1 热路径；
- Live/Snapshot 增加的 CPU1 成本只能来自 Phase 3 已定义的 scope producer；
- CPU2 codec、串口、重试或断连不得增加 CPU1 block 编码、复制或轮询；
- host 停止消费时 CPU1 不等待，最多增加 producer overrun。

### 15.2 波特率阶梯

按以下候选逐档测试，不预设都能稳定：

```text
115200 -> 230400 -> 460800 -> 921600 -> 1500000
```

每档：

1. 固件与 Scope2000 使用相同 baud；
2. 先跑 HELLO/ENUM/CAL/CMD；
3. 再跑带宽预算内的 Live；
4. 连续至少 30 分钟；
5. 记录 good/bad frame、RX overflow、重试、block gap、producer overrun。

最终选择“持续运行无异常且有余量”的最高档，而不是短时间能连上的最高数字。
修改固件固定 baud 时必须回 SysConfig，不在 C 源码另写寄存器覆盖。

## 16. FLASH 与断连 smoke test

RAM 全量通过后验证 CPU1/CPU2 FLASH：

1. 断电重上电，不连接 CCS；
2. 确认双核启动、保护封锁、CPU2 LED 和 VCP 枚举；
3. Scope2000 完成 HELLO/ENUM；
4. 跑一次参数 Commit、START/STOP、短 Live 和 Snapshot；
5. 运行中拔插 USB/VCP；
6. 确认 CPU1 控制与保护状态不受影响，恢复后可以重新建立会话。

## 17. 验收与退出

| 验收项 | 通过条件 |
|---|---|
| SysConfig 职责 | CPU1 只生成 SCIA→CPU2 归属；CPU2 生成 SCIA/pinmux；业务 C 无静态覆盖 |
| 四配置构建 | CPU1/CPU2 的 RAM/FLASH 均由 CCS `buildProject` 成功 |
| 协议 conformance | Viewer vectors 与 Scope2000 tests 全通过 |
| HELLO/ENUM/STATUS | 版本、能力、tick、hash、枚举与重枚举正确 |
| CAL/CMD | 两阶段异步对账、拒绝语义和重试幂等正确 |
| Live | 原生 block、partial block、连续序号和断口正确 |
| Snapshot | 触发、pre-trigger、冻结顺序和慢速排空正确 |
| 错误恢复 | CRC/COBS/拆包/粘包/超时/seq 错配均可恢复 |
| 隔离性 | host/CPU2 异常不影响 CPU1 tick、ISR 预算和保护 |
| 波特率 | 找到并记录最高长期稳定档 |
| 品牌扫描 | Scope2000 public tracked 内容通过 `check-brand.py` |

写入 `BRINGUP.md`：

- 日期、板卡、CCS/C2000Ware 版本；
- Viewer2000 与 Scope2000 commit；
- CPU1/CPU2 RAM/FLASH build 结论；
- `V2K_ISR_HZ`、wire/contract、build hash；
- SysConfig 生成的 SCIA CPUSEL、GPIO42/43、baud 与 FIFO 配置；
- 每档 baud、持续时间和有效载荷配置；
- good/bad frame、RX overflow、host retry；
- 每组 block gap、producer overrun、Snapshot frozen count；
- 五种性能隔离场景的 CPU1 cycle/overflow/budget 数据；
- 参数、命令、Live、Snapshot、断连和 FLASH smoke test 结论。

只有本节全部通过并形成实测记录后，Phase 3.5 才算完成。
