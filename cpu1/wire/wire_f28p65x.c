//=============================================================================
// wire_f28p65x.c - board composition for the CPU1 runtime seam
//
// Read this file with wire.h and v2k.h to see what L1 consumes and publishes.
// Peripheral transactions and register checks live in the private wire_* drivers.
//=============================================================================

#include "driverlib.h"
#include "board.h"
#include "wire.h"
#include "wire_adc.h"
#include "wire_as5600_internal.h"
#include "wire_drv8323rs.h"
#include "wire_pwm.h"
#include "../runtime/v2k_registry.h"

#ifndef WIRE_POWERSTAGE_MODE
#define WIRE_POWERSTAGE_MODE WIRE_POWERSTAGE_MODE_DRY_RUN
#endif

#ifndef WIRE_POWERSTAGE_POWERED_CONFIG_APPROVED
#define WIRE_POWERSTAGE_POWERED_CONFIG_APPROVED 0u
#endif

#if ((WIRE_POWERSTAGE_MODE != WIRE_POWERSTAGE_MODE_POWERED) && \
     (WIRE_POWERSTAGE_MODE != WIRE_POWERSTAGE_MODE_DRY_RUN))
#error "WIRE_POWERSTAGE_MODE must be POWERED or DRY_RUN"
#endif

#if ((WIRE_POWERSTAGE_POWERED_CONFIG_APPROVED != 0u) && \
     (WIRE_POWERSTAGE_POWERED_CONFIG_APPROVED != 1u))
#error "WIRE_POWERSTAGE_POWERED_CONFIG_APPROVED must be 0 or 1"
#endif

#define WIRE_START_PHASE_IDLE       0u
#define WIRE_START_PHASE_SLEEP_WAIT 1u
#define WIRE_START_PHASE_WAKE_WAIT  2u
#define WIRE_START_PHASE_READY      3u
#define WIRE_START_PHASE_FAILED     4u

static volatile v2k_io_out_t s_applied = {
    V2K_DUTY_NEUTRAL,
    V2K_DUTY_NEUTRAL,
    V2K_DUTY_NEUTRAL
};
static volatile uint16_t s_powerstage_mode = WIRE_POWERSTAGE_MODE;
static volatile uint16_t s_powered_config_approved =
    WIRE_POWERSTAGE_POWERED_CONFIG_APPROVED;
static volatile uint16_t s_start_phase = WIRE_START_PHASE_IDLE;
static volatile uint16_t s_start_block_reason;
static volatile uint16_t s_fault_shutdown_pending;
static volatile uint16_t s_fault_shutdown_complete = 1u;

void wire_panic_halt(void)
{
    for (;;) { ESTOP0; }
}

static uint16_t wire_cycle_timer_is_valid(void)
{
    uint16_t tcr = HWREGH(CPUTIMER1_BASE + CPUTIMER_O_TCR);

    return
        (HWREG(CPUTIMER1_BASE + CPUTIMER_O_PRD) == 0xFFFFFFFFuL) &&
        ((HWREGH(CPUTIMER1_BASE + CPUTIMER_O_TPR) &
          CPUTIMER_TPR_TDDR_M) == 0u) &&
        ((HWREGH(CPUTIMER1_BASE + CPUTIMER_O_TPRH) &
          CPUTIMER_TPRH_TDDRH_M) == 0u) &&
        ((tcr & CPUTIMER_TCR_TSS) == 0u) &&
        ((tcr & CPUTIMER_TCR_TIE) == 0u) &&
        ((tcr & CPUTIMER_TCR_FREE) != 0u);
}

static float wire_clamp_duty(float duty)
{
    if (!(duty >= V2K_DUTY_SAFE_MIN))
    {
        return V2K_DUTY_SAFE_MIN;
    }
    if (duty > V2K_DUTY_SAFE_MAX)
    {
        return V2K_DUTY_SAFE_MAX;
    }
    return duty;
}

static void wire_set_applied_neutral(void)
{
    s_applied.duty_a = V2K_DUTY_NEUTRAL;
    s_applied.duty_b = V2K_DUTY_NEUTRAL;
    s_applied.duty_c = V2K_DUTY_NEUTRAL;
    wire_pwm_apply_neutral();
}

void wire_timebase_init(wire_isr_handler_t adc_isr)
{
    if ((wire_pwm_prepare_timebase() == 0u) ||
        (wire_adc_config_is_valid() == 0u) ||
        (wire_cycle_timer_is_valid() == 0u))
    {
        wire_panic_halt();
    }
    wire_set_applied_neutral();
    wire_adc_init_interrupt(adc_isr);
}

void wire_timebase_start(void)
{
    wire_pwm_start_timebase();
}

void wire_apply(const volatile v2k_io_out_t *out)
{
    s_applied.duty_a = wire_clamp_duty(out->duty_a);
    s_applied.duty_b = wire_clamp_duty(out->duty_b);
    s_applied.duty_c = wire_clamp_duty(out->duty_c);
    wire_pwm_apply(s_applied.duty_a,
                   s_applied.duty_b,
                   s_applied.duty_c);
}

uint32_t wire_cycle_count(void)
{
    return CPUTimer_getTimerCount(CPUTIMER1_BASE);
}

uint16_t wire_pwm_counter(void)
{
    return wire_pwm_counter_value();
}

uint16_t wire_isr_ack(void)
{
    return wire_adc_ack_interrupt();
}

void wire_gpio_probe_set(uint16_t value)
{
    GPIO_writePin(ISR_PROBE, value);
}

void wire_background_service(void)
{
    wire_as5600_service();

    if (s_fault_shutdown_pending != 0u)
    {
        wire_pwm_disarm_current_trip();
        if ((s_powerstage_mode == WIRE_POWERSTAGE_MODE_POWERED) &&
            (wire_drv8323rs_wake_elapsed() != 0u))
        {
            // Preserve the complete DRV status before ENABLE enters sleep and
            // the device resets its SPI registers. The hardware TZ path has
            // already forced all PWM outputs low.
            (void)wire_drv8323rs_poll();
        }
        wire_drv8323rs_disable();
        s_fault_shutdown_pending = 0u;
        s_fault_shutdown_complete = 1u;
    }
    else if ((s_powerstage_mode == WIRE_POWERSTAGE_MODE_POWERED) &&
             (s_start_phase == WIRE_START_PHASE_READY) &&
             (wire_drv8323rs_wake_elapsed() != 0u))
    {
        (void)wire_drv8323rs_poll();
    }
}

static uint16_t wire_powerstage_fail_start(uint16_t reason)
{
    s_start_block_reason = reason;
    s_start_phase = WIRE_START_PHASE_FAILED;
    wire_pwm_disarm_current_trip();
    wire_drv8323rs_disable();
    s_fault_shutdown_complete = 1u;
    return WIRE_START_FAILED;
}

void wire_powerstage_start_begin(void)
{
    wire_pwm_disarm_current_trip();
    wire_drv8323rs_disable();
    wire_pwm_force_output_lock();
    wire_set_applied_neutral();
    s_start_block_reason = WIRE_START_BLOCK_NONE;
    s_fault_shutdown_pending = 0u;
    s_fault_shutdown_complete = 1u;

    if (s_powerstage_mode == WIRE_POWERSTAGE_MODE_DRY_RUN)
    {
        // Commissioning mode releases only the MCU PWM pins. The gate driver
        // remains asleep, so applying VM cannot energize the power stage.
        s_start_phase = WIRE_START_PHASE_READY;
    }
    else
    {
        s_start_phase = WIRE_START_PHASE_SLEEP_WAIT;
    }
}

uint16_t wire_powerstage_start_poll(void)
{
    if (s_start_phase == WIRE_START_PHASE_READY)
    {
        return WIRE_START_READY;
    }
    if (s_start_phase == WIRE_START_PHASE_FAILED)
    {
        return WIRE_START_FAILED;
    }
    if (s_start_phase == WIRE_START_PHASE_SLEEP_WAIT)
    {
        if (s_powered_config_approved == 0u)
        {
            return wire_powerstage_fail_start(
                WIRE_START_BLOCK_CONFIG_UNAPPROVED);
        }
        if (wire_drv8323rs_sleep_elapsed() == 0u)
        {
            return WIRE_START_PENDING;
        }
        wire_drv8323rs_enable();
        s_fault_shutdown_complete = 0u;
        s_start_phase = WIRE_START_PHASE_WAKE_WAIT;
        return WIRE_START_PENDING;
    }
    if (s_start_phase == WIRE_START_PHASE_WAKE_WAIT)
    {
        if (wire_drv8323rs_wake_elapsed() == 0u)
        {
            return WIRE_START_PENDING;
        }
        if (wire_drv8323rs_fault_source_is_released() == 0u)
        {
            return wire_powerstage_fail_start(WIRE_START_BLOCK_NFAULT);
        }
        if (wire_drv8323rs_configure_and_verify() == 0u)
        {
            return wire_powerstage_fail_start(WIRE_START_BLOCK_DRV_CONFIG);
        }
        if (wire_drv8323rs_poll() == 0u)
        {
            return wire_powerstage_fail_start(WIRE_START_BLOCK_SPI);
        }
        if (wire_drv8323rs_has_fault() != 0u)
        {
            return wire_powerstage_fail_start(WIRE_START_BLOCK_DRV_STATUS);
        }
        if (wire_drv8323rs_fault_source_is_released() == 0u)
        {
            return wire_powerstage_fail_start(WIRE_START_BLOCK_NFAULT);
        }
        if (wire_pwm_arm_current_trip() == 0u)
        {
            return wire_powerstage_fail_start(
                WIRE_START_BLOCK_CURRENT_PROTECTION);
        }
        s_start_phase = WIRE_START_PHASE_READY;
        return WIRE_START_READY;
    }
    return WIRE_START_PENDING;
}

void wire_powerstage_cancel_start(void)
{
    wire_pwm_force_output_lock();
    wire_pwm_disarm_current_trip();
    wire_drv8323rs_disable();
    s_start_phase = WIRE_START_PHASE_IDLE;
    s_fault_shutdown_pending = 0u;
    s_fault_shutdown_complete = 1u;
}

void wire_register_ports(uint16_t fast_prescaler)
{
    v2k_registry_add("duty_a_cmd", V2K_TYPE_F32, V2K_KIND_SCOPE,
                     (volatile void *)&v2k_io.out.duty_a, fast_prescaler);
    v2k_registry_add("duty_b_cmd", V2K_TYPE_F32, V2K_KIND_SCOPE,
                     (volatile void *)&v2k_io.out.duty_b, fast_prescaler);
    v2k_registry_add("duty_c_cmd", V2K_TYPE_F32, V2K_KIND_SCOPE,
                     (volatile void *)&v2k_io.out.duty_c, fast_prescaler);
    v2k_registry_add("duty_a", V2K_TYPE_F32, V2K_KIND_SCOPE,
                     &s_applied.duty_a, fast_prescaler);
    v2k_registry_add("duty_b", V2K_TYPE_F32, V2K_KIND_SCOPE,
                     &s_applied.duty_b, fast_prescaler);
    v2k_registry_add("duty_c", V2K_TYPE_F32, V2K_KIND_SCOPE,
                     &s_applied.duty_c, fast_prescaler);
    v2k_registry_add("pwr_mode", V2K_TYPE_U16, V2K_KIND_SCOPE,
                     &s_powerstage_mode, fast_prescaler);
    v2k_registry_add("pwr_cfg_ok", V2K_TYPE_U16, V2K_KIND_SCOPE,
                     &s_powered_config_approved, fast_prescaler);
    v2k_registry_add("start_state", V2K_TYPE_U16, V2K_KIND_SCOPE,
                     &s_start_phase, fast_prescaler);
    v2k_registry_add("start_block", V2K_TYPE_U16, V2K_KIND_SCOPE,
                     &s_start_block_reason, fast_prescaler);
    v2k_registry_add("curr_trip_arm", V2K_TYPE_U16, V2K_KIND_SCOPE,
                     wire_pwm_current_trip_armed_address(), fast_prescaler);
    v2k_registry_add("curr_trip_last", V2K_TYPE_U16, V2K_KIND_SCOPE,
                     wire_pwm_current_trip_last_address(), fast_prescaler);
    v2k_registry_add("curr_trip_cfg", V2K_TYPE_U16, V2K_KIND_SCOPE,
                     wire_pwm_current_trip_config_error_address(),
                     fast_prescaler);
    v2k_registry_add("curr_limit_lo", V2K_TYPE_U16, V2K_KIND_SCOPE,
                     wire_pwm_current_limit_low_address(), fast_prescaler);
    v2k_registry_add("curr_limit_hi", V2K_TYPE_U16, V2K_KIND_SCOPE,
                     wire_pwm_current_limit_high_address(), fast_prescaler);
    v2k_registry_add("drv_status1", V2K_TYPE_U16, V2K_KIND_SCOPE,
                     wire_drv8323rs_status1_address(), fast_prescaler);
    v2k_registry_add("drv_status2", V2K_TYPE_U16, V2K_KIND_SCOPE,
                     wire_drv8323rs_status2_address(), fast_prescaler);
    v2k_registry_add("drv_spi_errors", V2K_TYPE_U32, V2K_KIND_SCOPE,
                     wire_drv8323rs_spi_error_count_address(),
                     fast_prescaler);
    v2k_registry_add("drv_cfg_valid", V2K_TYPE_U16, V2K_KIND_SCOPE,
                     wire_drv8323rs_config_valid_address(),
                     fast_prescaler);
    v2k_registry_add("drv_ctrl_rd", V2K_TYPE_U16, V2K_KIND_SCOPE,
                     wire_drv8323rs_control_readback_address(),
                     fast_prescaler);
    v2k_registry_add("drv_ghs_rd", V2K_TYPE_U16, V2K_KIND_SCOPE,
                     wire_drv8323rs_gate_hs_readback_address(),
                     fast_prescaler);
    v2k_registry_add("drv_gls_rd", V2K_TYPE_U16, V2K_KIND_SCOPE,
                     wire_drv8323rs_gate_ls_readback_address(),
                     fast_prescaler);
    v2k_registry_add("drv_ocp_rd", V2K_TYPE_U16, V2K_KIND_SCOPE,
                     wire_drv8323rs_ocp_readback_address(),
                     fast_prescaler);
    v2k_registry_add("drv_csa_rd", V2K_TYPE_U16, V2K_KIND_SCOPE,
                     wire_drv8323rs_csa_readback_address(),
                     fast_prescaler);
}

void wire_fault_pre_board_lock_outputs(void)
{
    wire_pwm_pre_board_lock();
}

void wire_fault_init_trip_isr(wire_isr_handler_t tz_isr)
{
    wire_drv8323rs_disable();
    s_fault_shutdown_pending = 0u;
    s_fault_shutdown_complete = 1u;
    wire_pwm_init_trip_isr(tz_isr);
}

void wire_fault_disable_irq(void)
{
    wire_pwm_disable_trip_irq();
}

void wire_fault_enable_irq(void)
{
    wire_pwm_enable_trip_irq();
}

void wire_fault_clear_interrupt_flag(void)
{
    wire_pwm_clear_trip_interrupt();
}

void wire_fault_force_output_lock(void)
{
    wire_pwm_force_output_lock();
    wire_pwm_disarm_current_trip();
    wire_drv8323rs_disable();
    s_start_phase = WIRE_START_PHASE_IDLE;
    s_fault_shutdown_pending = 0u;
    s_fault_shutdown_complete = 1u;
}

uint16_t wire_fault_release_output_lock(void)
{
    return wire_pwm_release_output_lock();
}

void wire_fault_defer_driver_shutdown(void)
{
    s_fault_shutdown_pending = 1u;
    s_fault_shutdown_complete = 0u;
}

uint16_t wire_fault_driver_shutdown_is_complete(void)
{
    return s_fault_shutdown_complete;
}

uint16_t wire_fault_output_is_locked(void)
{
    return wire_pwm_output_is_locked();
}

uint16_t wire_fault_source_is_released(void)
{
    return (s_fault_shutdown_complete != 0u) &&
           (wire_drv8323rs_sleep_elapsed() != 0u) &&
           wire_drv8323rs_fault_source_is_released();
}

uint16_t wire_fault_trip_is_current(void)
{
    return wire_pwm_current_trip_was_active();
}

void wire_fault_ack_isr(void)
{
    wire_pwm_ack_trip_isr();
}
