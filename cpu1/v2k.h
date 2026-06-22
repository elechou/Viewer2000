//=============================================================================
// v2k.h - Viewer2000 user-facing control interface
//=============================================================================
#ifndef V2K_H
#define V2K_H

#include "../contracts/v2k_common.h"

#define V2K_DUE_1KHZ  0x0001u
#define V2K_DUE_100HZ 0x0002u

#define V2K_DUTY_NEUTRAL  0.50f
#define V2K_DUTY_SAFE_MIN 0.02f
#define V2K_DUTY_SAFE_MAX 0.98f

typedef struct {
    v2k_tick_t tick;
    uint16_t due_mask;
    uint16_t sys_state;
    uint16_t fault_code;
} v2k_io_in_t;

typedef struct {
    float duty_a;
    float duty_b;
    float duty_c;
} v2k_io_out_t;

typedef struct {
    v2k_io_in_t in;
    v2k_io_out_t out;
} v2k_io_t;

extern volatile v2k_io_t v2k_io;

void setup(void);
void control(void);

#endif // V2K_H
