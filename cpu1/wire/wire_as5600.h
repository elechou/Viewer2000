//=============================================================================
// wire_as5600.h - public L0 AS5600 cached-sample API
//
// The platform services I2C in the foreground. User control code may call
// wire_as5600_get_latest() from control(); it copies the latest coherent sample
// and never starts or waits for an I2C transaction.
//=============================================================================
#ifndef WIRE_AS5600_H
#define WIRE_AS5600_H

#include <stdint.h>

typedef struct
{
    uint16_t raw_angle;
    uint16_t status;
    float angle_rad;
    uint32_t sequence;
    uint16_t valid;
} wire_as5600_sample_t;

uint16_t wire_as5600_get_latest(wire_as5600_sample_t *sample);

#endif // WIRE_AS5600_H
