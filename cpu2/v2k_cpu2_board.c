//=============================================================================
// v2k_cpu2_board.c - F28P65x CPU2 physical board seam
//=============================================================================

#include "driverlib.h"
#include "device.h"
#include "board.h"
#include "v2k_cpu2_board.h"

#define V2K_CPU2_STATUS_LED_PIN 13u
#define V2K_CPU2_PIPE_RX_RING_WORDS 512u
#define V2K_CPU2_PIPE_RX_RING_MASK  (V2K_CPU2_PIPE_RX_RING_WORDS - 1u)

#if ((V2K_CPU2_PIPE_RX_RING_WORDS & V2K_CPU2_PIPE_RX_RING_MASK) != 0u)
#error "V2K_CPU2_PIPE_RX_RING_WORDS must be a power of two"
#endif

extern volatile uint32_t g_v2k_sci_rx_octets;
extern volatile uint32_t g_v2k_sci_tx_octets;
extern volatile uint32_t g_v2k_sci_rx_overflow;

static volatile uint16_t s_pipe_rx_ring[V2K_CPU2_PIPE_RX_RING_WORDS];
static volatile uint16_t s_pipe_rx_wr;
static volatile uint16_t s_pipe_rx_rd;

static __interrupt void v2k_cpu2_board_scia_rx_isr(void)
{
    while (SCI_getRxFIFOStatus(SCIA_BASE) != SCI_FIFO_RX0)
    {
        uint16_t value = SCI_readCharNonBlocking(SCIA_BASE) & 0xFFu;
        uint16_t next =
            (uint16_t)((s_pipe_rx_wr + 1u) & V2K_CPU2_PIPE_RX_RING_MASK);

        if (next == s_pipe_rx_rd)
        {
            g_v2k_sci_rx_overflow++;
        }
        else
        {
            s_pipe_rx_ring[s_pipe_rx_wr] = value;
            s_pipe_rx_wr = next;
            g_v2k_sci_rx_octets++;
        }
    }
    if (SCI_getOverflowStatus(SCIA_BASE))
    {
        SCI_clearOverflowStatus(SCIA_BASE);
        SCI_resetRxFIFO(SCIA_BASE);
        g_v2k_sci_rx_overflow++;
    }
    SCI_clearInterruptStatus(SCIA_BASE, SCI_INT_RXFF);
    Interrupt_clearACKGroup(INTERRUPT_ACK_GROUP9);
}

void v2k_cpu2_board_panic_halt(void)
{
    for (;;) { ESTOP0; }
}

void v2k_cpu2_board_init_device(void)
{
    Device_init();
}

//-----------------------------------------------------------------------------
// NMI backstop, symmetric with CPU1 (see cpu1/board/v2k_board_f28p65x.c). NMI
// flag registers and the interrupt vector are target-owned boot resources, so
// the diagnostics and the vector setup live in board support, not in the
// portable cpu2.c super-loop. The handler counts, leaves a trace, and clears
// flags — clearing stops the NMIWD count; the mirrored flags keep the event
// observable (rule 7).
//-----------------------------------------------------------------------------
volatile uint32_t g_nmi_cnt;
volatile uint32_t g_nmi_flags_last;
volatile uint32_t g_nmi_shadow_last;

static __interrupt void v2k_cpu2_board_nmi_isr(void)
{
    g_nmi_flags_last  = SysCtl_getNMIFlagStatus();
    g_nmi_shadow_last = SysCtl_getNMIShadowFlagStatus();
    g_nmi_cnt++;
    SysCtl_clearAllNMIFlags();
}

void v2k_cpu2_board_boot_init_interrupts(void)
{
    Interrupt_initModule();
    Interrupt_initVectorTable();
    SysCtl_clearAllNMIFlags();
    Interrupt_register(INT_NMI, &v2k_cpu2_board_nmi_isr);
    SysCtl_enableNMIGlobalInterrupt();
    Interrupt_enable(INT_NMI);
    EINT;
    ERTM;
}

void v2k_cpu2_board_boot_sync(void)
{
    IPC_clearFlagLtoR(IPC_CPU2_L_CPU1_R, IPC_FLAG_ALL);
    IPC_sync(IPC_CPU2_L_CPU1_R, IPC_FLAG31);
}

uint16_t v2k_cpu2_board_ipc_pong_ack(void)
{
    if (IPC_isFlagBusyRtoL(IPC_CPU2_L_CPU1_R, IPC_FLAG0))
    {
        IPC_ackFlagRtoL(IPC_CPU2_L_CPU1_R, IPC_FLAG0);
        return 1u;
    }
    return 0u;
}

void v2k_cpu2_board_assert_layout(const volatile v2k_cpu2_plane_t *cpu2_plane,
                                  const volatile v2k_msg_2to1_t *msg_2to1)
{
    if (((uint32_t)cpu2_plane != V2K_CPU2_PLANE_BASE) ||
        ((uint32_t)msg_2to1 != V2K_MSGRAM_2TO1_BASE))
    {
        v2k_cpu2_board_panic_halt();
    }
}

void v2k_cpu2_board_pipe_init(void)
{
    s_pipe_rx_wr = 0u;
    s_pipe_rx_rd = 0u;
    SysCtl_enablePeripheral(SYSCTL_PERIPH_CLK_SCIA);
    SCIA_BASE_init();
    SCI_setFIFOInterruptLevel(SCIA_BASE, SCI_FIFO_TX0, SCI_FIFO_RX1);
    SCI_clearOverflowStatus(SCIA_BASE);
    SCI_resetRxFIFO(SCIA_BASE);
    Interrupt_register(INT_SCIA_RX, &v2k_cpu2_board_scia_rx_isr);
    SCI_enableInterrupt(SCIA_BASE, SCI_INT_RXFF);
    Interrupt_enable(INT_SCIA_RX);
}

void v2k_cpu2_board_pipe_service(void)
{
    if (SCI_getOverflowStatus(SCIA_BASE))
    {
        SCI_clearOverflowStatus(SCIA_BASE);
        SCI_resetRxFIFO(SCIA_BASE);
        g_v2k_sci_rx_overflow++;
    }
}

uint16_t v2k_cpu2_board_pipe_read_octet(uint16_t *octet)
{
    if (s_pipe_rx_rd == s_pipe_rx_wr)
    {
        return 0u;
    }
    *octet = s_pipe_rx_ring[s_pipe_rx_rd] & 0xFFu;
    s_pipe_rx_rd =
        (uint16_t)((s_pipe_rx_rd + 1u) & V2K_CPU2_PIPE_RX_RING_MASK);
    return 1u;
}

uint16_t v2k_cpu2_board_pipe_can_write(void)
{
    return (SCI_getTxFIFOStatus(SCIA_BASE) != SCI_FIFO_TX16) ? 1u : 0u;
}

void v2k_cpu2_board_pipe_write_octet(uint16_t octet)
{
    SCI_writeCharNonBlocking(SCIA_BASE, octet & 0xFFu);
    g_v2k_sci_tx_octets++;
}

void v2k_cpu2_board_delay_us(uint16_t delay_us)
{
    uint32_t cycles = ((uint32_t)delay_us *
                       (DEVICE_SYSCLK_FREQ / 1000000uL));
    uint32_t count = (cycles > 9uL) ? ((cycles - 9uL) / 5uL) : 1uL;

    SysCtl_delay(count);
}

void v2k_cpu2_board_toggle_status_led(void)
{
    GPIO_togglePin(V2K_CPU2_STATUS_LED_PIN);
}
