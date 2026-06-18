//=============================================================================
// v2k_user.c - 默认 L3 示例；应用可用强定义替换此弱符号
//=============================================================================

#include "v2k_platform.h"
#include "v2k_timebase.h"

#define V2K_DBG_SINE_HZ       10u
#define V2K_DBG_PI            3.14159265358979323846f
#define V2K_DBG_TWO_PI        (2.0f * V2K_DBG_PI)
#define V2K_DBG_SINE_STEP_RAD \
    ((V2K_DBG_TWO_PI * (float)V2K_DBG_SINE_HZ) / (float)V2K_ISR_HZ)
#define V2K_DBG_SINE_STEP2    (V2K_DBG_SINE_STEP_RAD * V2K_DBG_SINE_STEP_RAD)
#define V2K_DBG_SINE_STEP3    (V2K_DBG_SINE_STEP_RAD * V2K_DBG_SINE_STEP2)
#define V2K_DBG_SINE_STEP4    (V2K_DBG_SINE_STEP2 * V2K_DBG_SINE_STEP2)
#define V2K_DBG_SINE_STEP5    (V2K_DBG_SINE_STEP_RAD * V2K_DBG_SINE_STEP4)
#define V2K_DBG_SINE_STEP6    (V2K_DBG_SINE_STEP2 * V2K_DBG_SINE_STEP4)
#define V2K_DBG_SINE_STEP_SIN \
    (V2K_DBG_SINE_STEP_RAD - (V2K_DBG_SINE_STEP3 / 6.0f) + \
     (V2K_DBG_SINE_STEP5 / 120.0f))
#define V2K_DBG_SINE_STEP_COS \
    (1.0f - (V2K_DBG_SINE_STEP2 / 2.0f) + \
     (V2K_DBG_SINE_STEP4 / 24.0f) - (V2K_DBG_SINE_STEP6 / 720.0f))
#define V2K_DBG_NOISE_AMP      0.03f
#define V2K_DBG_NOISE_U16_GAIN (2.0f / 65535.0f)

V2K_STATIC_ASSERT((V2K_ISR_HZ % V2K_DBG_SINE_HZ) == 0u);

extern volatile float g_v2k_pwm_duty_cmd;

volatile float g_v2k_dbg_sine_10hz;

static float s_dbg_sine;
static float s_dbg_cosine = 1.0f;
static uint32_t s_dbg_sine_tick;
static uint32_t s_dbg_noise_state = 0xA341316CUL;

static float v2k_dbg_white_noise(void)
{
    uint32_t x = s_dbg_noise_state;

    x ^= (x << 13);
    x ^= (x >> 17);
    x ^= (x << 5);
    s_dbg_noise_state = x;

    return ((((float)(x & 0xFFFFUL)) * V2K_DBG_NOISE_U16_GAIN) - 1.0f) *
           V2K_DBG_NOISE_AMP;
}

static void v2k_dbg_sine_step(void)
{
    float next_sine;
    float next_cosine;

    g_v2k_dbg_sine_10hz = s_dbg_sine + v2k_dbg_white_noise();

    next_sine = (s_dbg_sine * V2K_DBG_SINE_STEP_COS) +
                (s_dbg_cosine * V2K_DBG_SINE_STEP_SIN);
    next_cosine = (s_dbg_cosine * V2K_DBG_SINE_STEP_COS) -
                  (s_dbg_sine * V2K_DBG_SINE_STEP_SIN);
    s_dbg_sine = next_sine;
    s_dbg_cosine = next_cosine;

    s_dbg_sine_tick++;
    if (s_dbg_sine_tick >= (V2K_ISR_HZ / V2K_DBG_SINE_HZ))
    {
        s_dbg_sine_tick = 0u;
        s_dbg_sine = 0.0f;
        s_dbg_cosine = 1.0f;
    }
}

#pragma WEAK(user_step)
void user_step(const plat_in_t *in, plat_out_t *out)
{
    (void)in;
    v2k_dbg_sine_step();
    out->pwm1_duty = g_v2k_pwm_duty_cmd;
}
