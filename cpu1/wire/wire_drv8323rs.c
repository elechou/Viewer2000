//=============================================================================
// wire_drv8323rs.c - DRV8323RS GPIO and bounded TI driver adapter
//=============================================================================

#include <stdbool.h>
#include <stdint.h>

#include "driverlib.h"
#include "device.h"
#include "board.h"
#include "drv8323s.h"
#include "wire_drv8323rs.h"

#ifndef DRV_CS_GPIO
#error "DRV_CS_GPIO must be defined so the TI DRV8323 driver toggles manual CS."
#endif

#define WIRE_DRV_WAKE_CYCLES         (DEVICE_SYSCLK_FREQ / 1000uL)
#define WIRE_DRV_SLEEP_CYCLES        (DEVICE_SYSCLK_FREQ / 1000uL)
#define WIRE_DRV_TX_FIFO_DELAY       0x10u

#define WIRE_DRV_STATUS0_FAULT_MASK \
    (DRV8323_STATUS00_FAULT_BITS | DRV8323_STATUS00_VDS_LC_BITS | \
     DRV8323_STATUS00_VDS_HC_BITS | DRV8323_STATUS00_VDS_LB_BITS | \
     DRV8323_STATUS00_VDS_HB_BITS | DRV8323_STATUS00_VDS_LA_BITS | \
     DRV8323_STATUS00_VDS_HA_BITS | DRV8323_STATUS00_OTSD_BITS | \
     DRV8323_STATUS00_UVLO_BITS | DRV8323_STATUS00_GDF_BITS | \
     DRV8323_STATUS00_VDS_OCP_BITS)

#define WIRE_DRV_STATUS1_FAULT_MASK \
    (DRV8323_STATUS01_VGS_LC_BITS | DRV8323_STATUS01_VGS_HC_BITS | \
     DRV8323_STATUS01_VGS_LB_BITS | DRV8323_STATUS01_VGS_HB_BITS | \
     DRV8323_STATUS01_VGS_LA_BITS | DRV8323_STATUS01_VGS_HA_BITS | \
     DRV8323_STATUS01_CPUV_BITS | DRV8323_STATUS01_SC_OC_BITS | \
     DRV8323_STATUS01_SB_OC_BITS | DRV8323_STATUS01_SA_OC_BITS)

static DRV8323_Obj s_drv_obj;
static DRV8323_Handle s_drv_handle;
static DRV8323_VARS_t s_drv_vars;

static volatile uint16_t s_status1;
static volatile uint16_t s_status2;
static volatile uint32_t s_spi_error_count;
static volatile uint16_t s_config_valid;
static volatile uint16_t s_control_readback;
static volatile uint16_t s_gate_hs_readback;
static volatile uint16_t s_gate_ls_readback;
static volatile uint16_t s_ocp_readback;
static volatile uint16_t s_csa_readback;
static uint16_t s_enabled;
static uint16_t s_driver_initialized;
static uint16_t s_spi_configured;
static uint32_t s_enable_cycle;
static uint32_t s_disable_cycle;

static void wire_drv8323rs_init_driver(void)
{
    if (s_driver_initialized == 0u)
    {
        s_drv_handle = DRV8323_init(&s_drv_obj);
        DRV8323_setSPIHandle(s_drv_handle, DRV_SPI_BASE);
        DRV8323_setGPIOCSNumber(s_drv_handle, DRV_CS);
        DRV8323_setGPIOENNumber(s_drv_handle, DRV_ENABLE);
        s_driver_initialized = 1u;
    }
}

static void wire_drv8323rs_prepare_spi(void)
{
    wire_drv8323rs_init_driver();

    if (s_spi_configured == 0u)
    {
        // SysConfig owns SPID mode, bit rate, word width, FIFO, emulation,
        // pinmux, and module enable. SysConfig 1.28 does not expose FFCT.TXDLY.
        SPI_setTxFifoTransmitDelay(DRV_SPI_BASE, WIRE_DRV_TX_FIFO_DELAY);
        SPI_clearInterruptStatus(DRV_SPI_BASE, SPI_INT_TXFF);
        s_spi_configured = 1u;
    }
}

static uint16_t wire_drv8323rs_read(DRV8323_Address_e reg, uint16_t *value)
{
    DRV8323_Obj *obj;

    wire_drv8323rs_prepare_spi();
    DRV8323_resetRxTimeout(s_drv_handle);
    *value = DRV8323_readSPI(s_drv_handle, reg) & DRV8323_DATA_MASK;

    obj = (DRV8323_Obj *)s_drv_handle;
    if (obj->rxTimeOut == true)
    {
        s_spi_error_count++;
        return 0u;
    }

    return 1u;
}

static void wire_drv8323rs_apply_official_baseline(void)
{
    s_drv_vars.ctrlReg02.bit.OTW_REP = true;
    s_drv_vars.ctrlReg02.bit.PWM_MODE = DRV8323_PWMMODE_6;

    s_drv_vars.ctrlReg05.bit.VDS_LVL = DRV8323_VDS_LEVEL_1P700_V;
    s_drv_vars.ctrlReg05.bit.OCP_MODE = DRV8323_AUTOMATIC_RETRY;
    s_drv_vars.ctrlReg05.bit.DEAD_TIME = DRV8323_DEADTIME_100_NS;

    s_drv_vars.ctrlReg06.bit.CSA_GAIN = DRV8323_Gain_10VpV;
    s_drv_vars.ctrlReg06.bit.LS_REF = false;
    s_drv_vars.ctrlReg06.bit.VREF_DIV = true;
    s_drv_vars.ctrlReg06.bit.CSA_FET = false;
}

static uint16_t wire_drv8323rs_write_baseline(void)
{
    DRV8323_Obj *obj;

    wire_drv8323rs_prepare_spi();
    obj = (DRV8323_Obj *)s_drv_handle;

    DRV8323_resetRxTimeout(s_drv_handle);
    s_drv_vars.writeCmd = true;
    DRV8323_writeData(s_drv_handle, &s_drv_vars);
    if (obj->rxTimeOut == true)
    {
        s_spi_error_count++;
        return 0u;
    }
    DEVICE_DELAY_US(10u);

    // The TI universal motor-control lab writes the gate-driver image twice
    // during setup. Keep that behavior here, but still verify by readback.
    DRV8323_resetRxTimeout(s_drv_handle);
    s_drv_vars.writeCmd = true;
    DRV8323_writeData(s_drv_handle, &s_drv_vars);
    if (obj->rxTimeOut == true)
    {
        s_spi_error_count++;
        return 0u;
    }
    DEVICE_DELAY_US(10u);

    return 1u;
}

void wire_drv8323rs_disable(void)
{
    GPIO_writePin(DRV_ENABLE, 0u);
    s_enabled = 0u;
    s_disable_cycle = CPUTimer_getTimerCount(CPUTIMER1_BASE);
}

void wire_drv8323rs_enable(void)
{
    wire_drv8323rs_prepare_spi();

    GPIO_writePin(DRV_ENABLE, 1u);
    s_enabled = 1u;
    s_config_valid = 0u;
    s_enable_cycle = CPUTimer_getTimerCount(CPUTIMER1_BASE);
}

uint16_t wire_drv8323rs_wake_elapsed(void)
{
    return (s_enabled != 0u) &&
           ((s_enable_cycle - CPUTimer_getTimerCount(CPUTIMER1_BASE)) >=
            WIRE_DRV_WAKE_CYCLES);
}

uint16_t wire_drv8323rs_sleep_elapsed(void)
{
    return (s_enabled == 0u) &&
           ((s_disable_cycle - CPUTimer_getTimerCount(CPUTIMER1_BASE)) >=
            WIRE_DRV_SLEEP_CYCLES);
}

uint16_t wire_drv8323rs_poll(void)
{
    uint16_t status1;
    uint16_t status2;

    if ((wire_drv8323rs_read(DRV8323_ADDRESS_STATUS_0, &status1) == 0u) ||
        (wire_drv8323rs_read(DRV8323_ADDRESS_STATUS_1, &status2) == 0u))
    {
        return 0u;
    }

    s_status1 = status1;
    s_status2 = status2;
    s_drv_vars.statReg00.all = status1;
    s_drv_vars.statReg01.all = status2;
    return 1u;
}

uint16_t wire_drv8323rs_configure_and_verify(void)
{
    uint16_t control;
    uint16_t gate_hs;
    uint16_t gate_ls;
    uint16_t ocp;
    uint16_t csa;
    uint16_t control_expected;
    uint16_t gate_hs_expected;
    uint16_t gate_ls_expected;
    uint16_t ocp_expected;
    uint16_t csa_expected;

    s_config_valid = 0u;

    if ((wire_drv8323rs_read(DRV8323_ADDRESS_CONTROL_2, &control) == 0u) ||
        (wire_drv8323rs_read(DRV8323_ADDRESS_CONTROL_3, &gate_hs) == 0u) ||
        (wire_drv8323rs_read(DRV8323_ADDRESS_CONTROL_4, &gate_ls) == 0u) ||
        (wire_drv8323rs_read(DRV8323_ADDRESS_CONTROL_5, &ocp) == 0u) ||
        (wire_drv8323rs_read(DRV8323_ADDRESS_CONTROL_6, &csa) == 0u))
    {
        return 0u;
    }

    s_drv_vars.ctrlReg02.all = control;
    s_drv_vars.ctrlReg03.all = gate_hs;
    s_drv_vars.ctrlReg04.all = gate_ls;
    s_drv_vars.ctrlReg05.all = ocp;
    s_drv_vars.ctrlReg06.all = csa;
    wire_drv8323rs_apply_official_baseline();

    control_expected = s_drv_vars.ctrlReg02.all & DRV8323_DATA_MASK;
    gate_hs_expected = s_drv_vars.ctrlReg03.all & DRV8323_DATA_MASK;
    gate_ls_expected = s_drv_vars.ctrlReg04.all & DRV8323_DATA_MASK;
    ocp_expected = s_drv_vars.ctrlReg05.all & DRV8323_DATA_MASK;
    csa_expected = s_drv_vars.ctrlReg06.all & DRV8323_DATA_MASK;

    if (wire_drv8323rs_write_baseline() == 0u)
    {
        return 0u;
    }

    if ((wire_drv8323rs_read(DRV8323_ADDRESS_CONTROL_2, &control) == 0u) ||
        (wire_drv8323rs_read(DRV8323_ADDRESS_CONTROL_3, &gate_hs) == 0u) ||
        (wire_drv8323rs_read(DRV8323_ADDRESS_CONTROL_4, &gate_ls) == 0u) ||
        (wire_drv8323rs_read(DRV8323_ADDRESS_CONTROL_5, &ocp) == 0u) ||
        (wire_drv8323rs_read(DRV8323_ADDRESS_CONTROL_6, &csa) == 0u))
    {
        return 0u;
    }

    s_control_readback = control;
    s_gate_hs_readback = gate_hs;
    s_gate_ls_readback = gate_ls;
    s_ocp_readback = ocp;
    s_csa_readback = csa;

    s_config_valid =
        (control == control_expected) &&
        (gate_hs == gate_hs_expected) &&
        (gate_ls == gate_ls_expected) &&
        (ocp == ocp_expected) &&
        (csa == csa_expected);
    return s_config_valid;
}

uint16_t wire_drv8323rs_has_fault(void)
{
    return (((s_status1 & WIRE_DRV_STATUS0_FAULT_MASK) != 0u) ||
            ((s_status2 & WIRE_DRV_STATUS1_FAULT_MASK) != 0u)) ? 1u : 0u;
}

volatile uint16_t *wire_drv8323rs_status1_address(void)
{
    return &s_status1;
}

volatile uint16_t *wire_drv8323rs_status2_address(void)
{
    return &s_status2;
}

volatile uint32_t *wire_drv8323rs_spi_error_count_address(void)
{
    return &s_spi_error_count;
}

volatile uint16_t *wire_drv8323rs_config_valid_address(void)
{
    return &s_config_valid;
}

volatile uint16_t *wire_drv8323rs_control_readback_address(void)
{
    return &s_control_readback;
}

volatile uint16_t *wire_drv8323rs_gate_hs_readback_address(void)
{
    return &s_gate_hs_readback;
}

volatile uint16_t *wire_drv8323rs_gate_ls_readback_address(void)
{
    return &s_gate_ls_readback;
}

volatile uint16_t *wire_drv8323rs_ocp_readback_address(void)
{
    return &s_ocp_readback;
}

volatile uint16_t *wire_drv8323rs_csa_readback_address(void)
{
    return &s_csa_readback;
}

uint16_t wire_drv8323rs_fault_source_is_released(void)
{
    return GPIO_readPin(TZ_EXT) != 0u;
}
