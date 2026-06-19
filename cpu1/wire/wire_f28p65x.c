//=============================================================================
// wire_f28p65x.c - LAUNCHXL-F28P65X CPU1 wiring implementation
//=============================================================================

#include "driverlib.h"
#include "device.h"
#include "wire.h"
#include "../runtime/v2k_registry.h"
#include "../runtime/v2k_timebase.h"

#define WIRE_ADC_REF_V    3.0f
#define WIRE_ADC_MAX_CODE 4095.0f
#define WIRE_DUTY_MIN     0.02f
#define WIRE_DUTY_MAX     0.98f
#define WIRE_TB_CMPA_INIT ((V2K_TB_PRD * 3u) / 4u)
#define WIRE_TB_PROBE_GPIO 2u
#define WIRE_TB_SOC_SRC   EPWM_SOC_TBCTR_ZERO
#define WIRE_FAULT_TZ_GPIO 3u

volatile uint16_t g_wire_adc_a0_raw;
volatile float g_wire_vsense;
volatile float g_wire_duty_a_applied = V2K_DUTY_A_SAFE;

void wire_panic_halt(void)
{
    for (;;) { ESTOP0; }
}

static void wire_timebase_check(void)
{
    uint16_t tzsel = HWREGH(EPWM1_BASE + EPWM_O_TZSEL);
    uint16_t tzctl = HWREGH(EPWM1_BASE + EPWM_O_TZCTL);
    uint16_t tbctl = HWREGH(EPWM1_BASE + EPWM_O_TBCTL);
    uint16_t etsel = HWREGH(EPWM1_BASE + EPWM_O_ETSEL);
    uint16_t timer_tcr = HWREGH(CPUTIMER1_BASE + CPUTIMER_O_TCR);
    uint16_t ediv = HWREGH(CLKCFG_BASE + SYSCTL_O_PERCLKDIVSEL) &
                    SYSCTL_PERCLKDIVSEL_EPWMCLKDIV_M;

    if ((ediv != 0u) ||
        (EPWM_getTimeBasePeriod(EPWM1_BASE) != (uint16_t)V2K_TB_PRD) ||
        ((tzsel & (EPWM_TZSEL_OSHT1 | EPWM_TZSEL_CBC6)) !=
                  (EPWM_TZSEL_OSHT1 | EPWM_TZSEL_CBC6)) ||
        (((tzctl & EPWM_TZCTL_TZA_M) >> EPWM_TZCTL_TZA_S)
            != (uint16_t)EPWM_TZ_ACTION_LOW) ||
        (((tzctl & EPWM_TZCTL_TZB_M) >> EPWM_TZCTL_TZB_S)
            != (uint16_t)EPWM_TZ_ACTION_LOW) ||
        ((etsel & EPWM_ETSEL_SOCAEN) == 0u) ||
        (((etsel & EPWM_ETSEL_SOCASEL_M) >> EPWM_ETSEL_SOCASEL_S)
            != (uint16_t)WIRE_TB_SOC_SRC) ||
        (((tbctl & EPWM_TBCTL_FREE_SOFT_M) >> EPWM_TBCTL_FREE_SOFT_S) < 2u) ||
        (HWREG(CPUTIMER1_BASE + CPUTIMER_O_PRD) != 0xFFFFFFFFuL) ||
        ((HWREGH(CPUTIMER1_BASE + CPUTIMER_O_TPR) & CPUTIMER_TPR_TDDR_M) != 0u) ||
        ((HWREGH(CPUTIMER1_BASE + CPUTIMER_O_TPRH) &
          CPUTIMER_TPRH_TDDRH_M) != 0u) ||
        ((timer_tcr & CPUTIMER_TCR_TSS) != 0u) ||
        ((timer_tcr & CPUTIMER_TCR_TIE) != 0u) ||
        ((timer_tcr & CPUTIMER_TCR_FREE) == 0u) ||
        ((tbctl & (EPWM_TBCTL_CLKDIV_M | EPWM_TBCTL_HSPCLKDIV_M)) != 0u))
    {
        wire_panic_halt();
    }
}

void wire_timebase_init(wire_isr_handler_t adc_isr)
{
    EPWM_setADCTriggerSource(EPWM1_BASE, EPWM_SOC_A, WIRE_TB_SOC_SRC);
    wire_timebase_check();
    EPWM_setCounterCompareValue(EPWM1_BASE, EPWM_COUNTER_COMPARE_A,
                                (uint16_t)WIRE_TB_CMPA_INIT);
    ADC_clearInterruptStatus(ADCA_BASE, ADC_INT_NUMBER1);
    Interrupt_register(INT_ADCA1, adc_isr);
    Interrupt_enable(INT_ADCA1);
}

void wire_timebase_start(void)
{
    SysCtl_enablePeripheral(SYSCTL_PERIPH_CLK_TBCLKSYNC);
}

static float wire_clamp_duty(float duty)
{
    if (!(duty >= WIRE_DUTY_MIN))
    {
        return WIRE_DUTY_MIN;
    }
    if (duty > WIRE_DUTY_MAX)
    {
        return WIRE_DUTY_MAX;
    }
    return duty;
}

void wire_acquire(volatile v2k_io_in_t *in)
{
    uint16_t raw = ADC_readResult(ADCARESULT_BASE, ADC_SOC_NUMBER0);
    float volts = ((float)raw * WIRE_ADC_REF_V) / WIRE_ADC_MAX_CODE;

    g_wire_adc_a0_raw = raw;
    g_wire_vsense = volts;

    in->tick = g_v2k_tick;
    in->vsense = volts;
}

void wire_apply(const volatile v2k_io_out_t *out)
{
    float duty = wire_clamp_duty(out->duty_a);
    uint16_t cmpa = (uint16_t)((float)V2K_TB_PRD * (1.0f - duty));

    g_wire_duty_a_applied = duty;
    EPWM_setCounterCompareValue(EPWM1_BASE, EPWM_COUNTER_COMPARE_A, cmpa);
}

uint32_t wire_cycle_count(void)
{
    return CPUTimer_getTimerCount(CPUTIMER1_BASE);
}

uint16_t wire_pwm_counter(void)
{
    return (uint16_t)EPWM_getTimeBaseCounterValue(EPWM1_BASE);
}

uint16_t wire_isr_ack(void)
{
    uint16_t overflow = ADC_getInterruptOverflowStatus(ADCA_BASE, ADC_INT_NUMBER1);

    if (overflow != 0u)
    {
        ADC_clearInterruptOverflowStatus(ADCA_BASE, ADC_INT_NUMBER1);
    }
    ADC_clearInterruptStatus(ADCA_BASE, ADC_INT_NUMBER1);
    Interrupt_clearACKGroup(INTERRUPT_ACK_GROUP1);

    return overflow;
}

void wire_gpio_probe_set(uint16_t value)
{
    GPIO_writePin(WIRE_TB_PROBE_GPIO, value);
}

void wire_register_ports(uint16_t fast_prescaler)
{
    v2k_registry_add("vsense", V2K_TYPE_F32, V2K_KIND_SCOPE,
                     (volatile void *)&v2k_io.in.vsense, fast_prescaler);
    v2k_registry_add("duty_a_cmd", V2K_TYPE_F32, V2K_KIND_SCOPE,
                     (volatile void *)&v2k_io.out.duty_a, fast_prescaler);
    v2k_registry_add("duty_a", V2K_TYPE_F32, V2K_KIND_SCOPE,
                     &g_wire_duty_a_applied, fast_prescaler);
}

void wire_fault_pre_board_lock_outputs(void)
{
    SysCtl_enablePeripheral(SYSCTL_PERIPH_CLK_EPWM1);
    asm(" RPT #5 || NOP");
    EPWM_forceTripZoneEvent(EPWM1_BASE, EPWM_TZ_FORCE_EVENT_OST);
}

void wire_fault_init_trip_isr(wire_isr_handler_t tz_isr)
{
    EPWM_forceTripZoneEvent(EPWM1_BASE, EPWM_TZ_FORCE_EVENT_OST);
    EPWM_clearTripZoneFlag(EPWM1_BASE, EPWM_TZ_INTERRUPT);
    Interrupt_register(INT_EPWM1_TZ, tz_isr);
    Interrupt_disable(INT_EPWM1_TZ);
}

void wire_fault_disable_irq(void)
{
    Interrupt_disable(INT_EPWM1_TZ);
    EPWM_disableTripZoneInterrupt(EPWM1_BASE, EPWM_TZ_INTERRUPT_OST);
}

void wire_fault_enable_irq(void)
{
    EPWM_enableTripZoneInterrupt(EPWM1_BASE, EPWM_TZ_INTERRUPT_OST);
    Interrupt_enable(INT_EPWM1_TZ);
}

void wire_fault_clear_interrupt_flag(void)
{
    EPWM_clearTripZoneFlag(EPWM1_BASE, EPWM_TZ_INTERRUPT);
}

void wire_fault_force_output_lock(void)
{
    EPWM_forceTripZoneEvent(EPWM1_BASE, EPWM_TZ_FORCE_EVENT_OST);
}

void wire_fault_release_output_lock(void)
{
    EPWM_clearTripZoneFlag(EPWM1_BASE,
                           EPWM_TZ_INTERRUPT | EPWM_TZ_FLAG_OST);
}

uint16_t wire_fault_output_is_locked(void)
{
    return (EPWM_getTripZoneFlagStatus(EPWM1_BASE) & EPWM_TZ_FLAG_OST) != 0u;
}

uint16_t wire_fault_source_is_released(void)
{
    return GPIO_readPin(WIRE_FAULT_TZ_GPIO) != 0u;
}

void wire_fault_ack_isr(void)
{
    wire_fault_clear_interrupt_flag();
    Interrupt_clearACKGroup(INTERRUPT_ACK_GROUP2);
}
