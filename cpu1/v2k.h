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
    uint16_t state;
    uint16_t fault_code;
} v2k_sys_t;

typedef struct {
    uint16_t va_raw;
    uint16_t vb_raw;
    uint16_t vc_raw;
    uint16_t vbus_raw;
    uint16_t ia_raw;
    uint16_t ib_raw;
    uint16_t ic_raw;
} v2k_adc_t;

typedef struct {
    float duty_a;
    float duty_b;
    float duty_c;
} v2k_pwm_t;

typedef struct {
    v2k_sys_t sys;
    v2k_adc_t adc;
    v2k_pwm_t pwm;
} v2k_io_t;

extern volatile v2k_io_t v2k_io;

void v2k_pwm_apply(float duty_a, float duty_b, float duty_c);

void setup(void);
void control(void);

#endif // V2K_H
