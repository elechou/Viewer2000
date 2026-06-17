# Phase 1 — CCS / SysConfig 手工配置清单

固件代码、linker .cmd、共享平面、契约断言我已写好；本文档是你在 CCS GUI 里
要做的全部事情。分工原则：**SysConfig/工程设置/调试会话 = GUI（有引擎校验），
linker/C 代码/契约 = 文本（有断言校验）**。

我已完成的部分（仅供对照，不需要你动）：

| 产物 | 内容 |
|---|---|
| `cpu1/cpu1.c` | boot master：GS4 划转、描述符表发布、引导 CPU2、IPC ping-pong、心跳监视、红灯 1 Hz |
| `cpu2/cpu2.c` | sync 会合、契约版本握手、心跳、pong 应答、绿灯 2 Hz |
| `common/v2k_planes.h` | GS0/GS4/MSGRAM 区块聚合 struct + 对侧只读指针 |
| 四份 `*_lnk_*.cmd` | Phase 3 后：`v2k_gs0_cpu1`→GS0 前半、`v2k_scope_ring`→GS0 后半+GS1-3；GS4 切出 0x200 词的 `v2k_gs4_cpu2` |
| `cpu*/v2k_check_contracts.c` | Phase 0 契约静态断言在 cl2000（CHAR_BIT=16）一侧编译生效 |

---

## 0. 修复 cpu2 的 .syscfg —— ✅ 已完成

（原为 0 字节空文件；现已带正确的 board/device/context=CPU2 头，无需再动。）

## 1. cpu1 的 SysConfig：两个 LED 实例（已配大半，剩 Core Select 对调）

你已用 board components 的 LED 模块配好 `LED_CPU1`（LED4 红）与 `LED_CPU2`
（LED5 绿），$hardware 绑定 + 初值 1 都正确。**剩一处要修：Core Select 配反了**——

| 实例 | 现状 | 应为 |
|---|---|---|
| `LED_CPU1`（红，CPU1 用） | Core Select = CPU2 ← 错 | **CPU1**（默认） |
| `LED_CPU2`（绿，CPU2 用） | 未设（默认 CPU1） | **CPU2** |

不修的症状：红灯永远不亮（CPU1 写不动被划给 CPU2 的数据寄存器）；绿灯靠
`cpu1.c` 里的 `GPIO_setControllerCore` 兜底能亮，但归属分配的正式位置在这里。

说明：

- 两个 LED 都是**低电平点亮**（这块板和多数 LaunchPad 相反），初值 1 = 上电灭；
- board components LED 模块生成的 board.h 宏带 `_GPIO` 后缀
  （`LED_CPU1_GPIO` / `LED_CPU2_GPIO`），`cpu1.c` 已按此引用——**实例名别再改**；
- 避开 GPIO42/43（XDS110 串口背通道，Phase 3.5 要用）。

## 2. cpu2 的 SysConfig

Phase 1 **不需要添加任何模块**（CPU2 的 LED pad 配置由 CPU1 完成，CPU2 只写
数据寄存器，不经 sysconfig）。完成第 0 步的修复保存即可。

## 3. 构建配置选 RAM

flash bank 划分是未决项（AGENTS.md），Phase 1 只用 RAM 构建：

1. 两个工程分别：右键 → Build Configurations → Set Active → **RAM**
   （你之前构建过 FLASH 配置，记得切）；
2. 先 build cpu1，再 build cpu2，预期 0 error；
3. 如果 `v2k_check_contracts.c` 报 *size of array ... is negative* —— 契约断言
   在 cl2000 上不成立，是真问题，把完整报错发我；
4. 如果报 `LED_CPU1_GPIO`/`LED_CPU2_GPIO` 未定义 —— 实例名没对上，检查第 1 步；
   若名字确认无误，打开生成的 `cpu1/RAM/syscfg/board.h` 看实际宏名告诉我。

## 4. 双核调试会话（顺序关键，别换）

**前置（一次性）**：如果片上 flash 烧过旧固件，先擦掉——任何意外复位（看门狗、
NMI 看门狗、XRS）都会让 CPU1 从 flash 启动跑进旧固件，现象是"跑飞到无源码
地址 + CPU2 被按回 reset"（BRINGUP.md 2026-06-12 实测，PC 落 0x081A3A）。
操作：CPU1 连接后 Tools → On-Chip Flash → 勾 Bank0–2 → Erase。
bring-up 期间把 S3 左拨码拨 0（等待启动）作双保险也推荐。

1. Launch 目标配置（launch 后手动 connect，**不要直接 Debug As 自动全连**——
   自动流程会在 CPU1 还停在 main 时就去连 CPU2，而 CPU2 的复位要等 CPU1 跑过
   `Device_bootCPU2` 才解除，于是 CPU2 侧报一串
   *"Device is held in reset"* / GEL / load 失败。见到这个症状＝顺序错了，
   救法：Resume CPU1 → 再 Connect CPU2 → Load → Resume，不必重启会话。
   一劳永逸：Debug Configuration 里关掉 CPU2 核的自动程序加载）；
2. **Connect CPU1 → Load `cpu1.out` → Resume**。
   CPU1 会跑到 `IPC_sync` 里阻塞等 CPU2——此时红灯不闪、看似卡死，**正常**；
3. **Connect CPU2 → Load `cpu2.out` → Resume**。
   ⚠️ 顺序的原因：GS4 上电属 CPU1，CPU1 跑过 `MemCfg_setGSRAMControllerSel`
   之后 GS4 才归 CPU2；先加载 CPU2 的话，.out 往 GS4 的装载会失败或校验不过；
4. 预期：**红 LED4 闪 1 Hz，绿 LED5 闪 2 Hz**；
5. 任一核停在 `ESTOP0` 死循环 = `v2k_assert_layout` 自检失败（.cmd 与
   v2k_memmap.h 失配）或 CPU2 契约版本握手失败（看 `g_handshake_state`），发我。

## 5. Expressions 观测清单（开 Continuous Refresh / 实时模式）

| 会话 | 表达式 | 预期 |
|---|---|---|
| CPU1 | `g_ping_cnt` | 持续递增（≈1 kHz） |
| CPU1 | `g_cpu2_alive` | 1；halt CPU2 后 ~1 s 内变 0（顺手验证心跳监视） |
| CPU1 | `g_v2k_msg_1to2.cpu1_status.heartbeat` | 持续递增 |
| CPU1 | `g_v2k_gs0.desc_table.hdr.magic` | `0x564B4454`（"VKDT"） |
| CPU2 | `g_pong_cnt` | 持续递增，与 g_ping_cnt 同步 |
| CPU2 | `g_handshake_state` | 3（运行中） |
| CPU2 | `g_v2k_msg_2to1.cpu2_status.heartbeat` | 持续递增 |

加分项（基本规则 1 的实测）：Halt CPU2 → 红灯应照闪、`g_cpu2_alive` 变 0、
`status_flags` 置 `CPU2_LOST`；Resume CPU2 → 自动恢复 1。这条做了就记进 BRINGUP。

## 6. 验证完成后

- 实测结果记入 `BRINGUP.md`（Phase 1 条目模板已建好）；
- 按工作流约定：验证过的节点 commit，硬件验证节点打 tag（建议 `phase1-skeleton`）；
- Phase 2 的第一件事是 **EPWM TBCTL.FREE_SOFT**（调试器 halt 时 PWM 行为），
  Phase 1 没有 PWM 不涉及，但别忘。
