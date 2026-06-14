//=============================================================================
// v2k_user.c - 默认 L3 示例；应用可用强定义替换此弱符号
//=============================================================================

#include "v2k_platform.h"

extern volatile float g_v2k_pwm_duty_cmd;

#pragma WEAK(user_step)
void user_step(const plat_in_t *in, plat_out_t *out)
{
    (void)in;
    out->pwm1_duty = g_v2k_pwm_duty_cmd;
}
