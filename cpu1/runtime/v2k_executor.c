//=============================================================================
// v2k_executor.c - fixed-order control ISR
//=============================================================================

#include "v2k_executor.h"
#include "../v2k.h"
#include "../wire/wire.h"
#include "v2k_timebase.h"
#include "v2k_fault.h"
#include "v2k_profile.h"
#include "v2k_registry.h"
#include "v2k_scope_runtime.h"
#include "v2k_user_runtime.h"

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
static uint16_t s_profile_prev_valid;
static uint16_t s_profile_prev_latency;
static uint32_t s_profile_prev_control;
static uint32_t s_profile_prev_scope;
static v2k_tick_t s_profile_prev_tick;

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
        mask |= V2K_DUE_1KHZ;
    }
    if (v2k_countdown_due(&s_due_100hz_count, V2K_DUE_100HZ_DIV))
    {
        mask |= V2K_DUE_100HZ;
    }
    *param_due = v2k_countdown_due(&s_param_count, V2K_DUE_1KHZ_DIV);
    return mask;
}

__interrupt void v2k_executor_isr(void)
{
    uint16_t latency;
    uint16_t param_due;
    uint16_t control_enabled;
    uint32_t cycle_start;
    uint32_t control_start;
    uint32_t control_done;
    uint32_t control_end;
    uint32_t scope_end;
    uint32_t elapsed;

    wire_gpio_probe_set(1u);
    cycle_start = wire_cycle_count();
    latency = wire_pwm_counter();

    wire_acquire(&v2k_io.in);
    v2k_io.in.sys_state = g_v2k_sm_state;
    v2k_io.in.fault_code = g_v2k_fault_code;
    v2k_io.in.due_mask = v2k_schedule(&param_due);
    if ((param_due != 0u) && (v2k_user_reset_is_active() == 0u))
    {
        v2k_param_apply_ready();
    }
    g_v2k_due_mask = v2k_io.in.due_mask;

    control_enabled = (g_v2k_sm_state == V2K_STATE_RUNNING) ? 1u : 0u;
    if (control_enabled != 0u)
    {
        // Time only the user control() body. CPUTIMER1 counts down, so the
        // elapsed cycles are (earlier reading - later reading). wire_apply and
        // the rest of the ISR are accounted to the Runtime segment, not here.
        control_start = wire_cycle_count();
        v2k_user_control_tick();
        control_done = wire_cycle_count();
        elapsed = control_start - control_done;
    }
    else
    {
        elapsed = 0u;
    }
    wire_apply(&v2k_io.out);

    control_end = wire_cycle_count();
    g_v2k_control_cycles = elapsed;
    if (elapsed > g_v2k_control_cycles_max)
    {
        g_v2k_control_cycles_max = elapsed;
    }

    v2k_scope_sample_all(v2k_io.in.tick);
    scope_end = wire_cycle_count();
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
    if (s_profile_prev_valid != 0u)
    {
        v2k_profile_sample(g_v2k_isr_cycles,
                           s_profile_prev_control,
                           s_profile_prev_scope,
                           s_profile_prev_latency,
                           s_profile_prev_tick);
    }

    if (wire_isr_ack() != 0u)
    {
        g_v2k_isr_ovf_cnt++;
    }

    elapsed = cycle_start - wire_cycle_count();
    g_v2k_isr_cycles = elapsed;
    if (elapsed > g_v2k_isr_cycles_max)
    {
        g_v2k_isr_cycles_max = elapsed;
    }
    if (elapsed >= V2K_ISR_BUDGET_CYCLES)
    {
        g_v2k_isr_budget_violation_cnt++;
    }
    s_profile_prev_control = g_v2k_control_cycles;
    s_profile_prev_scope = g_v2k_scope_cycles;
    s_profile_prev_latency = latency;
    s_profile_prev_tick = v2k_io.in.tick;
    s_profile_prev_valid = 1u;
    wire_gpio_probe_set(0u);
}
