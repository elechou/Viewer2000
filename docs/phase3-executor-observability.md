# Phase 3 - 执行器与可观测性：操作与验证清单

本阶段继续遵守硬件/软件边界：

- SysConfig：ePWM/ADC/TZ 保持 Phase 2 配置，新增 CPUTIMER1 静态配置。
- C：ADC ISR、错相调度、参数提交、示波状态机、CMPA、TBCLKSYNC 与保护门控。
- 硬件 TZ 是输出的唯一放行门；`user_step` 在 IDLE/RUNNING/FAULT 都执行。

## 1. CCS 工程与 SysConfig

以下修改只能用 CCS Project/SysConfig 工具完成，禁止直接编辑工程元数据或
`sysconfig_cpu1.syscfg`：

1. CPU1/CPU2 工程器件统一为 `TMS320F28P650DK9`。
2. CPU1 新增 `CPUTIMER1`：
   - Period：`0xFFFFFFFF`
   - Prescaler：0
   - Emulation：Run Free
   - Start Timer：启用
   - Interrupt：禁用
   - Register ISR：禁用
3. CPU1 pre-build 运行 `python3 ../tools/gen_build_hash.py`。

`cpu1/f28p65x_dbgier.asm` 来自 C2000Ware 26.01；CPU1 启动时调用
`SetDBGIER(INTERRUPT_CPU_INT1)`，将 ADCA1 所在 PIE Group 1 标为
time-critical。

`v2k_tb_check` 会读回 CPUTIMER1 的周期、分频、运行、Run Free 和中断关闭状态。
SysConfig 未按上表配置时会停在 `ESTOP0`，不会带着无效性能统计运行。

## 2. 内存布局

| 区域 | 用途 |
|---|---|
| GS0 `0x10000..0x10FFF` | 描述符、参数状态、scope producer |
| GS0 `0x11000..0x11FFF` | group 1 慢速环，0x1000 words |
| GS1-GS3 `0x12000..0x17FFF` | group 0 快速环，0x6000 words |
| RAMD2 | 冻结后 CCS view，`float data[2048]` |
| GS4 前 0x200 words | 参数 shadow、scope cfg/bind/consumer |

默认 group 0 为 8 通道、prescaler=1、N=10；group 1 为 1 kHz、N=10；
group 2/3 为 OFF 且容量 0。

所有示波组上电默认 `OFF`。未采集时，ISR 示波路径只有 group 0/1 的
active 判断。配置稳定复制、校验、绑定复制、容量计算、CCS view 解交错和
CPU2/host 传输全部在后台；ISR 只保留同拍 RAM 采样与触发判定。

CPU1 后台是不含固定 Delay 的裸机事件循环：

- 参数、scope 配置、命令和 CCS view 在约 1 ms poll point 检查 seq/请求变化。
- 心跳、10 Hz mirror、CPU2 健康检查和 LED 使用 `g_v2k_tick` 判断 deadline。
- tick 只提供时间；周期任务仍在后台运行，可被下一次控制 ISR 抢占。
- 主循环是普通无限 `for (;;)`；没有新控制 tick 时只读 `g_v2k_tick` 后返回循环，
  不访问 GS4/MSGRAM，也不执行后台服务。

CPU2 Phase 3 暂时使用约 1 ms 本地低速心跳证明通信核自身仍在运行；它不参与
控制 tick、采样时间戳或 block 时间。CPU1 每约 1 ms 发出 ping，CPU2 见 IPC flag
即应答。Phase 3.5 接入 SCI 后，CPU2 后台改由通信事件/timeout 驱动。

## 3. 关键观测量

CPU1 Expressions：

| 表达式 | 含义 |
|---|---|
| `g_v2k_tick` | 全平台唯一 ISR tick |
| `g_v2k_due_mask` | `PLAT_DUE_1KHZ` / `PLAT_DUE_100HZ` |
| `g_v2k_isr_cycles` / `_max` | CPUTIMER1 测得的 ISR 周期数 |
| `g_v2k_control_cycles` / `_max` | acquire 到 PWM apply，含 `user_step` |
| `g_v2k_scope_cycles` / `_max` | 仅示波采样/触发 epilogue |
| `g_v2k_isr_budget_violation_cnt` | ISR 耗时达到控制周期预算的次数 |
| `g_v2k_isr_ovf_cnt` | ADC interrupt overflow |
| `g_v2k_gs0.param_status` | 参数批次结果、失败下标、unguarded 计数、镜像 |
| `g_v2k_gs0.scope_prod[0..3]` | 示波状态、容量、配置结果、overrun、冻结范围 |
| `g_v2k_ccs_view` | snapshot 解交错后的连续 float 数据 |

## 4. 参数批次

在 CPU2 会话编辑 `g_v2k_gs4.param_shadow`：

1. 填 `count` 与 `writes[]`。
2. 最后递增 `commit_seq`。
3. CPU1 后台在下一个约 1 ms poll point 稳定复制并验证整个批次。
4. ISR 在独立的 1 kHz 错相槽位一次性写入全部已批准条目；该槽位避开
   `PLAT_DUE_1KHZ` 与 `PLAT_DUE_100HZ`。验证完成到生效不超过 1 ms，
   从任意时刻发布到生效的端到端最坏延迟小于 2 ms。

预期结果：

- `pwm1_duty_cmd` 类型 F32、范围 0.02..0.98。
- 任一条类型、范围、数量、地址或对齐错误，整批不写。
- 未注册但位于 CPU1 `.bss/.data/.bss:output` 的应用变量允许写，
  `unguarded_cnt` 按成功写入条数累计。
- Flash、代码、外设、GS 环与未对齐 32-bit 地址返回 `V2K_CAL_BAD_ADDR`。

## 5. Snapshot

配置顺序：`DAQ_CTRL(OFF)` -> `DAQ_BIND` -> `DAQ_CTRL(SNAP_ARMED)`。
直接用 CCS 时遵守相同发布协议：先填字段，最后递增 `bind_seq/cfg_seq`。

冻结后：

1. 设置 `g_v2k_ccs_view.group` 与 `channel_slot`。
2. 递增 `request_seq`。
3. 等 `done_seq == request_seq` 且 `result == V2K_SCOPE_RESULT_OK`。
4. CCS Graph 指向 `g_v2k_ccs_view.data`，长度取 `count`，格式 32-bit float。

验证上升沿、下降沿、0/30/50/100% pre-trigger、部分末块和重复 ARM。
触发样本属于 post 段；冻结块按时间顺序由
`frozen_end_idx - frozen_count` 起读。

## 6. Live 与 CPU2 Consumer

LIVE 环满时丢弃新 block，`overrun_cnt` 增加，ISR 不等待消费者。

Phase 3 不再提供单核编译路径。Viewer2000 固定按 F28P65x 双核平台实现：
CPU1 始终划转 GS4 并引导 CPU2；CPU2/Phase 3.5 数据泵通过公共
`v2k_scope_consumer_peek/release` 消费 LIVE 或冻结后的 SNAPSHOT block。

## 7. 验收

RAM/FLASH 两配置、CPU1/CPU2 均须通过 CCS `buildProject`。硬件以 RAM 双核会话
为主，回归 Phase 1/2 后在 20 kHz 与 100 kHz 各验证：

- 1 kHz 与 100 Hz due 位频率正确，且不在同一 tick 置位。
- group 0 八通道同拍，block start tick、stride、bind sequence 正确。
- `g_v2k_isr_cycles_max < 200 MHz / V2K_ISR_HZ`，
  budget violation 与无 halt 窗口内 ADC overflow 不增长。
- 参数正常/拒绝/unguarded、snapshot/live 与 CPU2 consumer API 全测试通过。
- Silicon Real-time Mode 下 ISR/tick 继续，TZ6 仍保证普通 halt 输出安全。

只有以上实物项目完成后才创建 tag `phase3-executor-observability`。
