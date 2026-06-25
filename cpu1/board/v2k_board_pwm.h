//=============================================================================
// v2k_board_pwm.h - private three-phase PWM/TZ driver inside the board package
//=============================================================================
#ifndef V2K_BOARD_PWM_H
#define V2K_BOARD_PWM_H

#include <stdint.h>

uint16_t v2k_board_pwm_prepare_timebase(void);
void v2k_board_pwm_start_timebase(void);
void v2k_board_pwm_apply(float duty_a, float duty_b, float duty_c);
void v2k_board_pwm_apply_neutral(void);
uint16_t v2k_board_pwm_counter_value(void);

void v2k_board_pwm_pre_board_lock(void);
void v2k_board_pwm_init_trip_isr(void (*tz_isr)(void));
void v2k_board_pwm_disable_trip_irq(void);
void v2k_board_pwm_enable_trip_irq(void);
void v2k_board_pwm_clear_trip_interrupt(void);
void v2k_board_pwm_force_output_lock(void);
uint16_t v2k_board_pwm_release_output_lock(void);
uint16_t v2k_board_pwm_output_is_locked(void);
void v2k_board_pwm_ack_trip_isr(void);

uint16_t v2k_board_pwm_arm_current_trip(void);
void v2k_board_pwm_disarm_current_trip(void);
uint16_t v2k_board_pwm_current_trip_was_active(void);
volatile uint16_t *v2k_board_pwm_current_trip_armed_address(void);
volatile uint16_t *v2k_board_pwm_current_trip_last_address(void);
volatile uint16_t *v2k_board_pwm_current_trip_config_error_address(void);
volatile uint16_t *v2k_board_pwm_current_limit_low_address(void);
volatile uint16_t *v2k_board_pwm_current_limit_high_address(void);

#endif // V2K_BOARD_PWM_H
