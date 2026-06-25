//=============================================================================
// v2k_board_pwm.c - F28P65x three-phase PWM, synchronization, and trip-zone driver
//=============================================================================

#include "driverlib.h"
#include "board.h"
#include "v2k_board_pwm.h"
#include "v2k_board_adc.h"
#include "../runtime/v2k_timebase.h"

#define V2K_BOARD_PWM_DEADBAND_COUNTS 200u
#define V2K_BOARD_PWM_NEUTRAL_CMPA    ((uint16_t)(V2K_TB_PRD / 2u))
#define V2K_BOARD_PWM_SOC_SOURCE      EPWM_SOC_TBCTR_ZERO
#define V2K_BOARD_PWM_MASTER_BASE     PWM_TB_BASE
#define V2K_BOARD_PWM_PHASE_A_BASE    PWM_PHASE_A_BASE
#define V2K_BOARD_PWM_PHASE_B_BASE    PWM_TB_BASE
#define V2K_BOARD_PWM_PHASE_C_BASE    PWM_PHASE_C_BASE

#define V2K_BOARD_CURRENT_SOURCE_PHASE_A_HIGH 0x0001u
#define V2K_BOARD_CURRENT_SOURCE_PHASE_A_LOW  0x0002u
#define V2K_BOARD_CURRENT_SOURCE_PHASE_B_HIGH 0x0004u
#define V2K_BOARD_CURRENT_SOURCE_PHASE_B_LOW  0x0008u
#define V2K_BOARD_CURRENT_SOURCE_AGGREGATE    0x8000u

#define V2K_BOARD_CURRENT_CONFIG_ERR_A_DAC_HIGH 0x0001u
#define V2K_BOARD_CURRENT_CONFIG_ERR_A_DAC_LOW  0x0002u
#define V2K_BOARD_CURRENT_CONFIG_ERR_B_DAC_HIGH 0x0004u
#define V2K_BOARD_CURRENT_CONFIG_ERR_B_DAC_LOW  0x0008u
#define V2K_BOARD_CURRENT_CONFIG_ERR_XBAR_ENABLE 0x0010u
#define V2K_BOARD_CURRENT_CONFIG_ERR_XBAR_MUX    0x0020u
#define V2K_BOARD_CURRENT_CONFIG_ERR_PHASE_C_PPB 0x0040u
#define V2K_BOARD_CURRENT_CONFIG_ERR_PHASE_A_DCA 0x0080u
#define V2K_BOARD_CURRENT_CONFIG_ERR_PHASE_B_DCA 0x0100u
#define V2K_BOARD_CURRENT_CONFIG_ERR_PHASE_C_DCA 0x0200u
#define V2K_BOARD_CURRENT_CONFIG_ERR_PHASE_A_DCB 0x0400u
#define V2K_BOARD_CURRENT_CONFIG_ERR_PHASE_B_DCB 0x0800u
#define V2K_BOARD_CURRENT_CONFIG_ERR_PHASE_C_DCB 0x1000u
#define V2K_BOARD_CURRENT_CONFIG_ERR_RUNTIME_STATE 0x2000u

#define V2K_BOARD_CURRENT_TRIP_TZ_SIGNAL \
    (EPWM_TZ_SIGNAL_DCAEVT1 | EPWM_TZ_SIGNAL_DCBEVT1)
#define V2K_BOARD_CURRENT_TRIP_TZ_FLAG \
    (EPWM_TZ_FLAG_DCAEVT1 | EPWM_TZ_FLAG_DCBEVT1)
#define V2K_BOARD_CURRENT_TRIP_OST_FLAG \
    (EPWM_TZ_OST_FLAG_DCAEVT1 | EPWM_TZ_OST_FLAG_DCBEVT1)

#define V2K_BOARD_POWER_TRIP_MUX_CONFIG_MASK 0xFF00000CuL
#define V2K_BOARD_POWER_TRIP_MUX_CONFIG_VALUE 0x0000000CuL

static volatile uint16_t s_current_trip_armed;
static volatile uint16_t s_current_trip_last;
static volatile uint16_t s_current_trip_config_error;
static volatile uint16_t s_current_limit_low = V2K_BOARD_CURRENT_LIMIT_LOW_COUNTS;
static volatile uint16_t s_current_limit_high =
    V2K_BOARD_CURRENT_LIMIT_HIGH_COUNTS;

static uint16_t v2k_board_pwm_base_is_locked(uint32_t base)
{
    return (EPWM_getTripZoneFlagStatus(base) & EPWM_TZ_FLAG_OST) != 0u;
}

static void v2k_board_pwm_set_cmpa(uint32_t base, float duty)
{
    uint16_t cmpa = (uint16_t)((float)V2K_TB_PRD * (1.0f - duty));
    EPWM_setCounterCompareValue(base, EPWM_COUNTER_COMPARE_A, cmpa);
}

static void v2k_board_pwm_force_ost_all(void)
{
    EPWM_forceTripZoneEvent(V2K_BOARD_PWM_PHASE_A_BASE, EPWM_TZ_FORCE_EVENT_OST);
    EPWM_forceTripZoneEvent(V2K_BOARD_PWM_PHASE_B_BASE, EPWM_TZ_FORCE_EVENT_OST);
    EPWM_forceTripZoneEvent(V2K_BOARD_PWM_PHASE_C_BASE, EPWM_TZ_FORCE_EVENT_OST);
}

static void v2k_board_pwm_clear_tz_all(uint16_t flags)
{
    EPWM_clearTripZoneFlag(V2K_BOARD_PWM_PHASE_A_BASE, flags);
    EPWM_clearTripZoneFlag(V2K_BOARD_PWM_PHASE_B_BASE, flags);
    EPWM_clearTripZoneFlag(V2K_BOARD_PWM_PHASE_C_BASE, flags);
}

static void v2k_board_pwm_clear_current_trip_events_all(void)
{
    EPWM_clearTripZoneFlag(V2K_BOARD_PWM_PHASE_A_BASE,
                           V2K_BOARD_CURRENT_TRIP_TZ_FLAG);
    EPWM_clearTripZoneFlag(V2K_BOARD_PWM_PHASE_B_BASE,
                           V2K_BOARD_CURRENT_TRIP_TZ_FLAG);
    EPWM_clearTripZoneFlag(V2K_BOARD_PWM_PHASE_C_BASE,
                           V2K_BOARD_CURRENT_TRIP_TZ_FLAG);
    EPWM_clearOneShotTripZoneFlag(V2K_BOARD_PWM_PHASE_A_BASE,
                                  V2K_BOARD_CURRENT_TRIP_OST_FLAG);
    EPWM_clearOneShotTripZoneFlag(V2K_BOARD_PWM_PHASE_B_BASE,
                                  V2K_BOARD_CURRENT_TRIP_OST_FLAG);
    EPWM_clearOneShotTripZoneFlag(V2K_BOARD_PWM_PHASE_C_BASE,
                                  V2K_BOARD_CURRENT_TRIP_OST_FLAG);
}

// Install (armed) or remove (disarmed) the DCAEVT1/DCBEVT1 output force on all
// phases. Armed = force both EPWMxA and EPWMxB low on a current trip; disarmed =
// no action, so the level-sensitive DC events cannot drive pins outside an armed
// POWERED run.
static void v2k_board_pwm_set_current_trip_output_action(uint16_t armed)
{
    EPWM_TripZoneAction action =
        (armed != 0u) ? EPWM_TZ_ACTION_LOW : EPWM_TZ_ACTION_DISABLE;

    EPWM_setTripZoneAction(V2K_BOARD_PWM_PHASE_A_BASE,
                           EPWM_TZ_ACTION_EVENT_DCAEVT1, action);
    EPWM_setTripZoneAction(V2K_BOARD_PWM_PHASE_B_BASE,
                           EPWM_TZ_ACTION_EVENT_DCAEVT1, action);
    EPWM_setTripZoneAction(V2K_BOARD_PWM_PHASE_C_BASE,
                           EPWM_TZ_ACTION_EVENT_DCAEVT1, action);
    EPWM_setTripZoneAction(V2K_BOARD_PWM_PHASE_A_BASE,
                           EPWM_TZ_ACTION_EVENT_DCBEVT1, action);
    EPWM_setTripZoneAction(V2K_BOARD_PWM_PHASE_B_BASE,
                           EPWM_TZ_ACTION_EVENT_DCBEVT1, action);
    EPWM_setTripZoneAction(V2K_BOARD_PWM_PHASE_C_BASE,
                           EPWM_TZ_ACTION_EVENT_DCBEVT1, action);
}

static uint16_t v2k_board_pwm_current_trip_state_base_is_valid(uint32_t base,
                                                           uint16_t armed)
{
    uint16_t tzsel = HWREGH(base + EPWM_O_TZSEL);
    uint16_t tzctl = HWREGH(base + EPWM_O_TZCTL);
    uint16_t expected_signal = (armed != 0u) ? V2K_BOARD_CURRENT_TRIP_TZ_SIGNAL : 0u;
    uint16_t expected_action = (armed != 0u) ?
        (uint16_t)EPWM_TZ_ACTION_LOW : (uint16_t)EPWM_TZ_ACTION_DISABLE;

    return
        ((tzsel & V2K_BOARD_CURRENT_TRIP_TZ_SIGNAL) == expected_signal) &&
        (((tzctl & EPWM_TZCTL_DCAEVT1_M) >> EPWM_TZCTL_DCAEVT1_S) ==
         expected_action) &&
        (((tzctl & EPWM_TZCTL_DCBEVT1_M) >> EPWM_TZCTL_DCBEVT1_S) ==
         expected_action);
}

static uint16_t v2k_board_pwm_current_trip_state_is_valid(uint16_t armed)
{
    return
        v2k_board_pwm_current_trip_state_base_is_valid(V2K_BOARD_PWM_PHASE_A_BASE,
                                                   armed) &&
        v2k_board_pwm_current_trip_state_base_is_valid(V2K_BOARD_PWM_PHASE_B_BASE,
                                                   armed) &&
        v2k_board_pwm_current_trip_state_base_is_valid(V2K_BOARD_PWM_PHASE_C_BASE,
                                                   armed);
}

static void v2k_board_pwm_set_current_trip_signals(uint16_t armed)
{
    if (armed != 0u)
    {
        EPWM_enableTripZoneSignals(V2K_BOARD_PWM_PHASE_A_BASE,
                                   V2K_BOARD_CURRENT_TRIP_TZ_SIGNAL);
        EPWM_enableTripZoneSignals(V2K_BOARD_PWM_PHASE_B_BASE,
                                   V2K_BOARD_CURRENT_TRIP_TZ_SIGNAL);
        EPWM_enableTripZoneSignals(V2K_BOARD_PWM_PHASE_C_BASE,
                                   V2K_BOARD_CURRENT_TRIP_TZ_SIGNAL);
    }
    else
    {
        EPWM_disableTripZoneSignals(V2K_BOARD_PWM_PHASE_A_BASE,
                                    V2K_BOARD_CURRENT_TRIP_TZ_SIGNAL);
        EPWM_disableTripZoneSignals(V2K_BOARD_PWM_PHASE_B_BASE,
                                    V2K_BOARD_CURRENT_TRIP_TZ_SIGNAL);
        EPWM_disableTripZoneSignals(V2K_BOARD_PWM_PHASE_C_BASE,
                                    V2K_BOARD_CURRENT_TRIP_TZ_SIGNAL);
    }
}

static uint16_t v2k_board_pwm_apply_current_trip_state(uint16_t armed)
{
    if (armed != 0u)
    {
        v2k_board_pwm_set_current_trip_output_action(1u);
        v2k_board_pwm_set_current_trip_signals(1u);
        s_current_trip_armed = 1u;
    }
    else
    {
        v2k_board_pwm_set_current_trip_signals(0u);
        v2k_board_pwm_set_current_trip_output_action(0u);
        s_current_trip_armed = 0u;
    }

    return v2k_board_pwm_current_trip_state_is_valid(armed);
}

static uint16_t v2k_board_pwm_current_trip_dca_is_valid(uint32_t base)
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

static uint16_t v2k_board_pwm_current_trip_dcb_is_valid(uint32_t base)
{
    uint16_t dctripsel = HWREGH(base + EPWM_O_DCTRIPSEL);
    uint16_t tzdcsel = HWREGH(base + EPWM_O_TZDCSEL);
    uint16_t dcbctl = HWREGH(base + EPWM_O_DCBCTL);

    return
        (((dctripsel & EPWM_DCTRIPSEL_DCBHCOMPSEL_M) >>
          EPWM_DCTRIPSEL_DCBHCOMPSEL_S) ==
         (uint16_t)EPWM_DC_TRIP_TRIPIN7) &&
        (((tzdcsel & EPWM_TZDCSEL_DCBEVT1_M) >>
          EPWM_TZDCSEL_DCBEVT1_S) ==
         (uint16_t)EPWM_TZ_EVENT_DCXH_HIGH) &&
        ((dcbctl & EPWM_DCBCTL_EVT1SRCSEL) == 0u) &&
        ((dcbctl & EPWM_DCBCTL_EVT1FRCSYNCSEL) != 0u);
}

static uint16_t v2k_board_pwm_current_trip_config_errors(void)
{
    uint32_t enabled_muxes = HWREG(XBARA_EPWM_EN_REG_BASE +
                                   (uint32_t)POWER_TRIP_XBAR);
    uint32_t mux_config = HWREG(XBARA_EPWM_CFG_REG_BASE +
                                ((uint32_t)POWER_TRIP_XBAR << 1u));
    uint16_t errors = 0u;

    if (CMPSS_getDACValueHigh(PHASE_A_CURRENT_CMPSS_BASE) !=
        V2K_BOARD_CURRENT_LIMIT_HIGH_COUNTS)
    {
        errors |= V2K_BOARD_CURRENT_CONFIG_ERR_A_DAC_HIGH;
    }
    if (CMPSS_getDACValueLow(PHASE_A_CURRENT_CMPSS_BASE) !=
        V2K_BOARD_CURRENT_LIMIT_LOW_COUNTS)
    {
        errors |= V2K_BOARD_CURRENT_CONFIG_ERR_A_DAC_LOW;
    }
    if (CMPSS_getDACValueHigh(PHASE_B_CURRENT_CMPSS_BASE) !=
        V2K_BOARD_CURRENT_LIMIT_HIGH_COUNTS)
    {
        errors |= V2K_BOARD_CURRENT_CONFIG_ERR_B_DAC_HIGH;
    }
    if (CMPSS_getDACValueLow(PHASE_B_CURRENT_CMPSS_BASE) !=
        V2K_BOARD_CURRENT_LIMIT_LOW_COUNTS)
    {
        errors |= V2K_BOARD_CURRENT_CONFIG_ERR_B_DAC_LOW;
    }
    if ((enabled_muxes & (uint32_t)POWER_TRIP_XBAR_ENABLED_MUXES) !=
        (uint32_t)POWER_TRIP_XBAR_ENABLED_MUXES)
    {
        errors |= V2K_BOARD_CURRENT_CONFIG_ERR_XBAR_ENABLE;
    }
    if ((mux_config & V2K_BOARD_POWER_TRIP_MUX_CONFIG_MASK) !=
        V2K_BOARD_POWER_TRIP_MUX_CONFIG_VALUE)
    {
        errors |= V2K_BOARD_CURRENT_CONFIG_ERR_XBAR_MUX;
    }
    if (v2k_board_adc_current_limit_config_is_valid() == 0u)
    {
        errors |= V2K_BOARD_CURRENT_CONFIG_ERR_PHASE_C_PPB;
    }
    if (v2k_board_pwm_current_trip_dca_is_valid(V2K_BOARD_PWM_PHASE_A_BASE) == 0u)
    {
        errors |= V2K_BOARD_CURRENT_CONFIG_ERR_PHASE_A_DCA;
    }
    if (v2k_board_pwm_current_trip_dca_is_valid(V2K_BOARD_PWM_PHASE_B_BASE) == 0u)
    {
        errors |= V2K_BOARD_CURRENT_CONFIG_ERR_PHASE_B_DCA;
    }
    if (v2k_board_pwm_current_trip_dca_is_valid(V2K_BOARD_PWM_PHASE_C_BASE) == 0u)
    {
        errors |= V2K_BOARD_CURRENT_CONFIG_ERR_PHASE_C_DCA;
    }
    if (v2k_board_pwm_current_trip_dcb_is_valid(V2K_BOARD_PWM_PHASE_A_BASE) == 0u)
    {
        errors |= V2K_BOARD_CURRENT_CONFIG_ERR_PHASE_A_DCB;
    }
    if (v2k_board_pwm_current_trip_dcb_is_valid(V2K_BOARD_PWM_PHASE_B_BASE) == 0u)
    {
        errors |= V2K_BOARD_CURRENT_CONFIG_ERR_PHASE_B_DCB;
    }
    if (v2k_board_pwm_current_trip_dcb_is_valid(V2K_BOARD_PWM_PHASE_C_BASE) == 0u)
    {
        errors |= V2K_BOARD_CURRENT_CONFIG_ERR_PHASE_C_DCB;
    }
    return errors;
}

static uint16_t v2k_board_pwm_current_trip_config_is_valid(void)
{
    s_current_trip_config_error = v2k_board_pwm_current_trip_config_errors();
    if (v2k_board_pwm_current_trip_state_is_valid(s_current_trip_armed) == 0u)
    {
        s_current_trip_config_error |= V2K_BOARD_CURRENT_CONFIG_ERR_RUNTIME_STATE;
    }
    return s_current_trip_config_error == 0u;
}

static uint16_t v2k_board_pwm_capture_current_sources(void)
{
    uint16_t phase_a = CMPSS_getStatus(PHASE_A_CURRENT_CMPSS_BASE);
    uint16_t phase_b = CMPSS_getStatus(PHASE_B_CURRENT_CMPSS_BASE);
    uint16_t sources = v2k_board_adc_current_limit_status();

    if ((phase_a & CMPSS_STS_HI_LATCHFILTOUT) != 0u)
    {
        sources |= V2K_BOARD_CURRENT_SOURCE_PHASE_A_HIGH;
    }
    if ((phase_a & CMPSS_STS_LO_LATCHFILTOUT) != 0u)
    {
        sources |= V2K_BOARD_CURRENT_SOURCE_PHASE_A_LOW;
    }
    if ((phase_b & CMPSS_STS_HI_LATCHFILTOUT) != 0u)
    {
        sources |= V2K_BOARD_CURRENT_SOURCE_PHASE_B_HIGH;
    }
    if ((phase_b & CMPSS_STS_LO_LATCHFILTOUT) != 0u)
    {
        sources |= V2K_BOARD_CURRENT_SOURCE_PHASE_B_LOW;
    }
    return sources;
}

static void v2k_board_pwm_clear_current_source_flags(void)
{
    CMPSS_clearFilterLatchHigh(PHASE_A_CURRENT_CMPSS_BASE);
    CMPSS_clearFilterLatchLow(PHASE_A_CURRENT_CMPSS_BASE);
    CMPSS_clearFilterLatchHigh(PHASE_B_CURRENT_CMPSS_BASE);
    CMPSS_clearFilterLatchLow(PHASE_B_CURRENT_CMPSS_BASE);
    v2k_board_adc_clear_current_limit_status();
}

static uint16_t v2k_board_pwm_check_common(uint32_t base, uint16_t phase_slave)
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
         V2K_BOARD_PWM_DEADBAND_COUNTS) ||
        ((HWREGH(base + EPWM_O_DBFED) & EPWM_DBFED_DBFED_M) !=
         V2K_BOARD_PWM_DEADBAND_COUNTS))
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

static uint16_t v2k_board_pwm_config_is_valid(void)
{
    uint16_t etsel = HWREGH(V2K_BOARD_PWM_MASTER_BASE + EPWM_O_ETSEL);
    uint16_t syncouten = HWREGH(V2K_BOARD_PWM_MASTER_BASE + EPWM_O_SYNCOUTEN);
    uint16_t ediv = HWREGH(CLKCFG_BASE + SYSCTL_O_PERCLKDIVSEL) &
                    SYSCTL_PERCLKDIVSEL_EPWMCLKDIV_M;
    uint16_t input = (uint16_t)TZ_EXT_INPUT_XBAR_INPUT;
    uint32_t input_lock = 1UL << input;

    return
        (ediv == 0u) &&
        (HWREGH(INPUTXBAR_BASE + XBAR_O_INPUT1SELECT + input) ==
         (uint16_t)TZ_EXT_INPUT_XBAR_SOURCE) &&
        ((HWREG(INPUTXBAR_BASE + XBAR_O_INPUTSELECTLOCK) & input_lock) != 0u) &&
        v2k_board_pwm_check_common(V2K_BOARD_PWM_PHASE_A_BASE, 1u) &&
        v2k_board_pwm_check_common(V2K_BOARD_PWM_PHASE_B_BASE, 0u) &&
        v2k_board_pwm_check_common(V2K_BOARD_PWM_PHASE_C_BASE, 1u) &&
        ((syncouten & EPWM_SYNCOUTEN_ZEROEN) != 0u) &&
        ((etsel & EPWM_ETSEL_SOCAEN) != 0u) &&
        (((etsel & EPWM_ETSEL_SOCASEL_M) >> EPWM_ETSEL_SOCASEL_S) ==
         (uint16_t)V2K_BOARD_PWM_SOC_SOURCE);
}

uint16_t v2k_board_pwm_prepare_timebase(void)
{
    EPWM_setSyncInPulseSource(V2K_BOARD_PWM_PHASE_A_BASE,
                              EPWM_SYNC_IN_PULSE_SRC_SYNCOUT_EPWM1);
    EPWM_setSyncInPulseSource(V2K_BOARD_PWM_PHASE_C_BASE,
                              EPWM_SYNC_IN_PULSE_SRC_SYNCOUT_EPWM1);
    EPWM_setADCTriggerSource(V2K_BOARD_PWM_MASTER_BASE,
                             EPWM_SOC_A,
                             V2K_BOARD_PWM_SOC_SOURCE);

    if (v2k_board_pwm_config_is_valid() == 0u)
    {
        return 0u;
    }
    v2k_board_pwm_apply_neutral();
    return 1u;
}

void v2k_board_pwm_start_timebase(void)
{
    SysCtl_enablePeripheral(SYSCTL_PERIPH_CLK_TBCLKSYNC);
    EPWM_forceSyncPulse(V2K_BOARD_PWM_MASTER_BASE);
}

void v2k_board_pwm_apply(float duty_a, float duty_b, float duty_c)
{
    v2k_board_pwm_set_cmpa(V2K_BOARD_PWM_PHASE_A_BASE, duty_a);
    v2k_board_pwm_set_cmpa(V2K_BOARD_PWM_PHASE_B_BASE, duty_b);
    v2k_board_pwm_set_cmpa(V2K_BOARD_PWM_PHASE_C_BASE, duty_c);
}

void v2k_board_pwm_apply_neutral(void)
{
    EPWM_setCounterCompareValue(V2K_BOARD_PWM_PHASE_A_BASE,
                                EPWM_COUNTER_COMPARE_A,
                                V2K_BOARD_PWM_NEUTRAL_CMPA);
    EPWM_setCounterCompareValue(V2K_BOARD_PWM_PHASE_B_BASE,
                                EPWM_COUNTER_COMPARE_A,
                                V2K_BOARD_PWM_NEUTRAL_CMPA);
    EPWM_setCounterCompareValue(V2K_BOARD_PWM_PHASE_C_BASE,
                                EPWM_COUNTER_COMPARE_A,
                                V2K_BOARD_PWM_NEUTRAL_CMPA);
}

uint16_t v2k_board_pwm_counter_value(void)
{
    return (uint16_t)EPWM_getTimeBaseCounterValue(V2K_BOARD_PWM_MASTER_BASE);
}

void v2k_board_pwm_pre_board_lock(void)
{
    SysCtl_enablePeripheral(SYSCTL_PERIPH_CLK_EPWM1);
    SysCtl_enablePeripheral(SYSCTL_PERIPH_CLK_EPWM2);
    SysCtl_enablePeripheral(SYSCTL_PERIPH_CLK_EPWM8);
    asm(" RPT #5 || NOP");
    v2k_board_pwm_force_ost_all();
}

void v2k_board_pwm_init_trip_isr(void (*tz_isr)(void))
{
    s_current_trip_armed = 0u;
    s_current_trip_last = 0u;
    v2k_board_pwm_apply_neutral();
    v2k_board_pwm_force_ost_all();
    (void)v2k_board_pwm_apply_current_trip_state(0u);
    (void)v2k_board_pwm_current_trip_config_is_valid();
    v2k_board_pwm_clear_current_source_flags();
    v2k_board_pwm_clear_current_trip_events_all();
    v2k_board_pwm_clear_tz_all(EPWM_TZ_INTERRUPT);
    Interrupt_register(INT_EPWM1_TZ, tz_isr);
    Interrupt_disable(INT_EPWM1_TZ);
}

void v2k_board_pwm_disable_trip_irq(void)
{
    Interrupt_disable(INT_EPWM1_TZ);
    EPWM_disableTripZoneInterrupt(V2K_BOARD_PWM_PHASE_A_BASE,
                                  EPWM_TZ_INTERRUPT_OST);
    EPWM_disableTripZoneInterrupt(V2K_BOARD_PWM_PHASE_B_BASE,
                                  EPWM_TZ_INTERRUPT_OST);
    EPWM_disableTripZoneInterrupt(V2K_BOARD_PWM_PHASE_C_BASE,
                                  EPWM_TZ_INTERRUPT_OST);
}

void v2k_board_pwm_enable_trip_irq(void)
{
    EPWM_enableTripZoneInterrupt(V2K_BOARD_PWM_PHASE_A_BASE,
                                 EPWM_TZ_INTERRUPT_OST);
    EPWM_enableTripZoneInterrupt(V2K_BOARD_PWM_PHASE_B_BASE,
                                 EPWM_TZ_INTERRUPT_OST);
    EPWM_enableTripZoneInterrupt(V2K_BOARD_PWM_PHASE_C_BASE,
                                 EPWM_TZ_INTERRUPT_OST);
    Interrupt_enable(INT_EPWM1_TZ);
}

void v2k_board_pwm_clear_trip_interrupt(void)
{
    v2k_board_pwm_clear_tz_all(EPWM_TZ_INTERRUPT);
}

void v2k_board_pwm_force_output_lock(void)
{
    v2k_board_pwm_force_ost_all();
}

uint16_t v2k_board_pwm_release_output_lock(void)
{
    if ((s_current_trip_armed != 0u) &&
        (v2k_board_pwm_current_trip_was_active() != 0u))
    {
        return 0u;
    }

    v2k_board_pwm_clear_tz_all(EPWM_TZ_INTERRUPT | EPWM_TZ_FLAG_OST);

    if (((s_current_trip_armed != 0u) &&
         (v2k_board_pwm_current_trip_was_active() != 0u)) ||
        (v2k_board_pwm_output_is_locked() != 0u))
    {
        v2k_board_pwm_force_ost_all();
        return 0u;
    }
    return 1u;
}

uint16_t v2k_board_pwm_output_is_locked(void)
{
    return v2k_board_pwm_base_is_locked(V2K_BOARD_PWM_PHASE_A_BASE) ||
           v2k_board_pwm_base_is_locked(V2K_BOARD_PWM_PHASE_B_BASE) ||
           v2k_board_pwm_base_is_locked(V2K_BOARD_PWM_PHASE_C_BASE);
}

void v2k_board_pwm_ack_trip_isr(void)
{
    v2k_board_pwm_clear_trip_interrupt();
    Interrupt_clearACKGroup(INTERRUPT_ACK_GROUP2);
}

uint16_t v2k_board_pwm_arm_current_trip(void)
{
    if ((v2k_board_pwm_current_trip_config_is_valid() == 0u) ||
        (v2k_board_adc_current_window_is_valid() == 0u))
    {
        return 0u;
    }

    v2k_board_pwm_clear_current_source_flags();
    v2k_board_pwm_clear_current_trip_events_all();
    s_current_trip_last = 0u;

    if (v2k_board_pwm_apply_current_trip_state(1u) == 0u)
    {
        v2k_board_pwm_force_ost_all();
        (void)v2k_board_pwm_apply_current_trip_state(0u);
        s_current_trip_config_error = v2k_board_pwm_current_trip_config_errors() |
            V2K_BOARD_CURRENT_CONFIG_ERR_RUNTIME_STATE;
        return 0u;
    }
    s_current_trip_config_error = v2k_board_pwm_current_trip_config_errors();

    if (v2k_board_pwm_capture_current_sources() != 0u)
    {
        (void)v2k_board_pwm_current_trip_was_active();
        return 0u;
    }
    return 1u;
}

void v2k_board_pwm_disarm_current_trip(void)
{
    uint16_t state_valid = v2k_board_pwm_apply_current_trip_state(0u);

    if (state_valid == 0u)
    {
        v2k_board_pwm_force_ost_all();
    }
    v2k_board_pwm_clear_current_trip_events_all();
    v2k_board_pwm_clear_current_source_flags();
    (void)v2k_board_pwm_current_trip_config_is_valid();
}

uint16_t v2k_board_pwm_current_trip_was_active(void)
{
    uint16_t ost_sources =
        EPWM_getOneShotTripZoneFlagStatus(V2K_BOARD_PWM_MASTER_BASE);
    uint16_t sources;

    if ((ost_sources & V2K_BOARD_CURRENT_TRIP_OST_FLAG) == 0u)
    {
        return 0u;
    }

    sources = v2k_board_pwm_capture_current_sources();
    if (sources == 0u)
    {
        sources = V2K_BOARD_CURRENT_SOURCE_AGGREGATE;
    }
    s_current_trip_last = sources;
    return 1u;
}

volatile uint16_t *v2k_board_pwm_current_trip_armed_address(void)
{
    return &s_current_trip_armed;
}

volatile uint16_t *v2k_board_pwm_current_trip_last_address(void)
{
    return &s_current_trip_last;
}

volatile uint16_t *v2k_board_pwm_current_trip_config_error_address(void)
{
    return &s_current_trip_config_error;
}

volatile uint16_t *v2k_board_pwm_current_limit_low_address(void)
{
    return &s_current_limit_low;
}

volatile uint16_t *v2k_board_pwm_current_limit_high_address(void)
{
    return &s_current_limit_high;
}
