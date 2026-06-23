//=============================================================================
// user.c - Phase 5.5 low-energy open-loop V/f first-rotation application
//=============================================================================

#include "board.h"
#include "driverlib.h"
#include "v2k.h"
#include "wire/wire_as5600.h"

#define USER_CONTROL_HZ              20000.0f
#define USER_CONTROL_DT              (1.0f / USER_CONTROL_HZ)
#define USER_VBUS_ADC_FULL_SCALE_V   52.29859719f
#define USER_VBUS_VOLTS_PER_COUNT    (USER_VBUS_ADC_FULL_SCALE_V / 4096.0f)
#define USER_TWO_THIRDS              0.6666666667f
#define USER_ONE_THIRD               0.3333333333f
#define USER_MOD_HARD_MAX            0.20f
#define USER_FREQ_HARD_MAX_HZ        10.0f
#define USER_I_DEV_HARD_MAX          900.0f
#define USER_OFFSET_MIN_TICKS        64u
#define USER_OFFSET_MAX_TICKS        60000u

#define USER_APP_OFFSET              0u
#define USER_APP_READY               1u
#define USER_APP_RUN                 2u
#define USER_APP_FAULT               3u

#define USER_FAULT_NONE              0u
#define USER_FAULT_VBUS_LOW          1u
#define USER_FAULT_VBUS_HIGH         2u
#define USER_FAULT_CURRENT_DEV       3u
#define USER_FAULT_BAD_PARAM         4u

uint32_t setup_count;
uint32_t control_ticks;
uint16_t adc_va_raw;
uint16_t adc_vb_raw;
uint16_t adc_vc_raw;
uint16_t adc_vbus_raw;
float vbus_V;
uint16_t adc_ia_raw;
uint16_t adc_ib_raw;
uint16_t adc_ic_raw;
uint16_t enc_raw;
uint16_t enc_status;
float enc_angle;
uint32_t enc_seq;
uint16_t enc_ok;

uint16_t motor_enable = 0u;
uint16_t app_clear = 0u;
float freq_cmd_hz = 2.0f;
float freq_slew = 5.0f;
float vf_v_per_hz = 0.15f;
float vf_boost_V = 0.30f;
float mod_max = 0.10f;
float vbus_min_V = 8.0f;
float vbus_max_V = 18.0f;
float i_dev_limit = 320.0f;
uint16_t offset_ticks = 4000u;

uint16_t app_state;
uint16_t app_fault;
uint32_t offset_count;
float ia_offset;
float ib_offset;
float ic_offset;
float ia_dev;
float ib_dev;
float ic_dev;
float i_dev_abs;
float freq_run_hz;
float elec_phase;
float mod_cmd;
float app_duty_a;
float app_duty_b;
float app_duty_c;

static float user_absf(float value)
{
    return (value < 0.0f) ? -value : value;
}

static float user_clampf(float value, float low, float high)
{
    if (!(value >= low))
    {
        return low;
    }
    if (value > high)
    {
        return high;
    }
    return value;
}

static uint16_t user_offset_target(void)
{
    uint16_t ticks = offset_ticks;

    if (ticks < USER_OFFSET_MIN_TICKS)
    {
        ticks = USER_OFFSET_MIN_TICKS;
    }
    if (ticks > USER_OFFSET_MAX_TICKS)
    {
        ticks = USER_OFFSET_MAX_TICKS;
    }
    return ticks;
}

static float user_wrap_phase(float phase)
{
    while (phase >= 1.0f)
    {
        phase -= 1.0f;
    }
    while (phase < 0.0f)
    {
        phase += 1.0f;
    }
    return phase;
}

static float user_sin01(float phase)
{
    float x;
    float y;
    float correction;
    float sign = 1.0f;

    phase = user_wrap_phase(phase);
    if (phase >= 0.5f)
    {
        phase -= 0.5f;
        sign = -1.0f;
    }

    x = phase * 2.0f;
    y = 4.0f * x * (1.0f - x);
    correction = 0.225f * ((y * user_absf(y)) - y);
    return sign * (y + correction);
}

static void user_set_neutral(void)
{
    app_duty_a = V2K_DUTY_NEUTRAL;
    app_duty_b = V2K_DUTY_NEUTRAL;
    app_duty_c = V2K_DUTY_NEUTRAL;
    v2k_io.out.duty_a = app_duty_a;
    v2k_io.out.duty_b = app_duty_b;
    v2k_io.out.duty_c = app_duty_c;
}

static void user_reset_motion(void)
{
    freq_run_hz = 0.0f;
    elec_phase = 0.0f;
    mod_cmd = 0.0f;
    user_set_neutral();
}

static void user_reset_offsets(void)
{
    offset_count = 0uL;
    ia_offset = 0.0f;
    ib_offset = 0.0f;
    ic_offset = 0.0f;
}

static void user_latch_fault(uint16_t fault)
{
    app_fault = fault;
    app_state = USER_APP_FAULT;
    motor_enable = 0u;
    user_reset_motion();
}

static void user_sample_adc(void)
{
    wire_as5600_sample_t encoder;

    // EPWM1 SOCA has completed this seven-channel frame before ISR entry.
    adc_va_raw = ADC_readResult(myADC0_RESULT_BASE, myADC0_SOC0);
    adc_vb_raw = ADC_readResult(myADC0_RESULT_BASE, myADC0_SOC1);
    adc_vc_raw = ADC_readResult(myADC1_RESULT_BASE, myADC1_SOC0);
    adc_vbus_raw = ADC_readResult(myADC0_RESULT_BASE, myADC0_SOC3);
    // BOOSTXL-DRV8323RS VBUS divider with the LaunchPad's external 3.0 V VREF.
    vbus_V = (float)adc_vbus_raw * USER_VBUS_VOLTS_PER_COUNT;
    adc_ia_raw = ADC_readResult(myADC1_RESULT_BASE, myADC1_SOC1);
    adc_ib_raw = ADC_readResult(myADC0_RESULT_BASE, myADC0_SOC2);
    adc_ic_raw = ADC_readResult(myADC2_RESULT_BASE, myADC2_SOC0);

    enc_ok = wire_as5600_get_latest(&encoder);
    enc_raw = encoder.raw_angle;
    enc_status = encoder.status;
    enc_angle = encoder.angle_rad;
    enc_seq = encoder.sequence;

    ia_dev = (float)adc_ia_raw - ia_offset;
    ib_dev = (float)adc_ib_raw - ib_offset;
    ic_dev = (float)adc_ic_raw - ic_offset;
    i_dev_abs = user_absf(ia_dev);
    if (user_absf(ib_dev) > i_dev_abs)
    {
        i_dev_abs = user_absf(ib_dev);
    }
    if (user_absf(ic_dev) > i_dev_abs)
    {
        i_dev_abs = user_absf(ic_dev);
    }
}

static uint16_t user_offsets_ready(void)
{
    uint16_t target = user_offset_target();
    uint32_t next_count;
    float denom;

    if (offset_count >= (uint32_t)target)
    {
        return 1u;
    }

    next_count = offset_count + 1uL;
    denom = (float)next_count;
    ia_offset += (((float)adc_ia_raw - ia_offset) / denom);
    ib_offset += (((float)adc_ib_raw - ib_offset) / denom);
    ic_offset += (((float)adc_ic_raw - ic_offset) / denom);
    offset_count = next_count;

    if (offset_count >= (uint32_t)target)
    {
        ia_dev = (float)adc_ia_raw - ia_offset;
        ib_dev = (float)adc_ib_raw - ib_offset;
        ic_dev = (float)adc_ic_raw - ic_offset;
        return 1u;
    }
    return 0u;
}

static uint16_t user_parameters_valid(void)
{
    return
        (freq_slew > 0.0f) &&
        (vf_v_per_hz >= 0.0f) &&
        (vf_boost_V >= 0.0f) &&
        (mod_max > 0.0f) &&
        (vbus_min_V > 0.0f) &&
        (vbus_max_V > vbus_min_V) &&
        (i_dev_limit > 0.0f);
}

static void user_run_vf(void)
{
    float freq_limit = USER_FREQ_HARD_MAX_HZ;
    float freq_target = user_clampf(freq_cmd_hz, -freq_limit, freq_limit);
    float slew_step = user_clampf(freq_slew, 0.0f, USER_FREQ_HARD_MAX_HZ) *
                      USER_CONTROL_DT;
    float freq_delta = freq_target - freq_run_hz;
    float v_phase;
    float mod_limit;
    float s_a;
    float s_b;
    float s_c;

    if (freq_delta > slew_step)
    {
        freq_delta = slew_step;
    }
    else if (freq_delta < -slew_step)
    {
        freq_delta = -slew_step;
    }
    freq_run_hz += freq_delta;

    v_phase = vf_boost_V + (vf_v_per_hz * user_absf(freq_run_hz));
    if (vbus_V > 0.1f)
    {
        mod_cmd = (2.0f * v_phase) / vbus_V;
    }
    else
    {
        mod_cmd = 0.0f;
    }

    mod_limit = user_clampf(mod_max, 0.0f, USER_MOD_HARD_MAX);
    mod_cmd = user_clampf(mod_cmd, 0.0f, mod_limit);

    elec_phase = user_wrap_phase(elec_phase + (freq_run_hz * USER_CONTROL_DT));
    s_a = user_sin01(elec_phase);
    s_b = user_sin01(elec_phase + USER_TWO_THIRDS);
    s_c = user_sin01(elec_phase + USER_ONE_THIRD);

    app_duty_a = V2K_DUTY_NEUTRAL + (0.5f * mod_cmd * s_a);
    app_duty_b = V2K_DUTY_NEUTRAL + (0.5f * mod_cmd * s_b);
    app_duty_c = V2K_DUTY_NEUTRAL + (0.5f * mod_cmd * s_c);
    v2k_io.out.duty_a = app_duty_a;
    v2k_io.out.duty_b = app_duty_b;
    v2k_io.out.duty_c = app_duty_c;
}

void setup(void)
{
    setup_count++;
    app_state = USER_APP_OFFSET;
    app_fault = USER_FAULT_NONE;
    app_clear = 0u;
    user_reset_offsets();
    user_reset_motion();
}

void control(void)
{
    user_sample_adc();

    if (app_state == USER_APP_FAULT)
    {
        user_set_neutral();
        if ((app_clear != 0u) && (motor_enable == 0u))
        {
            app_clear = 0u;
            app_fault = USER_FAULT_NONE;
            app_state = USER_APP_OFFSET;
            user_reset_offsets();
        }
        control_ticks++;
        return;
    }

    if (user_offsets_ready() == 0u)
    {
        app_state = USER_APP_OFFSET;
        user_reset_motion();
        control_ticks++;
        return;
    }

    if (user_parameters_valid() == 0u)
    {
        user_latch_fault(USER_FAULT_BAD_PARAM);
        control_ticks++;
        return;
    }

    if (motor_enable == 0u)
    {
        app_state = USER_APP_READY;
        user_reset_motion();
        control_ticks++;
        return;
    }

    if (vbus_V < vbus_min_V)
    {
        user_latch_fault(USER_FAULT_VBUS_LOW);
    }
    else if (vbus_V > vbus_max_V)
    {
        user_latch_fault(USER_FAULT_VBUS_HIGH);
    }
    else if (i_dev_abs > user_clampf(i_dev_limit, 1.0f, USER_I_DEV_HARD_MAX))
    {
        user_latch_fault(USER_FAULT_CURRENT_DEV);
    }
    else
    {
        app_state = USER_APP_RUN;
        user_run_vf();
    }

    control_ticks++;
}
