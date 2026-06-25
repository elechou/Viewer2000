//=============================================================================
// v2k_board_adc.h - private ADC schedule and ISR contract inside the board package
//=============================================================================
#ifndef V2K_BOARD_ADC_H
#define V2K_BOARD_ADC_H

#include <stdint.h>

#define V2K_BOARD_CURRENT_LIMIT_LOW_COUNTS  512u
#define V2K_BOARD_CURRENT_LIMIT_HIGH_COUNTS 3584u

#define V2K_BOARD_CURRENT_SOURCE_PHASE_C_HIGH 0x0010u
#define V2K_BOARD_CURRENT_SOURCE_PHASE_C_LOW  0x0020u

uint16_t v2k_board_adc_config_is_valid(void);
void v2k_board_adc_init_interrupt(void (*adc_isr)(void));
uint16_t v2k_board_adc_ack_interrupt(void);

uint16_t v2k_board_adc_current_limit_config_is_valid(void);
uint16_t v2k_board_adc_current_window_is_valid(void);
uint16_t v2k_board_adc_current_limit_status(void);
void v2k_board_adc_clear_current_limit_status(void);

#endif // V2K_BOARD_ADC_H
