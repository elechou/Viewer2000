# BRINGUP.md — 实物验证记录

工作流约定：每一步记录**在实物上验证了什么、用什么方法验证**（示波器实测值、
CCS Graph 截图、Expressions 读数等）。验证知识不能只活在 commit message 里。

记录格式：日期 / 验证项 / 方法 / 实测结果 / 结论（+遗留问题）。

---

## Phase 1 — 双核骨架 ✅ 验收通过（2026-06-12，LAUNCHXL-F28P65X 实物）

操作步骤见 `docs/phase1-sysconfig.md`。验证项清单（全部通过）：

- [x] 两工程 RAM 配置 0 error 构建（含 v2k_check_contracts.c 契约断言在 cl2000 通过）
- [x] CPU1 单独 Resume 后阻塞于 IPC_sync（红灯不闪，符合预期）
- [x] CPU2 Resume 后双灯闪烁：红 LED4 1 Hz / 绿 LED5 2 Hz（目测）
- [x] `v2k_assert_layout` 未触发（两核都没停在 ESTOP0）＝ .cmd 落位与 v2k_memmap.h 一致
- [x] Expressions：g_ping_cnt / g_pong_cnt 同步递增；g_handshake_state == 3
- [x] `g_v2k_gs0.desc_table.hdr.magic == 0x564B4454`
- [x] 心跳监视（加分项）：halt CPU2 → g_cpu2_alive 1→0、status_flags 置 CPU2_LOST、红灯照闪；resume 恢复（基本规则 1 实测）
- [x] 额外实测：**两核各自 halt 均不影响另一核 blink**——双向的故障域独立性，
      不止规则 1 要求的"CPU2 死、CPU1 照跑"方向

记录区：

| 日期 | 验证项 | 方法 | 实测 | 结论 |
|---|---|---|---|---|
| 2026-06-12 | v2k_assert_layout 自检 | 上板首跑停在 ESTOP0 → 查 .map | g_v2k_msg_1to2 落在 0x3A088 而非 0x3A000（driverlib ipc.obj 的消息队列缓冲占住 MSGRAM_* 惯例 section 名且排前） | 自检机制有效。修复：v2k 改用独立 section 名 + .cmd 切 0x40 词子区，契约基址不变 |
| 2026-06-12 | Resume 后"跑飞"+ CPU2 反复 held in reset | Registers 读 PC=0x081A3A（FLASH_BANK0），RESC.NMIWDRSn=1 | 未处理 NMI → NMI 看门狗整片复位 → S3 默认 flash 启动跑进**旧单核固件**；整片复位同时把 CPU2 按回 reset。单步能活=halt 时 NMIWD 挂起 | NMI 源头号嫌疑 CPU2WDRS（CPU2 在 .out 加载前跑 M0 垃圾→其 WD 复位；TI nmi_ex1 例程证实此为双核已知 NMI 路径）。修复：擦 flash 旧固件 + 两核挂 NMI 兜底 ISR（g_nmi_cnt/flags 可观测）|
| 2026-06-12 | **Phase 1 整体验收** | 擦 flash 后按 docs/phase1-sysconfig.md §4 顺序上板；§5 Expressions 清单逐项核对 | 双灯 1 Hz/2 Hz；g_ping_cnt/g_pong_cnt 同步递增；g_handshake_state==3；desc magic==0x564B4454；halt CPU2 → g_cpu2_alive 1→0 + CPU2_LOST 置位 + 红灯照闪，resume 自动恢复；两核各自 halt 互不影响对方 blink | **Phase 1 完成**：双核引导、GSx/MSGRAM 归属与契约落位、IPC 会合+ping-pong、心跳监视、NMI 兜底全部实证。遗留：NMI 兜底窗口期 g_nmi_cnt 是否非零未记录（下次会话顺手看一眼 g_nmi_flags_last 可确证 CPU2WDRS 假说） |
| 2026-06-12 | NMI 源 CPU2WDRS 假说确证（清上一行遗留） | boot 引脚已改 SCI/wait、整片 flash 已擦除后再跑双核调试会话，Expressions 读两核 NMI 观测量 | CPU1：g_nmi_cnt=1，g_nmi_flags_last=513=0x201=NMIINT\|**CPU2WDRSN**（driverlib sysctl.h 位定义核对）；CPU2：g_nmi_cnt=0 | **假说确证**：NMI 源就是 CPU2 看门狗复位（CPU2 被 Device_bootCPU2 放出后、.out 加载前跑 M0 垃圾→其 WD 复位）。计 1 次而非反复=WD 复位后 CPU2 boot ROM 等新 IPC 引导命令并自喂 WD，每次会话恰一发。**该窗口与 boot 引脚、flash 内容均无关**（boot 目标是 M0 RAM 不是 flash），引脚/擦 flash 只改"整片复位后落点"；NMI 兜底是切断"CPU2 死→NMIWD 整片复位"的常驻件（规则 1），不可删 |

---

## Phase 2 — 时基证明 + 保护 ✅ 验收通过（2026-06-13，LAUNCHXL-F28P65X 实物）

操作步骤见 `docs/phase2-bringup.md`。验证项清单：

- [x] Device Support 迁移：两工程生成 device.c/h 取代模板、时钟树 EPWMCLKDIV=/1（errata 警告消失）、模板 device.c/h 排除
- [x] 迁移后 Phase 1 全项回归（双灯/握手/ping-pong/心跳/NMI 计数）
- [x] SysConfig 配置完成并 review（EPWM1/ADCA/DACA/XBAR/GPIO 五模块）——SOC-A 源因 TI syscfg codegen bug 改由 C 拥有（v2k_tb_init 补 EPWM_setADCTriggerSource + v2k_tb_check 读回 SOCASEL，见记录区）
- [x] cpu1 工程 0 error（新增 v2k_timebase.c / v2k_fault.c 自动入编译）；上电不停 v2k_tb_check 的 ESTOP0（syscfg 与 C 契约对账通过）
- [x] g_v2k_tick ≈ 20000/s；g_v2k_adc_a0 ≈ 2048（DACA 中位经 ADC 回读）；ovf=2 后冻结（FREE_RUN+调试 halt 良性、非丢拍，判据修正见记录区）
- [x] 示波器：EPWM1A/B 互补 + 1 µs 死区（实测：占空比 23% / 73%、交接处同低 1 µs ×2）
- [x] 抖动：CH1(PWM) 触发余辉下 CH3(ISR 探针) 边沿散布 = 25 ns @20k；@100k 散布未单独记录（D 测的是 PWM↑→ISR↑ 相位 5.9 µs，非散布——见记录区末行）
- [x] ISR 耗时（CH3 脉宽）= 1000 ns @20k / 1000 ns @100k（与频率无关、符合预期；@100k 即 10 µs 周期的 ~10% CPU——Phase 3 把示波采样塞进 ISR 时要计这笔预算）
- [x] halt CPU1 → 输出立即安全（TZ6 CBC）、resume 自动恢复 —— FREE_SOFT 决策实证
- [x] 命令全序列：START/STOP/trip→FAULT(fault_code=1)/带源 CLEAR 重入/清源 CLEAR→IDLE/BAD_STATE（修掉伪 trip 后整序通过，见记录区末两行）
- [ ] 硬件 trip 延迟：GPIO3↓ → EPWM1A↓ = ____ ns（规则 2「不经过 CPU」实证）——**加分项，本轮未单独示波器测量**
- [x] 100 kHz 压测：tick ≈ 1e5/s、波形完好（占空比 15%/65% @死区 1 µs、halt 安全）、ISR 1 µs；ovf 未单独读数，但 1 µs « 10 µs 周期、压测无丢拍

> ⚠ **2026-06-13 订正**：验证 A/B 当时的“保护语义”是假的——`v2k_fault_arm()` 在 EPWM1
> 外设时钟开启前写 TZFRC，OST 从未锁存，IDLE 不封输出、开机即带电自由跑。A 行
> `sm_state=1` 读数没错但“IDLE 封锁”不成立；B 行波形是开机自由跑、非 START 放行。
> **EPWM 配置类测量（占空比/死区/抖动/ISR 耗时）仍有效**；保护门控相关结论作废，
> 修复后须重做 A（确认 IDLE 无波形）+ B（START 放行）+ C。详见记录区末行。
>
> **2026-06-13 续**：保护门控的两个 bug（OST 抢锁失败、以及随后暴露的伪 trip）均已
> 修复，A/B/C/D 全部重做通过——Phase 2 收尾。详见记录区末两行。

记录区：

| 日期 | 验证项 | 方法 | 实测 | 结论 |
|---|---|---|---|---|
| 2026-06-12 | Device Support / 时钟树迁移回归 | 两工程 syscfg 加 Device Support，生成 device.c/h 取代手写模板（含 codestartbranch）；时钟树 SYSCLK=200MHz、EPWMCLKDIV=/1；系统工程 system.xml 接到 cpu1/cpu2（@match，活动配置切 RAM 治好"强制回退 FLASH"）；Phase 2 固件先 stash 隔离，纯迁移树 clean build 上板 | Phase 1 全项回归通过：双灯 1/2 Hz、ping/pong 同步递增、握手=3、心跳监视正常、窗口期 g_nmi_cnt=1（CPU2WDRS 照旧）；cpu1 侧 errata 警告消除，cpu2 侧"clocking functions"黄警告=规则 5 预期常驻 | **设备初始化与时钟收敛到 syscfg 单一来源**；EPWMCLKDIV=/1（errata）由时钟树生成代码落实，后续 v2k_tb_check 读回断言 |
| 2026-06-13 | TI SysConfig SOC-A 源 codegen bug | review board.c 发现 EPWM_init 缺 EPWM_setADCTriggerSource；查 syscfg 元数据：源字段默认=枚举[0]=DCxEVT1（非 TBCTR_ZERO），等于默认即不生成代码；硬件 ETSEL 读回 SOCASEL=0x0（=DCxEVT1） | 选 TBCTR_ZERO 时 syscfg 不出代码、SOCASEL 停复位值 DCxEVT1 → SOC 永不触发、tick 卡 0；而 v2k_tb_check 原未读该字段、自检会误放行 | **绕过**：SOC 源改由 C 拥有——v2k_tb_init 显式 `EPWM_setADCTriggerSource(EPWM1_BASE, EPWM_SOC_A, V2K_TB_SOC_SRC)`（置于自检前），v2k_tb_check 增读回 SOCAEN/SOCASEL 断言。TI 修复后退化为无害重复设置 |
| 2026-06-13 | 验证 A — 时基与 ISR | CPU1 会话 Expressions + Continuous Refresh，按 docs/phase2-bringup.md §4 逐项核对；g_v2k_isr_ovf_cnt 连续观察 10 min | g_v2k_tick 持续递增 ≈20000/s；g_v2k_adc_a0 ≈2048；g_v2k_sm_state=1(IDLE)；Phase 1 全项回归通过；**g_v2k_isr_ovf_cnt=2 后冻结**（10 min 不增长） | 时基链通，C 侧 SOC 源补写实证生效（TI bug 绕过成立）。ovf=2 **非丢拍**：FREE_RUN 下 CPU1 每被 halt 一段，ePWM 照走→ADC EOC 照置 ADCINTOVF（粘性），resume 时 ISR 计 +1；错开启动两核期间 CPU1 被 halt 2 次 = 2。**冻结即证 ISR 不超时**。判据修正：看"连续不 halt 窗口内是否增长"，非绝对值 |
| 2026-06-13 | 验证 B — 示波器实测 | 先 START 出波形；CH1=EPWM1A(J8.78)、CH2=EPWM1B(J8.77)、CH3=ISR探针 GPIO2(J8.80)、GND J8.60/62；CH1↑ 触发 + 无限余辉 | CH1/CH2 互补 20 kHz；占空比 CH1 23% / CH2 73%（裸 25/75 各被死区削 1 µs）；交接处两路同低 1 µs ×2；CH1↑→CH3↑ = 30.9/30.875/30.9/30.9/30.9 µs（散布 25 ns p-p）；CH3 脉宽 = 1 µs ×5；halt CPU1→CH1/CH2 立即变低、resume 下一周期自动恢复、tick 续增 | 波形/死区/互补符合设计。CH1↑→CH3↑ 拆解 = 几何 30.25 µs（CH1↑@count 3950 → 下一谷 count 0）+ ~0.65 µs（ADC 采样转换 + EOC→PIE→ISR 入口延迟，与 g_v2k_isr_lat 同源）；25 ns p-p = 软硬合计中断抖动，余量极大。FREE_RUN「resume 第一拍即完整拍」成立。⚠ 注：此处“先 START 出波形”事后证伪——波形实为开机自由跑（见下行），但 EPWM 配置测量值不受影响 |
| 2026-06-13 | **保护先行失效（订正 A/B，硬件确证）** | 做验证 C 时发现：双核一跑、未发任何命令，EPWM1A/B 即出波形。查 cpu1.c 调用序——`v2k_fault_arm()`（line 122）在 `Board_init`（line 130）之前，而 EPWM1 外设时钟要到 `Board_init→SYSCTL_init`（board.c:997）才开（`Device_init` 不含外设时钟使能——那在从未被调用的 `Device_enableAllPeripherals` 里） | arm() 在无 EPWM 时钟下写 TZFRC.OST，写被丢弃 → OST 从未锁存 → IDLE 形同虚设、上电输出带电自由跑。**A 行 sm_state=1 没错但“IDLE 封输出”不变量当时为假；B 行波形是开机自由跑、非 START 放行**（占空比/死区/抖动仍有效）。根因与 GPIO3 挂哪个核无关（那是 Obs2 上拉浮空的独立问题） | 修复（v2k_fault.c）：arm() 先 `SysCtl_enablePeripheral(EPWM1)`+`RPT #5\|\|NOP` 再 force OST；`v2k_fault_init` 在 Board_init 后**权威再 force 一次 + 读回断言 `TZFLG.OST` 否则 ESTOP0**。教训同 SOCASEL：保护态必须寄存器读回验证、不可假设“我以为封了”。**待重建后重做 A（IDLE 无波形）+ B（START 放行）+ C** |
| 2026-06-13 | **状态机卡 RUNNING、命令全 BAD_STATE（伪 trip 根因）** | 验证 C 现象：插跳线前 fault_code 已=1；插跳线后 PWM 关但 sys_state 卡 2(RUNNING)；CLEAR/START 全 cmd_result=2(BAD_STATE)。判据：未插任何跳线，每发一次 START，g_v2k_tz_int_cnt 即 +1 | 双缺陷叠加：① v2k_fault_init 在 OST 锁存期间就开 EPWM 级 TZEINT.OST → latched-OST 持续把 TZFLG.INT 转发进 **PIEIFR 并锁存**（PIE 关着不触发、标志位攒着，EPWM_clearTripZoneFlag 清不掉 PIEIFR）→ 下次 START 的 Interrupt_enable 瞬间补发伪 trip；② 伪 ISR 置 FAULT 后，START 紧跟的 g_sm_state=RUNNING（写在 enable 之后）把 FAULT 覆写回 RUNNING、fault_code=1 残留、TZ 中断已被伪 ISR 关死 → 真插跳线只剩硬件 OST 封波形、状态机不动 | 修复（v2k_fault.c）：EPWM 级 TZEINT.OST 与 PIE 级两级同步、都只在 RUNNING 开（init 删 enable / START 开 / STOP+ISR 关）；START 改为先置 RUNNING 再开中断（带 trip 的 START 进 ISR 的 FAULT 不被覆写）。修正了此前“TZEINT 可常开、门控只放 PIE 级”的旧认知 |
| 2026-06-13 | **验证 C/D 验收通过（Phase 2 收尾）** | C：经 CPU2 会话戳 cmd_req 跑 §6 命令全序列；D：syscfg Period=1000 + 编译器 -D V2K_ISR_HZ=100000 重建重载，重复 §4/§5 | C：START→RUNNING 出波形、STOP→IDLE 灭、START 后插跳线→FAULT(fault_code=1) 波形即灭、带源 CLEAR 仍 FAULT、清源 CLEAR→IDLE、再 START→RUNNING、IDLE 发 STOP→cmd_result=2；ack_seq 严格跟随 cmd_seq、tz_int_cnt 仅真插跳线 +1。D@100k：tick≈1e5/s、占空比 EPWMA 15%/EPWMB 65%（标称 25% 经 1 µs 死区，高频死区占比大）、死区 1 µs、PWM↑→ISR↑=5.9 µs（与 EPWMA↑@4.75µs → 下一谷 SOC@10µs + ~0.65µs ADC/中断延迟吻合）、ISR 脉宽 1 µs、halt CPU1→输出立即安全 + resume 自动恢复 | **Phase 2 验收通过**：时基链(ePWM→ADC→EOC ISR)、FREE_RUN halt 安全、TZ 硬件 trip + 故障锁存状态机、20k/100k 双档全部实证。遗留加分项：GPIO3↓→EPWM1A↓ 纯硬件 trip 延迟、100k 抖动散布均未单独测。下一步 Phase 3（执行器多速率调度 + 双模式 RAM 示波器 + 参数双缓冲 + 描述符表）|

> **遗留 TODO（Phase 5，上功率级前必做）— `v2k_tb_check` 自检补全（code review #4）**：
> 当前读回覆盖 EPWMCLKDIV / 周期 / TBCLK 分频器 / TZ 源 / TZ 动作 / SOC(ePWM 侧) /
> FREE_SOFT，但**未覆盖**两项安全关键配置（当前实测均正确，仅缺读回兜底）：
> ① **死区** `DBCTL`(极性/IN-MODE) + `DBRED`/`DBFED`(=200)——错配即 DRV8323 半桥
>    shoot-through，上功率级前最硬的一道防线；
> ② **ADC 侧 SOC 触发源** `ADCSOC0CTL.TRIGSEL`(=EPWM1_SOCA)——SOC 链的收端，漂了与
>    ePWM 侧同样 tick 卡 0（SOCASEL 那条只补了发端）。
> 实现 = 在 `v2k_tb_check` 加这两组寄存器读回断言，与现有项同模式。
>
> 另：本轮 code review 的 #2（APP_STOP 竞态丢 FAULT + START 防御性清码）与 #3（自检补
> TBCLK 分频器）已在 v2k_fault.c / v2k_timebase.c 修复，**待重建 + 冒烟重验**（下条）。

---

## Phase 3 - 执行器 + 可观测性（软件实现完成，硬件验收待进行）

操作步骤见 `docs/phase3-executor-observability.md`。

- [x] Phase 3 基线 contract version 2；DAQ_CTRL 12/14-octet 兼容向量通过 host 检查（Phase 3.5 因状态块追加 `tick_hz` 升至 version 3）
- [x] GS0 平面/慢速环与 GS1-GS3 快速环链接布局完成
- [x] 固定顺序 L1 executor、错相 1 kHz/100 Hz due mask、duty clamp/apply
- [x] 描述符表、后台整批验证/ISR 同拍应用参数、10 Hz 值镜像
- [x] LIVE/SNAPSHOT、边沿触发、partial final block、consumer API、CCS view
- [x] 固定双核路径；CPU1 后台为普通无限循环，仅在控制 tick 前进后检查 deadline，
      约 1 ms poll point
      按 seq/flag 处理共享请求；CPU2 暂用本地 1 ms 诊断心跳
- [x] ISR 收敛：scope 配置/绑定/容量计算移至后台；快慢组用 active 位和倒计数，
      参数提交放在独立 1 kHz 错相槽位；控制段/scope 段周期分开统计
- [x] PC 端 contract 静态断言与 24 组 golden vectors 检查通过（2026-06-14）
- [x] §5 调度与 ISR 预算 实物验收（2026-06-14, RAM/20 kHz, CCS MCP）：1kHz/100Hz due 间隔与错相经 snapshot due_mask 通道实证（1kHz 间隔 20 tick、100Hz 间隔 200 tick、永不同拍）；ISR 预算 isr_max=840 cycles (4.2 µs) « 10000 cycles 预算
- [x] §6 参数双缓冲实物验收（2026-06-14, RAM/20 kHz, CCS MCP 驱动）：合法/旧范围检查×2/错类型/错数量/错地址 Flash/错地址未对齐/未注册地址写/批原子性 7 用例全过；§6 步骤 6 三态照跑（IDLE→APP_START→RUNNING→软 TZ→FAULT→CLEAR_FAULT→IDLE，每态各一次合法写）全过。当前 contract 已删除旧范围检查语义，需按新结果码复测错误路径。
- [x] §7 Snapshot + CCS Graph 实物验收（用户自检）
- [x] §8 LIVE + 跨组独立 实物验收（2026-06-14, RAM/20 kHz, CCS MCP）：OFF→host BIND(2ch)→LIVE 序列、block hdr 全字段、自然 overrun 不阻塞控制 ISR、LIVE 中 BIND→BAD_STATE、group 1 慢组 LIVE 与 group 0 独立。**待 Phase 3.5**：CPU2 consumer API (peek/release/begin_snapshot) 语义
- [x] RAM / 100 kHz 实物验收（2026-06-14, CCS MCP 链内完成切换：ccs-sysconfig EPWM Period 5000→1000 + Edit v2k_timebase.h 默认 V2K_ISR_HZ 20000→100000 + ccs-project buildProject + ccs-debug load/run）：tick 100k/s、tb_check 不停 ESTOP0、isr_cycles_max=1844/2000 (SNAPSHOT 8ch 峰值，余 8%) / OFF 稳态 840 (42%)、budget_violation=0、ovf_cnt=0、§5 due 错相 1kHz=100 tick 1kHz/100Hz 永不同拍、§6 参数链 + 状态机闭环
- [ ] CCS Project：CPU1/CPU2 器件从 DK6 修正为 `TMS320F28P650DK9`
- [ ] SysConfig：新增 CPUTIMER1；CCS pre-build 接入 git hash 生成器
- [ ] CPU1/CPU2 RAM 与 FLASH 配置 `buildProject` 0 error
- [ ] 20 kHz / 100 kHz 实物验收、Phase 1/2 回归与 Silicon Real-time Mode
- [ ] 验收通过后创建 `phase3-executor-observability` tag

未完成项不得用软件自检替代实物结论，也不得提前打硬件 tag。

记录区（RAM/20 kHz，CCS MCP 驱动，2026-06-14 同一会话连跑）：

| 日期 | 验证项 | 方法 | 实测 | 结论 |
|---|---|---|---|---|
| 2026-06-14 | §6 基线 + 地址常量 | CPU1/CPU2 各 `getTargetState`=Running；CPU1 Expressions 读 tick / ovf / budget / param_status；CPU2 读 param_shadow | tick 1.36M 持续递增；`isr_ovf_cnt=1`（基线值，load/connect 窗口一次性）；`budget_violation=0`；`param_status` 全 0、`mirror_seq` 在 10 Hz 推进；shadow.count/commit_seq 全 0。地址锁定：`&g_v2k_pwm_duty_cmd=0xAA46`（F32, kind PARAM\|SCOPE）；`&g_v2k_scope_cycles_max=0xAA24`（U32, 未注册——案例 6 用） | 基线健康，可起跑 |
| 2026-06-14 | §6.1 合法写 | CPU2 写 shadow {addr=0xAA46, type=F32(4), value_bits=0x3F000000 (=0.5f), count=1}，最后 `commit_seq=1` | `applied_seq=1`/`result=OK(0)`；`pwm_duty_cmd` 0.25 → 0.5（同拍生效） | ✓ |
| 2026-06-14 | §6.2a 旧范围检查下界（已废弃） | 同上 addr/type，`value_bits=0x3C23D70A`(=0.01f)，`commit_seq=2` | 旧固件曾按范围检查拒绝；当前 contract 已删除范围错误码，这一项不再作为验收条件 | 历史记录 |
| 2026-06-14 | §6.2b 旧范围检查上界（已废弃） | `value_bits=0x3F800000`(=1.0f)，`commit_seq=3` | 旧固件曾按范围检查拒绝；当前 contract 已删除范围错误码，这一项不再作为验收条件 | 历史记录 |
| 2026-06-14 | §6.3 错类型 | `type=U32(3)` 写 F32 注册地址 0xAA46，`value_bits=42`，`commit_seq=4` | `applied_seq=4`/`result=BAD_TYPE(1)`；pwm 维持 0.5 | ✓ |
| 2026-06-14 | §6.4 错数量 | `type` 恢复 F32 + value 0.6f；`count=17`(>16=V2K_PARAM_BATCH_MAX)，`commit_seq=5` | 当前结果码应为 `BAD_COUNT(2)`；pwm 维持 0.5 | 待按新 contract 复测 |
| 2026-06-14 | §6.5a 错地址 Flash | `count=1`、`addr=0x80000`(Flash Bank0)，`commit_seq=6` | 当前结果码应为 `BAD_ADDR(3)`；pwm 维持 0.5 | 待按新 contract 复测 |
| 2026-06-14 | §6.5b 错地址 未对齐 | `addr=0xAA47`(奇地址 + 32-bit type=F32)，`commit_seq=7` | 当前结果码应为 `BAD_ADDR(3)`；pwm 维持 0.5 | 待按新 contract 复测 |
| 2026-06-14 | §6.6 未注册地址写 | `addr=0xAA24`(`g_v2k_scope_cycles_max`, 未注册, U32) `value_bits=0`(避免影响 ISR 预算统计)，`commit_seq=8` | 当前语义：地址落在 CPU1 可写数据区且 type 合法即 `OK`；不再有旧计数 | 待按新 contract 复测 |
| 2026-06-14 | §6.7 批原子性 | writes[0]={addr=0xAA46,F32,0.6f 合法}, writes[1]={addr=0xAA46,F32,5.0f=0x40A00000 旧范围失败}，`count=2`、`commit_seq=9` | 旧固件曾用范围失败验证批原子性；当前应改用 BAD_TYPE/BAD_ADDR 场景复测 | 历史记录 |
| 2026-06-14 | §6 步骤 6a IDLE 合法写 | 当前 sys_state=1，写 0.3f (`commit_seq=10`)；记 mirror_seq=15700 | `applied_seq=10`/OK；pwm_duty_cmd=0.3；sys_state 仍 1；mirror_seq → 16182（10 Hz 推进 482） | ✓ IDLE 下参数链与 mirror 照跑 |
| 2026-06-14 | §6 步骤 6b RUNNING 合法写 | CPU2 写 `g_v2k_msg_2to1.cmd_req` {cmd_code=APP_START(1), cmd_seq=1}；切到 RUNNING 后写 0.45f (`commit_seq=11`) | sys_state 1→2、cmd_result=OK；`applied_seq=11`/OK；`pwm_duty_cmd=pwm_duty_applied=0.45`（RUNNING 下输出真正放行） | ✓ |
| 2026-06-14 | §6 步骤 6b' 软触发 TZ 进 FAULT | `writeMemory(coreId=0, 0x409B, 0x0004)`(EPWM**9** TZFRC)→无反应；改 `writeMemory(0x309B, 0x0004)`(EPWM**1** TZFRC.OST) | sys_state 2→3(FAULT)；`fault_code` 0→1(`V2K_FAULT_TZ1_EXT`)；`tz_int_cnt` 0→1 | ✓ 顺手识坑：F28P65x EPWM1 base=0x3000（不是 0x4000），调试器符号名显示 `EPwm9Regs_*` 可一眼分辨 |
| 2026-06-14 | §6 步骤 6c FAULT 合法写 | FAULT 下写 0.4f (`commit_seq=12`)；记 mirror_seq=19086 | `applied_seq=12`/OK；`pwm_duty_cmd=0.4`；sys_state 仍 3；mirror_seq → 19358（10 Hz 持续推进）；实际 PWM 输出由 EPWM1.OST 硬件锁存封锁 | ✓ FAULT 下参数链与 mirror 仍照跑、硬件输出仍被封 |
| 2026-06-14 | §6 步骤 6d CLEAR_FAULT 回 IDLE | CPU2 写 `cmd_req` {cmd_code=CLEAR_FAULT(3), cmd_seq=2}（外部跳线一直未动 = trip 源已无） | sys_state 3→1、`fault_code` 1→0、cmd_result=OK | ✓ 三态闭环 |
| 2026-06-14 | §6 收尾基线 | 全套跑完后再读 tick / ovf / budget | tick 涨到 ≈ 4.0e7 仍持续递增；`budget_violation_cnt=0` 全程未增；`ovf_cnt=1` 仍与起跑基线一致——状态切换 + 软 TZ trip + 12 次 commit 全程 ISR 无掉拍 | **§6 全部 9 用例 + 三态闭环通过**。意外收获：`writeMemory(EPWM1_BASE+0x9B, 0x4)` 软 TZ trip 走真 EPWM 级中断、与硬件路径等价，可作回归常用工具，已写进 docs/phase3-executor-observability.md §6 步骤 6 |
| 2026-06-14 | §5 步骤 5 ISR 预算（先重置 max 字段） | CPU1 会话 `g_v2k_isr_cycles_max=0` / `g_v2k_control_cycles_max=0` / `g_v2k_scope_cycles_max=0`（写完即被 ISR 下一拍重新峰值跟踪），让 ISR 跑几秒后再读 | `isr_cycles_max=840`(4.20 µs)、`control_cycles_max=620`(3.10 µs)、`scope_cycles_max=67`(335 ns, scope OFF 状态) | ISR 预算 ✓：@20 kHz 预算 200 MHz/20kHz=10000 cycles → 8.4% 使用；@100 kHz 预算 2000 cycles 仍 < 50%、留稳定余量 |
| 2026-06-14 | §5 步骤 3 due 错相（用 snapshot 取代示波器） | CPU2 写 `scope_cfg[0]`{`mode_req=SNAP_ARMED(2), trig_ch_slot=6 (due_mask), trig_edge=RISE, trig_level=0.5, pre_trig_pct=50`}、`cfg_seq=1`；触发瞬间冻结；CPU1 写 `g_v2k_ccs_view`{`group=0, channel_slot=6, request_seq=1`} 解交错 | mode 走 ARMED→TRIG→FROZEN(state_seq=3)；frozen_count=65 blocks (650 ticks 窗口)；CCS view `count=647`、`start_tick=trig-7`（pre 段被 ARM 后第一拍触发裁短为 7 样本）。data 抽样：**1 kHz due (=1.0) 位置 {7,27,47,67,87,107,127,147,167,187,207,227,247,267,287,307,327,347,367,387} 间隔严格 20 tick**；**100 Hz due (=2.0) 位置 {117,317} 间隔 200 tick**；**前 400 个样本零值 3**（两个 due 永不同拍） | ✓ 错相机制（编译期 STATIC_ASSERT 钉死的相位常量：1 kHz phase=7, 100 Hz phase=17, param phase=15）在运行时实证：1kHz @ phase 7、100Hz @ phase 17、param phase=15 与两个 due 都错开。`v2k_schedule` 倒计时分频结果与设计一致 |
| 2026-06-14 | §8 步骤 1 OFF 应答 | CPU2 写 `scope_cfg[0].mode_req=OFF` + `cfg_seq=2` | `cfg_ack_seq=2`、`cfg_result=OK`、`mode=0` | ✓ |
| 2026-06-14 | §8 步骤 2 OFF 下合法 BIND（2 ch host 路径） | CPU2 写 `scope_bind[0]`{n_ch=2, ch[0]={addr=0xAA46, type=F32}, ch[1]={addr=0xAA48, type=F32}}, `bind_seq=1` | `bind_ack_seq=1, bind_result=OK`；prod 自动重算：`n_ch=2`、`block_slot_words=128→48`(=8 头 + 2 ch×10 ticks×2 word/F32=40)、`ring_capacity=128→512`(scope0 区 24576 words / 48)、`wr_idx=0` | ✓ host BIND 路径在 OFF 下接受、容量自动重新分配 |
| 2026-06-14 | §8 步骤 3 LIVE 切换（顺手发现 validate_cfg 一个细节） | 直接 `mode_req=LIVE + cfg_seq=3` → `cfg_result=BAD_PARAM(2)`！原因：上一段 SNAPSHOT 留下的 `trig_ch_slot=6 ≥ 新 n_ch=2`；`v2k_validate_cfg` 对 LIVE 也校验 trig 字段。把 `trig_ch_slot=0` + `cfg_seq=4` 重发 → `mode=1` | ✓ ARM 与 LIVE 共用 cfg、validate 一视同仁；BIND 改 n_ch 后 host 须把 trig_ch_slot 也带到 < n_ch（实战提醒：换通道布局先确认 `trig_ch_slot < n_ch`） |
| 2026-06-14 | §8 步骤 4 wr_idx 推进 + block hdr 字段 | LIVE 期间 evaluate `*(v2k_block_hdr_t *)g_v2k_scope_fast`（地址 0x12000） | block 0 hdr：start_tick=56,814,553（活值）、block_seq=65（跨 SNAPSHOT→OFF→BIND→LIVE 单调累加：SNAPSHOT 已 publish 65 → LIVE 第一个 publish 复用 seq 65）、group_id=0、n_ticks=10、n_ch=2、bind_seq=1、stride_octets=8 (=2 ch × 4 B F32) | ✓ block 头全字段对齐；block_seq 跨 mode 切换单调累加是 SPSC 设计 feature（host 凭 seq 不连续即发现断口） |
| 2026-06-14 | §8 步骤 5 自然写满 → overrun | 消费者从未启动（CPU2 没读，`s_cons_rd_cache=0`），观察一段时间 | `wr_idx` 稳定在 512=ring_capacity（满后 drop 不增 wr_idx）；`overrun_cnt=31895`；`g_v2k_scope_overrun_total=182385`（含历史）；**ISR `ovf_cnt=1` `budget_violation=0` 全程不增** | ✓ 基本规则 1 实证：满则丢、不阻塞控制 ISR；丢 block 由 seq 不连续披露给 host |
| 2026-06-14 | §8 步骤 6 LIVE 中改 BIND 拒绝 | LIVE 下 CPU2 写 `scope_bind[0].bind_seq=2` | `bind_ack_seq=2、bind_result=1=V2K_SCOPE_RESULT_BAD_STATE` | ✓ 一个 LIVE 环里不混两套通道布局，机制由 result code 强制 |
| 2026-06-14 | §8 步骤 7 group 1 LIVE + 跨组独立 | CPU2 写 `scope_cfg[1].mode_req=LIVE + cfg_seq=1`；读 block 0/1 hdr | group 1 mode=1；block 0 hdr：start_tick=60,866,353、block_seq=0、group_id=1、n_ticks=10、n_ch=8、bind_seq=0（default bind 不经 host）、stride_octets=28（2+2+4×6=28）；block 1 start_tick=60,866,553 → **间隔 = 200 tick =10 prescaled × 20 prescaler，符合 §8 步骤 7 预期**；同时 group 0 mode=1、wr_idx=512、ISR ovf/budget 仍不增 | ✓ 跨组独立；group 1 慢组 prescaler 链路（20:1 分频）路径成立 |
| 2026-06-14 | §8 收尾 + Phase 3 RAM/20 kHz 未尽项 | 两组都切回 OFF（`mode_req=0` + cfg_seq +1）让板子回静默 | 两组 cfg_ack 追平、mode=0 | **§8 主体通过**；**Phase 3 RAM/20 kHz 全章节实证完成**。**未做项**（标注，等 Phase 3.5/真实硬件再补）：①CPU2 consumer API (`peek/release/begin_snapshot`) 单元语义——目前 CPU2 上没有 consumer 代码（cpu2.c 只跑心跳），Phase 3.5 SCI 数据泵上线时随之验证；② §5 步骤 6 示波器交叉、§5 步骤 8 LIVE 跨状态连续性的「真正消费者侧序号断口检测」——同上等 3.5 |
| 2026-06-14 | 100 kHz 切换（"两边都改"） | ①ccs-sysconfig MCP 把 cpu1 EPWM1 `epwmTimebase_period` 5000→1000、save 触发 board.c 重新生成；②`.cproject` 不让直接编辑（CCS 规则）—— Edit `cpu1/v2k_timebase.h:29` 的 `#ifndef V2K_ISR_HZ` 兜底默认 20000u→100000u（单 token 修改，可逆）；③terminate 旧 debug session → ccs-project MCP `buildProject` cpu1 + cpu2（auto outputMode，build log 落 RAM/cpu*_build.log）→ ccs-debug MCP launch + connect 双核 + loadProgram + continue | 两 build 都 `success:true, errors:[]`；launch session id 重置；两核 `state=Running`；tick 涨速实测 ~590k tick / ~6 s ≈ 100 kHz ✓；`v2k_tb_check` 不触发 ESTOP0 → SysConfig EPWM Period 1000 与 `V2K_TB_PRD = 200MHz/(2*100kHz) = 1000` 对账成立 | **切换全程在 MCP 链内完成**，未走任何 Bash gmake。一道小坑：CCS 不让 Edit `.cproject` 直接改 Predefined Symbol，所以走「改 v2k_timebase.h 默认值」的路径——这是兜底 `#ifndef` 设计就允许的、文档化的备选 |
| 2026-06-14 | §5 步骤 5 ISR 预算 @100 kHz（OFF 稳态） | 重置 max；让 ISR 跑几秒读 | OFF 稳态：`isr_cycles_max=840` (4.20 µs)、`control_cycles_max=620` (3.10 µs)、`scope_cycles_max=68/69` (340 ns) — 与 @20kHz 几乎一致（ISR 耗时与频率无关，符合 Phase 2 §B 结论） | ✓ 预算 = 200MHz/100kHz = 2000 cycles → **OFF 稳态 42% 使用率**；scope 关闭时充分留余量 |
| 2026-06-14 | §6 case 1 烟雾 @100 kHz | CPU2 发参数批 {addr=0xAA46, F32, 0.5f} `commit_seq=1` | `applied_seq=1`/OK；`pwm_duty_cmd` 0→0.5；下次 evaluate 已追平（端到端 < 2 ms） | ✓ 参数双缓冲链路在 100 kHz 下功能等价 |
| 2026-06-14 | §5 步骤 3 due 错相 @100 kHz | CPU2 ARM `scope_cfg[0]`{trig_ch_slot=6, level=0.5, edge=RISE, pre=50%}, cfg_seq=1；触发后 CPU1 解交错 channel_slot=6 → CCS view count=692, frozen_count=70 blocks=700 ticks | **1 kHz due (=1.0) 位置 [52, 152, 252, 352, 451, 552, 651]**：间隔严格 100 tick ✓（=V2K_ISR_HZ/1000）；**100 Hz due (=2.0) 位置 [402]**：单点（数据窗口 700 < 1000 tick 不够看相邻间隔），相位偏 1 kHz 序列 50 tick 验证错相 ✓；**全程无值 3** ✓（两个 due 永不同拍）；100 Hz 间隔的完整验证由编译期 `V2K_STATIC_ASSERT((V2K_ISR_HZ%100u)==0u)` + 分频常量 `V2K_DUE_100HZ_DIV=ISR_HZ/100=1000` 推论等价确证 | ✓ 多速率调度与错相机制在 100 kHz 下成立 |
| 2026-06-14 | §6 步骤 6 跨状态烟雾 @100 kHz | CPU2 发 APP_START → 软 TZ trip (`writeMemory(0x309B,0x0004)`) → CLEAR_FAULT | sys_state 1(IDLE)→2(RUNNING)→3(FAULT)、`tz_int_cnt` 0→1、`fault_code` 0→1→0、sys_state 3→1；cmd_result 全 OK | ✓ 状态机在 100 kHz 下完整闭环 |
| 2026-06-14 | @100 kHz 全程 ISR 预算 + overflow（含 SNAPSHOT 8ch 期间） | 跑完 SNAPSHOT + 跨状态后读 max | **SNAPSHOT 期间峰值** `isr_cycles_max=1844` (9.22 µs)、`scope_cycles_max=1091`、`control_cycles_max=693`；OFF 重置后 isr_max 回落到 840 | **`isr_max=1844 < 2000` 预算，余 156 cycles (~8%)**——@100 kHz 全负载 8 ch SNAPSHOT 真的把预算扣得很紧。`budget_violation_cnt=0`、`isr_ovf_cnt=0` 全程 ✓ |
| 2026-06-14 | **Phase 3 RAM/100 kHz 验收通过** | 上面 5 行综合 | tick 100k/s、预算无违规、overflow=0、调度与错相成立、参数链 + 状态机 + scope 全活 | **§5 §6 §8 重点项在 RAM/100 kHz 上全部通过**。剩余项与 20 kHz 相同：CPU2 consumer 单元语义留给 Phase 3.5；FLASH 20k+100k 配置也尚未做（Phase 3 § 4 的「FLASH 启动 smoke + tb_check + tick smoke」） |

---

## Phase 3.5 - SCI 数据泵 + Scope2000（软件实现完成，CCS/实物验收进行中）

操作步骤见 `docs/phase3.5-sci-scope2000.md`。

> **2026-06-17 语义修正**：描述符表不再承载 `min/max/scale/offset` 语义；
> 线上值就是真实值，host 只按原生类型解码。当前 contract/固件实现已删除旧字段、
> 范围检查结果码与未注册计数，后续验证只覆盖机械一致性校验。

- [x] wire v6：HELLO tick_hz/capabilities，STATUS cmd ack/result，Stream/Capture 单 Scope 入口
- [x] contract version 8、静态断言、生成器与 golden vectors 同步
- [x] CPU1 配置 GPIO42/43 与 SCIA CPU2 归属；CPU2 RX ISR + COBS + CRC-32C
- [x] CPU2 完成 HELLO/ENUM/STATUS/CAL/DAQ_BIND/DAQ_CTRL/BLOCK/CMD 服务
- [x] 相同 frame seq 超时重试重放缓存响应，不重复执行 COMMIT/CMD/消费 block
- [x] Snapshot 仅在 FROZEN 后排空；Live 满环继续 drop，不阻塞 CPU1
- [x] Scope2000 Rust/egui 初版：SCI transport、完整能力模型、原生 ScopeBlock、
      参数/命令、Live/Snapshot、断口、CSV、控制台、build-hash 重枚举
- [x] Scope2000 golden-vector、坏 CRC、COBS 重同步、拆包/粘包、timeout、
      seq 错配与版本不匹配测试
- [x] Scope2000 独立 root commit：`0fe4067`（Viewer2000 待 CCS/实物验收后提交）
- [x] 验证 A — 串口与 HELLO：115200 / `/dev/tty.usbmodemCL6500011`
- [x] 验证 B — ENUM 分页与描述符字段（wire-level 实物链路，当前 wire v6/contract v8 复测）
- [ ] 验证 B — Scope2000 GUI 枚举 + build-hash 热重枚举（刷入不同 hash 固件）
- [x] CCS CPU1/CPU2 RAM `buildProject` 0 error
- [ ] CCS CPU1/CPU2 FLASH `buildProject` 0 error
- [x] 115200 实物闭环短跑（HELLO/ENUM/CAL/STREAM/CAPTURE/G 错误注入/H 隔离）
- [ ] 最高稳定波特率阶梯与 30 min 长跑
- [x] 验证 C — 参数事务：stage/commit/读回/原子拒绝/重复 commit 重放
- [x] 验证 E — Scope Stream：原生 block、序号、BAD_STATE、overrun 断口语义
- [x] 验证 G — CRC/COBS/超长/拆包/粘包/未知消息/重试恢复
- [x] 验证 H — 115200 短跑性能隔离：host/status/stream/overrun/capture 均不增加 CPU1 通信负担
- [ ] 记录双仓库最终 commit、FLASH smoke、GUI 截图/日志与长跑结果

记录区：

| 日期 | 验证项 | 方法 | 实测 | 结论 |
|---|---|---|---|---|
| 2026-06-14 | 验证 A — 串口与 HELLO timeout 根因 | Scope2000 HELLO 超时后用 CCS Expressions 读 CPU2 诊断量与 SCIA 寄存器；再按 golden HELLO wire frame 经 XDS110 VCP 发包 | 修复前 `g_handshake_state=3`、CPU2 heartbeat 递增，但 `rx_octets=0`；SCIA GPIO42/43 mux 与 CPUSEL 生成正确，`SCICCR/SCICTL1/HBAUD/LBAUD` 却保持 0。根因：CPU2 为省 RAM 绕开 `Board_init()` 直调 `SCIA_BASE_init()`，漏掉 CPU2 `SYSCTL_init()` 里的本地 `SysCtl_enablePeripheral(SCIA)` clock gate，导致 SCIA 配置写入被门控吞掉。修复后寄存器读回 `SCICCR=7`、`SCICTL1=0x23`、`HBAUD=0`、`LBAUD=53`、`RXFFIL=1`；向 `/dev/tty.usbmodemCL6500011` 发 `U` 后 `rx_octets` 0→1；HELLO 响应解码：wire=1、contract=3、build_hash=0x26cd7396、desc_count=16、firmware=viewer2000、tick_hz=20000、capabilities=0x7f；`good_frames=1`、`tx_octets=49` | **验证 A 通过**。正确端口是 `/dev/tty.usbmodemCL6500011`；`...14` 不是本阶段 XDS110 UART backchannel。遗留：Scope2000 GUI 端重连截图/日志、ENUM 及后续 B-G 验证仍待跑 |
| 2026-06-17 | 验证 B — ENUM 分页与描述符字段（wire-level） | 临时 Python 串口探针直接访问 `/dev/cu.usbmodemCL6500011`，复用 `tools/gen_vectors.py` 的 COBS/CRC-32C/raw frame 实现，依次发送 HELLO、STATUS、ENUM(start=0,max=8)、ENUM(start=8,max=8)；`/dev/cu.usbmodemCL6500014` 先试探无完整 COBS 响应 | HELLO：wire=1、contract=3、build_hash=`0x26cd7396`、desc_count=16、firmware=`viewer2000`、tick_hz=20000、capabilities=0x7f；STATUS：sys_state=IDLE、fault_code=0、status_flags=0、build_hash 同 HELLO；ENUM 两页各 8 条、total=16、total_read=16。字段核对：`pwm1_duty_cmd` 为 F32 PARAM\|SCOPE，addr=0xAA46，group0/prescaler1；group0 前 8 条、group1 后 8 条，group1 prescaler=20 | **ENUM 链路通过（旧固件记录）**：分页、count、字段解码、描述符总数与 HELLO 一致。当前新 contract 已将描述符 entry 收紧为 28 octets；刷入新固件后需重跑该项并更新实测 build_hash |
| 2026-06-18 | CPU1/CPU2 RAM 构建与串口基线 | CCS MCP：`buildProject(cpu1)`、`buildProject(cpu2)` 均 outputMode=file；debug session 用 `cpu2` launch config，core index 0=CPU1、2=CPU2；串口 MCP 识别 `/dev/tty.usbmodemCL6500011` 与 `...14`，二进制探针独占 `/dev/cu.usbmodemCL6500011` | CPU1 RAM build log：`cpu1/RAM/cpu1_build.log`，CPU2 RAM build log：`cpu2/RAM/cpu2_build.log`，均 success/0 errors。HELLO 只在 `CL6500011` 有合法 COBS/CRC 响应；`CL6500014` 返回乱码 | RAM 构建与正确 VCP 端口确认通过。FLASH 构建/启动 smoke 未做 |
| 2026-06-18 | 验证 B/C — 当前 wire v6 / contract v8 ENUM 与参数事务 | 串口二进制探针复用 `tools/gen_vectors.py` 的 COBS/CRC-32C；B：HELLO + ENUM 8/8/1 + empty page；C：CAL_READ、CAL_WRITE stage、CAL_COMMIT、STATUS 对账、坏批次、恢复参数 | HELLO：wire=6、contract=8、build_hash=`0x26cd7396`、desc_count=17、tick_hz=20000、capabilities=0x7f。ENUM：17 条，空页 count=0；描述符 payload 尺寸 = 6+28×count；唯一 PARAM 为 `pwm1_duty_cmd`，全部 17 条均为 SCOPE。C：stage 后 `pwm1_duty_cmd` 仍 0.25；commit seq=1 后变 0.37；坏批次 `{pwm=0.61, g_nmi_cnt type=99}` → `cal_result=BAD_TYPE(1)`、`fail_idx=1`、pwm 保持 0.37；恢复到 0.25。补充全量负例：重复同 seq `CAL_COMMIT` 响应逐字节相同且 data 不变；同地址多帧 stage 最后值覆盖；count>16 返回 `BAD_PARAM`，清理 commit 被 CPU1 以 `BAD_ADDR` 拒绝 | **B/C 通过（RAM/115200）**。当前固件只有一个注册 PARAM，因此“两个以上注册 PARAM”无法实测；用注册 PARAM + 未注册但可写 RAM 地址覆盖 DWARF/RAM 地址语义 |
| 2026-06-18 | 验证 E — Scope Stream 与 overrun | 串口：OFF → BIND 2ch (`dbg_sine_10hz`, `pwm1_duty_cmd`) → STREAM prescaler=200 正常消费；随后 STREAM prescaler=1 暂停消费 0.65–0.75 s 制造 overrun；STREAM 中尝试 BIND | 正常流：收 5 个 block，`bind_seq=1`，`block_seq=0..4` 连续，`start_tick` 每块 +2000，`n_ticks=10`、`n_ch=2`、`stride=8`、`flags=0`，样本保持 F32 原生 octet。STREAM 中 BIND → ACK `BAD_STATE(3)`。过载：`overrun=819`（首次）/`1005`（H4），`remain=510`，恢复 BLOCK_REQ 得到断口后的 block | **E 通过（短跑）**：正常消费无 gap，过载只产生 producer overrun/序号断口，不阻塞 CPU1 |
| 2026-06-18 | 验证 G — 错误注入、拆包/粘包和重试 | 串口注入坏 CRC、非法 COBS、300 octet 无定界超长帧、拆包 STATUS、粘包 HELLO+STATUS、未知 msg=0x7A、重复同 seq BLOCK_REQ；同时读 CPU2 诊断计数 | 初跑发现粘包失败：第一帧响应后 TX pending，RX loop 继续消费第二帧，`v2k_process_encoded_frame()` 直接 return，随后清空接收帧。修复 `cpu2/v2k_sci_service.c`：TX 未完成时暂停解析后续 RX ring，保留后续帧。复测：坏 CRC/COBS/超长均无响应且随后 STATUS 恢复；拆包返回 STATUS；粘包返回 `(0x81,seq2010)` 与 `(0x82,seq2011)`；未知消息 ACK `UNSUPPORTED(4)`；重复 BLOCK_REQ 两次 payload 完全相同 `[0,1]`，下一新请求只推进到 `[2]`。最终 CPU2：`good_frames=118`、`bad_frames=3`、`rx_overflow=0` | **G 通过，且修复一个真实粘包丢帧 bug**。错误注入期间 CPU1 `isr_ovf_cnt=0`、`budget_violation_cnt=0` |
| 2026-06-18 | 验证 H — 115200 短跑性能隔离 | 每个场景前用 CCS debug 写 0 到 `g_v2k_isr_cycles_max/control_cycles_max/scope_cycles_max`；串口执行对应 host 行为后读 CPU1 Expressions | 场景读数（cycles）：OFF/no host = `938/728/58`；OFF + 250 ms STATUS（9 次，tick 22443898→22485498）= `938/728/58`；STREAM presc=200 正常消费 20 blocks/0 gap = `1315/728/455`；STREAM presc=1 停消费产生 `overrun=1005` = `1315/728/455`；CAPTURE_ARMED presc=1 record=200 → modes `[2,4]`，排空 20 blocks、`trig_tick=27502993` = `1459/728/598`。全部场景 `g_v2k_isr_ovf_cnt=0`、`g_v2k_isr_budget_violation_cnt=0`，最终 `g_v2k_tick=29901049`、`pwm1_duty_cmd=0.25` | **H 的隔离性短跑通过**：host 是否轮询、CPU2 串口/codec、STREAM 正常/overrun、CAPTURE 排空均未引入 CPU1 通信等待。未做 230400+ 阶梯与每档 30 min 长跑 |
