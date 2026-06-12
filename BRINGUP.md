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
