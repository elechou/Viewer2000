//=============================================================================
// v2k_board_as5600.c - non-blocking AS5600 I2C acquisition
//=============================================================================

#include "driverlib.h"
#include "board.h"
#include "v2k_board_as5600_internal.h"

#define V2K_BOARD_AS5600_ADDR             0x36u
#define V2K_BOARD_AS5600_STATUS_REG       0x0Bu
#define V2K_BOARD_AS5600_RAW_ANGLE_H_REG  0x0Cu
#define V2K_BOARD_AS5600_STATUS_MD        0x20u
#define V2K_BOARD_AS5600_TIMEOUT_CYCLES   400000uL
#define V2K_BOARD_AS5600_RAD_PER_COUNT    0.00153398078789f

typedef enum
{
    V2K_BOARD_AS5600_WAIT_BUS = 0,
    V2K_BOARD_AS5600_WAIT_POINTER_STOP,
    V2K_BOARD_AS5600_WAIT_RX_DATA,
    V2K_BOARD_AS5600_WAIT_RX_STOP
} v2k_board_as5600_state_t;

typedef enum
{
    V2K_BOARD_AS5600_TRANSFER_STATUS = 0,
    V2K_BOARD_AS5600_TRANSFER_ANGLE
} v2k_board_as5600_transfer_t;

static volatile v2k_board_as5600_sample_t s_samples[2];
static volatile uint16_t s_active_sample;
static volatile uint32_t s_error_count;
static volatile uint32_t s_published_sequence;
static volatile uint16_t s_last_status;
static volatile uint16_t s_bus_healthy;

static v2k_board_as5600_state_t s_state = V2K_BOARD_AS5600_WAIT_BUS;
static v2k_board_as5600_transfer_t s_transfer = V2K_BOARD_AS5600_TRANSFER_STATUS;
static uint32_t s_state_started;
static uint16_t s_rx_data[2];
static uint16_t s_rx_count;
static uint16_t s_rx_index;
static uint16_t s_pending_status;

static void v2k_board_as5600_start_deadline(void)
{
    s_state_started = CPUTimer_getTimerCount(CPUTIMER1_BASE);
}

static uint16_t v2k_board_as5600_timed_out(void)
{
    uint32_t now = CPUTimer_getTimerCount(CPUTIMER1_BASE);

    return (s_state_started - now) >= V2K_BOARD_AS5600_TIMEOUT_CYCLES;
}

static uint16_t v2k_board_as5600_has_bus_error(uint16_t status)
{
    return (status & (I2C_STS_NO_ACK | I2C_STS_ARB_LOST)) != 0u;
}

static void v2k_board_as5600_recover(void)
{
    I2C_disableModule(AS5600_I2C_BASE);
    I2C_enableModule(AS5600_I2C_BASE);
    I2C_clearStatus(AS5600_I2C_BASE,
                    I2C_STS_ARB_LOST |
                    I2C_STS_NO_ACK |
                    I2C_STS_REG_ACCESS_RDY |
                    I2C_STS_RX_DATA_RDY |
                    I2C_STS_STOP_CONDITION);
    s_bus_healthy = 0u;
    s_error_count++;
    s_state = V2K_BOARD_AS5600_WAIT_BUS;
    v2k_board_as5600_start_deadline();
}

static void v2k_board_as5600_begin_pointer_write(void)
{
    uint16_t reg = (s_transfer == V2K_BOARD_AS5600_TRANSFER_STATUS) ?
                   V2K_BOARD_AS5600_STATUS_REG : V2K_BOARD_AS5600_RAW_ANGLE_H_REG;

    I2C_clearStatus(AS5600_I2C_BASE,
                    I2C_STS_ARB_LOST |
                    I2C_STS_NO_ACK |
                    I2C_STS_REG_ACCESS_RDY |
                    I2C_STS_RX_DATA_RDY |
                    I2C_STS_STOP_CONDITION);
    I2C_setTargetAddress(AS5600_I2C_BASE, V2K_BOARD_AS5600_ADDR);
    I2C_setConfig(AS5600_I2C_BASE, I2C_CONTROLLER_SEND_MODE);
    I2C_setDataCount(AS5600_I2C_BASE, 1u);
    I2C_putData(AS5600_I2C_BASE, reg);
    I2C_sendStartCondition(AS5600_I2C_BASE);
    I2C_sendStopCondition(AS5600_I2C_BASE);
    s_state = V2K_BOARD_AS5600_WAIT_POINTER_STOP;
    v2k_board_as5600_start_deadline();
}

static void v2k_board_as5600_begin_receive(void)
{
    s_rx_count = (s_transfer == V2K_BOARD_AS5600_TRANSFER_STATUS) ? 1u : 2u;
    s_rx_index = 0u;
    I2C_clearStatus(AS5600_I2C_BASE,
                    I2C_STS_REG_ACCESS_RDY |
                    I2C_STS_RX_DATA_RDY |
                    I2C_STS_STOP_CONDITION);
    I2C_setConfig(AS5600_I2C_BASE, I2C_CONTROLLER_RECEIVE_MODE);
    I2C_setDataCount(AS5600_I2C_BASE, s_rx_count);
    I2C_sendStartCondition(AS5600_I2C_BASE);
    I2C_sendStopCondition(AS5600_I2C_BASE);
    s_state = V2K_BOARD_AS5600_WAIT_RX_DATA;
    v2k_board_as5600_start_deadline();
}

static void v2k_board_as5600_publish_angle(void)
{
    uint16_t raw_angle =
        (uint16_t)(((s_rx_data[0] & 0x000Fu) << 8) |
                   (s_rx_data[1] & 0x00FFu));
    uint16_t next = s_active_sample ^ 1u;
    uint32_t sequence = s_published_sequence + 1uL;

    s_samples[next].raw_angle = raw_angle;
    s_samples[next].status = s_pending_status;
    s_samples[next].angle_rad =
        (float)raw_angle * V2K_BOARD_AS5600_RAD_PER_COUNT;
    s_samples[next].sequence = sequence;
    s_samples[next].valid =
        ((s_pending_status & V2K_BOARD_AS5600_STATUS_MD) != 0u) ? 1u : 0u;
    s_published_sequence = sequence;
    s_last_status = s_pending_status;
    s_bus_healthy = 1u;
    s_active_sample = next;
}

void v2k_board_as5600_service(void)
{
    uint16_t status = I2C_getStatus(AS5600_I2C_BASE);

    if (v2k_board_as5600_has_bus_error(status) != 0u)
    {
        v2k_board_as5600_recover();
        return;
    }

    switch (s_state)
    {
        case V2K_BOARD_AS5600_WAIT_BUS:
            if (I2C_isBusBusy(AS5600_I2C_BASE) == 0u)
            {
                v2k_board_as5600_begin_pointer_write();
            }
            else if (v2k_board_as5600_timed_out() != 0u)
            {
                v2k_board_as5600_recover();
            }
            break;

        case V2K_BOARD_AS5600_WAIT_POINTER_STOP:
            if ((I2C_getStopConditionStatus(AS5600_I2C_BASE) == 0u) &&
                (I2C_isBusBusy(AS5600_I2C_BASE) == 0u))
            {
                v2k_board_as5600_begin_receive();
            }
            else if (v2k_board_as5600_timed_out() != 0u)
            {
                v2k_board_as5600_recover();
            }
            break;

        case V2K_BOARD_AS5600_WAIT_RX_DATA:
            if ((status & I2C_STS_RX_DATA_RDY) != 0u)
            {
                s_rx_data[s_rx_index] =
                    I2C_getData(AS5600_I2C_BASE) & 0x00FFu;
                s_rx_index++;
                if (s_rx_index >= s_rx_count)
                {
                    s_state = V2K_BOARD_AS5600_WAIT_RX_STOP;
                    v2k_board_as5600_start_deadline();
                }
            }
            else if (v2k_board_as5600_timed_out() != 0u)
            {
                v2k_board_as5600_recover();
            }
            break;

        case V2K_BOARD_AS5600_WAIT_RX_STOP:
            if ((I2C_getStopConditionStatus(AS5600_I2C_BASE) == 0u) &&
                (I2C_isBusBusy(AS5600_I2C_BASE) == 0u))
            {
                I2C_clearStatus(AS5600_I2C_BASE,
                                I2C_STS_RX_DATA_RDY |
                                I2C_STS_STOP_CONDITION);
                if (s_transfer == V2K_BOARD_AS5600_TRANSFER_STATUS)
                {
                    s_pending_status = s_rx_data[0];
                    s_transfer = V2K_BOARD_AS5600_TRANSFER_ANGLE;
                }
                else
                {
                    v2k_board_as5600_publish_angle();
                    s_transfer = V2K_BOARD_AS5600_TRANSFER_STATUS;
                }
                s_state = V2K_BOARD_AS5600_WAIT_BUS;
                v2k_board_as5600_start_deadline();
            }
            else if (v2k_board_as5600_timed_out() != 0u)
            {
                v2k_board_as5600_recover();
            }
            break;

        default:
            v2k_board_as5600_recover();
            break;
    }
}

uint16_t v2k_board_as5600_get_latest(v2k_board_as5600_sample_t *sample)
{
    uint16_t active = s_active_sample;

    sample->raw_angle = s_samples[active].raw_angle;
    sample->status = s_samples[active].status;
    sample->angle_rad = s_samples[active].angle_rad;
    sample->sequence = s_samples[active].sequence;
    sample->valid = s_samples[active].valid;
    return (sample->valid != 0u) && (s_bus_healthy != 0u);
}

volatile uint32_t *v2k_board_as5600_error_count_address(void)
{
    return &s_error_count;
}

volatile uint32_t *v2k_board_as5600_sequence_address(void)
{
    return &s_published_sequence;
}

volatile uint16_t *v2k_board_as5600_status_address(void)
{
    return &s_last_status;
}
