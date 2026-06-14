//#############################################################################
// cpu2.c — CPU2 通信核（Phase 3.5 SCI 数据泵）
//
// 当前职责：
//   1. IPC_sync 会合 → 等描述符表 magic → 契约版本握手（v2k_command.h）
//   2. 拥有 GS4 平面与 CPU2→CPU1 MSGRAM，服务参数/示波/命令平面
//   3. SCIA ISR 收 octet，超级循环完成 COBS/CRC/消息服务与 TX
//   4. 应答 IPC ping-pong并闪灯 2 Hz（LED5 绿 = GPIO13，低电平点亮；pad 配置与 CSEL→CPU2 由
//      CPU1 侧完成，本核只写数据寄存器——归属分配权在 boot master）
//
// CPU2 不拥有控制时间：block 时间戳、采样纪元和控制调度都来自 CPU1。
// 本地低速心跳只证明通信核仍在运行，不进入采样或控制时间。
//#############################################################################

#include <string.h>
#include "driverlib.h"
#include "device.h"
#include "board.h"   // sysconfig 生成：SCIA_BASE_init
#include "../common/v2k_planes.h"
#include "v2k_sci_service.h"

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
    // Phase 3.5 SCIA 静态配置（pin/baud/FIFO/module）由 sysconfig 落地：
    //   - CPU1 syscfg cpuSel_SCIA→CPU2 → CPU1 board.c SYSCTL_init 含
    //     SysCtl_selectCPUForPeripheralInstance(SYSCTL_CPUSEL_SCIA, CPU2)
    //   - CPU2 syscfg SCI 实例（SCIA + GPIO42/43 + 115200 8N1 + FIFO）双
    //     context 协同 → CPU1 board.c 出 SCIA pinmux + pad/qual，CPU2
    //     board.c 出 SCIA_BASE_init（reset/FIFO/SCI_setConfig/enableModule）
    // CPU2 Board_init() 会先调 SYSCTL_init() 再调 SCI_init()；这里绕开
    // Board_init() 聚合入口，因此必须保留最小本地 clock gate，否则后续
    // SCIA_BASE_init() 对 SCICCR/BAUD 的写入会被外设时钟门控吞掉。
    // 不调用完整 SYSCTL_init()，避免顺带拉进数百条 boot-master 专属
    // SysCtl_setPeripheralAccessControl/CPUSEL（对 CPU2 无效但会撑爆 RAMGS4）；
    // 函数级 dead-strip 由 --gen_func_subsections=on 提供。
    //
    SysCtl_enablePeripheral(SYSCTL_PERIPH_CLK_SCIA);
    SCIA_BASE_init();

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
    // 共享布局不一致时不能安全启动线上服务，直接停在可诊断状态。
    //
    if ((V2K_MSG_1TO2_RO->cpu1_status.contract_ver != V2K_CONTRACT_VER) ||
        (V2K_GS0_RO->desc_table.hdr.contract_ver   != V2K_CONTRACT_VER))
    {
        g_handshake_state = 2u;
        for (;;) { ESTOP0; }
    }
    g_handshake_state = 3u;
    v2k_sci_init();

    for (;;)
    {
        // ping-pong 应答：见 ping 即 ack
        if (IPC_isFlagBusyRtoL(IPC_CPU2_L_CPU1_R, IPC_FLAG0))
        {
            g_pong_cnt++;
            IPC_ackFlagRtoL(IPC_CPU2_L_CPU1_R, IPC_FLAG0);
        }

        // SCI ISR 只收 octet；协议解释、共享平面服务与 TX 均在超级循环。
        v2k_sci_service();

        // 本地诊断 heartbeat 不进入控制时间或示波时间戳。
        DEVICE_DELAY_US(100);
        led_count++;
        if ((led_count % 10u) == 0u)
        {
            g_v2k_msg_2to1.cpu2_status.heartbeat++;
        }

        // 2500 × 100 us 翻转一次，约 2 Hz；只用于肉眼诊断。
        if (led_count >= 2500u)
        {
            led_count = 0u;
            GPIO_togglePin(V2K_LED_CPU2_PIN);
        }
    }
}
