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

## Phase 2 — 时基证明 + 保护（进行中，2026-06-12 开工）

操作步骤见 `docs/phase2-bringup.md`。验证项清单：

- [x] Device Support 迁移：两工程生成 device.c/h 取代模板、时钟树 EPWMCLKDIV=/1（errata 警告消失）、模板 device.c/h 排除
- [x] 迁移后 Phase 1 全项回归（双灯/握手/ping-pong/心跳/NMI 计数）
- [ ] SysConfig 配置完成并 review（EPWM1/ADCA/DACA/XBAR/GPIO 五模块）
- [ ] cpu1 工程 0 error（新增 v2k_timebase.c / v2k_fault.c 自动入编译）；上电不停 v2k_tb_check 的 ESTOP0（syscfg 与 C 契约对账通过）
- [ ] g_v2k_tick ≈ 20000/s；g_v2k_adc_a0 ≈ 2048（DACA 中位经 ADC 回读）；ovf 恒 0
- [ ] 示波器：EPWM1A/B 互补 + 1 µs 死区（实测值：____）
- [ ] 抖动：CH1(PWM) 触发余辉下 CH3(ISR 探针) 边沿散布 = ____ ns @20k / ____ ns @100k
- [ ] ISR 耗时（CH3 脉宽）= ____ ns
- [ ] halt CPU1 → 输出立即安全（TZ6 CBC）、resume 自动恢复 —— FREE_SOFT 决策实证
- [ ] 命令全序列：START/STOP/trip→FAULT(fault_code=1)/带源 CLEAR 重入/清源 CLEAR→IDLE/BAD_STATE
- [ ] 硬件 trip 延迟：GPIO3↓ → EPWM1A↓ = ____ ns（规则 2「不经过 CPU」实证）
- [ ] 100 kHz 压测：tick ≈ 1e5/s、ovf==0、波形完好（两档数据都记录）

记录区：

| 日期 | 验证项 | 方法 | 实测 | 结论 |
|---|---|---|---|---|
| 2026-06-12 | Device Support / 时钟树迁移回归 | 两工程 syscfg 加 Device Support，生成 device.c/h 取代手写模板（含 codestartbranch）；时钟树 SYSCLK=200MHz、EPWMCLKDIV=/1；系统工程 system.xml 接到 cpu1/cpu2（@match，活动配置切 RAM 治好"强制回退 FLASH"）；Phase 2 固件先 stash 隔离，纯迁移树 clean build 上板 | Phase 1 全项回归通过：双灯 1/2 Hz、ping/pong 同步递增、握手=3、心跳监视正常、窗口期 g_nmi_cnt=1（CPU2WDRS 照旧）；cpu1 侧 errata 警告消除，cpu2 侧"clocking functions"黄警告=规则 5 预期常驻 | **设备初始化与时钟收敛到 syscfg 单一来源**；EPWMCLKDIV=/1（errata）由时钟树生成代码落实，后续 v2k_tb_check 读回断言 |
