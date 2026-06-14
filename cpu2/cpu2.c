//#############################################################################
// cpu2.c — Phase 1 双核骨架：CPU2（通信核）
//
// 本阶段职责（AGENTS.md 路线图 Phase 1）：
//   1. IPC_sync 会合 → 等描述符表 magic → 契约版本握手（v2k_command.h）
//   2. 拥有 GS4 平面与 CPU2→CPU1 MSGRAM，主循环递增心跳
//   3. 应答 IPC ping-pong
//   4. 闪灯 2 Hz（LED5 绿 = GPIO13，低电平点亮；pad 配置与 CSEL→CPU2 由
//      CPU1 侧完成，本核只写数据寄存器——归属分配权在 boot master）
//
// CPU2 不拥有控制时间：block 时间戳、采样纪元和控制调度都来自 CPU1。
// Phase 3 临时保留一个本地低速后台心跳，用于证明通信核自身仍在运行；
// Phase 3.5 接入 SCI 后再改成通信事件/timeout 驱动。
//#############################################################################

#include <string.h>
#include "driverlib.h"
#include "device.h"
#include "../common/v2k_planes.h"

// LED5 绿（GPIO13）。引脚来自板卡文档（LAUNCHXL-F28P65X），pad 配置在
// CPU1 的 sysconfig 里（实例名 LED_CPU2），本核不依赖生成的 board.h。
#define V2K_LED_CPU2_PIN 13u

//-----------------------------------------------------------------------------
// 共享内存实体（section → 物理区块的映射见 28p65x_generic_*_lnk_cpu2.cmd）
//-----------------------------------------------------------------------------
#pragma DATA_SECTION(g_v2k_gs4, "v2k_gs4_cpu2")
v2k_gs4_plane_t g_v2k_gs4;

#pragma DATA_SECTION(g_v2k_msg_2to1, "v2k_msg_2to1")
v2k_msg_2to1_t g_v2k_msg_2to1;

//-----------------------------------------------------------------------------
// 观测量（CCS Expressions）
//-----------------------------------------------------------------------------
uint32_t g_pong_cnt;          // 已应答的 ping 次数
uint16_t g_handshake_state;   // 0=等sync 1=等描述符表 2=契约版本失败 3=运行中

// NMI 兜底（与 cpu1.c 同一模式，理由见彼处注释）：计数、留痕、清标志
volatile uint32_t g_nmi_cnt;
volatile uint32_t g_nmi_flags_last;
volatile uint32_t g_nmi_shadow_last;

static __interrupt void v2k_nmi_isr(void)
{
    g_nmi_flags_last  = SysCtl_getNMIFlagStatus();
    g_nmi_shadow_last = SysCtl_getNMIShadowFlagStatus();
    g_nmi_cnt++;
    SysCtl_clearAllNMIFlags();
}

static void v2k_assert_layout(void)
{
    if (((uint32_t)&g_v2k_gs4      != V2K_GS4_BASE) ||
        ((uint32_t)&g_v2k_msg_2to1 != V2K_MSGRAM_2TO1_BASE))
    {
        for (;;) { ESTOP0; }
    }
}

void main(void)
{
    uint16_t led_count = 0u;

    Device_init();
    v2k_assert_layout();

    memset(&g_v2k_gs4, 0, sizeof(g_v2k_gs4));
    memset(&g_v2k_msg_2to1, 0, sizeof(g_v2k_msg_2to1));

    //
    // NMI 兜底（与 cpu1.c 对称）
    //
    Interrupt_initModule();
    Interrupt_initVectorTable();
    SysCtl_clearAllNMIFlags();
    Interrupt_register(INT_NMI, &v2k_nmi_isr);
    SysCtl_enableNMIGlobalInterrupt();
    Interrupt_enable(INT_NMI);
    EINT;
    ERTM;

    //
    // 与 CPU1 会合
    //
    IPC_clearFlagLtoR(IPC_CPU2_L_CPU1_R, IPC_FLAG_ALL);
    IPC_sync(IPC_CPU2_L_CPU1_R, IPC_FLAG31);

    //
    // 等描述符表发布（单写者发布协议：见 magic 才允许读表）
    //
    g_handshake_state = 1u;
    while (V2K_GS0_RO->desc_table.hdr.magic != V2K_DESC_MAGIC) { }

    //
    // 契约版本握手：不符 = CPU1/CPU2 固件不同期烧录，停在失败状态
    //（Phase 3.5 起改为经线上 STATUS 上报，而非死等）
    //
    if ((V2K_MSG_1TO2_RO->cpu1_status.contract_ver != V2K_CONTRACT_VER) ||
        (V2K_GS0_RO->desc_table.hdr.contract_ver   != V2K_CONTRACT_VER))
    {
        g_handshake_state = 2u;
        for (;;) { ESTOP0; }
    }
    g_handshake_state = 3u;

    for (;;)
    {
        // ping-pong 应答：见 ping 即 ack
        if (IPC_isFlagBusyRtoL(IPC_CPU2_L_CPU1_R, IPC_FLAG0))
        {
            g_pong_cnt++;
            IPC_ackFlagRtoL(IPC_CPU2_L_CPU1_R, IPC_FLAG0);
        }

        // 临时本地心跳：不参与控制时间或采样时间戳，只证明 CPU2 主循环活着。
        // 忙等不关中断；Phase 3.5 的 SCI/EtherCAT 事件源就位后替换掉它。
        DEVICE_DELAY_US(1000);
        g_v2k_msg_2to1.cpu2_status.heartbeat++;
        led_count++;

        // 250 个本地心跳翻转一次，约 2 Hz；只用于肉眼诊断。
        if (led_count >= 250u)
        {
            led_count = 0u;
            GPIO_togglePin(V2K_LED_CPU2_PIN);
        }
    }
}
