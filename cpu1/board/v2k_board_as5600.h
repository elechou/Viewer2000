//=============================================================================
// v2k_board_as5600.h - public L0 AS5600 cached-sample API
//
// The platform services I2C in the foreground. User control code may call
// v2k_board_as5600_get_latest() from control(); it copies the latest coherent sample
// and never starts or waits for an I2C transaction.
//=============================================================================
#ifndef V2K_BOARD_AS5600_H
#define V2K_BOARD_AS5600_H

#include <stdint.h>

typedef struct
{
    uint16_t raw_angle;
    uint16_t status;
    float angle_rad;
    uint32_t sequence;
    uint16_t valid;
} v2k_board_as5600_sample_t;

uint16_t v2k_board_as5600_get_latest(v2k_board_as5600_sample_t *sample);

#endif // V2K_BOARD_AS5600_H
