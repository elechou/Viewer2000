//=============================================================================
// v2k_board.h - readable L0-to-L1 seam for the CPU1 runtime
//
// L1 runtime code calls this interface for timing, ADC frame acquisition,
// explicit PWM command application, background device service, and protection.
// User code may directly read completed peripheral results through TI
// DriverLib, but it does not configure timing, output, interrupt, ownership,
// or protection resources.
//=============================================================================
#ifndef V2K_BOARD_H
#define V2K_BOARD_H

#include "../v2k.h"

typedef void (*v2k_board_isr_handler_t)(void);

#define V2K_BOARD_POWERSTAGE_MODE_POWERED 0u
#define V2K_BOARD_POWERSTAGE_MODE_DRY_RUN 1u

#define V2K_BOARD_START_PENDING 0u
#define V2K_BOARD_START_READY   1u
#define V2K_BOARD_START_FAILED  2u

#define V2K_BOARD_START_BLOCK_NONE              0x0000u
#define V2K_BOARD_START_BLOCK_CONFIG_UNAPPROVED 0x0001u
#define V2K_BOARD_START_BLOCK_GATE_FAULT        0x0002u
#define V2K_BOARD_START_BLOCK_GATE_COMM         0x0004u
#define V2K_BOARD_START_BLOCK_GATE_CONFIG       0x0008u
#define V2K_BOARD_START_BLOCK_GATE_STATUS       0x0010u
#define V2K_BOARD_START_BLOCK_CURRENT_PROTECTION 0x0020u

// Control time base and fixed ISR fast path.
void v2k_board_timebase_init(v2k_board_isr_handler_t adc_isr);
void v2k_board_timebase_start(void);
void v2k_board_acquire_adc(volatile v2k_adc_t *adc);
void v2k_board_pwm_apply_command(const volatile v2k_pwm_t *pwm);
uint32_t v2k_board_cycle_count(void);
uint16_t v2k_board_pwm_counter(void);
uint16_t v2k_board_isr_ack(void);
void v2k_board_gpio_probe_set(uint16_t value);

// Bounded background work and platform variable enumeration.
void v2k_board_background_service(void);
void v2k_board_powerstage_start_begin(void);
uint16_t v2k_board_powerstage_start_poll(void);
void v2k_board_powerstage_cancel_start(void);
void v2k_board_register_ports(uint16_t fast_prescaler);
void v2k_board_register_diagnostics(uint16_t slow_prescaler);

// Protection lifecycle. These are the only output lock/release operations.
void v2k_board_fault_pre_board_lock_outputs(void);
void v2k_board_fault_init_trip_isr(v2k_board_isr_handler_t tz_isr);
void v2k_board_fault_disable_irq(void);
void v2k_board_fault_enable_irq(void);
void v2k_board_fault_clear_interrupt_flag(void);
void v2k_board_fault_force_output_lock(void);
uint16_t v2k_board_fault_release_output_lock(void);
void v2k_board_fault_defer_driver_shutdown(void);
uint16_t v2k_board_fault_driver_shutdown_is_complete(void);
uint16_t v2k_board_fault_output_is_locked(void);
uint16_t v2k_board_fault_source_is_released(void);
uint16_t v2k_board_fault_trip_is_current(void);
void v2k_board_fault_ack_isr(void);

void v2k_board_panic_halt(void);
void v2k_board_assign_boot_resources(void);
void v2k_board_freeze_timebase_clock(void);
void v2k_board_init_generated_peripherals(void);
void v2k_board_status_led_toggle(void);

#endif // V2K_BOARD_H
