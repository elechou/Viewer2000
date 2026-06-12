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
| 四份 `*_lnk_*.cmd` | `v2k_gs0_cpu1`→GS0、`v2k_gs13_ring`→GS1-3、GS4 切出 0x200 词的 `v2k_gs4_cpu2` |
| `cpu*/v2k_check_contracts.c` | Phase 0 契约静态断言在 cl2000（CHAR_BIT=16）一侧编译生效 |

---

## 0. 修复 cpu2 的 .syscfg（必做）

`cpu2/sysconfig_cpu2.syscfg` 目前是 **0 字节空文件**（重命名后没保存过？），
缺少 device/context 头，构建时 SysConfig CLI 大概率报错。

1. 在 CCS 里双击 `sysconfig_cpu2.syscfg` 用 SysConfig 打开；
2. 确认 **Device = F28P65x，Context = CPU2**，然后保存（头注释会自动补全）；
3. 如果空文件打不开：用 CCS 文本编辑器把 `cpu1/sysconfig_cpu1.syscfg` 的头部
   注释块拷过来，把其中两处 `"CPU1"` 改成 `"CPU2"`，保存后再用 SysConfig 打开。

## 1. cpu1 的 SysConfig：添加两个 GPIO 实例

打开 `cpu1/sysconfig_cpu1.syscfg`，添加 GPIO 模块实例两个。
**实例名必须和下表完全一致**——`cpu1.c` 引用的是 board.h 按实例名生成的宏。

| 配置项 | 实例 1 | 实例 2 |
|---|---|---|
| Name | `LED_CPU1` | `LED_CPU2` |
| 引脚 | **GPIO12**（板上 LED4 红） | **GPIO13**（板上 LED5 绿） |
| Direction | Output | Output |
| Output Type | Push-pull | Push-pull |
| Write Initial Value | 勾选，值 = **1** | 勾选，值 = **1** |
| **Core Select** | CPU1（默认） | **CPU2** ← 关键 |

说明：

- 两个 LED 都是**低电平点亮**（这块板和多数 LaunchPad 相反），初值 1 = 上电灭；
- 如果引脚选择支持按 Hardware 过滤，直接选 "LaunchPad LED Red / Green" 更稳；
- `LED_CPU2` 的 Core Select = CPU2 是「pad 配置归 boot master、数据寄存器归
  使用核」的正式落位（SysConfig 会在 board.c 里生成 `GPIO_setControllerCore`）。
  `cpu1.c` 里另有一行兜底调用，所以这项漏了也能跑，但请配上；
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
4. 如果报 `LED_CPU1`/`LED_CPU2` 未定义 —— 实例名没对上，检查第 1 步；若名字
   确认无误，打开生成的 `cpu1/RAM/syscfg/board.h` 看实际宏名告诉我。

## 4. 双核调试会话（顺序关键，别换）

1. Launch 目标配置（launch 后手动 connect，不要直接 Debug As 自动全连）；
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
