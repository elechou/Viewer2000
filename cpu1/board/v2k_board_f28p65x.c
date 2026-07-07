//=============================================================================
// v2k_board_f28p65x.c - board composition for the CPU1 runtime seam
//
// Read this file with v2k_board.h and v2k.h to see what L1 consumes and publishes.
// Peripheral transactions and register checks live in the private v2k_board_* drivers.
//=============================================================================

#include "driverlib.h"
#include "board.h"
#include "v2k_board.h"
#include "v2k_board_adc.h"
#include "v2k_board_as5600_internal.h"
#include "v2k_board_drv8323rs.h"
#include "v2k_board_pwm.h"
#include "../../common/v2k_planes.h"
#include "../runtime/v2k_registry.h"

extern void SetDBGIER(uint16_t dbgier);

#ifndef V2K_BOARD_POWERSTAGE_MODE
#define V2K_BOARD_POWERSTAGE_MODE V2K_BOARD_POWERSTAGE_MODE_DRY_RUN
#endif

#ifndef V2K_BOARD_POWERSTAGE_POWERED_CONFIG_APPROVED
#define V2K_BOARD_POWERSTAGE_POWERED_CONFIG_APPROVED 0u
#endif

#if ((V2K_BOARD_POWERSTAGE_MODE != V2K_BOARD_POWERSTAGE_MODE_POWERED) && \
     (V2K_BOARD_POWERSTAGE_MODE != V2K_BOARD_POWERSTAGE_MODE_DRY_RUN))
#error "V2K_BOARD_POWERSTAGE_MODE must be POWERED or DRY_RUN"
#endif

#if ((V2K_BOARD_POWERSTAGE_POWERED_CONFIG_APPROVED != 0u) && \
     (V2K_BOARD_POWERSTAGE_POWERED_CONFIG_APPROVED != 1u))
#error "V2K_BOARD_POWERSTAGE_POWERED_CONFIG_APPROVED must be 0 or 1"
#endif

#define V2K_BOARD_START_PHASE_IDLE       0u
#define V2K_BOARD_START_PHASE_SLEEP_WAIT 1u
#define V2K_BOARD_START_PHASE_WAKE_WAIT  2u
#define V2K_BOARD_START_PHASE_READY      3u
#define V2K_BOARD_START_PHASE_FAILED     4u

static volatile v2k_pwm_t s_applied = {
    V2K_DUTY_NEUTRAL,
    V2K_DUTY_NEUTRAL,
    V2K_DUTY_NEUTRAL
};
static volatile uint16_t s_powerstage_mode = V2K_BOARD_POWERSTAGE_MODE;
static volatile uint16_t s_powered_config_approved =
    V2K_BOARD_POWERSTAGE_POWERED_CONFIG_APPROVED;
static volatile uint16_t s_start_phase = V2K_BOARD_START_PHASE_IDLE;
static volatile uint16_t s_start_block_reason;
static volatile uint16_t s_fault_shutdown_pending;
static volatile uint16_t s_fault_shutdown_complete = 1u;

// NMI diagnostics stay in board support because NMI flag registers and the
// interrupt vector are target-owned boot resources.
volatile uint32_t g_nmi_cnt;
volatile uint32_t g_nmi_flags_last;
volatile uint32_t g_nmi_shadow_last;

static __interrupt void v2k_board_nmi_isr(void)
{
    g_nmi_flags_last  = SysCtl_getNMIFlagStatus();
    g_nmi_shadow_last = SysCtl_getNMIShadowFlagStatus();
    g_nmi_cnt++;
    SysCtl_clearAllNMIFlags();
}

void v2k_board_panic_halt(void)
{
    for (;;) { ESTOP0; }
}

void v2k_board_boot_init_device(void)
{
    Device_init();
}

// The NMI backstop must be installed here, before v2k_board_boot_cpu2_and_sync()
// releases CPU2. An unhandled NMI is escalated by the NMI watchdog into a
// whole-chip reset, and there is a window — from CPU2's release from reset until
// its .out finishes loading — where CPU2 runs garbage in M0, faults, and raises a
// CPU1 NMI (Phase 1, BRINGUP.md 2026-06-12: the "runaway" that NMIWD-reset the
// chip back into old flash firmware). Flow follows TI nmi_ex1_cpu1handling.
void v2k_board_boot_init_interrupts(void)
{
    Interrupt_initModule();
    Interrupt_initVectorTable();
    SysCtl_clearAllNMIFlags();
    Interrupt_register(INT_NMI, &v2k_board_nmi_isr);
    SysCtl_enableNMIGlobalInterrupt();
    Interrupt_enable(INT_NMI);
    SetDBGIER(INTERRUPT_CPU_INT1); // ADCA1 is in PIE Group 1 = time-critical.
    EINT;
    ERTM;
}

void v2k_board_boot_cpu2_and_sync(void)
{
#ifdef _FLASH
    Device_bootCPU2(BOOTMODE_BOOT_TO_FLASH_BANK3_SECTOR0);
#else
    Device_bootCPU2(BOOTMODE_BOOT_TO_M0RAM);
#endif
    IPC_clearFlagLtoR(IPC_CPU1_L_CPU2_R, IPC_FLAG_ALL);
    IPC_sync(IPC_CPU1_L_CPU2_R, IPC_FLAG31);
}

uint16_t v2k_board_ipc_ping_try_send(void)
{
    if (IPC_isFlagBusyLtoR(IPC_CPU1_L_CPU2_R, IPC_FLAG0))
    {
        return 0u;
    }
    IPC_setFlagLtoR(IPC_CPU1_L_CPU2_R, IPC_FLAG0);
    return 1u;
}

uint32_t v2k_board_cpu2_heartbeat_read(void)
{
    return V2K_MSG_2TO1_RO->cpu2_status.heartbeat;
}

void v2k_board_assign_boot_resources(void)
{
    SysCtl_allocateFlashBank(SYSCTL_FLASH_BANK0, SYSCTL_CPUSEL_CPU1);
    SysCtl_allocateFlashBank(SYSCTL_FLASH_BANK1, SYSCTL_CPUSEL_CPU1);
    SysCtl_allocateFlashBank(SYSCTL_FLASH_BANK2, SYSCTL_CPUSEL_CPU1);
    SysCtl_allocateFlashBank(SYSCTL_FLASH_BANK3, SYSCTL_CPUSEL_CPU2);
    SysCtl_allocateFlashBank(SYSCTL_FLASH_BANK4, SYSCTL_CPUSEL_CPU2);
    MemCfg_setGSRAMControllerSel(MEMCFG_SECT_GS4, MEMCFG_GSRAMCONTROLLER_CPU2);
}

void v2k_board_freeze_timebase_clock(void)
{
    SysCtl_disablePeripheral(SYSCTL_PERIPH_CLK_TBCLKSYNC);
}

void v2k_board_init_generated_peripherals(void)
{
    Device_initGPIO();
    Board_init();
    GPIO_setControllerCore(LED_CPU2_GPIO, GPIO_CORE_CPU2);
}

void v2k_board_status_led_toggle(void)
{
    GPIO_togglePin(LED_CPU1_GPIO);
}

static uint16_t v2k_board_cycle_timer_is_valid(void)
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

static float v2k_board_clamp_duty(float duty)
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

static void v2k_board_set_applied_neutral(void)
{
    s_applied.duty_a = V2K_DUTY_NEUTRAL;
    s_applied.duty_b = V2K_DUTY_NEUTRAL;
    s_applied.duty_c = V2K_DUTY_NEUTRAL;
    v2k_board_pwm_apply_neutral();
}

void v2k_board_timebase_init(v2k_board_isr_handler_t adc_isr)
{
    if ((v2k_board_pwm_prepare_timebase() == 0u) ||
        (v2k_board_adc_config_is_valid() == 0u) ||
        (v2k_board_cycle_timer_is_valid() == 0u))
    {
        v2k_board_panic_halt();
    }
    v2k_board_set_applied_neutral();
    v2k_board_adc_init_interrupt(adc_isr);
}

void v2k_board_timebase_start(void)
{
    v2k_board_pwm_start_timebase();
}

void v2k_board_acquire_inputs(volatile v2k_io_t *io)
{
    volatile v2k_adc_t *adc = &io->adc;

    // EPWM1 SOCA has completed this seven-channel frame before ISR entry.
    adc->va_raw = ADC_readResult(myADC0_RESULT_BASE, myADC0_SOC0);
    adc->vb_raw = ADC_readResult(myADC0_RESULT_BASE, myADC0_SOC1);
    adc->vc_raw = ADC_readResult(myADC1_RESULT_BASE, myADC1_SOC0);
    adc->vbus_raw = ADC_readResult(myADC0_RESULT_BASE, myADC0_SOC3);
    adc->ia_raw = ADC_readResult(myADC1_RESULT_BASE, myADC1_SOC1);
    adc->ib_raw = ADC_readResult(myADC0_RESULT_BASE, myADC0_SOC2);
    adc->ic_raw = ADC_readResult(myADC2_RESULT_BASE, myADC2_SOC0);
}

void v2k_board_user_io_neutralize(volatile v2k_io_t *io)
{
    io->pwm.duty_a = V2K_DUTY_NEUTRAL;
    io->pwm.duty_b = V2K_DUTY_NEUTRAL;
    io->pwm.duty_c = V2K_DUTY_NEUTRAL;
}

void v2k_board_output_apply(const volatile v2k_io_t *io)
{
    const volatile v2k_pwm_t *pwm = &io->pwm;

    s_applied.duty_a = v2k_board_clamp_duty(pwm->duty_a);
    s_applied.duty_b = v2k_board_clamp_duty(pwm->duty_b);
    s_applied.duty_c = v2k_board_clamp_duty(pwm->duty_c);
    v2k_board_pwm_apply(s_applied.duty_a,
                   s_applied.duty_b,
                   s_applied.duty_c);
}

void v2k_pwm_apply(float duty_a, float duty_b, float duty_c)
{
    v2k_io.pwm.duty_a = duty_a;
    v2k_io.pwm.duty_b = duty_b;
    v2k_io.pwm.duty_c = duty_c;
    v2k_board_output_apply(&v2k_io);
}

uint32_t v2k_board_cycle_count(void)
{
    return CPUTimer_getTimerCount(CPUTIMER1_BASE);
}

uint16_t v2k_board_pwm_counter(void)
{
    return v2k_board_pwm_counter_value();
}

uint16_t v2k_board_isr_ack(void)
{
    return v2k_board_adc_ack_interrupt();
}

void v2k_board_gpio_probe_set(uint16_t value)
{
    GPIO_writePin(ISR_PROBE, value);
}

void v2k_board_background_service(void)
{
    v2k_board_as5600_service();

    if (s_fault_shutdown_pending != 0u)
    {
        v2k_board_pwm_disarm_current_trip();
        if ((s_powerstage_mode == V2K_BOARD_POWERSTAGE_MODE_POWERED) &&
            (v2k_board_drv8323rs_wake_elapsed() != 0u))
        {
            // Preserve the complete DRV status before ENABLE enters sleep and
            // the device resets its SPI registers. The hardware TZ path has
            // already forced all PWM outputs low.
            (void)v2k_board_drv8323rs_poll();
        }
        v2k_board_drv8323rs_disable();
        s_fault_shutdown_pending = 0u;
        s_fault_shutdown_complete = 1u;
    }
    else if ((s_powerstage_mode == V2K_BOARD_POWERSTAGE_MODE_POWERED) &&
             (s_start_phase == V2K_BOARD_START_PHASE_READY) &&
             (v2k_board_drv8323rs_wake_elapsed() != 0u))
    {
        (void)v2k_board_drv8323rs_poll();
    }
}

static uint16_t v2k_board_powerstage_fail_start(uint16_t reason)
{
    s_start_block_reason = reason;
    s_start_phase = V2K_BOARD_START_PHASE_FAILED;
    v2k_board_pwm_disarm_current_trip();
    v2k_board_drv8323rs_disable();
    s_fault_shutdown_complete = 1u;
    return V2K_BOARD_START_FAILED;
}

void v2k_board_powerstage_start_begin(void)
{
    v2k_board_pwm_disarm_current_trip();
    v2k_board_drv8323rs_disable();
    v2k_board_pwm_force_output_lock();
    v2k_board_set_applied_neutral();
    s_start_block_reason = V2K_BOARD_START_BLOCK_NONE;
    s_fault_shutdown_pending = 0u;
    s_fault_shutdown_complete = 1u;

    if (s_powerstage_mode == V2K_BOARD_POWERSTAGE_MODE_DRY_RUN)
    {
        // Commissioning mode releases only the MCU PWM pins. The gate driver
        // remains asleep, so applying VM cannot energize the power stage.
        s_start_phase = V2K_BOARD_START_PHASE_READY;
    }
    else
    {
        s_start_phase = V2K_BOARD_START_PHASE_SLEEP_WAIT;
    }
}

uint16_t v2k_board_powerstage_start_poll(void)
{
    if (s_start_phase == V2K_BOARD_START_PHASE_READY)
    {
        return V2K_BOARD_START_READY;
    }
    if (s_start_phase == V2K_BOARD_START_PHASE_FAILED)
    {
        return V2K_BOARD_START_FAILED;
    }
    if (s_start_phase == V2K_BOARD_START_PHASE_SLEEP_WAIT)
    {
        if (s_powered_config_approved == 0u)
        {
            return v2k_board_powerstage_fail_start(
                V2K_BOARD_START_BLOCK_CONFIG_UNAPPROVED);
        }
        if (v2k_board_drv8323rs_sleep_elapsed() == 0u)
        {
            return V2K_BOARD_START_PENDING;
        }
        v2k_board_drv8323rs_enable();
        s_fault_shutdown_complete = 0u;
        s_start_phase = V2K_BOARD_START_PHASE_WAKE_WAIT;
        return V2K_BOARD_START_PENDING;
    }
    if (s_start_phase == V2K_BOARD_START_PHASE_WAKE_WAIT)
    {
        if (v2k_board_drv8323rs_wake_elapsed() == 0u)
        {
            return V2K_BOARD_START_PENDING;
        }
        if (v2k_board_drv8323rs_fault_source_is_released() == 0u)
        {
            return v2k_board_powerstage_fail_start(V2K_BOARD_START_BLOCK_GATE_FAULT);
        }
        if (v2k_board_drv8323rs_configure_and_verify() == 0u)
        {
            return v2k_board_powerstage_fail_start(V2K_BOARD_START_BLOCK_GATE_CONFIG);
        }
        if (v2k_board_drv8323rs_poll() == 0u)
        {
            return v2k_board_powerstage_fail_start(V2K_BOARD_START_BLOCK_GATE_COMM);
        }
        if (v2k_board_drv8323rs_has_fault() != 0u)
        {
            return v2k_board_powerstage_fail_start(V2K_BOARD_START_BLOCK_GATE_STATUS);
        }
        if (v2k_board_drv8323rs_fault_source_is_released() == 0u)
        {
            return v2k_board_powerstage_fail_start(V2K_BOARD_START_BLOCK_GATE_FAULT);
        }
        if (v2k_board_pwm_arm_current_trip() == 0u)
        {
            return v2k_board_powerstage_fail_start(
                V2K_BOARD_START_BLOCK_CURRENT_PROTECTION);
        }
        s_start_phase = V2K_BOARD_START_PHASE_READY;
        return V2K_BOARD_START_READY;
    }
    return V2K_BOARD_START_PENDING;
}

void v2k_board_powerstage_cancel_start(void)
{
    v2k_board_pwm_force_output_lock();
    v2k_board_pwm_disarm_current_trip();
    v2k_board_drv8323rs_disable();
    s_start_phase = V2K_BOARD_START_PHASE_IDLE;
    s_fault_shutdown_pending = 0u;
    s_fault_shutdown_complete = 1u;
}

void v2k_board_register_ports(uint16_t fast_prescaler)
{
    v2k_registry_add("duty_a_cmd", V2K_TYPE_F32, V2K_KIND_SCOPE,
                     (volatile void *)&v2k_io.pwm.duty_a, fast_prescaler);
    v2k_registry_add("duty_b_cmd", V2K_TYPE_F32, V2K_KIND_SCOPE,
                     (volatile void *)&v2k_io.pwm.duty_b, fast_prescaler);
    v2k_registry_add("duty_c_cmd", V2K_TYPE_F32, V2K_KIND_SCOPE,
                     (volatile void *)&v2k_io.pwm.duty_c, fast_prescaler);
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
                     v2k_board_pwm_current_trip_armed_address(), fast_prescaler);
    v2k_registry_add("curr_trip_last", V2K_TYPE_U16, V2K_KIND_SCOPE,
                     v2k_board_pwm_current_trip_last_address(), fast_prescaler);
    v2k_registry_add("curr_trip_cfg", V2K_TYPE_U16, V2K_KIND_SCOPE,
                     v2k_board_pwm_current_trip_config_error_address(),
                     fast_prescaler);
    v2k_registry_add("curr_limit_lo", V2K_TYPE_U16, V2K_KIND_SCOPE,
                     v2k_board_pwm_current_limit_low_address(), fast_prescaler);
    v2k_registry_add("curr_limit_hi", V2K_TYPE_U16, V2K_KIND_SCOPE,
                     v2k_board_pwm_current_limit_high_address(), fast_prescaler);
    v2k_registry_add("drv_status1", V2K_TYPE_U16, V2K_KIND_SCOPE,
                     v2k_board_drv8323rs_status1_address(), fast_prescaler);
    v2k_registry_add("drv_status2", V2K_TYPE_U16, V2K_KIND_SCOPE,
                     v2k_board_drv8323rs_status2_address(), fast_prescaler);
    v2k_registry_add("drv_spi_errors", V2K_TYPE_U32, V2K_KIND_SCOPE,
                     v2k_board_drv8323rs_spi_error_count_address(),
                     fast_prescaler);
    v2k_registry_add("drv_cfg_valid", V2K_TYPE_U16, V2K_KIND_SCOPE,
                     v2k_board_drv8323rs_config_valid_address(),
                     fast_prescaler);
    v2k_registry_add("drv_ctrl_rd", V2K_TYPE_U16, V2K_KIND_SCOPE,
                     v2k_board_drv8323rs_control_readback_address(),
                     fast_prescaler);
    v2k_registry_add("drv_ghs_rd", V2K_TYPE_U16, V2K_KIND_SCOPE,
                     v2k_board_drv8323rs_gate_hs_readback_address(),
                     fast_prescaler);
    v2k_registry_add("drv_gls_rd", V2K_TYPE_U16, V2K_KIND_SCOPE,
                     v2k_board_drv8323rs_gate_ls_readback_address(),
                     fast_prescaler);
    v2k_registry_add("drv_ocp_rd", V2K_TYPE_U16, V2K_KIND_SCOPE,
                     v2k_board_drv8323rs_ocp_readback_address(),
                     fast_prescaler);
    v2k_registry_add("drv_csa_rd", V2K_TYPE_U16, V2K_KIND_SCOPE,
                     v2k_board_drv8323rs_csa_readback_address(),
                     fast_prescaler);
}

void v2k_board_register_diagnostics(uint16_t slow_prescaler)
{
    v2k_registry_add("as5600_errors", V2K_TYPE_U32, V2K_KIND_SCOPE,
                     v2k_board_as5600_error_count_address(), slow_prescaler);
    v2k_registry_add("as5600_seq", V2K_TYPE_U32, V2K_KIND_SCOPE,
                     v2k_board_as5600_sequence_address(), slow_prescaler);
    v2k_registry_add("as5600_status", V2K_TYPE_U16, V2K_KIND_SCOPE,
                     v2k_board_as5600_status_address(), slow_prescaler);
}

void v2k_board_fault_pre_board_lock_outputs(void)
{
    v2k_board_pwm_pre_board_lock();
}

void v2k_board_fault_init_trip_isr(v2k_board_isr_handler_t tz_isr)
{
    v2k_board_drv8323rs_disable();
    s_fault_shutdown_pending = 0u;
    s_fault_shutdown_complete = 1u;
    v2k_board_pwm_init_trip_isr(tz_isr);
}

void v2k_board_fault_disable_irq(void)
{
    v2k_board_pwm_disable_trip_irq();
}

void v2k_board_fault_enable_irq(void)
{
    v2k_board_pwm_enable_trip_irq();
}

void v2k_board_fault_clear_interrupt_flag(void)
{
    v2k_board_pwm_clear_trip_interrupt();
}

void v2k_board_fault_force_output_lock(void)
{
    v2k_board_pwm_force_output_lock();
    v2k_board_pwm_disarm_current_trip();
    v2k_board_drv8323rs_disable();
    s_start_phase = V2K_BOARD_START_PHASE_IDLE;
    s_fault_shutdown_pending = 0u;
    s_fault_shutdown_complete = 1u;
}

uint16_t v2k_board_fault_release_output_lock(void)
{
    return v2k_board_pwm_release_output_lock();
}

void v2k_board_fault_post_trip_begin(void)
{
    s_fault_shutdown_pending = 1u;
    s_fault_shutdown_complete = 0u;
}

uint16_t v2k_board_fault_output_is_locked(void)
{
    return v2k_board_pwm_output_is_locked();
}

uint16_t v2k_board_fault_source_is_released(void)
{
    return (s_fault_shutdown_complete != 0u) &&
           (v2k_board_drv8323rs_sleep_elapsed() != 0u) &&
           v2k_board_drv8323rs_fault_source_is_released();
}

uint16_t v2k_board_fault_trip_is_current(void)
{
    return v2k_board_pwm_current_trip_was_active();
}

void v2k_board_fault_ack_isr(void)
{
    v2k_board_pwm_ack_trip_isr();
}
