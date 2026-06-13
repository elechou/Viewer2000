//=============================================================================
// v2k_fault.h — Phase 2 保护：故障锁存状态机（CPU1 专用）
//
// 保护是纯硬件链路（基本规则 2）：trip 源 → TZ → 关 PWM 不经过任何 CPU；
// 本模块软件部分只做"事后"工作——锁存状态、上报 fault_code、受理命令。
// TZ 静态配置（源选择/动作/X-BAR/引脚）在 syscfg，v2k_tb_check 对账。
//
// Phase 2 的 trip 源（功率级未上，CMPSS 模拟源推迟到电流采样引脚定型）：
//   TZ1  ← INPUT X-BAR INPUT1 ← GPIO3（J8 排针 79，上拉）：跳线拉地 = 外部 trip
//   TZ6  ← emulation stop（CBC）：调试器 halt 时强制输出拉低，resume 自动恢复
//   软件 ← EPWM_forceTripZoneEvent(OST)：STOP 命令与初始封锁复用
// 换 CMPSS / DRV8323 nFAULT 只动 syscfg 的 TZ 源选择 + 自检对账位，逻辑不变。
//
// 状态机（sys_state，值域 = 契约 V2K_STATE_*，经 v2k_fault_poll 进 MSGRAM）：
//   IDLE/FAULT 输出都由 TZ 一次性锁存（OST）封死，区别只在语义与可受理命令；
//   只有 APP_START 才清 OST 放行 → 全程不存在"输出短暂放开"的窗口；
//   trip 源未消失时 START 立即重入 FAULT、CLEAR_FAULT 原地保持（契约语义）。
//
//   IDLE  --APP_START（清 OST）--> RUNNING --TZ 事件--> FAULT
//   RUNNING --APP_STOP（强制 OST，expected）--> IDLE
//   FAULT --CLEAR_FAULT（源已消失）--> IDLE（OST 保持锁存）
//=============================================================================
#ifndef V2K_FAULT_H
#define V2K_FAULT_H

#include "../common/v2k_planes.h"

#define V2K_FAULT_TZ_GPIO  3u   // 外部 trip 跳线引脚（pad/XBAR 配置在 syscfg）

//-----------------------------------------------------------------------------
// 观测量（CCS Expressions）
//-----------------------------------------------------------------------------
extern volatile uint16_t g_v2k_sm_state;    // V2K_STATE_*（MSGRAM sys_state 的内部源）
extern volatile uint16_t g_v2k_fault_code;  // V2K_FAULT_*
extern volatile uint32_t g_v2k_tz_int_cnt;  // TZ OST 中断累计（仅运行中真实 trip；
                                            // 命令性 STOP/初始封锁先关中断再 force，不计）

void v2k_fault_arm(void);    // Board_init 前调用：抢先锁存 OST，syscfg 配置
                             // 落地期间输出不可能放开（TZ 中断未开，不产生中断）
void v2k_fault_init(void);   // TZ 中断注册/使能 + 状态机落 IDLE（Board_init 后）
void v2k_fault_poll(volatile v2k_cpu1_status_t *st);
                             // 慢环调用：受理 cmd_req 命令 + 同步状态到 MSGRAM

#endif // V2K_FAULT_H
