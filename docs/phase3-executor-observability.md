# Phase 3 — 执行器与可观测性：操作与验证清单

> 本清单是**待执行的 bring-up 流程**，不是验收记录。实测值、波形、CCS Graph
> 截图按惯例落 [BRINGUP.md](../BRINGUP.md) Phase 3 区；全部跑通并记录后才打
> tag `phase3-executor-observability`。

Phase 2 证明了一条裸时基（ePWM→ADC→EOC ISR）和一条纯硬件保护链。Phase 3
在这条时基上**长出平台真正的本职**：

- **执行器**——一条固定顺序的控制 ISR（`acquire → 参数提交 → user_step →
  apply → scope 采样 → 触发判定 → tick++`）。用户只拥有 `user_step` 一个槽，
  其余每一步都归 L1。这是基本规则 7（可观测性 day 0）和规则 4（L3 只见物理量、
  不碰寄存器）在时间维度上的机制化：**没有不被采样的控制路径**。
- **可观测性**——Phase 0 定稿的四个共享接口里，本阶段落地三个的生产侧：
  描述符表（运行时枚举）、参数双缓冲（host→控制）、双模式 RAM 示波器
  （控制→host，Live + Snapshot）。命令/状态平面 Phase 2 已落地。

**本阶段的硬件验收对象是执行器本身，不是 FOC。** 验收只证明 ISR 在
IDLE/RUNNING/FAULT 三态下持续采样、调度、示波，以及参数/示波两个平面端到端
可用。真实电机应用（有状态的 PI/PLL/observer 启停语义）是 Phase 5 的事。

分工沿用 Phase 2 并外推：**SysConfig 管静态硬件**（Phase 3 只新增一个
CPUTIMER1 静态实例），**C 管运行时**（ISR 内容、多速率调度、参数提交、示波
状态机、CMPA、TBCLKSYNC 放行），**外加契约自检**——`v2k_tb_check` 上电把
安全/性能关键配置从寄存器读回对账，不一致就停 `ESTOP0`，绝不带着无效的性能
统计跑下去。

## 我已完成的部分（仅供对照）

| 产物 | 内容 |
|---|---|
| `cpu1/v2k_executor.c/.h` | 固定顺序控制 ISR；倒计时分频的多速率调度（1 kHz / 100 Hz / 参数提交三槽相位错开）；CPUTIMER1 自计时秒表分段量 control/scope/total 周期；ADC overflow 与 ISR 预算计数 |
| `cpu1/v2k_platform.h` | L1 暴露给 L3 的唯一逐拍接口：`user_step(const plat_in_t*, plat_out_t*)`，进 = 本拍物理量 + due 掩码，出 = 本拍占空比 |
| `cpu1/v2k_user.c` | 默认 L3 示例（**弱符号**，应用可强定义覆盖）：把 `pwm1_duty_cmd` 直通到输出，无内部状态 |
| `cpu1/v2k_registry.c/.h` | 描述符表注册；参数批次的后台机械校验 + ISR 安全点原子提交；10 Hz 值镜像刷新 |
| `cpu1/v2k_scope_runtime.c/.h` | Live/Snapshot 示波生产者；后台配置/绑定的序号握手与容量计算；冻结后 CCS view 解交错 |
| `common/v2k_scope_consumer.h` | CPU2/单元测试用的 SPSC 消费者 API（peek/release/begin_snapshot），内联只读 |
| `cpu1/cpu1.c` | 后台超级循环：按 `g_v2k_tick` deadline 服务四个共享平面，不阻塞等通信核 |
| `tools/gen_build_hash.py` | pre-build 从 git HEAD 生成 `v2k_build_hash.h`，写入描述符表头 |

## 关键决策（定稿）

- **ISR 固定序列归 L1，用户只有 `user_step` 一个槽。** 采集、参数原子提交、
  示波采样、触发判定都在 ISR 里由平台完成，用户代码无法绕过也无法关闭采样。
  默认 `user_step` 是弱符号占位（占空比直通），应用用强定义替换——
  L3 应用的 bring-up 是 Phase 5，本阶段用直通版验收平台骨架。
- **ISR 里只放"本拍必须做完"的事，其余全进后台超级循环。** 凡是要遍历表、
  复制配置、做容量计算、解交错、跨核传输的活，都在 `cpu1.c` 的 `for(;;)` 里
  按 `g_v2k_tick` deadline 触发（参数校验、示波配置/绑定、值镜像、CCS view
  生成、心跳、CPU2 健康检查）。这回答了路线图 Phase 3 留的那个问题——
  **平台杂务进后台、不开低优先级软中断**；用户若要做多速率控制，靠 ISR 传进来
  的 `due_mask` 自己在 `user_step` 内分。后台是无固定 `Delay` 的事件循环，
  没有新 `seq`/请求就立即返回，空闲控制核不会持续戳 GS4/MSGRAM。
- **多速率调度 = 倒计时分频 + 相位错开（stagger）。** 1 kHz、100 Hz、参数提交
  三个槽各自相位偏置，**永不落在同一拍**——慢环不挤在 `k%N==0` 同一个 tick，
  WCET 被摊平。错相关系由编译期 `V2K_STATIC_ASSERT` 钉死（参数相位
  `= 3/4 周期`、100 Hz 相位 `= 1/2 周期`，互不相等且在范围内），改频率时若
  撞相直接编译失败。
- **CPUTIMER1 当 ISR 的自计时秒表。** 32-bit free-run 倒计数、不开中断
  （Phase 3 新增的唯一 SysConfig 静态实例）。ISR 用它分三段量耗时：
  acquire→apply 的**控制段**、示波 epilogue 的**示波段**、整个 ISR 的**总时长**，
  各自记 max。它和 ePWM TBCTR 不是一回事——TBCTR 每周期回绕、只当中断延迟的
  代理量；CPUTIMER1 给的是绝对周期账。它的配置同样进 `v2k_tb_check` 读回兜底。
- **示波默认全 `OFF`，但开箱即有默认绑定。** 上电时 `v2k_default_bind` 按注册序
  自动绑描述符表里对应 group 的 `kind&SCOPE` 通道：group 0 = 8 个快通道，
  group 1 = 8 个 1 kHz 慢速健康/保护通道（含 `tz_trip_cnt`），group 2/3 容量
  为 0、永不进 ISR 热路径。**单组上限 `V2K_SCOPE_MAX_CH = 8`**——注册数超 8 会
  被静默截掉尾部，所以慢组恰好注册 8 个；要更多慢量靠 host 重绑或启用第二慢组。
  未采集时 ISR 示波路径只剩 group 0/1 的一个 active 判断。换通道必须先 `OFF`
  （一个环里不混两套通道布局）。
- **参数提交只做机械校验。** 每条写入按 `(addr,type)` 处理：type 必须合法，
  地址必须位于允许写入的 CPU1 数据区，32-bit 类型必须对齐；命中描述符表时
  还要求 `kind&PARAM` 且 type 一致。固件不做 `min/max` 范围检查、不 clamp、
  不做 `scale/offset` 反算。**批是原子的**：任一条机械非法则整批不写，
  `fail_idx` 指向首个非法条目。
- **跨核握手统一序号制，无双写者标志位。** GSx 硬件写保护让"对方清我的标志"
  不可能，于是一切请求/应答都是：请求方在自己属主区**最后写 `xxx_seq`**
  （发布动作），应答方写 `xxx_ack_seq` + 结果码回应。CCS 直接戳 shadow 区
  调参也遵守同一协议（先填字段，最后递增 seq）。

## 1. SysConfig 与构建前置

只能用 CCS Project/SysConfig 工具完成，禁止手改工程元数据或 `*.syscfg`：

1. CPU1/CPU2 工程器件统一为 `TMS320F28P650DK9`。
2. CPU1 新增 **CPUTIMER1** 实例（Phase 3 唯一新增静态硬件）：

   | 字段 | 值 |
   |---|---|
   | Period | `0xFFFFFFFF` |
   | Prescaler | 0 |
   | Emulation Mode | Run Free |
   | Start Timer | 启用 |
   | Interrupt | 禁用 |
   | Register Interrupt Handler | 禁用 |

3. CPU1 工程加 pre-build 步骤运行 `python3 ../tools/gen_build_hash.py`——
   它从 git HEAD 生成 `cpu1/v2k_build_hash.h`，`v2k_registry_init` 把这个 hash
   写进描述符表头。host 重连发现 hash 变了就强制重新枚举，杜绝拿旧表读新固件。

`cpu1/f28p65x_dbgier.asm` 取自 C2000Ware 26.01；CPU1 启动调
`SetDBGIER(INTERRUPT_CPU_INT1)`，把 ADCA1 所在的 PIE Group 1 标成
**time-critical**——后台被 CCS halt 时，控制 ISR 与 tick 照常执行（实时模式的
前提，见 Phase 2 FREE_RUN 决策第③层）。

`v2k_tb_check` 会读回 CPUTIMER1 的周期、分频、运行位、Run Free、中断关闭五项
状态。SysConfig 没按上表配，上电就停 `ESTOP0`，不会带着错的性能统计跑。

⚠️ 切换 20/100 kHz：和 Phase 2 一样**两边都改**——SysConfig 的 ePWM Period
（5000↔1000）与编译器 Predefined Symbol `V2K_ISR_HZ`（20000↔100000）。只改
一边会停在 `v2k_tb_check` 的 `ESTOP0`，这正是自检的用处。

## 2. 内存布局（这一拍的数据住在哪）

| 区域 | 地址 | 用途 |
|---|---|---|
| GS0 前半 | `0x10000..0x10FFF` | 描述符表 + 参数状态 + 4 个示波生产者控制块 |
| GS0 后半 | `0x11000..0x11FFF` | group 1 慢速环（`0x1000` words） |
| GS1–GS3 | `0x12000..0x17FFF` | group 0 快速环（`0x6000` words） |
| GS4 前 `0x200` words | `0x18000..` | 参数 shadow + 示波 cfg/bind/cons（**CPU2 属主**） |
| RAMD2 | `0x1A000..` | 冻结后的 CCS view：解交错出的连续 `float data[2048]` |

默认档位：group 0 = 8 通道、prescaler=1、`block_n_ticks=10`、mode=`OFF`；
group 1 = 1 kHz（prescaler=`V2K_ISR_HZ/1000`）、`block_n_ticks=10`、mode=`OFF`；
group 2/3 = `OFF` 且容量 0。

CPU1 后台是不含固定 `Delay` 的裸机事件循环：约 1 ms 一个 poll point 检查参数/
示波/命令/CCS view 的 `seq` 变化；心跳、10 Hz 镜像、CPU2 健康检查、LED 各按
`g_v2k_tick` 算 deadline。**tick 只提供时间**，周期任务仍在后台跑、可被下一次
控制 ISR 抢占；deadline 没到就不碰 GS4/MSGRAM。

CPU2 在 Phase 3 仍只跑约 1 ms 的**本地低速心跳**证明通信核自己活着——它不参与
控制 tick、采样时间戳或 block 时间（规则 5）。CPU1 每约 1 ms 发一记 ping，CPU2
见 IPC flag 即应答。Phase 3.5 接 SCI 后，CPU2 后台改由通信事件/timeout 驱动。

## 3. 执行器：ISR 固定序列

`v2k_executor_isr`（挂 ADCA1 EOC，进 ISR 时 ADC 结果已就绪）每拍按死序列走：

```
探针 GPIO2 ↑ + 读 CPUTIMER1（cycle_start）+ 读 TBCTR（延迟代理）
  → acquire(in)          读 ADC A0、填 plat_in（tick / adc / sys_state / fault_code）
  → schedule             倒计时分频出 due_mask 与 param_due
  → 若 param_due：apply_ready   在错相安全点一次性原子写入已批准的整批参数
  → user_step(in, out)   L3 唯一槽（默认弱符号 = 占空比直通）
  → apply(out.pwm1_duty) 钳到 [0.02, 0.98]，写 CMPA = PRD×(1−duty)
  ──────────────────────  control 段在此结束，记 g_v2k_control_cycles[_max]
  → scope_sample_all(tick)  遍历 active 组采样 + 触发判定 + 冻结
  ──────────────────────  scope 段在此结束，记 g_v2k_scope_cycles[_max]
  → g_v2k_tick++         全平台唯一控制/采样时间在此前进
  → 记延迟 min/max、查 ADC overflow、清中断、ACK
  ──────────────────────  ISR 总时长在此结束，记 g_v2k_isr_cycles[_max] + 预算违规
探针 GPIO2 ↓
```

参数提交安排在 `user_step` **之前**的安全点（契约 `v2k_param.h` 规定），保证一批
参数要么全生效于同一拍、要么全不生效。可观测量必须是可寻址的静态量（非栈上
局部），每个快通道约 5 周期/拍计入 ISR 预算。

**CPUTIMER1 是秒表，不是第二个时基（规则 5 澄清）。** 全平台唯一的控制/采样
时间仍只有 `g_v2k_tick`，只由上面这条 ePWM→ADC→EOC ISR 链产生。序列里多次出现
的 CPUTIMER1 **不产生时间、不触发任何东西、不发中断**——它被配成 32-bit free-run
倒计数（中断关闭），ISR 在进入、控制段末、示波段末、退出各**读**一次，相减得到
各段周期数（`control_cycles` / `scope_cycles` / `isr_cycles` 与预算违规）。它是
量具不是节拍器：节拍器是 ePWM，秒表是 CPUTIMER1。

- **为什么不用 ePWM 的 TBCTR 当秒表**：TBCTR 每个 PWM 周期就回绕（@20 kHz 满量程
  才 5000、还是上下三角不单调），量不了逼近甚至超过一拍的 ISR；CPUTIMER1 是干净的
  32-bit 单调计数，@200 MHz 约 21 s 才回绕，两次读数之差无歧义。
- **为什么必须 free-run + 关中断**：关中断 = 它不能调度任何东西，否则就成了第二
  时间源、破坏规则 5；`v2k_tb_check` 因此**读回 `TIE==0` 对账**，用自检硬性保证
  它只能是秒表。free-run = 调试器 halt 时它照走，跨 halt 的周期测量不失真（与
  Phase 2 FREE_RUN 同源）。

**Phase 3 的 L3 没有内部状态**（占空比命令直通），因此本阶段不验证 PI 积分、
PLL、observer、ramp 等用户状态的清理。但"`user_step` 在三态都执行"不能当成
最终契约写死：Phase 5 进真实电机前，L1/L3 接口必须补 RUNNING session 边界
（每次 `APP_START` 允许用户重做初始化、`CLEAR_FAULT` 只回 IDLE、下次 RUNNING
不复用旧控制器状态）。这是执行器 API 的设计债，记在这里，不属于 Phase 3 验收。

## 4. 会话与调试入口

主验收用 **RAM 双核会话**，FLASH 只做启动 smoke test。调试会话顺序同
phase1-sysconfig.md §4（Connect CPU1 → Load → Resume → Connect CPU2 → Load →
Resume）；窗口期 CPU1 `g_nmi_cnt` +1 是预期（CPU2WDRS，Phase 1 已实证）。

⚠️ CPU1 停在 `ESTOP0`：先看 PC——在 `v2k_timebase.c` = `v2k_tb_check`
（SysConfig 与 C 常量没对上）；在 `cpu1.c` = `v2k_assert_layout`（.cmd 与
`v2k_memmap.h` 失配）。

| 配置 | 验收范围 |
|---|---|
| RAM / 20 kHz | §5–§8 全量功能，作为 bring-up 基准 |
| RAM / 100 kHz | §5–§8 全量功能，重点看 ISR 预算与 overflow |
| FLASH / 20 kHz | 启动、双核握手、Phase 2 保护 smoke test |
| FLASH / 100 kHz | 启动、`v2k_tb_check`、tick 与 ISR 预算 smoke test |

CPU1 会话常驻 Expressions（开 Continuous Refresh）：

| 表达式 | 含义 |
|---|---|
| `g_v2k_tick` | 全平台唯一 ISR tick |
| `g_v2k_due_mask` | 本拍置位的 `PLAT_DUE_1KHZ` / `PLAT_DUE_100HZ` |
| `g_v2k_isr_cycles` / `_max` | CPUTIMER1 测得的 ISR 总周期 |
| `g_v2k_control_cycles` / `_max` | acquire→apply（含无状态 L3 示例）周期 |
| `g_v2k_scope_cycles` / `_max` | 仅示波 epilogue 周期 |
| `g_v2k_isr_budget_violation_cnt` | ISR 耗时达到控制周期预算的次数 |
| `g_v2k_isr_ovf_cnt` | ADC 中断 overflow（丢拍）计数 |
| `g_v2k_gs0.param_status` | 参数批次结果/失败下标/值镜像 |
| `g_v2k_gs0.scope_prod[0..3]` | 各组示波状态/容量/配置结果/overrun/冻结范围 |
| `g_v2k_ccs_view` | Snapshot 解交错后的连续 float 数据 |

### 4.1 器材：Phase 3 验收不需要逻辑分析仪

§5–§8 全程靠 **CCS**（JTAG 内存读写 + Expressions + Graph）+ 片上 **CPUTIMER1**
完成——ISR 预算、参数平面、示波两平面都不依赖任何外部计测器。ISR 预算之所以能
在片上读出，正是因为加了 CPUTIMER1 这把秒表（见 §3 决策）。唯一外部接点是
**GPIO2 的 ISR 探针**（ISR 入口 `↑`、出口 `↓`，`v2k_executor.c` 里逐拍翻转），
用途只是给 CPUTIMER1 的周期账做一次**可选交叉验证**：脉冲宽度 = ISR 时长、
无限余辉散布 = 抖动。这正是 Phase 2 §5 用过的那路探针（J8 排针 **80**），接
Phase 2 那台**示波器**即可——不接也能完成 §5 全部验收。**逻辑分析仪 Phase 3
不用**，留给 3.5/6 的 SCI / EtherCAT 总线时序。

### 4.2 CCS 操作惯例与 WATCH 变量（§5–§8 通用，只说一次）

**先认准"哪个会话看哪个根符号"**——GSx / 本地 RAM 的硬件归属决定了实体只存在
于属主核的 `.out`，跨核那侧没有同名符号（Expressions 打错会话只会
`identifier not found`）：

| 会话 | 根符号（Expressions 直接键入） | 内容 | 用途 |
|---|---|---|---|
| **CPU1** | `g_v2k_gs0` | `desc_table` / `param_status` / `scope_prod[]` | 只读对照（所有应答/结果码在此看） |
| **CPU1** | `g_v2k_ccs_view` | Snapshot 解交错缓冲；**请求也在 CPU1 会话发**（它在 CPU1 属主 RAMD2，不在 GS4） | 读 + 写请求 |
| **CPU1** | `g_v2k_tick` / `g_v2k_due_mask` / `g_v2k_isr_cycles`(`_max`) / `g_v2k_control_cycles`(`_max`) / `g_v2k_scope_cycles`(`_max`) / `g_v2k_isr_ovf_cnt` / `g_v2k_isr_budget_violation_cnt` / `g_v2k_scope_overrun_total` | 执行器标量（§4 表已列） | 只读 |
| **CPU2** | `g_v2k_gs4` | `param_shadow` / `scope_cfg[]` / `scope_bind[]` / `scope_cons[]` | 一切参数 / 示波**写入** |

CPU2 会话里**没有** `g_v2k_gs0` 实体（对侧只有只读指针），所以流程天生跨两个
会话：**发布在 CPU2 会话写 `g_v2k_gs4.*`，应答/结果码回到 CPU1 会话看
`g_v2k_gs0.*`**——两个会话的 Expressions 都开着对照。

**写共享平面 = 先填所有字段，最后把序号写成旧值 +1（发布），再轮询应答端追平 +
读结果码确认受理**（跨核握手序号制，见关键决策）。发布/应答序号配对：

| 动作 | 发布序号（CPU2 会话写，最后 +1） | 应答（CPU1 会话轮询其追平 + 读结果码） |
|---|---|---|
| 参数提交 | `g_v2k_gs4.param_shadow.commit_seq` | `g_v2k_gs0.param_status.applied_seq` + `.result` |
| 示波配置 | `g_v2k_gs4.scope_cfg[g].cfg_seq` | `g_v2k_gs0.scope_prod[g].cfg_ack_seq` + `.cfg_result` |
| 通道绑定 | `g_v2k_gs4.scope_bind[g].bind_seq` | `g_v2k_gs0.scope_prod[g].bind_ack_seq` + `.bind_result` |
| CCS view | `g_v2k_ccs_view.request_seq` | `g_v2k_ccs_view.done_seq` + `.result`（同在 CPU1 会话） |

`g` = 组号 0..3（默认 0=快组、1=慢组）。LIVE 排空还要看：生产者
`g_v2k_gs0.scope_prod[g].wr_idx`（CPU1 会话）、消费者
`g_v2k_gs4.scope_cons[g].rd_idx`（CPU2 会话）。

**填字段的完整路径**（粘进 Expressions）：

- **参数**（CPU2 会话）：`g_v2k_gs4.param_shadow.count`、`.writes[0].addr`、
  `.writes[0].value_bits`、`.writes[0].type`，最后 `.commit_seq`。
  `writes[].addr` 取自描述符表（CPU1 会话看 `g_v2k_gs0.desc_table`）或 CPU1 `.map`；
  `value_bits` 是 32-bit 位模式——F32 参数想直接键入物理值，右键该表达式 →
  Number Format → **Float** 再填 `0.5` 这类值（否则得填 IEEE-754 十六进制）。
- **绑定**（CPU2 会话）：`g_v2k_gs4.scope_bind[0].n_ch`、`.ch[0].addr`、
  `.ch[0].type`（每通道一组 addr / type），最后 `.bind_seq`。
- **配置**（CPU2 会话）：`g_v2k_gs4.scope_cfg[0].mode_req`（`0`=OFF / `1`=LIVE /
  `2`=SNAP_ARMED）、`.trig_ch_slot`、`.trig_level`、`.trig_edge`、`.pre_trig_pct`，
  最后 `.cfg_seq`。

**看波形 = snapshot 画图（§7），别看 Expressions 瞬时值。** 任何"随 tick 变化"的
量（`g_v2k_due_mask`、`pwm1_duty_cmd` vs `pwm1_duty`、触发前后波形）都靠 §7 的
snapshot → 冻结 → CCS view → Graph 才画得出；Expressions 只适合读标量状态 / 计数，
LIVE 环是给机器消费者的二进制流、不能直接画（§8）。

**CCS Graph 指向连续缓冲**（CPU1 会话）：Window → Show View → Graph → Single Time；
Start Address = `&g_v2k_ccs_view.data`、Acquisition Buffer Size = `g_v2k_ccs_view.count`、
DSP Data Type = **32-bit floating point**、Q value = 0；需要横轴物理时间再填
Sampling Rate Hz = 等效采样率。

## 5. 验证 A — 调度与 ISR 预算

20 kHz 与 100 kHz 各跑一遍，步骤相同：

1. 按 §1 选 RAM / 20 kHz 构建装载，§4 顺序 Resume 双核；Expressions 加 §4 整组
   `g_v2k_*` 并开 Continuous Refresh。
2. **基线**：确认 `g_v2k_tick` 持续递增、`g_v2k_isr_ovf_cnt == 0`、
   `g_v2k_isr_budget_violation_cnt == 0`（无 halt 窗口内）。
3. **due 位别**（用 snapshot 看波形，不是 LIVE）：`g_v2k_due_mask` 的 Expressions
   瞬时值刷新太慢、抓不全每拍。按 §7 的 A→D 跑一次，触发源与查看通道都设成
   `due_mask`（group 0 槽位 **6**）：`g_v2k_gs4.scope_cfg[0].trig_ch_slot = 6`、
   `.trig_level = 0.5`、`.trig_edge = 0`、`.pre_trig_pct = 50`，冻结后
   `g_v2k_ccs_view.channel_slot = 6` 画图。曲线是每拍的 due 位模式
   （`1`=1 kHz、`2`=100 Hz、`3`=两者同拍）：数相邻非零样本的 tick 间隔按下方 due
   间隔表核对，并确认**全程无值 3 的样本**（两个 due 永不同拍）。若环深不足以显出
   100 Hz 的 200-tick 间隔，把 `g_v2k_gs4.scope_cfg[0].block_n_ticks` 调大再触发。
4. **参数提交槽**：跑一次 §6 的合法写，确认提交槽也与两个 due 错开、发布到生效
   端到端 < 2 ms。
5. **ISR 预算**：连跑一段（中途按 §7/§8 开关一次 scope），读
   `g_v2k_isr_cycles_max` / `control_cycles_max` / `scope_cycles_max` 对照下方
   通过标准；scope 开 / 关前后 `scope_cycles_max` 的差值应符合通道数、关掉后不再增长。
6. **（可选示波交叉）**：示波器接 GPIO2 探针（J8 pin 80），触发 CH1 PWM 上升沿、
   无限余辉——脉冲宽度应 ≈ `isr_cycles_max × 5 ns`、散布 ≈ 软件视角抖动，与
   CPUTIMER1 数字互证。跳过此步不影响验收。
7. 按 §1 切 RAM / 100 kHz（**两边都改**），重复 2–6，重点看预算 / overflow 恒 0、
   `control_cycles_max` 仍留稳定余量。
8. **跨状态连续性**（单独一次）：保持 group 0 LIVE，按 Phase 2 的
   START/STOP/TZ/CLEAR_FAULT 流程切 IDLE/RUNNING/FAULT。状态通道应反映跃迁，但
   `g_v2k_tick`、block `start_tick`、示波生产者**不得因软件状态切换而重置或停走**；
   只有消费者不及时导致的显式 overrun 才允许出现序号断口。

**通过标准**：

| 观测项 | 通过标准 |
|---|---|
| `PLAT_DUE_1KHZ` | 每 `V2K_ISR_HZ/1000` tick 出现一次 |
| `PLAT_DUE_100HZ` | 每 `V2K_ISR_HZ/100` tick 出现一次 |
| due 错相 | 两个 due 不在同一 tick 置位 |
| 参数提交槽 | 与两个 due 都错开；发布到生效端到端 < 2 ms |
| `g_v2k_isr_cycles_max` | `< 200 MHz / V2K_ISR_HZ`（即 ISR 预算） |
| `g_v2k_control_cycles_max` | 小于 ISR 预算，100 kHz 下留稳定余量 |
| `g_v2k_scope_cycles_max` | 开/关 scope 前后差值符合通道数预期，无持续增长 |
| `g_v2k_isr_budget_violation_cnt` | 无 halt 窗口内恒 0 |
| `g_v2k_isr_ovf_cnt` | 恒 0 |

步骤 3 的 due 间隔（snapshot 画出 due_mask 后数相邻非零样本的 tick 间隔）：

| ISR 频率 | 1 kHz due 间隔 | 100 Hz due 间隔 |
|---|---:|---:|
| 20 kHz | 20 tick | 200 tick |
| 100 kHz | 100 tick | 1000 tick |

## 6. 验证 B — 参数双缓冲

逐用例操作（写法见 §4.2，全部在 **CPU2 会话**）：

1. Expressions 展开 `g_v2k_gs4.param_shadow`。
2. 按用例填 `count` 与 `writes[]`（每条 addr / type / value），**最后**把
   `commit_seq` 写成旧值 +1。
3. 等 `g_v2k_gs0.param_status.applied_seq == commit_seq`：CPU1 后台在下一个约
   1 ms poll point 稳定复制并机械校验整批，ISR 在错相参数槽一次性原子写入全部已批准
   条目（校验到生效 < 1 ms，发布到生效端到端 < 2 ms）。
4. 读 `result / fail_idx / value_mirror[]` 对照下表。

| 用例 | 操作 | 预期 |
|---|---|---|
| 合法写 | `pwm1_duty_cmd`，type=`F32`，任意 F32 位模式 | `V2K_CAL_OK`，整批同拍生效 |
| 错类型 | 对 `pwm1_duty_cmd` 用非 F32 type | `V2K_CAL_BAD_TYPE`，整批不写 |
| 错数量 | `count > V2K_PARAM_BATCH_MAX` | `V2K_CAL_BAD_COUNT`，整批不写 |
| 错地址 | Flash/代码/外设/示波环/未对齐 32-bit 地址 | `V2K_CAL_BAD_ADDR`，整批不写 |
| 未注册但允许写地址 | CPU1 `.bss/.data/.bss:output` 中未注册的测试变量 | 写入成功 |
| 批原子性 | 一批里先合法项、后机械非法项 | 整批拒绝、合法项也不变，`fail_idx` 指向首个非法项 |

5. **命令 vs 应用同线**：合法写入后按 §7 做一次 snapshot（二者在 group 0 槽位
   2 / 3，同一冻结窗口分别取 `channel_slot = 2`、`= 3` 画图），两条曲线 `start_tick`
   相同 → 命令值与应用值在同一控制时间线上。
6. **三态照跑**：IDLE / FAULT 下重复一条合法写，确认参数提交与 10 Hz 镜像照常
   运行，但输出仍由 TZ 封锁。状态切换走 Phase 2 命令通道（CPU2 会话写
   `g_v2k_msg_2to1.cmd_req`：填 `cmd_code` 再递增 `cmd_seq`，`1=APP_START`、
   `2=APP_STOP`、`3=CLEAR_FAULT`）。**无外部跳线时软触发 TZ 进 FAULT**：RUNNING 下
   往 EPWM1 `TZFRC` 直接写 OST 位 `*(uint16_t*)(EPWM1_BASE+0x9B) = 0x0004`
   （F28P65x：`EPWM1_BASE=0x3000`，写地址 `0x309B`；CCS Memory Browser / debug MCP
   `writeMemory` 都行）——它走 EPWM 级 TZ 中断路径，等同硬件 trip，但 `CLEAR_FAULT`
   仍要外部跳线引脚拉高才放行（`v2k_fault.c` 读 `V2K_FAULT_TZ_GPIO`），跳线本身未动
   → 直接 CLEAR_FAULT 即可回 IDLE。

## 7. 验证 C — Snapshot + CCS Graph

**在 CCS 里能画成波形的只有 snapshot。** 冻结后 CPU1 后台
`v2k_scope_ccs_view_service` 把环里**交错的多通道 block 解交错**成单通道连续
`float` 数组 `g_v2k_ccs_view.data[]`，Graph 直接读它。**LIVE 不产生可绘图数组**
（环里是原生宽度交错的二进制块，解交错器只认 FROZEN，见 §8）——想"看波形"就走
本节。

group 0 上电默认已绑好 8 个快通道，**无需重新 BIND**；通道槽位 = 描述符表登记顺序：

| group 0 槽位 | 通道 | 类型 |
|---:|---|---|
| 0 | `adc_a0_raw` | U16 |
| 1 | `adc_a0_v` | F32 |
| **2** | **`pwm1_duty_cmd`** | F32 |
| **3** | **`pwm1_duty`** | F32 |
| 4 | `isr_cycles` | U32 |
| 5 | `isr_latency` | U16 |
| **6** | **`due_mask`** | U16 |
| 7 | `sys_state` | U16 |

下面**从头到尾跑通一次**，以触发源 = `pwm1_duty_cmd`（槽位 2）为例：

**A. 武装 snapshot**（CPU2 会话，写 `g_v2k_gs4.scope_cfg[0]`）：

1. `.mode_req = 2`（SNAP_ARMED）、`.trig_ch_slot = 2`、`.trig_edge = 0`（RISE）、
   `.trig_level = 0.5`、`.pre_trig_pct = 50`；
2. **最后** `.cfg_seq = 旧值 + 1`；
3. 回 **CPU1 会话**确认 `g_v2k_gs0.scope_prod[0].cfg_ack_seq` 追平、
   `.cfg_result == 0`、`.mode == 2`。

**B. 制造触发跃迁**（让 `pwm1_duty_cmd` 从 <0.5 升到 ≥0.5），两种都行：

- 简单法（CPU1 会话）：直接在 Expressions 把 `pwm1_duty_cmd` 改成 `0.6`（直写
  CPU1 变量即触发，绕过参数平面）；
- 走参数平面（CPU2 会话，顺带验 §6）：按 §6 提交 `pwm1_duty_cmd = 0.6`。

  观察 CPU1 会话 `g_v2k_gs0.scope_prod[0]`：`.mode` 走 `2(ARMED) → 3(TRIG) →
  4(FROZEN)`、`.state_seq` 递增、`.trig_tick` 落在跃迁附近、`.frozen_count > 0`。

**C. 解交错出 CCS view**（CPU1 会话——`g_v2k_ccs_view` 在 CPU1 属主 RAMD2，不在
GS4）。想看哪个通道就填它的槽位，例如看触发的 `pwm1_duty_cmd`：

1. `g_v2k_ccs_view.group = 0`、`.channel_slot = 2`；
2. **最后** `.request_seq = 旧值 + 1`；
3. 等 `.done_seq == request_seq`，确认 `.result == 0`（OK）、`.count > 0`、
   `.start_tick` = 冻结窗口首块 tick。

**D. 画出来**（CPU1 会话）：Window → Show View → Graph → Single Time；
Start Address = `&g_v2k_ccs_view.data`、Acquisition Buffer Size =
`g_v2k_ccs_view.count`、DSP Data Type = **32-bit floating point**、Q value = 0。
曲线即该通道随 tick 的波形（每样本 = 一个组 tick；prescaler=1 时 = 一个 ISR tick）。
**换通道不必重新触发**：同一冻结窗口改 `.channel_slot`（如 `3` 看 `pwm1_duty`）→
再递增 `.request_seq` → 重画即可。

每个用例都重走 A→D（**不在非 OFF 状态换绑**；要换通道布局先提交一次
`.mode_req = 0`(OFF)）：

| 用例 | 操作 | 预期 |
|---|---|---|
| 上升沿 | 触发源 `pwm1_duty_cmd`、`trig_level=0.5`、`trig_edge=0`，B 步升到 0.6 | `mode` 走 `2→3→4`，`trig_tick` 落在跃迁附近 |
| 下降沿 | `trig_edge=1`(FALL)，B 步反向把 `pwm1_duty_cmd` 降到 0.4 | 状态机同上，命中下降沿 |
| pre-trigger | `pre_trig_pct` 分别测 0 / 30 / 50 / 100% | 触发前后样本比例符合配置；100% 时仍至少留 1 个 post 样本 |
| 部分末块 | 在非 block 边界触发冻结 | 末块 `hdr.n_ticks` 可小于 `block_n_ticks` |
| 重复 ARM | FROZEN 后 OFF → ARM → 再触发 | `state_seq` 增加，旧冻结范围不污染新 snapshot |
| 非法配置 | 非法 `trig_ch_slot` / `pre_trig_pct>100` / `trig_edge` | `cfg_result = BAD_PARAM`（非 0），原运行态不被破坏 |

冻结块按时间顺序从 `frozen_end_idx − frozen_count` 起，触发样本属于 post 段。

## 8. 验证 D — Live + CPU2 consumer

LIVE 环满时丢弃新 block、`overrun_cnt` 加，ISR 绝不等消费者（基本规则 1）。
生产侧配置写法见 §4.2，全部从 **CPU2 会话**戳。

⚠️ **LIVE 不能画波形**：环里是原生宽度交错的二进制 block，CCS view 解交错器只认
FROZEN 快照（`v2k_scope_ccs_view_service`），把 LIVE 喂给 Graph 是乱码。要看波形
走 §7 的 snapshot。本节只验**块头字段 + SPSC 索引语义**，用 Expressions / Memory
Browser 看，不走 Graph：**CPU1 会话**加表达式 `*(v2k_block_hdr_t *)g_v2k_scope_fast`
（group 1 用 `g_v2k_scope_slow`）展开即是 block 0 的头，LIVE 写满回绕时其字段随之
刷新。

1. 对 group 0 发 `OFF`（`scope_cfg[0].mode_req = 0` + 递增 `cfg_seq`），确认
   `cfg_ack_seq` 前进、`cfg_result == OK`；
2. `OFF` 下发合法绑定（`scope_bind[0]` + 递增 `bind_seq`），确认 `bind_ack_seq`
   前进、`bind_result == OK`，`scope_prod[0]` 的 `n_ch / block_slot_words /
   ring_capacity` 与绑定匹配；
3. 发 `LIVE`（`scope_cfg[0].mode_req = 1` + 递增 `cfg_seq`），确认 `mode == 1`、
   `state_seq` 增加；
4. 默认 group 0 每 10 tick 发一个 block：看 `g_v2k_gs0.scope_prod[0].wr_idx` 每
   10 tick +1；用上面的 `*(v2k_block_hdr_t *)g_v2k_scope_fast` 检查块头
   `start_tick` / `block_seq` / `group_id` / `n_ticks` / `n_ch` / `bind_seq` /
   `stride_octets`；
5. 暂停消费者直到环满：`overrun_cnt` 与 `g_v2k_scope_overrun_total` 增加，但
   ISR overflow / 预算违规**不增加**；
6. LIVE 中发新绑定必须返回 `V2K_SCOPE_RESULT_BAD_STATE`；
7. 对 group 1 重复 LIVE，block `start_tick` 间隔应为 `10 × V2K_ISR_HZ/1000` tick，
   且不扰动 group 0。

Phase 3 不验 SCI/EtherCAT 吞吐，只验**公共 consumer API 的 SPSC 索引语义**。用
CPU2 侧最小诊断函数或单元测试调 `v2k_scope_consumer_peek/release/begin_snapshot`：

| 场景 | 通过标准 |
|---|---|
| LIVE 空环 | `peek` 返回无数据，`rd_idx` 不变 |
| LIVE 有块 | `peek` 返回的 header 与 CPU1 环内数据一致 |
| release | 每次只让 `rd_idx + 1`，不动 CPU1 的 `wr_idx` |
| 连续读取 | 正常时 `block_seq` 连续；overrun 后凭 seq 跳变发现断口 |
| SNAPSHOT frozen | 从 `frozen_end_idx − frozen_count` 起，正好读完 `frozen_count` 个 block |
| SNAPSHOT partial | 保留末块真实 `hdr.n_ticks`，消费者不假设固定 N |

Phase 3 不提供单核编译路径：CPU1 始终划转 GS4 并引导 CPU2，CPU2 不得写 CPU1
的 producer 字段。

## 9. 验收与退出

Phase 2 已验过的 ePWM/ADC/TZ 与 START/STOP/FAULT/CLEAR_FAULT，按
[Phase 2 bring-up](phase2-bringup.md) 回归，不在此复制。

| 验收项 | 操作来源 | 通过条件 |
|---|---|---|
| CPU1/CPU2 RAM 与 FLASH 构建 | §1 | 四个配置均由 CCS `buildProject` 成功 |
| SysConfig 与 linker 对账 | §1、§2 | `v2k_tb_check` 与布局断言不触发 `ESTOP0` |
| 双核、时基、TZ、命令状态机 | Phase 2 | 原 Phase 2 验收全部回归通过 |
| 调度、状态连续性、ISR 预算 | §5 | 20/100 kHz 的 due、cycle、overflow、跨状态 scope 全通过 |
| 参数双缓冲 | §6 | 合法、机械非法拒绝、批原子性全通过 |
| Snapshot 与 CCS Graph | §7 | 两种边沿、四档 pre-trigger、partial block、重复 ARM 全通过 |
| Live 与 CPU2 consumer | §8 | block 头、overrun、换绑拒绝、SPSC 索引语义全通过 |
| 实时模式（halt 行为） | Phase 2 + §5 | halt 时 TZ6 保持安全，time-critical ISR/tick 继续，overflow 不增长 |

记录进 BRINGUP.md Phase 3 区：

- 日期、板卡、CCS 版本、RAM/FLASH、`V2K_ISR_HZ`、固件 build hash；
- CPU1/CPU2 build 结论；
- 20 kHz 与 100 kHz 各一组 `g_v2k_isr_cycles_max` / `g_v2k_control_cycles_max` /
  `g_v2k_scope_cycles_max` 与 overflow/预算计数；
- 参数平面用例结果表；
- Live/Snapshot 的关键 producer 字段 + CCS Graph 截图或文字记录；
- START/STOP/FAULT/CLEAR_FAULT 与实时模式回归结果。

以上全部完成并落地后，才打 tag `phase3-executor-observability`。
