//=============================================================================
// v2k_board_as5600_internal.h - private AS5600 service hooks
//=============================================================================
#ifndef V2K_BOARD_AS5600_INTERNAL_H
#define V2K_BOARD_AS5600_INTERNAL_H

#include "v2k_board_as5600.h"

void v2k_board_as5600_service(void);

// Stable diagnostic addresses used only by the platform descriptor registry.
volatile uint32_t *v2k_board_as5600_error_count_address(void);
volatile uint32_t *v2k_board_as5600_sequence_address(void);
volatile uint16_t *v2k_board_as5600_status_address(void);

#endif // V2K_BOARD_AS5600_INTERNAL_H
