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
#define V2K_CPU2_PIPE_TX_FRAME_WORDS \
    (7u + V2K_WIRE_MAX_PAYLOAD + 4u + \
     ((7u + V2K_WIRE_MAX_PAYLOAD + 4u) / 254u) + 2u)
#define V2K_CPU2_PIPE_TX_FRAME_SLOTS 3u
#define V2K_CPU2_PIPE_TX_SLOT_INVALID 0xFFFFu
#define V2K_CPU2_BOARD_LOCAL_MS_CYCLES (DEVICE_SYSCLK_FREQ / 1000uL)

#define V2K_CPU2_PIPE_TX_FREE    0u
#define V2K_CPU2_PIPE_TX_FILLING 1u
#define V2K_CPU2_PIPE_TX_PENDING 2u
#define V2K_CPU2_PIPE_TX_ACTIVE  3u

#if ((V2K_CPU2_PIPE_RX_RING_WORDS & V2K_CPU2_PIPE_RX_RING_MASK) != 0u)
#error "V2K_CPU2_PIPE_RX_RING_WORDS must be a power of two"
#endif

extern volatile uint32_t g_v2k_sci_rx_octets;
extern volatile uint32_t g_v2k_sci_tx_octets;
extern volatile uint32_t g_v2k_sci_rx_overflow;
extern volatile uint32_t g_v2k_sci_tx_frames;
extern volatile uint32_t g_v2k_sci_tx_queue_full;
extern volatile uint32_t g_v2k_sci_tx_refill_isr;
extern volatile uint32_t g_v2k_sci_tx_refill_kick;
extern volatile uint32_t g_v2k_sci_tx_fifo_empty_refills;

typedef struct {
    volatile uint16_t state;
    volatile uint16_t prio;
    volatile uint16_t len;
    volatile uint16_t pos;
    volatile uint32_t order;
    volatile uint16_t data[V2K_CPU2_PIPE_TX_FRAME_WORDS];
} v2k_cpu2_pipe_tx_slot_t;

static volatile uint16_t s_pipe_rx_ring[V2K_CPU2_PIPE_RX_RING_WORDS];
static volatile uint16_t s_pipe_rx_wr;
static volatile uint16_t s_pipe_rx_rd;
static v2k_cpu2_pipe_tx_slot_t s_pipe_tx_slots[V2K_CPU2_PIPE_TX_FRAME_SLOTS];
static volatile uint16_t s_pipe_tx_active;
static uint32_t s_pipe_tx_submit_order;
static uint32_t s_cpu2_local_ms_last_count;
static uint32_t s_cpu2_local_ms_cycle_rem;
static uint32_t s_cpu2_local_ms;
static uint16_t s_cpu2_local_time_ready;

static void v2k_cpu2_board_local_time_init(void)
{
    SysCtl_enablePeripheral(SYSCTL_PERIPH_CLK_TIMER0);
    CPUTimer_stopTimer(CPUTIMER0_BASE);
    CPUTimer_disableInterrupt(CPUTIMER0_BASE);
    CPUTimer_setPeriod(CPUTIMER0_BASE, 0xFFFFFFFFuL);
    CPUTimer_setPreScaler(CPUTIMER0_BASE, 0u);
    CPUTimer_reloadTimerCounter(CPUTIMER0_BASE);
    CPUTimer_setEmulationMode(CPUTIMER0_BASE, CPUTIMER_EMULATIONMODE_RUNFREE);
    CPUTimer_startTimer(CPUTIMER0_BASE);

    s_cpu2_local_ms_last_count = CPUTimer_getTimerCount(CPUTIMER0_BASE);
    s_cpu2_local_ms_cycle_rem = 0uL;
    s_cpu2_local_ms = 0uL;
    s_cpu2_local_time_ready = 1u;
}

uint32_t v2k_cpu2_board_millis(void)
{
    uint32_t now_count;
    uint32_t delta_cycles;

    if (!s_cpu2_local_time_ready)
    {
        return 0uL;
    }

    now_count = CPUTimer_getTimerCount(CPUTIMER0_BASE);
    delta_cycles = s_cpu2_local_ms_last_count - now_count;
    s_cpu2_local_ms_last_count = now_count;
    s_cpu2_local_ms_cycle_rem += delta_cycles;

    while (s_cpu2_local_ms_cycle_rem >= V2K_CPU2_BOARD_LOCAL_MS_CYCLES)
    {
        s_cpu2_local_ms_cycle_rem -= V2K_CPU2_BOARD_LOCAL_MS_CYCLES;
        s_cpu2_local_ms++;
    }

    return s_cpu2_local_ms;
}

static uint16_t v2k_cpu2_board_pipe_tx_prio_valid(uint16_t prio)
{
    return (prio <= V2K_CPU2_BOARD_PIPE_TX_PRIO_HIGH) ? 1u : 0u;
}

static uint16_t v2k_cpu2_board_pipe_tx_free_count(void)
{
    uint16_t i;
    uint16_t free_count = 0u;

    for (i = 0u; i < V2K_CPU2_PIPE_TX_FRAME_SLOTS; i++)
    {
        if (s_pipe_tx_slots[i].state == V2K_CPU2_PIPE_TX_FREE)
        {
            free_count++;
        }
    }
    return free_count;
}

uint16_t v2k_cpu2_board_pipe_tx_can_submit(uint16_t prio)
{
    uint16_t free_count;

    if (!v2k_cpu2_board_pipe_tx_prio_valid(prio))
    {
        return 0u;
    }

    free_count = v2k_cpu2_board_pipe_tx_free_count();
    if (prio == V2K_CPU2_BOARD_PIPE_TX_PRIO_HIGH)
    {
        return (free_count != 0u) ? 1u : 0u;
    }

    // Normal traffic always leaves one slot for command responses/replay.
    return (free_count > 1u) ? 1u : 0u;
}

static uint16_t v2k_cpu2_board_pipe_tx_pick_pending(uint16_t prio)
{
    uint16_t i;
    uint16_t best = V2K_CPU2_PIPE_TX_SLOT_INVALID;

    for (i = 0u; i < V2K_CPU2_PIPE_TX_FRAME_SLOTS; i++)
    {
        if ((s_pipe_tx_slots[i].state == V2K_CPU2_PIPE_TX_PENDING) &&
            (s_pipe_tx_slots[i].prio == prio))
        {
            if ((best == V2K_CPU2_PIPE_TX_SLOT_INVALID) ||
                ((int32_t)(s_pipe_tx_slots[i].order -
                           s_pipe_tx_slots[best].order) < 0L))
            {
                best = i;
            }
        }
    }
    return best;
}

static uint16_t v2k_cpu2_board_pipe_tx_select_active(void)
{
    uint16_t slot;

    if (s_pipe_tx_active != V2K_CPU2_PIPE_TX_SLOT_INVALID)
    {
        return 1u;
    }

    slot = v2k_cpu2_board_pipe_tx_pick_pending(
        V2K_CPU2_BOARD_PIPE_TX_PRIO_HIGH);
    if (slot == V2K_CPU2_PIPE_TX_SLOT_INVALID)
    {
        slot = v2k_cpu2_board_pipe_tx_pick_pending(
            V2K_CPU2_BOARD_PIPE_TX_PRIO_NORMAL);
    }
    if (slot == V2K_CPU2_PIPE_TX_SLOT_INVALID)
    {
        return 0u;
    }

    s_pipe_tx_slots[slot].pos = 0u;
    s_pipe_tx_slots[slot].state = V2K_CPU2_PIPE_TX_ACTIVE;
    s_pipe_tx_active = slot;
    return 1u;
}

static uint16_t v2k_cpu2_board_pipe_tx_has_data(void)
{
    uint16_t i;

    if (s_pipe_tx_active != V2K_CPU2_PIPE_TX_SLOT_INVALID)
    {
        return 1u;
    }
    for (i = 0u; i < V2K_CPU2_PIPE_TX_FRAME_SLOTS; i++)
    {
        if (s_pipe_tx_slots[i].state == V2K_CPU2_PIPE_TX_PENDING)
        {
            return 1u;
        }
    }
    return 0u;
}

uint16_t v2k_cpu2_board_pipe_tx_idle(void)
{
    return v2k_cpu2_board_pipe_tx_has_data() ? 0u : 1u;
}

static void v2k_cpu2_board_scia_tx_refill(void)
{
    uint16_t started_empty =
        (SCI_getTxFIFOStatus(SCIA_BASE) == SCI_FIFO_TX0) ? 1u : 0u;
    uint16_t wrote = 0u;

    while (SCI_getTxFIFOStatus(SCIA_BASE) != SCI_FIFO_TX16)
    {
        v2k_cpu2_pipe_tx_slot_t *slot;
        uint16_t active;
        uint16_t pos;

        if (!v2k_cpu2_board_pipe_tx_select_active())
        {
            break;
        }

        active = s_pipe_tx_active;
        slot = &s_pipe_tx_slots[active];
        pos = slot->pos;
        SCI_writeCharNonBlocking(SCIA_BASE, slot->data[pos] & 0xFFu);
        slot->pos = (uint16_t)(pos + 1u);
        g_v2k_sci_tx_octets++;
        wrote = 1u;

        if (slot->pos >= slot->len)
        {
            slot->state = V2K_CPU2_PIPE_TX_FREE;
            s_pipe_tx_active = V2K_CPU2_PIPE_TX_SLOT_INVALID;
            g_v2k_sci_tx_frames++;
        }
    }

    if (started_empty && wrote)
    {
        g_v2k_sci_tx_fifo_empty_refills++;
    }
}

static void v2k_cpu2_board_scia_tx_update_interrupt(void)
{
    SCI_clearInterruptStatus(SCIA_BASE, SCI_INT_TXFF);
    if (v2k_cpu2_board_pipe_tx_has_data())
    {
        SCI_enableInterrupt(SCIA_BASE, SCI_INT_TXFF);
    }
    else
    {
        SCI_disableInterrupt(SCIA_BASE, SCI_INT_TXFF);
    }
}

static void v2k_cpu2_board_scia_tx_kick(void)
{
    //
    // Mask the TX ISR at the PIE level for the whole refill, not only at the
    // peripheral level: a TXFF event that already latched into PIEIFR still
    // fires after TXFFIENA is cleared (same latched-PIEIFR behavior as the TZ
    // notes in cpu1/runtime/v2k_fault.c). If that ISR lands between this
    // refill's slot->pos read and write-back, one octet is duplicated into the
    // FIFO or a freed slot keeps transmitting, and the frame fails CRC at the
    // host. With PIEIER masked the latched request is simply delivered after
    // the re-enable below and runs one harmless extra refill.
    //
    Interrupt_disable(INT_SCIA_TX);
    SCI_disableInterrupt(SCIA_BASE, SCI_INT_TXFF);
    g_v2k_sci_tx_refill_kick++;
    v2k_cpu2_board_scia_tx_refill();
    v2k_cpu2_board_scia_tx_update_interrupt();
    Interrupt_enable(INT_SCIA_TX);
}

static __interrupt void v2k_cpu2_board_scia_tx_isr(void)
{
    g_v2k_sci_tx_refill_isr++;
    v2k_cpu2_board_scia_tx_refill();
    v2k_cpu2_board_scia_tx_update_interrupt();
    Interrupt_clearACKGroup(INTERRUPT_ACK_GROUP9);
}

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
    uint16_t i;

    v2k_cpu2_board_local_time_init();

    s_pipe_rx_wr = 0u;
    s_pipe_rx_rd = 0u;
    s_pipe_tx_active = V2K_CPU2_PIPE_TX_SLOT_INVALID;
    s_pipe_tx_submit_order = 0uL;
    for (i = 0u; i < V2K_CPU2_PIPE_TX_FRAME_SLOTS; i++)
    {
        s_pipe_tx_slots[i].state = V2K_CPU2_PIPE_TX_FREE;
        s_pipe_tx_slots[i].prio = V2K_CPU2_BOARD_PIPE_TX_PRIO_NORMAL;
        s_pipe_tx_slots[i].len = 0u;
        s_pipe_tx_slots[i].pos = 0u;
        s_pipe_tx_slots[i].order = 0uL;
    }

    SysCtl_enablePeripheral(SYSCTL_PERIPH_CLK_SCIA);
    SCIA_BASE_init();
    SCI_setFIFOInterruptLevel(SCIA_BASE, SCI_FIFO_TX8, SCI_FIFO_RX1);
    SCI_clearOverflowStatus(SCIA_BASE);
    SCI_resetRxFIFO(SCIA_BASE);
    SCI_clearInterruptStatus(SCIA_BASE, SCI_INT_TXFF | SCI_INT_RXFF);
    Interrupt_register(INT_SCIA_RX, &v2k_cpu2_board_scia_rx_isr);
    Interrupt_register(INT_SCIA_TX, &v2k_cpu2_board_scia_tx_isr);
    SCI_disableInterrupt(SCIA_BASE, SCI_INT_TXFF);
    SCI_enableInterrupt(SCIA_BASE, SCI_INT_RXFF);
    Interrupt_enable(INT_SCIA_RX);
    Interrupt_enable(INT_SCIA_TX);
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

uint16_t v2k_cpu2_board_pipe_tx_submit_frame(const uint16_t *buf,
                                             uint16_t len,
                                             uint16_t prio)
{
    uint16_t i;
    uint16_t slot_idx = V2K_CPU2_PIPE_TX_SLOT_INVALID;
    v2k_cpu2_pipe_tx_slot_t *slot;

    if ((buf == 0) || (len == 0u) ||
        (len > V2K_CPU2_PIPE_TX_FRAME_WORDS) ||
        !v2k_cpu2_board_pipe_tx_can_submit(prio))
    {
        g_v2k_sci_tx_queue_full++;
        return 0u;
    }

    for (i = 0u; i < V2K_CPU2_PIPE_TX_FRAME_SLOTS; i++)
    {
        if (s_pipe_tx_slots[i].state == V2K_CPU2_PIPE_TX_FREE)
        {
            slot_idx = i;
            break;
        }
    }
    if (slot_idx == V2K_CPU2_PIPE_TX_SLOT_INVALID)
    {
        g_v2k_sci_tx_queue_full++;
        return 0u;
    }

    slot = &s_pipe_tx_slots[slot_idx];
    slot->state = V2K_CPU2_PIPE_TX_FILLING;
    slot->prio = prio;
    slot->len = len;
    slot->pos = 0u;
    slot->order = s_pipe_tx_submit_order++;
    for (i = 0u; i < len; i++)
    {
        slot->data[i] = buf[i] & 0xFFu;
    }
    slot->state = V2K_CPU2_PIPE_TX_PENDING;

    v2k_cpu2_board_scia_tx_kick();
    return 1u;
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
