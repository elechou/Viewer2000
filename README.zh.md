# Viewer2000

**基于双核 C2000 (F28P65x) 的电机快速原型平台。**

![MCU](https://img.shields.io/badge/MCU-C2000%20F28P65x-CC0000)
![Host](https://img.shields.io/badge/host-Scope2000%20·%20Rust%20%2B%20egui-DEA584)
![License](https://img.shields.io/badge/license-MIT%20OR%20Apache--2.0-blue)
![Status](https://img.shields.io/badge/status-active%20development-yellow)

[English](README.md) · **中文** · [日本語](README.ja.md)

<table>
<tr>
<td width="50%"><img src="assets/Motor.jpg" alt="LAUNCHXL-F28P65X + BOOSTXL-DRV8323RX 驱动 PMSM 电机，配 AS5600 编码器"></td>
<td width="50%"><img src="assets/Screenshot.png" alt="Scope2000 上位机实时采集三相波形"></td>
</tr>
</table>

*左 — **LAUNCHXL-F28P65X + BOOSTXL-DRV8323RX** 功率级驱动一台 PMSM，配 AS5600
编码器。右 — 同一块板子跑开环 V/f，实时串流到 **Scope2000**：电角度、三相电流、
三相 PWM 占空比 — 每一条曲线都是 `control()` 里的一个普通 C 变量。*

如果你跟电机控制打过交道，大概知道这种体验：在 C2000 上跑一个 FOC，光是把启动、
时钟、引脚复用、ePWM→ADC→EOC 链、故障保护、双核通信这些东西配好，差不多就已经疯掉了。
然后你才刚开始写控制算法。观测也麻烦，CCS 的 Graph 窗口能用但不够灵活，
要看多路波形还得折腾半天。

Viewer2000 做的事情很简单：**把这些底层全接管掉**。你写一个 `setup()` 加一个
`control()`，操作一个扁平的 I/O 结构体，就是你的全部应用。与此同时，配套的
[Scope2000](https://github.com/elechou/Scope2000) 上位机能以控制环速率实时观察
并修改里面的每一个变量 — 不需要重新烧录，不需要配 CCS 窗口，变量名是从固件里自动带过去的。
你甚至可以直接在一台完全没有任何原项目代码的电脑上，插上板子直接用
[Scope2000](https://github.com/elechou/Scope2000) 对电机做控制。

## 亮点

- **Arduino 风格的控制环** — 整个应用就是 `setup()` + `control()`，操作一个
  全局 `v2k_io`。没有样板代码，不用碰寄存器。
- **实时观测，不用重新烧录** — 构建时自动把你的普通 C 变量名烤进固件，Scope2000
  直接按名字显示；pin、示波、甚至实时写入都行，改完继续跑。
- **天然确定性** — 平台拥有 ePWM→ADC→EOC 链，每个周期调用一次 `control()`，
  一个采样永远对应一个控制周期，不存在"这个采样到底是哪个周期的"这种问题。
- **保护你关不掉** — 硬件 Trip（TZ/CMPSS）和故障状态机在你的代码*下面*，
  不是在你的代码*里面*。你的 `control()` 写炸了，PWM 该关还是关。
- **双核隔离** — CPU1 跑控制，CPU2 跑通信。通信那边卡了、断了、炸了，
  控制环该跑还跑。
- **直接用 TI 官方生态** — 纯 C，C2000Ware、MOTORCONTROL-SDK、DCL 直接 include
  进来就能用，和 TI 官方例程的用法一模一样。
- **触发快照与 CSV** — 预触发历史捕获和极其便捷的 CSV 导出。

---

## 像 Arduino 一样写电机控制

你的全部应用就是两个函数加一个全局 I/O 结构体：

```c
#include "v2k.h"

// 普通 C 全局变量。构建时自动烤进固件，Scope2000 直接按名字显示 —
// 可以 pin、watch、示波，还能实时写入，不用重新烧录。
float    duty   = V2K_DUTY_NEUTRAL;   // 0.50 = 中性点
uint16_t ia_raw;

void setup(void) {
    // 每次复位后跑一次，在 control() 之前。
    duty = V2K_DUTY_NEUTRAL;
}

void control(void) {
    // 每个控制周期跑一次（比如 20 kHz），ADC 帧关闭的瞬间进来。
    ia_raw = v2k_io.adc.ia_raw;          // 一个相干的、时间对齐的采样
    v2k_pwm_apply(duty, duty, duty);     // 提交三相占空比
}
```

就这些。需要解释的东西只有三个：

- **`v2k_io.adc`** — 这个控制周期已经采完的 ADC 原始帧：相电流、相电压、
  母线电压，全在里面。
- **`v2k_io.sys`** — 平台 tick、调度标志（`V2K_DUE_1KHZ` / `V2K_DUE_100HZ`，
  用来知道"这个 tick 该不该跑慢速任务"）、状态、故障码。
- **`v2k_pwm_apply(a, b, c)`** — 提交三相占空比。没有隐式输出，平台不会在
  `control()` 返回后替你偷偷 apply 一把，输出永远是你自己的显式调用。

注意这个`control()`跟 Arduino 的 `loop()` 还是有一点点区别。
Arduino 的 `loop()` 就是一个无视一切的超级循环，只要没有中断进来就会一直抢占所有 CPU。
而我们的`control()`是20kHz的ISR中断主体，因为对电机控制来说，
"这个函数恰好就是中断服务程序"这件事才是真正值得关注的。

一个完整的例子 — 低能量开环 V/f，包含电流偏移校准和频率爬升 — 在
[`cpu1/app/user.c`](cpu1/app/user.c)，可以直接抄着改。

---

## 硬件

开箱支持的硬件是 TI 的公开评估套件（顶部图片里那套）：

| 部件 | 它是什么 |
|---|---|
| **LAUNCHXL-F28P65X** | 双核 C2000 LaunchPad（控制 MCU） |
| **BOOSTXL-DRV8323RX** | 三相栅驱 + FET 功率模块（DRV8323RS） |
| **AS5600** | 磁性绝对角度位置传感器 |

如果你手头的板子不一样，改的是板级层（L0）— 下面会讲。

## ⚠️ 安全

这个平台驱动的是真功率级、转的是真电机，能出大电流、能开关危险电压。

- 新代码**必须**先在低母线电压、低调制深度下跑，用限流电源，确认没问题再加能量。
- 远离旋转部件。把电机固定好。
- 内建保护（过流 Trip、故障锁存）是**安全网**，不是你可以不设限流电源的理由。
- 你的硬件、你的接线、你的电机 — 安全是你自己的事。

---

## 快速上手

你需要 [Code Composer Studio](https://www.ti.com/tool/CCSTUDIO)、TI 的
C2000Ware MOTORCONTROL-SDK、上面那套评估板，以及
[Scope2000](https://github.com/elechou/Scope2000) 上位机。

1. **编译。** 把 `cpu1/` 和 `cpu2/` 导入 CCS，两个都编译（快速迭代选 `RAM`
   配置，要脱机跑选 `FLASH`）。构建过程会自动跑 SysConfig、自动烤录变量描述符，
   不用你操心。
2. **下载运行。** 板载 XDS110 连上，两个核都加载，跑起来。
3. **看波形、调参数。** 打开 Scope2000，连上板子的 XDS110 虚拟串口。你的变量会
   自动冒出来 — pin 上、示波、实时改值，随你折腾。
4. **改成你自己的。** 去 [`cpu1/app/user.c`](cpu1/app/user.c) 里改 `control()`，
   重新编译加载 — 新变量下次枚举就会出现在 Scope2000 里。

完整的带硬件验证的启动流程在 [`BRINGUP.md`](BRINGUP.md)，各阶段设计文档在
[`docs/`](docs/)。

---

## 四层架构

Viewer2000 的代码分成四层。关键分界线在 **L1 和 L2 之间**：
平台管 L0–L1，你管 L2–L3。

| 层 | 在哪 | 干什么 |
|:--:|---|---|
| **L3** | `cpu1/app/` | `setup()` / `control()`，你的应用状态 |
| **L2** | 你自己的控制模块 | 控制数学和电机语义：C2000Ware MOTORCONTROL-SDK、DCL、手写、或 Simulink 生成的 C |
| **L1** | `cpu1/runtime/` | ISR 调度、控制状态机、保护策略、参数/描述符、RAM 示波 |
| **L0** | `cpu1/board/` | 启动、内存映射、引脚复用、ePWM/ADC/CMPSS/TZ/X-BAR、DRV8323RS 和 AS5600 驱动 — 基于 C2000Ware driverlib |

还有第二个核在旁边跑着：**CPU1 是控制域，CPU2 是通信域**（现在走 SCI，以后换更高速的
EtherCAT）。这么拆的目的是隔离故障域*和*时间域 — 通信那边如果挂了，控制环的确定性不受影响。

### 改哪一层？

正常情况下你只需要碰跟你目标对口的那一层：

| 你想干什么 | 改哪里 | 层 |
|---|---|:--:|
| 跑一个控制算法（FOC / V/f / 你自己的环路） | `cpu1/app/user.c` — `setup()` / `control()` | **L3** |
| 接一个具体的控制库进来 | 你自己的控制模块：引入 MOTORCONTROL-SDK / DCL，或者直接在 `app/` 里写 | **L2** |
| 调保护阈值，改高级时序/调度 | `cpu1/runtime/` | **L1** |
| 换板子、改接线、换传感器 | `cpu1/board/`（板级配置） | **L0** |

大多数用户只在 L2–L3 里活动。L0 在板子或接线变了的时候做一次性适配，L1 基本不用动。

---

## 这个平台适合谁

1. **研究人员** — 只想专注控制算法本身，不想花时间在芯片启动和外设配置上。
2. **学生** — 在学 FOC 或者电机控制，想亲眼看到自己写的数学公式在屏幕上驱动
   真实的电流波形。
3. **嵌入式初学者** — 想在 C2000 上驱动电机，但不想一上来就被初始化代码、
   寄存器配置和保护管道淹死。
4. **需要快速评估的人** — 换个控制律 A/B 对比一下、评估一台新电机 —
   当天就要看到结果。
5. **老师和助教** — 需要一个可复现的教学平台，核心代码短小可读，
   每个实验台上都一样。
6. **算法工程师** — Simulink 生成的或手写的控制 C 代码，送上产品 BSP 之前
   先在真实板子上跑一遍。

## 这个平台不适合谁

1. **想深入学 C2000 本身 / C2000Ware SDK 的人。** 这一点说实话还在推。
   我想尽量保持和 C2000Ware、MOTORCONTROL-SDK 的兼容，让你能像 TI 官方例程
   一样调用官方 API。但平台运行时**强制了一些安全保护**，有些情况下官方 API 会
   "不生效" — 因为时序、输出释放、中断、保护的所有权在平台手里。
   这个差距在持续缩小，兼容性一直在改善。
2. **STM32 或其他非 C2000 用户，非双核的也不行。** 平台从头到尾假设的是
   双核 C2000 的故障域/时间域分离架构。因为兼容别家的DSP可能整个项目结构要大改，
   所以可能今后开新的 DSP 坑的时候也会倾向于新建 repo 。
3. **要做电机*产品*的人。** 平台能帮你把控制算法跑通、验证完 — 但控制算法往往
   反而是产品中*最小*的部分。平台替你管着的那些 IO、初始化、BSP，恰恰是做产品时
   你得全部重新来过的东西。这个迁移会超级痛苦。

---

## FAQ

**为什么 CPU1 管采样 — 不能让我自己读 ADC 吗？**
> 因为**时序统一性**。平台拥有 ePWM→ADC SOC→EOC 链，等 ADC 帧采完了才调你的
> `control()`。每个采样固定在周期的同一个时刻：20kHz 的控制周期的**最开始**。
> 同时我们的`control()`实在**完成采样后立即进入的**，
> 也就是你永远可以相信平台提供的`v2k_io.adc`接口，它是一个在固定周期上做 ADC 采样
> 并且已经确定完成了本拍 ADC 转换的数值。
> 你的控制律看到的永远是一个相干的、时间对齐的快照 — 而不是"在循环的某个随机位置
> 读了一下 ADC"。

**CPU1 算力那么紧，为什么示波采样也放在 CPU1 上，而不是丢给 CPU2？**

> 因为采样必须**时间确定**。如果让 CPU2 跨核去读 CPU1 的变量，没有任何机制能
> 保证每次读取恰好在上一个控制 tick 之后一个中断的位置。CPU2 可能漏一个 tick、
> 重复读一个 tick，更糟的情况是把两个相邻 tick 的值混在一起当成一个 tick 交给
> 上位机。对于要精确观测控制结果的人来说，这不能接受。
>
> 在 CPU1 的控制 ISR 内部采样，所有通道天然同 tick：一个采样，一个控制周期，
> 干净利落。
>
> 当然 CPU1 的周期确实很紧。超高速运行（100 kHz 及以上）的时候，能协作拆分的
> 示波工作我们计划往 CLA 或者 CPU2 搬。如果你不需要 tick 级精度但需要 CPU1 的最大算力，
> 今后可能也有一条路径可以把示波整个从 CPU1 上移走。

**Arduino 也是 C++ 啊 — 为什么这里用纯 C？**

> 因为在这个场景下 C++ 的好处还不够抵消它带来的麻烦：
>
> - **可观测性。** 我们的主要调试界面是 CCS Expressions/Graph 和 Scope2000 的
>   监视树，它们直接读扁平 C 结构体。C++ 的 name mangling、模板、private 成员
>   在 map 文件和 watch 窗口里全是障碍。
> - **直接用官方代码。** C2000Ware、MOTORCONTROL-SDK、DCL 全是纯 C。保持 C
>   就是直接 include 进来就用，和 TI 官方例程一模一样 — 不用 `extern "C"` 包一层，
>   不用跟混合编译的链接问题纠缠。
> - **CLA 选项。** CLA 只接受 C 子集。保持 C 就保留了将来把快速环路搬到 CLA 上
>   的可能性。
> - **受众。** 目标用户 — 初学者和电机方向的研究人员 — 日常写 C，
>   并且今后工作估计也都是在跟 TI 的官方 C2000Ware 打交道。
>
> 如果哪天平台的复杂度真的长到 C 撑不住了，这个决定会重新审视。但目前 C 就是
> 让 TI 官方生态只差一个 include 的最短路径。

---

## 当前状态

Viewer2000 还在**活跃开发**中，目前只跑在一套硬件上。已经端到端跑通的部分：
双核启动和 IPC、硬件保护、多速率调度、RAM 示波、原子参数事务、SCI 串流到
Scope2000，以及一个开环 V/f 的首次旋转应用。

接下来要做的：

- 基于同一 `setup()`/`control()` 接口的闭环 FOC 用户示例；
- **EtherCAT** 传输层（协议在管道里面，换物理层不影响上层）；
- 加固 L0 板级层，让换一块新板子只需要改一层。

---

## 文档

- [`docs/wire-spec.md`](docs/wire-spec.md) — 上位机↔固件线协议（权威定义）
- [`docs/board-portability.md`](docs/board-portability.md) — 板级层与移植模型
- [`docs/protection-architecture.md`](docs/protection-architecture.md) — 保护架构
- [`docs/`](docs/) — 各阶段设计文档
- [`BRINGUP.md`](BRINGUP.md) — 带硬件验证的启动日志

## 配套上位机：Scope2000

上面截图里的就是 [**Scope2000**](https://github.com/elechou/Scope2000)，
Rust + egui 写的上位机（独立仓库）。它在运行时自动枚举你烤进去的变量，串流
`ScopeBlock` 做实时监视和触发快照，画波形，导出 CSV — 不用解析 `.out`，
不用重新烧录，连上就能看、就能调。

## 仓库说明

本仓库（[`elechou/Viewer2000`](https://github.com/elechou/Viewer2000)）**只有
固件** — 也就是烧进 F28P65x 两个核里的全部代码。上位机在兄弟仓库
[`elechou/Scope2000`](https://github.com/elechou/Scope2000)。

线协议的定义在 [`docs/wire-spec.md`](docs/wire-spec.md)、
[`contracts/`](contracts/) 的头文件和
[`contracts/vectors/`](contracts/vectors/) 的黄金测试向量里。
老设备兼容由独立的进程外桥接负责，不会动 Viewer2000 本身的协议和数据通路。

## 许可证

[Apache License 2.0](LICENSE-APACHE) / [MIT](LICENSE-MIT)

## 致谢

基于 TI 的 C2000Ware、MOTORCONTROL-SDK 和 DCL 构建，示例板为 TI 官方 F28P65x LaunchPad + DRV8323 BoosterPack。

上位机[`Scope2000`](https://github.com/elechou/Scope2000) 基于 [egui](https://github.com/emilk/egui)构建，
风格灵感来自 [rerun](https://github.com/rerun-io/rerun)。
