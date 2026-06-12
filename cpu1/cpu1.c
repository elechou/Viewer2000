//#############################################################################
// cpu1.c — Phase 1 双核骨架：CPU1（控制核 / boot master）
//
// 本阶段职责（AGENTS.md 路线图 Phase 1）：
//   1. 归属分配：GS4 RAM → CPU2；CPU2 LED（GPIO13）数据寄存器归属 → CPU2
//   2. 发布共享接口实体：GS0 平面（描述符表 magic 发布屏障）+ MSGRAM 心跳
//   3. 引导 CPU2 → IPC_sync 会合 → IPC ping-pong
//   4. 闪灯 1 Hz（LED4 红 = GPIO12，低电平点亮）
//
// Phase 1 没有 ePWM 时基，主循环以 DEVICE_DELAY_US(1000) 充当 ~1 kHz 节拍；
// Phase 2 起由 ePWM→ADC→EOC ISR 接管时间所有权（基本规则 5）。
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

// 固件 build hash：Phase 3.5 起由构建注入 git 短哈希（-D 定义），当前占位
#ifndef V2K_BUILD_HASH
#define V2K_BUILD_HASH 0u
#endif

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
    if (((uint32_t)&g_v2k_gs0      != V2K_GS0_BASE) ||
        ((uint32_t)&g_v2k_msg_1to2 != V2K_MSGRAM_1TO2_BASE))
    {
        for (;;) { ESTOP0; }
    }
}

void main(void)
{
    uint32_t loop = 0u;
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
    g_v2k_gs0.desc_table.hdr.contract_ver       = V2K_CONTRACT_VER;
    g_v2k_gs0.desc_table.hdr.entry_count        = 0u;  // 注册 API Phase 3 落地，先发空表
    g_v2k_gs0.desc_table.hdr.build_hash         = V2K_BUILD_HASH;
    g_v2k_gs0.desc_table.hdr.entry_stride_words = (uint16_t)sizeof(v2k_desc_entry_t);
    g_v2k_gs0.desc_table.hdr.magic              = V2K_DESC_MAGIC;  // 最后写 = 发布

    memset(&g_v2k_msg_1to2, 0, sizeof(g_v2k_msg_1to2));
    g_v2k_msg_1to2.cpu1_status.contract_ver = V2K_CONTRACT_VER;
    g_v2k_msg_1to2.cpu1_status.sys_state    = V2K_STATE_INIT;

    //
    // GPIO：pad 配置（含 LED_CPU2 的 Core Select→CPU2）由 sysconfig 生成的
    // Board_init 完成；这里再显式设一次 CSEL 作兜底（重复设置无害）。
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
    EINT;   // PIE 内尚无使能源；为后续 CCS 实时模式与慢环中断打底
    ERTM;

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

    g_v2k_msg_1to2.cpu1_status.sys_state = V2K_STATE_IDLE;

    // 发出第一记 ping（此后在主循环里见 ack 即续发）
    IPC_setFlagLtoR(IPC_CPU1_L_CPU2_R, IPC_FLAG0);

    for (;;)
    {
        DEVICE_DELAY_US(1000);  // ~1 kHz 主循环节拍（Phase 2 起由 ISR tick 取代）
        loop++;

        // 心跳与 tick 快照发布（Phase 1 以循环数充当 tick）
        g_v2k_msg_1to2.cpu1_status.heartbeat++;
        g_v2k_msg_1to2.cpu1_status.tick = loop;

        // ping-pong：CPU2 ack 后 flag 不再 busy → 计一轮并续发
        if (!IPC_isFlagBusyLtoR(IPC_CPU1_L_CPU2_R, IPC_FLAG0))
        {
            g_ping_cnt++;
            IPC_setFlagLtoR(IPC_CPU1_L_CPU2_R, IPC_FLAG0);
        }

        // CPU2 心跳监视（~每 256 ms 查一次；连续 4 次不前进判失联。
        // 基本规则 1：失联只置 status_flags，控制核照跑）
        if ((loop & 0xFFu) == 0u)
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

        // 闪灯 1 Hz（500 ms 翻转一次；低电平点亮）
        if ((loop % 500u) == 0u)
        {
            GPIO_togglePin(LED_CPU1_GPIO);
        }
    }
}
