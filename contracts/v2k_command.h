//=============================================================================
// v2k_command.h — 共享内存接口：命令/状态平面 + 心跳
//
// 载体：IPC MSGRAM（CPU1TOCPU2 / CPU2TOCPU1 各 1K words，硬件单向写权限，
// 天然单写者）。状态机请求走 CPU2→CPU1 方向，状态/心跳走反方向。
// IPC 中断仅用作"有新命令"的敲门信号（可选），数据一律走本文件的 struct——
// 单核调试时（基本规则 3）退化为同核内存轮询，接口不变。
//
// ---- 心跳语义（基本规则 1 的监视面）----
// 双方各自在主循环递增自己的 heartbeat；对方周期性检查计数是否前进。
// CPU2 失联 → CPU1 仅置位 status_flags 的 CPU2_LOST，电机照常运行（失联≠故障）。
// CPU1 失联 → CPU2 上报 host；关 PWM 永远不依赖此路径（硬件 trip + 看门狗兜底）。
//
// ---- 启动握手 ----
// CPU1 引导 CPU2 后：CPU2 校验 cpu1_status.contract_ver == 本侧 V2K_CONTRACT_VER
// 且描述符表 magic 就绪，然后开始递增自己的 heartbeat；版本不符 → CPU2 停在
// 失败状态并经线上 STATUS 上报（防 CPU1/CPU2 固件不同期烧录）。
//=============================================================================
#ifndef V2K_COMMAND_H
#define V2K_COMMAND_H

#include "v2k_common.h"

//-----------------------------------------------------------------------------
// 命令码（cmd_req.cmd_code；0x8000 起为 L3 应用自定义号段）
//-----------------------------------------------------------------------------
#define V2K_CMD_NOP         0u
#define V2K_CMD_APP_START   1u   // 进入 RUNNING（使能 PWM 输出，前提 IDLE 且无故障）
#define V2K_CMD_APP_STOP    2u   // 回 IDLE（封 PWM）
#define V2K_CMD_CLEAR_FAULT 3u   // 清故障锁存（仅 FAULT 态接受；trip 源未消失则立即重入）
#define V2K_CMD_APP_BASE    0x8000u

//-----------------------------------------------------------------------------
// 平台状态机（cpu1_status.sys_state；L1 所有，L3 状态另行叠加）
//-----------------------------------------------------------------------------
#define V2K_STATE_INIT    0u   // 上电初始化中（描述符表未发布）
#define V2K_STATE_IDLE    1u   // 就绪，PWM 封锁
#define V2K_STATE_RUNNING 2u   // 控制运行中
#define V2K_STATE_FAULT   3u   // 故障锁存（硬件 trip 已动作，等 CLEAR_FAULT）

// 命令受理结果（cpu1_status.cmd_result）
#define V2K_CMDR_OK        0u
#define V2K_CMDR_BAD_CMD   1u
#define V2K_CMDR_BAD_STATE 2u   // 当前状态不接受该命令

// 状态标志位（cpu1_status.status_flags）
#define V2K_SF_CPU2_LOST  0x0001u  // CPU1 视角：CPU2 心跳停走（信息位，非故障）

//-----------------------------------------------------------------------------
// 命令请求（MSGRAM CPU2→CPU1；CCS 调试也可直接戳此结构）
//
// 提交协议：填 cmd_code/arg → 最后写 cmd_seq = 旧值+1（发布动作）。
// CPU1 慢环检测 cmd_seq != 已处理值即取命令，处理完把 ack_seq 写回 status。
//-----------------------------------------------------------------------------
typedef struct {
    uint32_t cmd_seq;    // 每条命令 +1；最后写入 = 发布
    uint16_t cmd_code;   // V2K_CMD_*
    uint16_t arg0;
    uint32_t arg1;
} v2k_cmd_req_t;

V2K_ASSERT_SIZE_BITS(v2k_cmd_req_t, 96u);

//-----------------------------------------------------------------------------
// CPU1 状态块（MSGRAM CPU1→CPU2）
//-----------------------------------------------------------------------------
typedef struct {
    uint16_t contract_ver;  // = V2K_CONTRACT_VER（启动握手校验）
    uint16_t sys_state;     // V2K_STATE_*
    uint32_t ack_seq;       // 已处理的最大 cmd_seq
    uint16_t cmd_result;    // V2K_CMDR_*（对应 ack_seq 那条）
    uint16_t fault_code;    // 0=无；1..255 平台保留（trip 源编号），256+ L3 自定义
    uint16_t status_flags;  // V2K_SF_*
    uint16_t reserved;
    uint32_t heartbeat;     // CPU1 慢环递增
    v2k_tick_t tick;        // 当前 ISR tick 快照（host 对时/活性双重判据）
} v2k_cpu1_status_t;

V2K_ASSERT_SIZE_BITS(v2k_cpu1_status_t, 192u);

//-----------------------------------------------------------------------------
// CPU2 状态块（MSGRAM CPU2→CPU1）
//-----------------------------------------------------------------------------
typedef struct {
    uint32_t heartbeat;     // CPU2 主循环递增
    uint16_t link_state;    // 0=无 host 连接 1=SCI 在线 2=EtherCAT OP（信息位）
    uint16_t reserved;
} v2k_cpu2_status_t;

V2K_ASSERT_SIZE_BITS(v2k_cpu2_status_t, 64u);

#endif // V2K_COMMAND_H
