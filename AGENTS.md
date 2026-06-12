# AGENTS.md — Viewer2000（C2000 RCP 平台）

## 项目定位

基于 TI C2000 **F28P65x（双 C28x 核, 200MHz）** 从零构建一个快速控制原型平台（RCP, Rapid Control Prototyping），面向通用电力电子快速控制原型场景。

**核心认知：本项目的产品是平台本身，不是电机控制器。** 平台 = 确定性控制任务调度 + 外设抽象 + 参数整定/数据示波 + 保护。FOC 电机控制只是跑在平台上的第一个示例应用，兼做平台验收测试。

**性能锚点：20–100 kHz（待定）× 8ch × f32 无损，0.64–3.2 MB/s**。这个数字以排除法决定了物理层选型（见「通信架构」）。

**仓库边界：本 repo 只做固件**（烧进 F28P65x 的部分）。上位机复用既有 Rust + egui 前端，其通讯层拆分为 `DataSource` trait：`SimSource`（L2 FFI 仿真）/ `CompatSource`（可选兼容协议）/ `V2kSource`（本项目）。前端与中立数据模型共用一套代码持续演进，本项目对接 V2k 一侧。

本项目的设计原则：**保护先行、可观测性先行、平台与应用分离**。

## 硬件

| 项 | 内容 | 状态 |
|---|---|---|
| 板子 | LAUNCHXL-F28P65X | 在手 |
| MCU | TMS320F28P650DK9，双 C28x @ 200MHz，两核 ISA 完全相同（共享 struct 无 ABI 问题）；双 CLA（每核一个） | 已确认 |
| 调试器 | 板载 XDS110（带虚拟串口 VCP → 早期 SCI 链路零额外硬件） | 前中期主人机接口 |
| 片上通信 | SCI / MCAN（板载 CAN 收发器）/ FSI / **EtherCAT ESC（板载双 PHY + RJ45）**。**无片上 USB、无以太网 MAC** | 已确认 |

前期参数整定、状态机指令、变量观测通过 CCS 实时模式（CPU 不停机读写内存）+ Expressions / Graph 窗口完成。但与前身项目不同：**线上协议与四个共享内存接口同在 Phase 0 定稿**（上位机需求与前端基础已知），最小 SCI 数据泵提前到 Phase 3.5，不写一次性 ASCII 解析器。

## 通信架构

物理层由 100 kHz × 8ch 全速率需求排除法决定：

| 链路 | 现实吞吐 | 结论 |
|---|---|---|
| SCI（XDS110 VCP） | 几 Mbps | bring-up 哑泵专用 |
| CAN-FD | 有效 2–3 Mbps | 排除 |
| W5500/SPI 以太网桥 | 个位数 Mbps | 排除 |
| FSI | 50–100 Mbps 但 PC 不认，需自制桥 | 排除 |
| **EtherCAT** | 100 Mbps 线速，实用 8–10 MB/s | **满足要求，采用** |

**阶梯只有两级：SCI 哑泵（Phase 3.5，验证接口与架构）→ EtherCAT（Phase 6，最终链路）。** 协议（block、描述符）活在管道里面，换物理层只换管道。

EtherCAT 要点：

- **最小可用集**：free-run 模式（**不用 DC**——时间戳是 block 头里的 ISR tick，与 EtherCAT 时钟体系无关）、不用 EoE/FoE、CoE 砍到最简。SSC 协议栈 / ESI / 状态机是**一次性工作**：打通 OP + PDO 映射后不再触碰，后续加通道、改协议都在管道内部进行。
- **设计点**：block = 50 tick × 8ch × int16 = **800 B**；master 2 kHz 循环，PDO 每周期装 **0–2 个 block + count 字段**（有数据就多拿，吸收两端晶振 ppm 偏差，见示波平面）；单标准帧装载（过程数据上限 ~1486 B）；线缆利用率平均 ~15%、峰值 ~30%。三缓冲 SyncManager 按 2-block PDO 计 3 × 1.6 KB = 4.8 KB ESC RAM——逼近 8 KB 量级预算，**ESC RAM 核对 TODO 的优先级因此提高**。
- **余量**：8→16ch 基本白送（2 kHz × 1.6 KB 或 4 kHz × 800 B）；24–32ch 为工程极限区（ESC RAM 与 master 抖动同时收紧）。更多名义通道靠**多速率通道组**与 **snapshot 模式**，不靠蛮力。CPU1 生产侧紧张时可把量化打包挪给 CPU2（片内环加倍换 ISR 周期）。
- **PC master 纯软件 + 普通网卡**：首选 ethercrab（纯 Rust，直接长在上位机里），SOEM 作 C 参考。Linux + RT 优先级线程跑 2 kHz；Windows 跑 master 是受罪，提前认知。
- 全部工作在 CPU2，与控制核开发并行——EtherCAT bring-up 调的是"管子通不通"，接口对不对在 Phase 3.5 已由 SCI 泵验证过，两种失败模式不叠加。

## 架构

### 双核分工

- **CPU1 = 控制核**：boot master，负责外设/内存归属分配、引导 CPU2、NMI/trip 配置。拥有全部功率级外设（ePWM / ADC / eQEP / CMPSS / SDFM）。跑 ISR 执行器 + 控制应用。**无 printf、无通信栈。**
- **CPU2 = 通信核**：拥有通信外设（SCI / MCAN / EtherCAT ESC）。跑参数服务、示波数据泵、心跳监视；Phase 3.5 起跑 SCI 哑泵，Phase 6 起跑 EtherCAT 链路与固件升级。

核分离的本质是**隔离故障域 + 隔离时间域**：控制域的确定性不被通信域的抖动/断连污染；保护在两个域之下的纯硬件层。

驱动方式对称：**CPU1 时间驱动，CPU2 事件驱动。** CPU1 全片唯一时基：ePWM 主定时器（同步组锁相）→ ADC SOC → **EOC 中断**进控制 ISR（中断源挂 EOC 不挂周期事件，进 ISR 时数据已就绪），慢环软件分频；ISR tick 是**全平台唯一的时间**，示波时间戳、分频、心跳纪元全部由它派生。CPU2 不设自己的节拍、不采样任何东西——它消费的全是 CPU1 盖好时间戳的数据；其负载（环有新 block、SM 被 master 读走、mailbox 来命令、SSC 主循环）全是事件，结构 = ISR 收事件 + 超级循环干活。它的本职是 CPU1 晶振与 PC 时钟两个外部时钟域之间的**弹性联轴器**——给联轴器装节拍器没有意义，上 RTOS 同理（规则 6 不开口子）。

### 分层

| 层 | 内容 | 归属 |
|---|---|---|
| L3 | 用户控制应用（FOC demo 等） | CPU1 |
| L2 | 控制库：Clarke/Park、PI、PLL、滤波、斜坡 — **纯可移植 C（C99），PC 上可单元测试/仿真** | CPU1 |
| L1 | 平台核心：ISR 调度执行器、保护管理器、参数注册表、RAM 示波器 | CPU1（消费端可在 CPU2） |
| L0 | 驱动层，基于 C2000Ware driverlib | 各核各自 |

**ISR 所有权归 L1，不归用户。** 用户代码以 `user_step(in, out)` 回调形式被执行器调用：进 = 本拍物理量，出 = 本拍占空比。ISR 固定序列 = `plat_acquire → user_step → plat_apply → scope_sample_all（遍历描述符表）→ trigger_eval → g_tick++`，示波采样与触发判定在 epilogue 由平台完成——用户仅在 init 注册，**没有不被采样的路径**（规则 7 的机制化）。约束随附：可观测量须为可寻址静态量（非栈上局部变量）；每快通道约 5 周期/tick 计入 ISR 预算。这是规则 4 在时间维度的镜像：**L3 不碰寄存器，也不碰时钟。**

### 语言策略

**目标侧固件（两个核、L0–L3 全部）纯 C，C99 + stdint 固定宽度类型，不用 C++。** 理由：

- 主 HMI 是 CCS Expressions/Graph：扁平 C struct 直观可读；C++ 名字修饰、模板、私有成员在 map 文件和 watch 窗口里全是阻力（cl2000 的 C++ 支持本身也是二等公民）；
- CLA 编译器只接受 C 子集：L2 保持纯 C，将来才有把快环搬上 CLA 的选项（每核各有一个 CLA）；
- 共享内存 struct 必须是固定布局 POD，要被 CPU1 / CPU2 / 上位机三方读懂，C 是最大公约数；
- C2000Ware / MotorControl SDK / DCL 全是 C，平台目标用户（新手）的语言也是 C；ODrive 的重 C++ 是其学习门槛的一部分，不复制；
- 规避全局对象构造顺序、隐式构造等裸机 C++ 坑。

弥补 C++ 缺失的三条编码约定（对齐 TI DCL 风格）：多实例 = struct 实例 + 操作函数（`pi_update(&pi_vel, err)`）；命名空间 = 模块前缀；init 完成后禁止动态分配。

**PC 侧统一用 Rust + egui**：L2 控制库编译成动态库，经 FFI（bindgen）接入 Rust 做单元测试与电机模型仿真，egui_plot 看波形。仿真宿主和上位机是同一个程序的三个数据源（`SimSource` / `CompatSource` / `V2kSource`）。

### 四个共享内存接口（先于一切代码定稿）

1. **描述符表**：控制核启动时把可调参数/可观测信号注册成表 `{名字, 类型, 地址, min/max, 换算系数, 降采样比, 所属任务}` 写入共享 RAM；通信核与上位机**枚举**该表，不预先知道任何变量。要点：
   - **示波通道默认 int16 + 描述符内换算系数**，host 端还原物理量——ADC 本就 ≤16 位，裹 float32 上行没有意义，带宽白白翻倍；
   - **降采样比字段实现多速率通道组**：8 个快通道（100 kHz）+ N 个慢通道（1 kHz 的温度/母线/状态量，带宽零头）；
   - 表头携带 **firmware build hash**：host 重连检测到变更即强制重新枚举，杜绝拿旧表读新固件。
2. **参数平面**（主机→控制）：双缓冲。任何写入先进 shadow 区 → 置 commit 标志 → 控制 ISR 在每周期固定安全点整组交换。解决多参数非原子写问题；XDS100/CCS 调参同样戳 shadow 区。
3. **示波平面**（控制→主机）：无锁 SPSC 环形缓冲。控制 ISR 是唯一生产者（**采样在 ISR 上下文，所有通道天然同拍**），通信核/后台循环是唯一消费者。**双模式 + block 化**：
   - **Live 模式**：降采样连续流，在线监控/调参看趋势；
   - **Snapshot 模式**：全速率采进环形缓冲，触发后冻结、慢速排空——环形结构天然支持 **pre-trigger**（查瞬态故障的关键）。触发判定（变量过阈值、状态机事件）在控制 ISR 内完成。CCS Graph 是 snapshot 模式的第零个消费者；
   - **帧 = block**：N tick × M ch + 头部（起始 tick、序号、通道组 id）。N 是参数：SCI 用小 N，EtherCAT 用 N=50（=800 B）。host 凭序号检测丢块——丢块画断口，控制核照跑；
   - **PDO 每周期装 0–2 个 block + count 字段**：master 有数据就多拿。两端晶振永远差几十 ppm，环形缓冲会缓慢涨/空，长时间录盘必撞——带宽余量正好花在这；序号机制顺便覆盖去重与断口检测；
   - 环形缓冲尺寸即 master 端抖动吸收余量（几十 KB ≈ 几十 ms @ 1.6 MB/s）。
4. **命令/状态平面**：状态机请求（启动/停止/清故障）走 IPC mailbox；两核互发心跳。

**线上协议 = 四个共享接口的序列化视图**，与接口同在 Phase 0 定稿：枚举描述符表 = 一条请求，参数提交 = 一个事务，示波流 = block。内存布局与线上格式是同一个数据模型，不允许各长各的。

用户代码 API 草案：

```c
// 用户无需声明监控变量，platform自动注册可用参数表

// The only user-owned slot, called by the L1 executor every tick
void user_step(const plat_in_t *in, plat_out_t *out);
```

### 跨 repo 接口管理（固件 ↔ 上位机）

- **禁止 struct memcpy 上线**（16-bit char + 端序），两侧各写显式序列化器；
- 唯一基准 = wire spec 文档 + **golden test vectors**（十六进制帧样本）：固件序列化器在 PC 上编译跑单元测试，Rust 解析器跑 conformance test，双端对同一组 vectors；
- 描述符表运行时枚举 → 两 repo 无需 codegen，天然解耦。

### 架构图

```
                ┌──────────────────────────────────────────────┐
                │  上位机 = Rust + egui 前端                     │
                │  DataSource: Sim / Compat / V2k(本项目)       │
                └────┬───────────────┬────────────────┬────────┘
                     │ JTAG/CCS      │ SCI (XDS110     │ EtherCAT
                     │ (Phase 1–)    │  VCP, Ph 3.5)   │ (ethercrab, Ph 6)
   ┌─────────────────┴────────────┐  ┌┴────────────────┴────────────┐
   │ CPU1 — 控制核                 │  │ CPU2 — 通信核                 │
   │  L3 用户控制应用               │  │  EtherCAT 数据泵 (Phase 6)    │
   │  L2 控制库 (可移植/PC可测)     │  │  SCI 哑泵 (Phase 3.5)         │
   │  L1 执行器: ISR调度+保护       │  │  参数服务 / 心跳监视/升级       │
   │  L0 ePWM ADC eQEP CMPSS      │  │  L0: SCI / MCAN / ESC(PDI)   │
   └──────────────┬───────────────┘  └──────────────┬───────────────┘
                  │      共享内存接口 (GSx RAM + MSGRAM)│
                  │  ┌──────────────────────────────┐ │
                  └──┤ 1. 描述符表 (参数/通道/build哈希)├─┘
                     │ 2. 参数平面: 双缓冲 + commit   │
                     │ 3. 示波平面: SPSC 环 + 双模式   │
                     │ 4. 命令/状态: IPC mailbox+心跳 │
                     └──────────────────────────────┘
      硬件保护链 (CMPSS → ePWM X-BAR → Trip Zone): 不经过任何 CPU，更不经过核间
```

## 基本规则（所有代码必须遵守）

1. **控制核在任何路径上都不阻塞等待通信核。** IPC 满则丢、示波缓冲满则覆盖或停采、链路丢块则丢块，控制 ISR 照跑。CPU2 死 → 电机继续稳定运行，只是"失联"；CPU1 死 → 关 PWM 靠硬件 trip 和各核独立看门狗，不靠 CPU2。
2. **保护是纯硬件链路**：CMPSS → ePWM X-BAR → Trip Zone 关 PWM，不经过任何 CPU。**上功率之前保护必须就位。**
3. **所有核间接口可单核运行**：编译开关可把消费端放回 CPU1 后台循环，调试时退回单核排除核间因素。移到 CPU2 是搬迁，不是重设计。
4. **L2/L3 不碰寄存器**：输入相电流 [A]、角度 [rad]、母线电压 [V]；输出三相占空比。这个接口边界就是平台暴露给用户代码的边界。
5. **时间所有权归控制核**：PWM 时基、ISR tick 由 CPU1 发布，CPU2 只消费。
6. **不用 RTOS**：采用典型裸机前后台架构——控制在 ISR，状态机/杂务在后台循环。
7. **可观测性 day 0 就位**，不允许"遇到查不了的问题再补工具"（前身项目的最大教训）。

## C2000 特有的坑

- **EPWM TBCTL.FREE_SOFT 第一天就配**：决定调试器 halt 时 PWM 的行为。默认配置下挂断点 PWM 可能维持输出 → 电机带电时炸管。主调试手段是调试器，此项优先级最高。
- **C28x 的 char 是 16 位**：一切按字节打包的代码（memcpy 字节流、packed struct、字节缓冲区）都有坑。线上协议以**显式序列化器 + golden vectors** 正面应对，禁止按字节 memcpy 上线。
- **CCS 实时模式写多字段参数非原子** → 必须走参数双缓冲提交（见 v2k_param.h）。
- **调试器不是急停**：XDS110 + USB 链路可能卡死，急停只信硬件 trip。
- 待 TRM 核对（TODO）：ESC 过程数据 RAM 实际大小与 SyncManager 配置上限（决定 block/PDO 尺寸天花板）；ePWM/eQEP 资源到 LaunchPad 引脚的映射；flash bank 划分与 CPU2 镜像存放。

## 路线图

- **Phase 0 — 接口 + 协议定稿**：内存映射 + 四个共享内存接口的数据结构（字段、内存布局、索引协议）写成头文件；**wire spec（block 帧格式、枚举/事务协议）+ golden test vectors 初版同步定稿**。这是后面所有代码的接口基准。
- **Phase 1 — 双核骨架**：两个 CCS 工程、两份 linker .cmd、GSx RAM 归属分配、CPU1 引导 CPU2、IPC ping-pong、共享 RAM 握手、CCS 双核调试会话。完成标志：两核各自闪灯 + 握手成功。（主要是工具链体力活，但它决定内存映射，必须早做。）
- **Phase 2 — 时基证明 + 保护**：EPWM → ADC SOC → EOC ISR 链路打通，GPIO 翻转 + 示波器实测中断延迟与抖动；配置 FREE_SOFT；CMPSS 硬件 trip + fault 锁存状态机。
- **Phase 3 — 执行器 + 可观测性**：ISR 多速率调度框架（软件分频 + **相位错开 stagger**——慢环不挤在 `k%N==0` 同一拍，摊平 WCET；慢环内联跑还是丢给低优先级软中断，在此决策）、双模式 RAM 示波器（snapshot 先行，CCS Graph 消费）、参数双缓冲、描述符表。
- **Phase 3.5 — SCI 哑数据泵**：CPU2 经 XDS110 VCP 跑最小协议子集（枚举描述符表 + Live 小 N block + snapshot 排空）。**意义：描述符表、示波平面、命令平面的第一个真实消费者**——CCS Graph 走 JTAG 直读内存，绕过 CPU2 / SPSC 消费端 / IPC，不算数；双核分离这个最大的架构风险点在此处提前验证，不留到 Phase 6。上位机侧同步落地 `V2kSource` 初版。
- **Phase 4 — 控制库**：L2 纯软件实现，PC 上对电机模型仿真验证（与"仿真平台"的终极目标同路）。
- **Phase 5 — 电机 bring-up（平台验收）**：开环 V/f → 电流采样校准 → 电流环 → 编码器 → 速度环 → 位置环。每一步是一个 L3 小应用，顺便检验平台接口设计。
- **Phase 6 — EtherCAT 链路成型**：SSC 移植、ESI/EEPROM、状态机至 OP、PDO 映射；ethercrab master @ 2 kHz 循环。**验收 = 100 kHz × 8ch 无损连续流（序号零丢失 × 长时间）+ 录盘回放。**
- **（远期）移植选项**：L2 + 共享接口 + 上位机与芯片无关；将来换芯片（F29x / AM26x）时，重写的只有 L0/L1。平台的积累在接口定义里，不在芯片上。

## 工作流约定

- **commit**：小步提交，每个 commit 对应一个实际验证过的节点；修 bug 把根因写进 message（沿用前身项目的好习惯）。
- **tag**：硬件验证过的节点打 git tag。
- **BRINGUP.md**：记录每一步在实物上验证了什么、用什么方法验证（示波器实测值、CCS Graph 截图等）。验证知识不能只活在 commit message 里。
- 注释中文为主，标识符英文（沿用前身项目风格）。

## 已决策 / 待决策

- [x] 平台命名：**Viewer2000**
- [x] 芯片与板：**TMS320F28P650DK9 / LAUNCHXL-F28P65X**（板载 XDS110 + VCP；无片上 USB）
- [x] 上位机链路物理层：**SCI 哑泵（3.5）→ EtherCAT（6）**，由 100 kHz × 8ch 需求排除法定，CAN-FD/W5500 不满足
- [x] CLA 归属：每核一个；**是否使用**仍开放（L2 纯 C 保留此选项）
- [ ] ESC 过程数据 RAM 大小与 SM 配置上限（TRM 核对，决定 block 天花板）
- [ ] 示波通道组与降采样比的具体档位
- [ ] flash bank 划分与 CPU2 镜像存放
