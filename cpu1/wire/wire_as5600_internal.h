//=============================================================================
// wire_as5600_internal.h - private AS5600 service hooks
//=============================================================================
#ifndef WIRE_AS5600_INTERNAL_H
#define WIRE_AS5600_INTERNAL_H

#include "wire_as5600.h"

void wire_as5600_service(void);

// Stable diagnostic addresses used only by the platform descriptor registry.
volatile uint32_t *wire_as5600_error_count_address(void);
volatile uint32_t *wire_as5600_sequence_address(void);
volatile uint16_t *wire_as5600_status_address(void);

#endif // WIRE_AS5600_INTERNAL_H
