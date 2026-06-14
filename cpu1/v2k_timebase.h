//=============================================================================
// v2k_timebase.h — Phase 2 时基证明：ePWM1 → ADC SOC → EOC ISR（CPU1 专用）
//
// 分工（与 SysConfig 的边界）：
//   SysConfig 管静态硬件——ePWM1 波形/死区/SOC/TZ、ADCA、DACA、引脚复用，
//   引擎校验冲突与 errata（配置清单见 docs/phase2-bringup.md §1）。
//   本模块只管运行时——ISR 内容与注册、占空比（运行时量）、EPWMCLKDIV
//  （时钟域归 device.c/C 侧）、TBCLKSYNC 放行时序，外加把安全关键配置从
//   寄存器读回对账的契约自检（v2k_tb_check，同 v2k_assert_layout 模式）。
//
// 启动顺序契约（保护先行，见 cpu1.c）：
//   ① TBCLKSYNC 关 + v2k_fault_arm()  —— Board_init 前抢先封锁
//   ② Board_init()                     —— syscfg 配置落地
//   ③ v2k_tb_init() / v2k_fault_init() —— 自检 + ISR 注册 + 状态机就位
//   ④ v2k_tb_start()                   —— TBCLKSYNC 放行，时基起走
//=============================================================================
#ifndef V2K_TIMEBASE_H
#define V2K_TIMEBASE_H

#include "../contracts/v2k_common.h"

//-----------------------------------------------------------------------------
// 时基参数（V2K_ISR_HZ 可由构建 -D 覆盖：20 kHz 跑通，100 kHz 压测。
// 改频率须同步改 syscfg 的 EPWM1 Period——v2k_tb_check 对账不过即 ESTOP0。
// 周期的双源头是已知取舍而非优点：缺陷被对账兜底、维护税真实存在，
// 账目与收敛方向见 docs/phase2-bringup.md「关键决策」）
//-----------------------------------------------------------------------------
#ifndef V2K_ISR_HZ
#define V2K_ISR_HZ          20000u
#endif

// EPWMCLK = PLLSYSCLK / 1 = 200 MHz。必须 /1：F28P65x errata——EPWMCLKDIV=/2
// 时 TZFRC/TZCLR 偶发丢失，而本平台状态机的封锁(STOP/初始)与放行(START)
// 全靠这两个寄存器，丢失即安全问题（syscfg 校验同样提示此条）。
// 配置来源 = syscfg 时钟树（Device Support 生成的 Device_init），
// v2k_tb_check 读回断言；本宏只是 C 侧推导 PRD 用的镜像值。
#define V2K_EPWMCLK_HZ      200000000u
#define V2K_TB_PRD          (V2K_EPWMCLK_HZ / (2u * V2K_ISR_HZ))  // up-down 计数
#define V2K_TB_CMPA_INIT    ((V2K_TB_PRD * 3u) / 4u)  // 初始占空比 25%（运行时量）
#define V2K_TB_PROBE_GPIO   2u           // ISR 探针引脚（J8 排针 80，syscfg 配 pad）

// SOC 触发点（= ADC 采样时刻在载波周期里的位置）。Phase 2 取 CTR=ZERO（谷点）。
// 本应是 syscfg 静态配置（EPWM Event Trigger → SOC-A Source），但 C2000Ware
// 26.01 SysConfig 有 codegen bug：选非默认源 TBCTR_ZERO 时不生成
// EPWM_setADCTriggerSource（默认源 DCxEVT1=枚举[0] 被当默认抑制），SOCASEL 停在
// 复位值 DCxEVT1 → SOC 永不触发、tick 卡 0（2026-06-13 硬件 ETSEL 读回实证）。
// 故此字段例外地改由 C 侧拥有：v2k_tb_init 显式写入 + v2k_tb_check 读回断言。
// TI 修复后此写法退化为无害的重复设置，届时可把所有权移回 syscfg。
// 待评估：下电阻采样时机 zero(谷点)↔period(峰点)，随电流采样方案定——只改此一处；
// SOCASEL 字段编码与枚举值一致（非比较器源：DCxEVT1=0 / ZERO=1 / PERIOD=2），
// v2k_tb_check 的读回对账即依赖此等价。
#define V2K_TB_SOC_SRC      EPWM_SOC_TBCTR_ZERO

// 频率必须整除，否则 PRD 截断导致实际 ISR 频率偏离标称值
V2K_STATIC_ASSERT((V2K_EPWMCLK_HZ % (2u * V2K_ISR_HZ)) == 0u);

//-----------------------------------------------------------------------------
// 观测量（CCS Expressions）
//-----------------------------------------------------------------------------
extern volatile v2k_tick_t g_v2k_tick;     // ISR tick，全平台唯一时间
extern volatile uint16_t g_v2k_adc_a0;     // ADCINA0 最新采样（DACA 中位 ≈ 2048）
extern volatile uint16_t g_v2k_isr_lat;    // ISR 入口处 TBCTR（EPWMCLK tick，5 ns）
extern volatile uint16_t g_v2k_isr_lat_min;// 上述值的历史最小（散布 = 抖动）
extern volatile uint16_t g_v2k_isr_lat_max;// 历史最大。注意含 ADC 采样+转换的常数
                                           // 偏置，看 min/max 散布不看绝对值；
                                           // 绝对延迟由示波器 GPIO 法实测
extern volatile uint32_t g_v2k_isr_ovf_cnt;// ADC INT overflow 计数（≠0 = ISR 超时）
extern volatile uint32_t g_v2k_isr_cycles; // CPUTIMER1 测得的最近 ISR 周期数
extern volatile uint32_t g_v2k_isr_cycles_max;
extern volatile uint32_t g_v2k_control_cycles; // acquire→apply（含 user_step）
extern volatile uint32_t g_v2k_control_cycles_max;
extern volatile uint32_t g_v2k_scope_cycles;   // scope epilogue 单独预算
extern volatile uint32_t g_v2k_scope_cycles_max;
extern volatile uint32_t g_v2k_isr_budget_violation_cnt;

void v2k_tb_init(void);    // EPWMCLKDIV=/1 + 契约自检 + 占空比 + ISR 注册
void v2k_tb_start(void);   // TBCLKSYNC 放行（保护与自检全部就位后调用）

#endif // V2K_TIMEBASE_H
