//=============================================================================
// wire_pwm.c - F28P65x three-phase PWM, synchronization, and trip-zone driver
//=============================================================================

#include "driverlib.h"
#include "board.h"
#include "wire_pwm.h"
#include "wire_adc.h"
#include "../runtime/v2k_timebase.h"

#define WIRE_PWM_DEADBAND_COUNTS 200u
#define WIRE_PWM_NEUTRAL_CMPA    ((uint16_t)(V2K_TB_PRD / 2u))
#define WIRE_PWM_SOC_SOURCE      EPWM_SOC_TBCTR_ZERO
#define WIRE_PWM_MASTER_BASE     PWM_TB_BASE
#define WIRE_PWM_PHASE_A_BASE    PWM_PHASE_A_BASE
#define WIRE_PWM_PHASE_B_BASE    PWM_TB_BASE
#define WIRE_PWM_PHASE_C_BASE    PWM_PHASE_C_BASE

#define WIRE_CURRENT_SOURCE_PHASE_A_HIGH 0x0001u
#define WIRE_CURRENT_SOURCE_PHASE_A_LOW  0x0002u
#define WIRE_CURRENT_SOURCE_PHASE_B_HIGH 0x0004u
#define WIRE_CURRENT_SOURCE_PHASE_B_LOW  0x0008u
#define WIRE_CURRENT_SOURCE_AGGREGATE    0x8000u

#define WIRE_CURRENT_CONFIG_ERR_A_DAC_HIGH 0x0001u
#define WIRE_CURRENT_CONFIG_ERR_A_DAC_LOW  0x0002u
#define WIRE_CURRENT_CONFIG_ERR_B_DAC_HIGH 0x0004u
#define WIRE_CURRENT_CONFIG_ERR_B_DAC_LOW  0x0008u
#define WIRE_CURRENT_CONFIG_ERR_XBAR_ENABLE 0x0010u
#define WIRE_CURRENT_CONFIG_ERR_XBAR_MUX    0x0020u
#define WIRE_CURRENT_CONFIG_ERR_PHASE_C_PPB 0x0040u
#define WIRE_CURRENT_CONFIG_ERR_PHASE_A_DCA 0x0080u
#define WIRE_CURRENT_CONFIG_ERR_PHASE_B_DCA 0x0100u
#define WIRE_CURRENT_CONFIG_ERR_PHASE_C_DCA 0x0200u

#define WIRE_POWER_TRIP_MUX_CONFIG_MASK 0xFF00000CuL
#define WIRE_POWER_TRIP_MUX_CONFIG_VALUE 0x0000000CuL

static volatile uint16_t s_current_trip_armed;
static volatile uint16_t s_current_trip_last;
static volatile uint16_t s_current_trip_config_error;
static volatile uint16_t s_current_limit_low = WIRE_CURRENT_LIMIT_LOW_COUNTS;
static volatile uint16_t s_current_limit_high =
    WIRE_CURRENT_LIMIT_HIGH_COUNTS;

static uint16_t wire_pwm_base_is_locked(uint32_t base)
{
    return (EPWM_getTripZoneFlagStatus(base) & EPWM_TZ_FLAG_OST) != 0u;
}

static void wire_pwm_set_cmpa(uint32_t base, float duty)
{
    uint16_t cmpa = (uint16_t)((float)V2K_TB_PRD * (1.0f - duty));
    EPWM_setCounterCompareValue(base, EPWM_COUNTER_COMPARE_A, cmpa);
}

static void wire_pwm_force_ost_all(void)
{
    EPWM_forceTripZoneEvent(WIRE_PWM_PHASE_A_BASE, EPWM_TZ_FORCE_EVENT_OST);
    EPWM_forceTripZoneEvent(WIRE_PWM_PHASE_B_BASE, EPWM_TZ_FORCE_EVENT_OST);
    EPWM_forceTripZoneEvent(WIRE_PWM_PHASE_C_BASE, EPWM_TZ_FORCE_EVENT_OST);
}

static void wire_pwm_clear_tz_all(uint16_t flags)
{
    EPWM_clearTripZoneFlag(WIRE_PWM_PHASE_A_BASE, flags);
    EPWM_clearTripZoneFlag(WIRE_PWM_PHASE_B_BASE, flags);
    EPWM_clearTripZoneFlag(WIRE_PWM_PHASE_C_BASE, flags);
}

static void wire_pwm_clear_dcaevt1_all(void)
{
    EPWM_clearTripZoneFlag(WIRE_PWM_PHASE_A_BASE,
                           EPWM_TZ_FLAG_DCAEVT1);
    EPWM_clearTripZoneFlag(WIRE_PWM_PHASE_B_BASE,
                           EPWM_TZ_FLAG_DCAEVT1);
    EPWM_clearTripZoneFlag(WIRE_PWM_PHASE_C_BASE,
                           EPWM_TZ_FLAG_DCAEVT1);
    EPWM_clearOneShotTripZoneFlag(WIRE_PWM_PHASE_A_BASE,
                                  EPWM_TZ_OST_FLAG_DCAEVT1);
    EPWM_clearOneShotTripZoneFlag(WIRE_PWM_PHASE_B_BASE,
                                  EPWM_TZ_OST_FLAG_DCAEVT1);
    EPWM_clearOneShotTripZoneFlag(WIRE_PWM_PHASE_C_BASE,
                                  EPWM_TZ_OST_FLAG_DCAEVT1);
}

static void wire_pwm_configure_current_trip_base(uint32_t base)
{
    EPWM_disableTripZoneSignals(base, EPWM_TZ_SIGNAL_DCAEVT1);
    EPWM_selectDigitalCompareTripInput(base,
                                       EPWM_DC_TRIP_TRIPIN7,
                                       EPWM_DC_TYPE_DCAH);
    EPWM_setTripZoneDigitalCompareEventCondition(
        base,
        EPWM_TZ_DC_OUTPUT_A1,
        EPWM_TZ_EVENT_DCXH_HIGH);
    EPWM_setDigitalCompareEventSource(base,
                                      EPWM_DC_MODULE_A,
                                      EPWM_DC_EVENT_1,
                                      EPWM_DC_EVENT_SOURCE_ORIG_SIGNAL);
    EPWM_setDigitalCompareEventSyncMode(base,
                                        EPWM_DC_MODULE_A,
                                        EPWM_DC_EVENT_1,
                                        EPWM_DC_EVENT_INPUT_NOT_SYNCED);
    // The TZCTL.DCAEVT1 per-event action drives EPWMxA whenever the DCAEVT1
    // digital-compare condition is asserted. That force is applied independently
    // of whether DCAEVT1 is selected as an OST source in TZSEL, so "disarmed"
    // (TZSEL cleared) does NOT make DCAEVT1 harmless. Its reset value is
    // High-Impedance (0), which floats EPWMxA only (DCBEVT1 is unused), and in
    // DRY_RUN the gate driver sleeps so the CSA outputs sit near 0 V and the
    // CMPSS low comparator asserts TRIPIN7 with no real current. That idle
    // DCAEVT1 floated all three high-side outputs (Phase 5.1). Default the action
    // to DISABLE here; the real force-low is installed only while armed.
    EPWM_setTripZoneAction(base, EPWM_TZ_ACTION_EVENT_DCAEVT1,
                           EPWM_TZ_ACTION_DISABLE);
}

// Install (armed) or remove (disarmed) the DCAEVT1 output force on all phases.
// Armed = force EPWMxA low on a current trip (safe state, replacing the unsafe
// reset High-Impedance); disarmed = no action, so the level-sensitive DCAEVT1
// condition can never drive the high-side pins outside an armed POWERED run.
static void wire_pwm_set_current_trip_output_action(uint16_t armed)
{
    EPWM_TripZoneAction action =
        (armed != 0u) ? EPWM_TZ_ACTION_LOW : EPWM_TZ_ACTION_DISABLE;

    EPWM_setTripZoneAction(WIRE_PWM_PHASE_A_BASE,
                           EPWM_TZ_ACTION_EVENT_DCAEVT1, action);
    EPWM_setTripZoneAction(WIRE_PWM_PHASE_B_BASE,
                           EPWM_TZ_ACTION_EVENT_DCAEVT1, action);
    EPWM_setTripZoneAction(WIRE_PWM_PHASE_C_BASE,
                           EPWM_TZ_ACTION_EVENT_DCAEVT1, action);
}

static uint16_t wire_pwm_current_trip_base_is_valid(uint32_t base)
{
    uint16_t dctripsel = HWREGH(base + EPWM_O_DCTRIPSEL);
    uint16_t tzdcsel = HWREGH(base + EPWM_O_TZDCSEL);
    uint16_t dcactl = HWREGH(base + EPWM_O_DCACTL);

    return
        ((dctripsel & EPWM_DCTRIPSEL_DCAHCOMPSEL_M) ==
         (uint16_t)EPWM_DC_TRIP_TRIPIN7) &&
        (((tzdcsel & EPWM_TZDCSEL_DCAEVT1_M) >>
          EPWM_TZDCSEL_DCAEVT1_S) ==
         (uint16_t)EPWM_TZ_EVENT_DCXH_HIGH) &&
        ((dcactl & EPWM_DCACTL_EVT1SRCSEL) == 0u) &&
        ((dcactl & EPWM_DCACTL_EVT1FRCSYNCSEL) != 0u);
}

static uint16_t wire_pwm_current_trip_config_errors(void)
{
    uint32_t enabled_muxes = HWREG(XBARA_EPWM_EN_REG_BASE +
                                   (uint32_t)POWER_TRIP_XBAR);
    uint32_t mux_config = HWREG(XBARA_EPWM_CFG_REG_BASE +
                                ((uint32_t)POWER_TRIP_XBAR << 1u));
    uint16_t errors = 0u;

    if (CMPSS_getDACValueHigh(PHASE_A_CURRENT_CMPSS_BASE) !=
        WIRE_CURRENT_LIMIT_HIGH_COUNTS)
    {
        errors |= WIRE_CURRENT_CONFIG_ERR_A_DAC_HIGH;
    }
    if (CMPSS_getDACValueLow(PHASE_A_CURRENT_CMPSS_BASE) !=
        WIRE_CURRENT_LIMIT_LOW_COUNTS)
    {
        errors |= WIRE_CURRENT_CONFIG_ERR_A_DAC_LOW;
    }
    if (CMPSS_getDACValueHigh(PHASE_B_CURRENT_CMPSS_BASE) !=
        WIRE_CURRENT_LIMIT_HIGH_COUNTS)
    {
        errors |= WIRE_CURRENT_CONFIG_ERR_B_DAC_HIGH;
    }
    if (CMPSS_getDACValueLow(PHASE_B_CURRENT_CMPSS_BASE) !=
        WIRE_CURRENT_LIMIT_LOW_COUNTS)
    {
        errors |= WIRE_CURRENT_CONFIG_ERR_B_DAC_LOW;
    }
    if ((enabled_muxes & (uint32_t)POWER_TRIP_XBAR_ENABLED_MUXES) !=
        (uint32_t)POWER_TRIP_XBAR_ENABLED_MUXES)
    {
        errors |= WIRE_CURRENT_CONFIG_ERR_XBAR_ENABLE;
    }
    if ((mux_config & WIRE_POWER_TRIP_MUX_CONFIG_MASK) !=
        WIRE_POWER_TRIP_MUX_CONFIG_VALUE)
    {
        errors |= WIRE_CURRENT_CONFIG_ERR_XBAR_MUX;
    }
    if (wire_adc_current_limit_config_is_valid() == 0u)
    {
        errors |= WIRE_CURRENT_CONFIG_ERR_PHASE_C_PPB;
    }
    if (wire_pwm_current_trip_base_is_valid(WIRE_PWM_PHASE_A_BASE) == 0u)
    {
        errors |= WIRE_CURRENT_CONFIG_ERR_PHASE_A_DCA;
    }
    if (wire_pwm_current_trip_base_is_valid(WIRE_PWM_PHASE_B_BASE) == 0u)
    {
        errors |= WIRE_CURRENT_CONFIG_ERR_PHASE_B_DCA;
    }
    if (wire_pwm_current_trip_base_is_valid(WIRE_PWM_PHASE_C_BASE) == 0u)
    {
        errors |= WIRE_CURRENT_CONFIG_ERR_PHASE_C_DCA;
    }
    return errors;
}

static uint16_t wire_pwm_current_trip_config_is_valid(void)
{
    s_current_trip_config_error = wire_pwm_current_trip_config_errors();
    return s_current_trip_config_error == 0u;
}

static uint16_t wire_pwm_capture_current_sources(void)
{
    uint16_t phase_a = CMPSS_getStatus(PHASE_A_CURRENT_CMPSS_BASE);
    uint16_t phase_b = CMPSS_getStatus(PHASE_B_CURRENT_CMPSS_BASE);
    uint16_t sources = wire_adc_current_limit_status();

    if ((phase_a & CMPSS_STS_HI_LATCHFILTOUT) != 0u)
    {
        sources |= WIRE_CURRENT_SOURCE_PHASE_A_HIGH;
    }
    if ((phase_a & CMPSS_STS_LO_LATCHFILTOUT) != 0u)
    {
        sources |= WIRE_CURRENT_SOURCE_PHASE_A_LOW;
    }
    if ((phase_b & CMPSS_STS_HI_LATCHFILTOUT) != 0u)
    {
        sources |= WIRE_CURRENT_SOURCE_PHASE_B_HIGH;
    }
    if ((phase_b & CMPSS_STS_LO_LATCHFILTOUT) != 0u)
    {
        sources |= WIRE_CURRENT_SOURCE_PHASE_B_LOW;
    }
    return sources;
}

static void wire_pwm_clear_current_source_flags(void)
{
    CMPSS_clearFilterLatchHigh(PHASE_A_CURRENT_CMPSS_BASE);
    CMPSS_clearFilterLatchLow(PHASE_A_CURRENT_CMPSS_BASE);
    CMPSS_clearFilterLatchHigh(PHASE_B_CURRENT_CMPSS_BASE);
    CMPSS_clearFilterLatchLow(PHASE_B_CURRENT_CMPSS_BASE);
    wire_adc_clear_current_limit_status();
}

static uint16_t wire_pwm_check_common(uint32_t base, uint16_t phase_slave)
{
    uint16_t tbctl = HWREGH(base + EPWM_O_TBCTL);
    uint16_t cmpctl = HWREGH(base + EPWM_O_CMPCTL);
    uint16_t dbctl = HWREGH(base + EPWM_O_DBCTL);
    uint16_t tzsel = HWREGH(base + EPWM_O_TZSEL);
    uint16_t tzctl = HWREGH(base + EPWM_O_TZCTL);

    if (EPWM_getTimeBasePeriod(base) != (uint16_t)V2K_TB_PRD)
    {
        return 0u;
    }
    if (((tbctl & EPWM_TBCTL_CTRMODE_M) >> EPWM_TBCTL_CTRMODE_S) !=
        (uint16_t)EPWM_COUNTER_MODE_UP_DOWN)
    {
        return 0u;
    }
    if (((tbctl & EPWM_TBCTL_FREE_SOFT_M) >> EPWM_TBCTL_FREE_SOFT_S) < 2u)
    {
        return 0u;
    }
    if ((tbctl & (EPWM_TBCTL_CLKDIV_M | EPWM_TBCTL_HSPCLKDIV_M)) != 0u)
    {
        return 0u;
    }
    if (phase_slave != 0u)
    {
        if (((tbctl & EPWM_TBCTL_PHSEN) == 0u) ||
            ((tbctl & EPWM_TBCTL_PHSDIR) == 0u))
        {
            return 0u;
        }
        if (((HWREGH(base + EPWM_O_SYNCINSEL) & EPWM_SYNCINSEL_SEL_M) >>
             EPWM_SYNCINSEL_SEL_S) !=
            (uint16_t)EPWM_SYNC_IN_PULSE_SRC_SYNCOUT_EPWM1)
        {
            return 0u;
        }
    }
    else if ((tbctl & EPWM_TBCTL_PHSEN) != 0u)
    {
        return 0u;
    }

    if (((cmpctl & EPWM_CMPCTL_SHDWAMODE) != 0u) ||
        (((cmpctl & EPWM_CMPCTL_LOADAMODE_M) >>
          EPWM_CMPCTL_LOADAMODE_S) !=
         (uint16_t)EPWM_COMP_LOAD_ON_CNTR_ZERO))
    {
        return 0u;
    }
    if ((((dbctl & EPWM_DBCTL_OUT_MODE_M) >> EPWM_DBCTL_OUT_MODE_S) != 3u) ||
        ((dbctl & EPWM_DBCTL_POLSEL_M) != 0x8u))
    {
        return 0u;
    }
    if (((HWREGH(base + EPWM_O_DBRED) & EPWM_DBRED_DBRED_M) !=
         WIRE_PWM_DEADBAND_COUNTS) ||
        ((HWREGH(base + EPWM_O_DBFED) & EPWM_DBFED_DBFED_M) !=
         WIRE_PWM_DEADBAND_COUNTS))
    {
        return 0u;
    }
    if ((tzsel & (EPWM_TZSEL_OSHT1 | EPWM_TZSEL_CBC6)) !=
        (EPWM_TZSEL_OSHT1 | EPWM_TZSEL_CBC6))
    {
        return 0u;
    }
    if ((((tzctl & EPWM_TZCTL_TZA_M) >> EPWM_TZCTL_TZA_S) !=
         (uint16_t)EPWM_TZ_ACTION_LOW) ||
        (((tzctl & EPWM_TZCTL_TZB_M) >> EPWM_TZCTL_TZB_S) !=
         (uint16_t)EPWM_TZ_ACTION_LOW))
    {
        return 0u;
    }
    return 1u;
}

static uint16_t wire_pwm_config_is_valid(void)
{
    uint16_t etsel = HWREGH(WIRE_PWM_MASTER_BASE + EPWM_O_ETSEL);
    uint16_t syncouten = HWREGH(WIRE_PWM_MASTER_BASE + EPWM_O_SYNCOUTEN);
    uint16_t ediv = HWREGH(CLKCFG_BASE + SYSCTL_O_PERCLKDIVSEL) &
                    SYSCTL_PERCLKDIVSEL_EPWMCLKDIV_M;

    return
        (ediv == 0u) &&
        wire_pwm_check_common(WIRE_PWM_PHASE_A_BASE, 1u) &&
        wire_pwm_check_common(WIRE_PWM_PHASE_B_BASE, 0u) &&
        wire_pwm_check_common(WIRE_PWM_PHASE_C_BASE, 1u) &&
        ((syncouten & EPWM_SYNCOUTEN_ZEROEN) != 0u) &&
        ((etsel & EPWM_ETSEL_SOCAEN) != 0u) &&
        (((etsel & EPWM_ETSEL_SOCASEL_M) >> EPWM_ETSEL_SOCASEL_S) ==
         (uint16_t)WIRE_PWM_SOC_SOURCE);
}

uint16_t wire_pwm_prepare_timebase(void)
{
    EPWM_setSyncInPulseSource(WIRE_PWM_PHASE_A_BASE,
                              EPWM_SYNC_IN_PULSE_SRC_SYNCOUT_EPWM1);
    EPWM_setSyncInPulseSource(WIRE_PWM_PHASE_C_BASE,
                              EPWM_SYNC_IN_PULSE_SRC_SYNCOUT_EPWM1);
    EPWM_setADCTriggerSource(WIRE_PWM_MASTER_BASE,
                             EPWM_SOC_A,
                             WIRE_PWM_SOC_SOURCE);

    if (wire_pwm_config_is_valid() == 0u)
    {
        return 0u;
    }
    wire_pwm_apply_neutral();
    return 1u;
}

void wire_pwm_start_timebase(void)
{
    SysCtl_enablePeripheral(SYSCTL_PERIPH_CLK_TBCLKSYNC);
    EPWM_forceSyncPulse(WIRE_PWM_MASTER_BASE);
}

void wire_pwm_apply(float duty_a, float duty_b, float duty_c)
{
    wire_pwm_set_cmpa(WIRE_PWM_PHASE_A_BASE, duty_a);
    wire_pwm_set_cmpa(WIRE_PWM_PHASE_B_BASE, duty_b);
    wire_pwm_set_cmpa(WIRE_PWM_PHASE_C_BASE, duty_c);
}

void wire_pwm_apply_neutral(void)
{
    EPWM_setCounterCompareValue(WIRE_PWM_PHASE_A_BASE,
                                EPWM_COUNTER_COMPARE_A,
                                WIRE_PWM_NEUTRAL_CMPA);
    EPWM_setCounterCompareValue(WIRE_PWM_PHASE_B_BASE,
                                EPWM_COUNTER_COMPARE_A,
                                WIRE_PWM_NEUTRAL_CMPA);
    EPWM_setCounterCompareValue(WIRE_PWM_PHASE_C_BASE,
                                EPWM_COUNTER_COMPARE_A,
                                WIRE_PWM_NEUTRAL_CMPA);
}

uint16_t wire_pwm_counter_value(void)
{
    return (uint16_t)EPWM_getTimeBaseCounterValue(WIRE_PWM_MASTER_BASE);
}

void wire_pwm_pre_board_lock(void)
{
    SysCtl_enablePeripheral(SYSCTL_PERIPH_CLK_EPWM1);
    SysCtl_enablePeripheral(SYSCTL_PERIPH_CLK_EPWM2);
    SysCtl_enablePeripheral(SYSCTL_PERIPH_CLK_EPWM8);
    asm(" RPT #5 || NOP");
    wire_pwm_force_ost_all();
}

void wire_pwm_init_trip_isr(void (*tz_isr)(void))
{
    XBAR_setInputPin(INPUTXBAR_BASE, XBAR_INPUT1, TZ_EXT);
    wire_pwm_configure_current_trip_base(WIRE_PWM_PHASE_A_BASE);
    wire_pwm_configure_current_trip_base(WIRE_PWM_PHASE_B_BASE);
    wire_pwm_configure_current_trip_base(WIRE_PWM_PHASE_C_BASE);
    (void)wire_pwm_current_trip_config_is_valid();
    s_current_trip_armed = 0u;
    s_current_trip_last = 0u;
    wire_pwm_apply_neutral();
    wire_pwm_force_ost_all();
    wire_pwm_clear_current_source_flags();
    wire_pwm_clear_dcaevt1_all();
    wire_pwm_clear_tz_all(EPWM_TZ_INTERRUPT);
    Interrupt_register(INT_EPWM1_TZ, tz_isr);
    Interrupt_disable(INT_EPWM1_TZ);
}

void wire_pwm_disable_trip_irq(void)
{
    Interrupt_disable(INT_EPWM1_TZ);
    EPWM_disableTripZoneInterrupt(WIRE_PWM_PHASE_A_BASE,
                                  EPWM_TZ_INTERRUPT_OST);
    EPWM_disableTripZoneInterrupt(WIRE_PWM_PHASE_B_BASE,
                                  EPWM_TZ_INTERRUPT_OST);
    EPWM_disableTripZoneInterrupt(WIRE_PWM_PHASE_C_BASE,
                                  EPWM_TZ_INTERRUPT_OST);
}

void wire_pwm_enable_trip_irq(void)
{
    EPWM_enableTripZoneInterrupt(WIRE_PWM_PHASE_A_BASE,
                                 EPWM_TZ_INTERRUPT_OST);
    EPWM_enableTripZoneInterrupt(WIRE_PWM_PHASE_B_BASE,
                                 EPWM_TZ_INTERRUPT_OST);
    EPWM_enableTripZoneInterrupt(WIRE_PWM_PHASE_C_BASE,
                                 EPWM_TZ_INTERRUPT_OST);
    Interrupt_enable(INT_EPWM1_TZ);
}

void wire_pwm_clear_trip_interrupt(void)
{
    wire_pwm_clear_tz_all(EPWM_TZ_INTERRUPT);
}

void wire_pwm_force_output_lock(void)
{
    wire_pwm_force_ost_all();
}

uint16_t wire_pwm_release_output_lock(void)
{
    if ((s_current_trip_armed != 0u) &&
        (wire_pwm_current_trip_was_active() != 0u))
    {
        return 0u;
    }

    wire_pwm_clear_tz_all(EPWM_TZ_INTERRUPT | EPWM_TZ_FLAG_OST);

    if (((s_current_trip_armed != 0u) &&
         (wire_pwm_current_trip_was_active() != 0u)) ||
        (wire_pwm_output_is_locked() != 0u))
    {
        wire_pwm_force_ost_all();
        return 0u;
    }
    return 1u;
}

uint16_t wire_pwm_output_is_locked(void)
{
    return wire_pwm_base_is_locked(WIRE_PWM_PHASE_A_BASE) ||
           wire_pwm_base_is_locked(WIRE_PWM_PHASE_B_BASE) ||
           wire_pwm_base_is_locked(WIRE_PWM_PHASE_C_BASE);
}

void wire_pwm_ack_trip_isr(void)
{
    wire_pwm_clear_trip_interrupt();
    Interrupt_clearACKGroup(INTERRUPT_ACK_GROUP2);
}

uint16_t wire_pwm_arm_current_trip(void)
{
    if ((wire_pwm_current_trip_config_is_valid() == 0u) ||
        (wire_adc_current_window_is_valid() == 0u))
    {
        return 0u;
    }

    wire_pwm_clear_current_source_flags();
    wire_pwm_clear_dcaevt1_all();
    s_current_trip_last = 0u;

    EPWM_enableTripZoneSignals(WIRE_PWM_PHASE_A_BASE,
                               EPWM_TZ_SIGNAL_DCAEVT1);
    EPWM_enableTripZoneSignals(WIRE_PWM_PHASE_B_BASE,
                               EPWM_TZ_SIGNAL_DCAEVT1);
    EPWM_enableTripZoneSignals(WIRE_PWM_PHASE_C_BASE,
                               EPWM_TZ_SIGNAL_DCAEVT1);
    wire_pwm_set_current_trip_output_action(1u);
    s_current_trip_armed = 1u;

    if (wire_pwm_capture_current_sources() != 0u)
    {
        (void)wire_pwm_current_trip_was_active();
        return 0u;
    }
    return 1u;
}

void wire_pwm_disarm_current_trip(void)
{
    EPWM_disableTripZoneSignals(WIRE_PWM_PHASE_A_BASE,
                                EPWM_TZ_SIGNAL_DCAEVT1);
    EPWM_disableTripZoneSignals(WIRE_PWM_PHASE_B_BASE,
                                EPWM_TZ_SIGNAL_DCAEVT1);
    EPWM_disableTripZoneSignals(WIRE_PWM_PHASE_C_BASE,
                                EPWM_TZ_SIGNAL_DCAEVT1);
    wire_pwm_set_current_trip_output_action(0u);
    s_current_trip_armed = 0u;
    XBAR_disableEPWMMux(POWER_TRIP_XBAR,
                        POWER_TRIP_XBAR_ENABLED_MUXES);
    XBAR_enableEPWMMux(POWER_TRIP_XBAR,
                       POWER_TRIP_XBAR_ENABLED_MUXES);
    wire_pwm_clear_dcaevt1_all();
    wire_pwm_clear_current_source_flags();
    (void)wire_pwm_current_trip_config_is_valid();
}

uint16_t wire_pwm_current_trip_was_active(void)
{
    uint16_t ost_sources =
        EPWM_getOneShotTripZoneFlagStatus(WIRE_PWM_MASTER_BASE);
    uint16_t sources;

    if ((ost_sources & EPWM_TZ_OST_FLAG_DCAEVT1) == 0u)
    {
        return 0u;
    }

    sources = wire_pwm_capture_current_sources();
    if (sources == 0u)
    {
        sources = WIRE_CURRENT_SOURCE_AGGREGATE;
    }
    s_current_trip_last = sources;
    return 1u;
}

volatile uint16_t *wire_pwm_current_trip_armed_address(void)
{
    return &s_current_trip_armed;
}

volatile uint16_t *wire_pwm_current_trip_last_address(void)
{
    return &s_current_trip_last;
}

volatile uint16_t *wire_pwm_current_trip_config_error_address(void)
{
    return &s_current_trip_config_error;
}

volatile uint16_t *wire_pwm_current_limit_low_address(void)
{
    return &s_current_limit_low;
}

volatile uint16_t *wire_pwm_current_limit_high_address(void)
{
    return &s_current_limit_high;
}
