//=============================================================================
// wire_pwm.h - private three-phase PWM/TZ driver inside the wire package
//=============================================================================
#ifndef WIRE_PWM_H
#define WIRE_PWM_H

#include <stdint.h>

uint16_t wire_pwm_prepare_timebase(void);
void wire_pwm_start_timebase(void);
void wire_pwm_apply(float duty_a, float duty_b, float duty_c);
void wire_pwm_apply_neutral(void);
uint16_t wire_pwm_counter_value(void);

void wire_pwm_pre_board_lock(void);
void wire_pwm_init_trip_isr(void (*tz_isr)(void));
void wire_pwm_disable_trip_irq(void);
void wire_pwm_enable_trip_irq(void);
void wire_pwm_clear_trip_interrupt(void);
void wire_pwm_force_output_lock(void);
uint16_t wire_pwm_release_output_lock(void);
uint16_t wire_pwm_output_is_locked(void);
void wire_pwm_ack_trip_isr(void);

uint16_t wire_pwm_arm_current_trip(void);
void wire_pwm_disarm_current_trip(void);
uint16_t wire_pwm_current_trip_was_active(void);
volatile uint16_t *wire_pwm_current_trip_armed_address(void);
volatile uint16_t *wire_pwm_current_trip_last_address(void);
volatile uint16_t *wire_pwm_current_trip_config_error_address(void);
volatile uint16_t *wire_pwm_current_limit_low_address(void);
volatile uint16_t *wire_pwm_current_limit_high_address(void);

#endif // WIRE_PWM_H
