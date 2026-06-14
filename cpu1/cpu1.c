//#############################################################################
// cpu1.c — Phase 1 双核骨架：CPU1（控制核 / boot master）
//
// 本阶段职责（AGENTS.md 路线图 Phase 1）：
//   1. 归属分配：GS4 RAM → CPU2；CPU2 LED（GPIO13）数据寄存器归属 → CPU2
//   2. 发布共享接口实体：GS0 平面（描述符表 magic 发布屏障）+ MSGRAM 心跳
//   3. 引导 CPU2 → IPC_sync 会合 → IPC ping-pong
//   4. 闪灯 1 Hz（LED4 红 = GPIO12，低电平点亮）
//
// Phase 2 追加（路线图「时基证明 + 保护」）：
//   5. 时基：ePWM1→ADC SOC→EOC ISR（v2k_timebase.c），g_v2k_tick 接管时间所有权
//   6. 保护：TZ trip + 故障锁存状态机（v2k_fault.c），保护先于 PWM 上引脚就位
// Phase 3 后台采用普通前后台循环：主循环只等 g_v2k_tick 前进，再按 deadline
// 服务共享平面请求。tick 只发布时间，不在 ISR 内执行后台工作。
//
// 注意：启动序列中的 IPC_sync 是 init 阶段的一次性会合，不属于
// "控制核不得阻塞等待通信核"（基本规则 1）约束的运行时路径。
//#############################################################################

#include <string.h>
#include "driverlib.h"
#include "device.h"
#include "board.h"   // sysconfig 生成：LED_CPU1_GPIO / LED_CPU2_GPIO 引脚宏
                     //（board components LED 模块给嵌套 GPIO 实例加 _GPIO 后缀）
#include "../common/v2k_planes.h"
#include "v2k_timebase.h"
#include "v2k_fault.h"
#include "v2k_registry.h"
#include "v2k_scope_runtime.h"
#include "v2k_build_hash.h"

extern void SetDBGIER(uint16_t dbgier);

//-----------------------------------------------------------------------------
// 共享内存实体（section → 物理区块的映射见 28p65x_generic_*_lnk_cpu1.cmd）
//-----------------------------------------------------------------------------
#pragma DATA_SECTION(g_v2k_gs0, "v2k_gs0_cpu1")
v2k_gs0_plane_t g_v2k_gs0;

#pragma DATA_SECTION(g_v2k_msg_1to2, "v2k_msg_1to2")
v2k_msg_1to2_t g_v2k_msg_1to2;

//-----------------------------------------------------------------------------
// 观测量（CCS Expressions）
//-----------------------------------------------------------------------------
uint32_t g_ping_cnt;    // IPC ping-pong 完成轮数（持续递增 = 核间中断链路活着）
uint16_t g_cpu2_alive;  // 1 = CPU2 心跳在走（CPU1 视角；0 仅置标志，不停机）

#define V2K_BG_1MS_TICKS       (V2K_ISR_HZ / 1000u)
#define V2K_BG_MIRROR_TICKS    (V2K_ISR_HZ / 10u)
#define V2K_BG_MONITOR_TICKS   ((V2K_ISR_HZ * 256uL) / 1000uL)
#define V2K_BG_LED_TICKS       (V2K_ISR_HZ / 2u)

V2K_STATIC_ASSERT((V2K_ISR_HZ % 1000u) == 0u);
V2K_STATIC_ASSERT((V2K_ISR_HZ % 10u) == 0u);
V2K_STATIC_ASSERT(V2K_BG_MONITOR_TICKS > 0u);

// 无符号减法使 tick 回绕仍然正确。后台落后多个周期时只执行一次并以 now
// 重新定相，避免恢复后为“补课”连续执行低优先级任务。
static uint16_t v2k_tick_due(v2k_tick_t now,
                             v2k_tick_t *last,
                             v2k_tick_t period)
{
    if ((v2k_tick_t)(now - *last) < period)
    {
        return 0u;
    }
    *last = now;
    return 1u;
}

//-----------------------------------------------------------------------------
// NMI 兜底（boot master 职责，AGENTS.md 双核分工）。
// 未处理的 NMI 会被 NMI 看门狗升级成整片复位（Phase 1 实测，BRINGUP.md
// 2026-06-12：CPU2 放出复位后、.out 加载前在 M0 跑垃圾指令 → CPU2 看门狗
// 复位事件 → CPU1 NMI → NMIWD 整片复位 → 从 flash 启动旧固件，现象="跑飞"）。
// 处理只做三件事：计数、留痕、清标志——清标志即停止 NMIWD 计数；
// 标志镜像留在变量里，不掩盖事件（规则 7）。
//-----------------------------------------------------------------------------
volatile uint32_t g_nmi_cnt;         // NMI 累计次数
volatile uint32_t g_nmi_flags_last;  // 最近一次 NMIFLG（SYSCTL_NMI_* 位）
volatile uint32_t g_nmi_shadow_last; // 最近一次 NMI shadow flags（历史并集）

static __interrupt void v2k_nmi_isr(void)
{
    g_nmi_flags_last  = SysCtl_getNMIFlagStatus();
    g_nmi_shadow_last = SysCtl_getNMIShadowFlagStatus();
    g_nmi_cnt++;
    SysCtl_clearAllNMIFlags();
}

//-----------------------------------------------------------------------------
// 链接落位自检：实体地址 != memmap 基址属于构建错误（.cmd 与 v2k_memmap.h
// 失配），立即停机查链接脚本。ESTOP0 在调试器下等效断点。
//-----------------------------------------------------------------------------
static void v2k_assert_layout(void)
{
    if (((uint32_t)&g_v2k_gs0 != V2K_GS0_PLANE_BASE) ||
        ((uint32_t)&g_v2k_msg_1to2 != V2K_MSGRAM_1TO2_BASE))
    {
        for (;;) { ESTOP0; }
    }
}

void main(void)
{
    v2k_tick_t loop_tick = 0u;
    v2k_tick_t heartbeat_tick = 0u;
    v2k_tick_t mirror_tick = 0u;
    v2k_tick_t led_tick = 0u;
    v2k_tick_t monitor_tick = 0u;
    uint32_t cpu2_hb_last = 0u;
    uint16_t cpu2_hb_stale = 0u;

    Device_init();
    v2k_assert_layout();

    //
    // 归属分配（boot master 职责，先于引导 CPU2）：
    // GS4 划给 CPU2（v2k_memmap.h：参数 shadow + 示波 cfg/cons + CPU2 代码）。
    // 调试器对 CPU2 的 .out 加载也写 GS4，因此必须先跑 CPU1 过此行之后再
    // 加载 CPU2（见 docs/phase1-sysconfig.md 调试会话顺序）。
    //
    MemCfg_setGSRAMControllerSel(MEMCFG_SECT_GS4, MEMCFG_GSRAMCONTROLLER_CPU2);

    //
    // 共享接口发布：先整面清零、填内容，最后写 magic（发布屏障，
    // 见 v2k_descriptor.h 发布协议）。memset 是片内属主区初始化，
    // 不是线上序列化路径，不受"禁止 memcpy 上线"约束。
    //
    memset(&g_v2k_gs0, 0, sizeof(g_v2k_gs0));
    memset(&g_v2k_msg_1to2, 0, sizeof(g_v2k_msg_1to2));
    g_v2k_msg_1to2.cpu1_status.contract_ver = V2K_CONTRACT_VER;
    g_v2k_msg_1to2.cpu1_status.sys_state    = V2K_STATE_INIT;
    v2k_registry_init(V2K_BUILD_HASH);
    v2k_scope_init();

    //
    // Phase 2 保护先行（一）：放行前抢先封锁。不依赖 device 初始化是否
    // 打开过 TBCLKSYNC（模板 device.c 会开，syscfg 生成版视配置而定），
    // 一律先显式关掉；再抢先锁存 OST——下面 Board_init 落地 syscfg 配置
    // （EPWM1/ADCA/DACA/X-BAR/探针与 trip 引脚）的全程，PWM 都不可能上引脚。
    //
    SysCtl_disablePeripheral(SYSCTL_PERIPH_CLK_TBCLKSYNC);
    v2k_fault_arm();

    //
    // GPIO：pad 配置（含 LED_CPU2 的 Core Select→CPU2）由 sysconfig 生成的
    // Board_init 完成；这里再显式设一次 CSEL 作兜底（重复设置无害）。
    // Phase 2 起 Board_init 同时落地 EPWM1/ADCA/DACA/INPUTXBAR/GPIO2/3。
    //
    Device_initGPIO();
    Board_init();
    GPIO_setControllerCore(LED_CPU2_GPIO, GPIO_CORE_CPU2);

    //
    // NMI 兜底必须先于引导 CPU2 就位——CPU2 放出复位到其 .out 加载完成的
    // 窗口期内随时可能打来 NMI（流程对齐 TI 例程 nmi_ex1_cpu1handling）
    //
    Interrupt_initModule();
    Interrupt_initVectorTable();
    SysCtl_clearAllNMIFlags();
    Interrupt_register(INT_NMI, &v2k_nmi_isr);
    SysCtl_enableNMIGlobalInterrupt();
    Interrupt_enable(INT_NMI);
    SetDBGIER(INTERRUPT_CPU_INT1); // ADCA1 所在 PIE Group 1 = time-critical
    EINT;
    ERTM;

    //
    // Phase 2 保护先行（二）：契约自检（syscfg 配置读回对账，含 EPWMCLKDIV
    // errata 项）+ ISR 注册 + 状态机落 IDLE，最后才放行 TBCLKSYNC。
    //
    v2k_tb_init();
    v2k_fault_init();
    v2k_tb_start();

    //
    // 引导 CPU2。flash bank 划分未定稿（AGENTS.md 待决策），Phase 1 只支持
    // RAM 构建；_FLASH 分支留 TI 模板默认值占位，定稿后修正。
    //
#ifdef _FLASH
    Device_bootCPU2(BOOTMODE_BOOT_TO_FLASH_BANK3_SECTOR0);
#else
    Device_bootCPU2(BOOTMODE_BOOT_TO_M0RAM);
#endif

    //
    // 与 CPU2 会合（一次性，阻塞直至两核都到达 sync 点）
    //
    IPC_clearFlagLtoR(IPC_CPU1_L_CPU2_R, IPC_FLAG_ALL);
    IPC_sync(IPC_CPU1_L_CPU2_R, IPC_FLAG31);

    // sys_state 自此由 v2k_fault_poll 同步（fault_init 已落 IDLE）

    // 发出第一记 ping（此后在主循环里见 ack 即续发）
    IPC_setFlagLtoR(IPC_CPU1_L_CPU2_R, IPC_FLAG0);

    for (;;)
    {
        v2k_tick_t now = g_v2k_tick;
        if (now == loop_tick)
        {
            continue;
        }
        loop_tick = now;

        if (v2k_tick_due(now, &heartbeat_tick, V2K_BG_1MS_TICKS))
        {
            // 共享平面服务均为有限、可抢占的 run-to-completion 工作单元。
            // 没有新 seq/request 时立即返回，不等待通信核或外设。集中在
            // 约 1 ms poll point，避免空闲控制核持续读取 GS4/MSGRAM。
            v2k_param_service();
            v2k_scope_service();
            v2k_scope_apply_ready();
            v2k_scope_ccs_view_service();
            v2k_fault_poll(&g_v2k_msg_1to2.cpu1_status);

            g_v2k_msg_1to2.cpu1_status.heartbeat++;
            g_v2k_msg_1to2.cpu1_status.tick = now;
            // ping 是 1 ms 周期诊断，不是控制任务；已有未应答 ping 时直接跳过。
            if (!IPC_isFlagBusyLtoR(IPC_CPU1_L_CPU2_R, IPC_FLAG0))
            {
                g_ping_cnt++;
                IPC_setFlagLtoR(IPC_CPU1_L_CPU2_R, IPC_FLAG0);
            }
        }

        if (v2k_tick_due(now, &mirror_tick, V2K_BG_MIRROR_TICKS))
        {
            v2k_param_refresh_mirror();
        }

        // 每 256 ms 检查一次 CPU2 心跳；失联只置状态位，控制 ISR 照跑。
        if (v2k_tick_due(now, &monitor_tick, V2K_BG_MONITOR_TICKS))
        {
            uint32_t hb = V2K_MSG_2TO1_RO->cpu2_status.heartbeat;
            if (hb == cpu2_hb_last)
            {
                if (cpu2_hb_stale < 4u) { cpu2_hb_stale++; }
            }
            else
            {
                cpu2_hb_stale = 0u;
                cpu2_hb_last  = hb;
            }
            g_cpu2_alive = (cpu2_hb_stale < 4u) ? 1u : 0u;
            if (g_cpu2_alive)
            {
                g_v2k_msg_1to2.cpu1_status.status_flags &= (uint16_t)~V2K_SF_CPU2_LOST;
            }
            else
            {
                g_v2k_msg_1to2.cpu1_status.status_flags |= V2K_SF_CPU2_LOST;
            }
        }

        if (v2k_tick_due(now, &led_tick, V2K_BG_LED_TICKS))
        {
            GPIO_togglePin(LED_CPU1_GPIO);
        }
    }
}
