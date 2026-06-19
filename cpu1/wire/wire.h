//=============================================================================
// wire.h - board/chip wiring boundary for the CPU1 control runtime
//=============================================================================
#ifndef WIRE_H
#define WIRE_H

#include "../v2k.h"

typedef void (*wire_isr_handler_t)(void);

extern volatile uint16_t g_wire_adc_a0_raw;
extern volatile float g_wire_vsense;
extern volatile float g_wire_duty_a_applied;

void wire_panic_halt(void);
void wire_timebase_init(wire_isr_handler_t adc_isr);
void wire_timebase_start(void);
void wire_acquire(volatile v2k_io_in_t *in);
void wire_apply(const volatile v2k_io_out_t *out);
uint32_t wire_cycle_count(void);
uint16_t wire_pwm_counter(void);
uint16_t wire_isr_ack(void);
void wire_gpio_probe_set(uint16_t value);
void wire_register_ports(uint16_t fast_prescaler);
void wire_fault_pre_board_lock_outputs(void);
void wire_fault_init_trip_isr(wire_isr_handler_t tz_isr);
void wire_fault_disable_irq(void);
void wire_fault_enable_irq(void);
void wire_fault_clear_interrupt_flag(void);
void wire_fault_force_output_lock(void);
void wire_fault_release_output_lock(void);
uint16_t wire_fault_output_is_locked(void);
uint16_t wire_fault_source_is_released(void);
void wire_fault_ack_isr(void);

#endif // WIRE_H
