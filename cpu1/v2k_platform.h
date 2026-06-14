//=============================================================================
// v2k_platform.h - L1 执行器对 L3 暴露的唯一逐拍接口
//=============================================================================
#ifndef V2K_PLATFORM_H
#define V2K_PLATFORM_H

#include "../contracts/v2k_common.h"

#define PLAT_DUE_1KHZ 0x0001u
#define PLAT_DUE_100HZ 0x0002u

typedef struct {
    v2k_tick_t tick;
    uint16_t due_mask;
    uint16_t adc_a0_raw;
    float adc_a0_v;
    uint16_t sys_state;
    uint16_t fault_code;
} plat_in_t;

typedef struct {
    float pwm1_duty;
} plat_out_t;

void user_step(const plat_in_t *in, plat_out_t *out);

#endif // V2K_PLATFORM_H
