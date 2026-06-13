# Phase 2 — 时基证明 + 保护：操作与验证清单

> ✅ **2026-06-13 验收通过**（LAUNCHXL-F28P65X 实物）。验证 A/B/C/D 全部实测，
> 期间修掉两个根因——SOC 源 codegen bug（tick 卡 0）与保护门控两 bug（开机带电
> 自由跑 / 伪 trip 卡 RUNNING）。**实测值与调试历史见 [BRINGUP.md](../BRINGUP.md)
> Phase 2 记录区**；本清单保留作操作参照。

分工沿用既定原则并细化：**SysConfig 管静态硬件**（引脚、波形、死区、SOC、
TZ 保护链——引擎校验冲突与 errata），**C 管运行时**（ISR 内容与注册、状态机、
占空比、TBCLKSYNC 放行时序）**外加契约自检**——`v2k_tb_check` 上电把安全
关键配置从寄存器读回与 `V2K_ISR_HZ` 对账，不一致停 ESTOP0（与
`v2k_assert_layout` 同模式）。**syscfg 配置和 C 常量两边都改才跑得起来。**

我已完成的部分（仅供对照）：

| 产物 | 内容 |
|---|---|
| `cpu1/v2k_timebase.c/.h` | 控制 ISR（探针 GPIO2、tick、延迟 min/max）、契约自检、EPWMCLKDIV=/1（errata）、占空比写入、TBCLKSYNC 放行 |
| `cpu1/v2k_fault.c/.h` | 故障锁存状态机 IDLE/RUNNING/FAULT、命令受理 START/STOP/CLEAR_FAULT、TZ 中断、Board_init 前的抢先 OST 封锁（arm） |
| `cpu1/cpu1.c` | 保护先行时序：关 TBCLKSYNC + arm → Board_init → 自检/注册 → 放行；慢环跑 v2k_fault_poll |
| `contracts/v2k_command.h` | 故障码 V2K_FAULT_NONE / V2K_FAULT_TZ1_EXT（仅 #define，布局与 CONTRACT_VER 不动） |

关键决策（定稿）：

- **EPWMCLKDIV = /1（EPWMCLK = 200 MHz）**：F28P65x errata——/2 时 TZFRC/TZCLR
  偶发丢失，本平台封锁/放行全靠这两个寄存器（syscfg 校验提示的就是这条）。
  配置来源 = syscfg 时钟树（**Device Support 模块**生成的 Device_init 取代
  手写模板 device.c，时钟单一来源、引擎校验生效，见 §1.0）；C 侧只读回
  断言（v2k_tb_check 第一项），检查 syscfg 的工作而非自己的回声。
- **周期是双源头——已知缺陷，对账兜底，不是优点**：同一物理量（PWM 周期）
  存在于 syscfg 的 Period 字段与 C 侧 `V2K_ISR_HZ` 推导值两处。这是两条
  单一来源路线都走不通后的被迫取舍：(a) 全进 syscfg 不可能——C 侧静态断言、
  ISR 频率相关逻辑、将来示波器降采样比换算都必须持有这个数；(b) 全在 C
  已被否决——丢掉引擎校验与 errata 提示，代价已实证更大（EPWMCLKDIV 那条
  errata 就是引擎抓的）。`v2k_tb_check` 的作用不是让双源头变好，而是把它
  最危险的故障模式——**两边不一致还静默运行**——转成上电即停的显性错误；
  维护税是真实的：改频率必须动两处（见 §2 第 3 条）。收敛方向（Phase 3
  描述符表落地、频率档位成为平台参数后）：由构建脚本从单一定义生成两边——
  syscfg 文件禁的是 Write/Edit 手改，工具链脚本化生成是另一回事。Phase 2
  只有一个参数两个档位，对账顶着，成本可控。
- **FREE_SOFT = FREE_RUN + TZ6 CBC**。FREE_RUN 的三层语义，受益者各不同。
  先把"相位"说清楚：**不是电机的相位**，指的是 **TBCTR 在自己载波周期里的
  位置**——0→PRD→0 的三角波数到哪儿了——及其派生的全部定时关系（SOC 触发点
  CTR=ZERO、死区边沿、将来三相逆变时同步组里各 ePWM 模块间的相对对齐），
  与电机转子电角度无关：
  ① **载波定时连续**——STOP 模式的具体毛病：halt 时计数器冻在任意位置
  （比如冻在高电平段中间），输出就钉在那个电平直到 resume；resume 后计数器
  从冻结点继续，第一个周期是个"残缺拍"——SOC 时刻、占空比、死区都从任意
  中间状态恢复，示波器上 halt 前后波形畸形一拍；带功率级且没有 TZ6 时，
  "钉在高电平"就是往绕组灌直流的危险场景。FREE_RUN 下计数器照走，ePWM→ADC
  硬件链在 halt 期间一致推进（SOC 仍在 CTR=ZERO 准时发），resume 的第一拍
  就是完整正常的拍，无恢复瞬态。诚实地说，这层在 Phase 2 当下分量不大；
  ② **硬件可观测**——SOC 是 ePWM→ADC 硬件触发不经 CPU，halt 期间转换照常、
  结果寄存器照更新，halt 下用 CCS 看寄存器是活的。注意：普通 halt 时 ISR
  不执行、tick 不走，**没有**软件意义上的数据流，电流环当然也不算；
  ③ **真正分量重的理由**——给 CCS 实时模式（平台既定主交互方式，Phase 3 后
  启用）铺路：ISR 标记 time-critical 后，后台代码 halt 时控制 ISR 照常执行，
  那个场景下数据流才真的不断，而它成立的前提就是 TBCTR 不能停。FREE_RUN
  本质是为该模式预设的，"resume 无瞬态"是顺带的当下收益。
  halt 时的**输出安全**从来不归 FREE_SOFT 管，归 TZ6 CBC（resume 自动恢复）。
  **电机相位是另一回事，任何 FREE_SOFT 配置都管不了**：转子在 halt 期间
  照常转，普通 halt 一秒钟，控制环恢复时和转子位置早就脱节了，该有的控制
  瞬态一点不少——这正是带电调试的既定方式是实时模式（环不停、只 halt 后台），
  而不是指望某个寄存器配置让普通断点对运行中的电机变得无害。
- **IDLE/FAULT 都是 OST 锁存封锁**，唯一放行路径 = APP_START 清锁存，
  全程无"输出短暂放开"窗口；trip 源未消失立即重入 FAULT。
- **TZ 动作 = A/B 强制拉低**：对齐目标功率级 DRV8323R（INHx=INLx=0 → 全关 →
  惰转）；nFAULT 将来多挂一路 TZ 源即可。CMPSS 模拟源推迟到电流采样引脚定型。

---

## 1. SysConfig 配置清单（cpu1 工程的 sysconfig_cpu1.syscfg）

现有 LED_CPU1 / LED_CPU2 实例**不动**。逐模块添加：

**1.0 Device Support + 时钟树（两个工程都做）**——让 syscfg 生成的
device.c/device.h **取代**手写模板（替换而非并存；模板可读性差、无校验，
时钟从此单一来源、引擎校验生效）：

1. cpu1 与 cpu2 的 syscfg 各自添加 **Device Support** 模块（选项保持默认）。
   cpu2 侧会出黄色警告 *"You will not be able to use clocking functions,
   unless both CPU1 and CPU2 are open in SysConfig"*——**预期且无害**
   （logWarning 不阻断生成）：单开 CPU2 上下文时时钟功能不可用，而 CPU2
   本来就被规则 5 禁止碰时钟，生成的 cpu2 device.c 自动不含时钟代码，
   这条警告等于工具在替架构守规矩。想让它消失的正路是 CCS 系统工程双
   上下文同开 syscfg（仓库 `dual_sysconfig_multi/` 是 Phase 1 例程遗留的
   系统工程，project 名还指向 TI 例程，改成我们的工程名即可用——可选
   收尾活，不挡 Phase 2）；
2. cpu1 的 Clock Tree：确认 **SYSCLK = 200 MHz**（25 MHz XTAL → PLL，应为
   默认），**EPWMCLKDIV 改 /1**——那条 TZFRC/TZCLR errata 警告此时应消失，
   EPWM 模块显示的频率/死区时间也恢复正确（Period=5000 → 20 kHz）。
   cpu2 不动时钟（生成代码自带 CPU2 守护，不会重配 PLL）；
3. **排除模板文件**（CCS 工程操作）：两个工程的 `device/device.c` 与
   `device/device.h` 从构建中排除或删除；`device/driverlib.h` 与
   `device/driverlib/` **保留**。若链接报 `code_start` 重复定义，把
   `device/f28p65x_codestartbranch.asm` 也排除（生成侧已含 code start）；
4. 此迁移动了两核启动路径——上板时先跑一遍 **Phase 1 回归**（双灯/握手/
   ping-pong/心跳）再做 Phase 2 验证，结果记 BRINGUP。

SYSCTL 外设时钟里 **TBCLKSYNC 保持默认不勾**（放行时序归 C，保护先行；
C 侧无论生成代码开没开都会先显式关掉再 arm，顺序不依赖生成细节）。

**1.1 EPWM 实例**（建议命名 `PWM_TB`，名字不影响 C 代码——C 用 EPWM1_BASE）：

| 子模块 | 配置 |
|---|---|
| PinMux | 外设 EPWM1，A→**GPIO0**，B→**GPIO1** |
| Time Base | Period = **5000**（=20 kHz @200 MHz up-down）；Counter Mode = **Up-Down**；Clock Divider 与 HS Clock Divider 都 **/1**；Phase 关闭；**Emulation Mode = Free Run** |
| Counter Compare | 不用配（CMPA 是运行时量，C 写 3750 = 25% 占空比） |
| Action Qualifier | Output A：CTR=CMPA（递增）→ **HIGH**；CTR=CMPA（递减）→ **LOW**；其余事件不动作 |
| Dead-Band | RED 与 FED 都启用；输入都选 **ePWMA**；FED 极性反相（Active-High Complementary）；RED = FED = **200**（=1 µs @200 MHz） |
| Event-Trigger | 使能 **SOCA**；触发源 = **CTR=ZERO**；event prescale = 1 |
| Trip Zone | One-Shot 源勾 **TZ1**；CBC 源勾 **TZ6**（emulator stop）；TZA action = **force low**；TZB action = **force low**；**TZ 中断不要在这里使能**（C 侧统一注册+使能，避免双源） |

**1.2 ADC 实例**（ADCA）：

- 时钟 prescale **/4**（ADCCLK 50 MHz）；Reference = **External**（板载 REF6230
  3.0 V，需 J15 短接）；
- SOC0：触发 = **EPWM1 SOCA**；通道 = **A0**；采样窗 ≥ **100 ns**（≈20 SYSCLK）；
- INT1：使能，源 = **EOC0/SOC0**；**不要勾 register interrupt handler**（C 注册）。

**1.3 DAC 实例**（DACA）：Reference = **ADC VREFHI**；Enable Output；
初值（shadow value）= **2048**。（与 ADCINA0 共脚 = BP1 排针 30 会带 ~1.5 V
直流，该排针留空。）

**1.4 INPUT X-BAR**：INPUT1 = **GPIO3**。

**1.5 GPIO 两个实例**：

| 实例名建议 | 引脚 | 配置 |
|---|---|---|
| `ISR_PROBE` | GPIO2 | 输出，推挽，初值 0 |
| `TZ_EXT` | GPIO3 | 输入，**Pull-Up**，Qualification = **Async** |

保存生成后把 diff 给我 review（或直接 commit，我看生成的 board.c/board.h）。

## 2. 构建

1. 两工程仍用 **RAM** 配置；`v2k_timebase.c` / `v2k_fault.c` 在 cpu1 工程目录内
   自动入编译。先 build cpu1 再 cpu2，预期 0 error；
2. IDE 编辑器里的红波浪线（clangd 解析 TI 头文件失败）是宿主端噪声，
   **以 cl2000 构建结果为准**；
3. 100 kHz 压测时：syscfg 的 Period 改 **1000** + 编译器 Predefined Symbols 加
   `V2K_ISR_HZ=100000`，**两边都改**——只改一边会停在 v2k_tb_check 的
   ESTOP0（这正是自检的作用）。测完都改回去。

## 3. 调试会话

顺序与 phase1-sysconfig.md §4 完全相同（Connect CPU1 → Load → Resume →
Connect CPU2 → Load → Resume）。窗口期 CPU1 的 `g_nmi_cnt` 照例 +1
（CPU2WDRS，已实测确证），不是异常。

⚠️ 若 CPU1 停在 ESTOP0：先查是不是 `v2k_tb_check`（PC 在 v2k_timebase.c）——
syscfg 配置与 C 常量没对上（EPWMCLKDIV、Period、TZ 源、TZ 动作、Emulation
Mode 六项），对照 §1.0/§1.1 改齐重来。

## 4. 验证 A — 时基与 ISR（CPU1 会话 Expressions，开 Continuous Refresh）

| 表达式 | 预期 |
|---|---|
| `g_v2k_tick` | 持续递增，速率 ≈ 20000/s（掐表两次读数差验证） |
| `g_v2k_adc_a0` | ≈ 2048 ± 噪声（DACA 中位）。接近 0 或乱漂 → 查 J15 与 §1.3 |
| `g_v2k_isr_lat` / `_min` / `_max` | 典型一两百（单位 5 ns，含 ADC 转换常数偏置）；**max−min = 软件视角抖动**，绝对延迟看示波器 |
| `g_v2k_isr_ovf_cnt` | 恒 0（非 0 = ISR 超时丢拍，立刻报我） |
| `g_v2k_msg_1to2.cpu1_status.tick` | 与 g_v2k_tick 同步前进（慢环快照） |
| `g_v2k_sm_state` | 1（IDLE，上电默认封锁） |
| Phase 1 全部观测项 | 回归通过（ping/pong、心跳、双灯） |

注意：IDLE 态 PWM 输出被 OST 封死（GPIO0/1 无波形是**正常**），但时基与
ISR 照跑——TZ 只封输出不停时基，验证 A 不需要先发 START。

## 5. 验证 B — 示波器实测（先按 §6 发 START 让波形出来）

接线：CH1 → J8 排针 **78**（EPWM1A），CH2 → 排针 **77**（EPWM1B），
CH3 → 排针 **80**（ISR 探针），GND → 排针 60/62。

1. **波形形态**：CH1/CH2 互补 20 kHz，CH1 占空比 ≈25%；放大边沿交接处看
   两路**同低 1 µs**（死区）；
2. **抖动**：触发选 CH1 上升沿（PWM 由晶振决定，是刚性参考），无限余辉，
   看 **CH3 上升沿的时间散布** = 中断抖动实测。20 kHz 与 100 kHz 各记一组；
3. **ISR 耗时**：CH3 脉冲宽度（Phase 2 ISR 极短，预期 <1 µs）；
4. **halt 安全（FREE_SOFT 决策实证）**：Suspend CPU1 → CH1/CH2 立即变低
   （TZ6 CBC），Resume → 波形下个周期自动恢复、`g_v2k_tick` 继续递增、
   状态机仍是 RUNNING。可顺带在示波器上确认 resume 后**第一拍就是完整拍**
   （周期/占空比/死区与稳态一致）——"残缺拍"只属于 STOP 模式，这就是
   FREE_RUN 载波定时连续的直接实证。记入 BRINGUP。

## 6. 验证 C — 命令与 trip（命令必须从 CPU2 会话戳：MSGRAM 硬件单向写权限）

CPU2 会话 Expressions 加 `g_v2k_msg_2to1.cmd_req` 展开。发布协议：先填
`cmd_code`，**最后写 `cmd_seq` = 旧值+1**。CPU1 侧观察 `cpu1_status`：

| 步骤 | 操作（CPU2 会话） | 预期（CPU1 侧 + 示波器） |
|---|---|---|
| 1 | cmd_code=1, cmd_seq=1（START） | ack_seq=1, cmd_result=0, sys_state=2；**波形出现** |
| 2 | cmd_code=2, cmd_seq=2（STOP） | sys_state=1；波形消失；g_v2k_tz_int_cnt **不变**（STOP 先关中断再 force OST，不进 ISR） |
| 3 | 再 START（seq=3），然后**插跳线** GPIO3（J8 排针 79）→GND | 波形即刻消失；sys_state=3, fault_code=1 |
| 4 | 带跳线 CLEAR_FAULT（cmd_code=3, seq=4） | 仍 FAULT（源未消失，契约语义） |
| 5 | 拔跳线 → CLEAR_FAULT（seq=5） | sys_state=1（IDLE） |
| 6 | START（seq=6） | sys_state=2，波形恢复 |
| 7 | 错误注入：IDLE 时发 STOP | cmd_result=2（BAD_STATE），状态不变 |

加分实测（规则 2「保护不经过 CPU」的直接证据）：CH3 改探 GPIO3，单次触发
其下降沿，量 **GPIO3↓ → EPWM1A↓ 的延迟**——纯硬件路径应 <100 ns；而
fault_code 变化在毫秒级慢环。两个时间尺度差 4 个量级，就是硬件保护链与
软件状态机的分界实证。

可选加分：RUNNING 中 halt CPU1 → 插跳线 → Resume → 立即 FAULT
（OST 在 halt 期间已被硬件锁存，trip 不需要 CPU 活着）。

## 7. 验证 D — 100 kHz 压测

按 §2 第 3 条同时改 syscfg Period=1000 与 `-D V2K_ISR_HZ=100000`，重建重载，
重复 §4/§5：tick ≈ 1e5/s、lat min/max、ovf==0、波形/死区完好。**20 kHz 与
100 kHz 两组数据都记 BRINGUP**——平台频率锚点（20–100 kHz 待定档）的第一份
实测依据。测完两边都还原。

## 8. 完成后

- BRINGUP.md Phase 2 模板逐项填实测值；
- commit（英文 message）+ 硬件验证 tag（建议 `phase2-timebase-protection`）；
- 遗留给后续阶段：ISR 多速率调度与执行器（Phase 3）、CMPSS 模拟 trip 源 +
  DRV8323 nFAULT 接入（随功率级）、ESC RAM 核对（Phase 6 前）。
