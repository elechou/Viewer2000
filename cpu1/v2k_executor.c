//=============================================================================
// v2k_executor.c - 固定顺序控制 ISR
//=============================================================================

#include "driverlib.h"
#include "device.h"
#include "v2k_executor.h"
#include "v2k_platform.h"
#include "v2k_timebase.h"
#include "v2k_fault.h"
#include "v2k_registry.h"
#include "v2k_scope_runtime.h"

#define V2K_ADC_REF_V       3.0f
#define V2K_ADC_MAX_CODE    4095.0f
#define V2K_PWM_DUTY_MIN    0.02f
#define V2K_PWM_DUTY_MAX    0.98f
#define V2K_ISR_BUDGET_CYCLES (V2K_EPWMCLK_HZ / V2K_ISR_HZ)
#define V2K_DUE_1KHZ_DIV    (V2K_ISR_HZ / 1000u)
#define V2K_DUE_100HZ_DIV   (V2K_ISR_HZ / 100u)
#define V2K_DUE_100HZ_PHASE (V2K_DUE_1KHZ_DIV / 2u)
#define V2K_PARAM_PHASE      ((V2K_DUE_1KHZ_DIV * 3u) / 4u)

V2K_STATIC_ASSERT((V2K_ISR_HZ % 1000u) == 0u);
V2K_STATIC_ASSERT((V2K_ISR_HZ % 100u) == 0u);
V2K_STATIC_ASSERT((V2K_DUE_1KHZ_DIV & 1u) == 0u);
V2K_STATIC_ASSERT(V2K_PARAM_PHASE > 0u);
V2K_STATIC_ASSERT(V2K_PARAM_PHASE < V2K_DUE_1KHZ_DIV);
V2K_STATIC_ASSERT(V2K_PARAM_PHASE != V2K_DUE_100HZ_PHASE);

volatile v2k_tick_t g_v2k_tick;
volatile uint16_t g_v2k_adc_a0;
volatile float g_v2k_adc_a0_v;
volatile float g_v2k_pwm_duty_cmd = 0.25f;
volatile float g_v2k_pwm_duty_applied = 0.25f;
volatile uint16_t g_v2k_due_mask;
volatile uint16_t g_v2k_isr_lat;
volatile uint16_t g_v2k_isr_lat_min = 0xFFFFu;
volatile uint16_t g_v2k_isr_lat_max;
volatile uint32_t g_v2k_isr_cycles;
volatile uint32_t g_v2k_isr_cycles_max;
volatile uint32_t g_v2k_control_cycles;
volatile uint32_t g_v2k_control_cycles_max;
volatile uint32_t g_v2k_scope_cycles;
volatile uint32_t g_v2k_scope_cycles_max;
volatile uint32_t g_v2k_isr_budget_violation_cnt;
volatile uint32_t g_v2k_isr_ovf_cnt;

static uint16_t s_due_1khz_count;
static uint16_t s_due_100hz_count = V2K_DUE_100HZ_PHASE;
static uint16_t s_param_count = V2K_PARAM_PHASE;

static inline uint16_t v2k_countdown_due(uint16_t *count, uint16_t period)
{
    if (*count == 0u)
    {
        *count = (uint16_t)(period - 1u);
        return 1u;
    }
    (*count)--;
    return 0u;
}

static uint16_t v2k_schedule(uint16_t *param_due)
{
    uint16_t mask = 0u;
    if (v2k_countdown_due(&s_due_1khz_count, V2K_DUE_1KHZ_DIV))
    {
        mask |= PLAT_DUE_1KHZ;
    }
    if (v2k_countdown_due(&s_due_100hz_count, V2K_DUE_100HZ_DIV))
    {
        mask |= PLAT_DUE_100HZ;
    }
    *param_due = v2k_countdown_due(&s_param_count, V2K_DUE_1KHZ_DIV);
    return mask;
}

static void v2k_acquire(plat_in_t *in)
{
    uint16_t raw = ADC_readResult(ADCARESULT_BASE, ADC_SOC_NUMBER0);
    g_v2k_adc_a0 = raw;
    g_v2k_adc_a0_v = ((float)raw * V2K_ADC_REF_V) / V2K_ADC_MAX_CODE;

    in->tick = g_v2k_tick;
    in->adc_a0_raw = raw;
    in->adc_a0_v = g_v2k_adc_a0_v;
    in->sys_state = g_v2k_sm_state;
    in->fault_code = g_v2k_fault_code;
}

static void v2k_apply(float duty)
{
    uint16_t cmpa;
    if (!(duty >= V2K_PWM_DUTY_MIN))
    {
        duty = V2K_PWM_DUTY_MIN;
    }
    else if (duty > V2K_PWM_DUTY_MAX)
    {
        duty = V2K_PWM_DUTY_MAX;
    }
    g_v2k_pwm_duty_applied = duty;
    cmpa = (uint16_t)((float)V2K_TB_PRD * (1.0f - duty));
    EPWM_setCounterCompareValue(EPWM1_BASE, EPWM_COUNTER_COMPARE_A, cmpa);
}

__interrupt void v2k_executor_isr(void)
{
    plat_in_t in;
    plat_out_t out;
    uint16_t latency;
    uint16_t param_due;
    uint32_t cycle_start;
    uint32_t control_end;
    uint32_t scope_end;
    uint32_t elapsed;

    GPIO_writePin(V2K_TB_PROBE_GPIO, 1u);
    cycle_start = CPUTimer_getTimerCount(CPUTIMER1_BASE);
    latency = (uint16_t)EPWM_getTimeBaseCounterValue(EPWM1_BASE);

    v2k_acquire(&in);
    in.due_mask = v2k_schedule(&param_due);
    if (param_due != 0u)
    {
        v2k_param_apply_ready();
    }
    g_v2k_due_mask = in.due_mask;

    out.pwm1_duty = g_v2k_pwm_duty_applied;
    user_step(&in, &out);
    v2k_apply(out.pwm1_duty);

    control_end = CPUTimer_getTimerCount(CPUTIMER1_BASE);
    elapsed = cycle_start - control_end;
    g_v2k_control_cycles = elapsed;
    if (elapsed > g_v2k_control_cycles_max)
    {
        g_v2k_control_cycles_max = elapsed;
    }

    v2k_scope_sample_all(in.tick);
    scope_end = CPUTimer_getTimerCount(CPUTIMER1_BASE);
    elapsed = control_end - scope_end;
    g_v2k_scope_cycles = elapsed;
    if (elapsed > g_v2k_scope_cycles_max)
    {
        g_v2k_scope_cycles_max = elapsed;
    }
    g_v2k_tick++;

    g_v2k_isr_lat = latency;
    if (latency < g_v2k_isr_lat_min)
    {
        g_v2k_isr_lat_min = latency;
    }
    if (latency > g_v2k_isr_lat_max)
    {
        g_v2k_isr_lat_max = latency;
    }

    if (ADC_getInterruptOverflowStatus(ADCA_BASE, ADC_INT_NUMBER1))
    {
        g_v2k_isr_ovf_cnt++;
        ADC_clearInterruptOverflowStatus(ADCA_BASE, ADC_INT_NUMBER1);
    }
    ADC_clearInterruptStatus(ADCA_BASE, ADC_INT_NUMBER1);
    Interrupt_clearACKGroup(INTERRUPT_ACK_GROUP1);

    elapsed = cycle_start - CPUTimer_getTimerCount(CPUTIMER1_BASE);
    g_v2k_isr_cycles = elapsed;
    if (elapsed > g_v2k_isr_cycles_max)
    {
        g_v2k_isr_cycles_max = elapsed;
    }
    if (elapsed >= V2K_ISR_BUDGET_CYCLES)
    {
        g_v2k_isr_budget_violation_cnt++;
    }
    GPIO_writePin(V2K_TB_PROBE_GPIO, 0u);
}
