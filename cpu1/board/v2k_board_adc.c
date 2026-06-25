//=============================================================================
// v2k_board_adc.c - F28P65x motor ADC schedule validation and ISR ownership
//=============================================================================

#include "driverlib.h"
#include "board.h"
#include "v2k_board_adc.h"

static uint16_t v2k_board_adc_count_in_current_window(uint16_t count)
{
    return (count >= V2K_BOARD_CURRENT_LIMIT_LOW_COUNTS) &&
           (count <= V2K_BOARD_CURRENT_LIMIT_HIGH_COUNTS);
}

static uint16_t v2k_board_adc_soc_trigger_matches(uint32_t base,
                                             uint16_t soc,
                                             uint16_t trigger)
{
    uint32_t reg = HWREG(base + ADC_O_SOC0CTL + ((uint32_t)soc * 2uL));
    uint16_t actual = (uint16_t)((reg & ADC_SOC0CTL_TRIGSEL_M) >>
                                 ADC_SOC0CTL_TRIGSEL_S);

    return actual == trigger;
}

static uint16_t v2k_board_adc_control_interrupt_is_valid(void)
{
    uint16_t reg = HWREGH(myADC0_BASE + ADC_O_INTSEL1N2);
    uint16_t ctl1 = HWREGH(myADC0_BASE + ADC_O_CTL1);

    return
        ((ctl1 & ADC_CTL1_INTPULSEPOS) ==
         (uint16_t)ADC_PULSE_END_OF_CONV) &&
        ((reg & ADC_INTSEL1N2_INT1E) != 0u) &&
        ((reg & ADC_INTSEL1N2_INT1SEL_M) == (uint16_t)myADC0_SOC3);
}

uint16_t v2k_board_adc_config_is_valid(void)
{
    return
        v2k_board_adc_soc_trigger_matches(myADC0_BASE, myADC0_SOC0,
                                     ADC_TRIGGER_EPWM1_SOCA) &&
        v2k_board_adc_soc_trigger_matches(myADC0_BASE, myADC0_SOC1,
                                     ADC_TRIGGER_EPWM1_SOCA) &&
        v2k_board_adc_soc_trigger_matches(myADC0_BASE, myADC0_SOC2,
                                     ADC_TRIGGER_EPWM1_SOCA) &&
        v2k_board_adc_soc_trigger_matches(myADC0_BASE, myADC0_SOC3,
                                     ADC_TRIGGER_EPWM1_SOCA) &&
        v2k_board_adc_soc_trigger_matches(myADC1_BASE, myADC1_SOC0,
                                     ADC_TRIGGER_EPWM1_SOCA) &&
        v2k_board_adc_soc_trigger_matches(myADC1_BASE, myADC1_SOC1,
                                     ADC_TRIGGER_EPWM1_SOCA) &&
        v2k_board_adc_soc_trigger_matches(myADC2_BASE, myADC2_SOC0,
                                     ADC_TRIGGER_EPWM1_SOCA) &&
        v2k_board_adc_control_interrupt_is_valid();
}

uint16_t v2k_board_adc_current_limit_config_is_valid(void)
{
    uint32_t high = HWREG(myADC2_BASE + ADC_O_PPB1TRIPHI) &
                    ADC_PPB1TRIPHI_LIMITHI_M;
    uint32_t low_config = HWREG(myADC2_BASE + ADC_O_PPB1TRIPLO);
    uint32_t low = HWREG(myADC2_BASE + ADC_O_PPB1TRIPLO2) &
                   ADC_PPB1TRIPLO2_LIMITLO_M;
    uint16_t events = HWREGH(myADC2_BASE + ADC_O_EVTSEL);
    uint16_t config = HWREGH(myADC2_BASE + ADC_O_PPB1CONFIG);
    uint16_t config2 = HWREGH(myADC2_BASE + ADC_O_PPB1CONFIG2);

    return
        (high == V2K_BOARD_CURRENT_LIMIT_HIGH_COUNTS) &&
        (low == V2K_BOARD_CURRENT_LIMIT_LOW_COUNTS) &&
        ((low_config & ADC_PPB1TRIPLO_LIMITLO2EN) != 0u) &&
        ((config & ADC_PPB1CONFIG_CONFIG_M) ==
         (uint16_t)myADC2_SOC_PHASE_C_CURRENT_LIMIT) &&
        ((config & (ADC_PPB1CONFIG_TWOSCOMPEN |
                    ADC_PPB1CONFIG_ABSEN)) == 0u) &&
        ((events & (ADC_EVTSEL_PPB1TRIPHI |
                    ADC_EVTSEL_PPB1TRIPLO)) ==
                   (ADC_EVTSEL_PPB1TRIPHI |
                    ADC_EVTSEL_PPB1TRIPLO)) &&
        ((config & ADC_PPB1CONFIG_CBCEN) != 0u) &&
        ((config2 & ADC_PPB1CONFIG2_COMPSEL_M) ==
         ((uint16_t)ADC_PPB_COMPSOURCE_RESULT <<
          ADC_PPB1CONFIG2_COMPSEL_S));
}

uint16_t v2k_board_adc_current_window_is_valid(void)
{
    uint16_t phase_a = ADC_readResult(myADC1_RESULT_BASE, myADC1_SOC1);
    uint16_t phase_b = ADC_readResult(myADC0_RESULT_BASE, myADC0_SOC2);
    uint16_t phase_c = ADC_readResult(myADC2_RESULT_BASE, myADC2_SOC0);

    return v2k_board_adc_count_in_current_window(phase_a) &&
           v2k_board_adc_count_in_current_window(phase_b) &&
           v2k_board_adc_count_in_current_window(phase_c);
}

uint16_t v2k_board_adc_current_limit_status(void)
{
    uint16_t status = ADC_getPPBEventStatus(myADC2_BASE,
                                            myADC2_PHASE_C_CURRENT_LIMIT);
    uint16_t sources = 0u;

    if ((status & ADC_EVT_TRIPHI) != 0u)
    {
        sources |= V2K_BOARD_CURRENT_SOURCE_PHASE_C_HIGH;
    }
    if ((status & ADC_EVT_TRIPLO) != 0u)
    {
        sources |= V2K_BOARD_CURRENT_SOURCE_PHASE_C_LOW;
    }
    return sources;
}

void v2k_board_adc_clear_current_limit_status(void)
{
    ADC_clearPPBEventStatus(myADC2_BASE,
                            myADC2_PHASE_C_CURRENT_LIMIT,
                            ADC_EVT_TRIPHI | ADC_EVT_TRIPLO);
}

void v2k_board_adc_init_interrupt(void (*adc_isr)(void))
{
    ADC_clearInterruptStatus(myADC0_BASE, ADC_INT_NUMBER1);
    Interrupt_register(INT_ADCA1, adc_isr);
    Interrupt_enable(INT_ADCA1);
}

uint16_t v2k_board_adc_ack_interrupt(void)
{
    uint16_t overflow =
        ADC_getInterruptOverflowStatus(myADC0_BASE, ADC_INT_NUMBER1);

    if (overflow != 0u)
    {
        ADC_clearInterruptOverflowStatus(myADC0_BASE, ADC_INT_NUMBER1);
    }
    ADC_clearInterruptStatus(myADC0_BASE, ADC_INT_NUMBER1);
    Interrupt_clearACKGroup(INTERRUPT_ACK_GROUP1);
    return overflow;
}
