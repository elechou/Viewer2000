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

- [x] contract version 2；DAQ_CTRL 12/14-octet 兼容向量通过 host 检查
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
- [ ] CCS Project：CPU1/CPU2 器件从 DK6 修正为 `TMS320F28P650DK9`
- [ ] SysConfig：新增 CPUTIMER1；CCS pre-build 接入 git hash 生成器
- [ ] CPU1/CPU2 RAM 与 FLASH 配置 `buildProject` 0 error
- [ ] 20 kHz / 100 kHz 实物验收、Phase 1/2 回归与 Silicon Real-time Mode
- [ ] 验收通过后创建 `phase3-executor-observability` tag

未完成项不得用软件自检替代实物结论，也不得提前打硬件 tag。
