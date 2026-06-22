//=============================================================================
// wire_adc.h - private ADC schedule and ISR contract inside wire
//=============================================================================
#ifndef WIRE_ADC_H
#define WIRE_ADC_H

#include <stdint.h>

#define WIRE_CURRENT_LIMIT_LOW_COUNTS  512u
#define WIRE_CURRENT_LIMIT_HIGH_COUNTS 3584u

#define WIRE_CURRENT_SOURCE_PHASE_C_HIGH 0x0010u
#define WIRE_CURRENT_SOURCE_PHASE_C_LOW  0x0020u

uint16_t wire_adc_config_is_valid(void);
void wire_adc_init_interrupt(void (*adc_isr)(void));
uint16_t wire_adc_ack_interrupt(void);

uint16_t wire_adc_current_limit_config_is_valid(void);
uint16_t wire_adc_current_window_is_valid(void);
uint16_t wire_adc_current_limit_status(void);
void wire_adc_clear_current_limit_status(void);

#endif // WIRE_ADC_H
