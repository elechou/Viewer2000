//=============================================================================
// user.c - Phase 5.5 low-energy open-loop V/f first-rotation application
//=============================================================================

#include "board.h"
#include "driverlib.h"
#include "v2k.h"
#include "wire/wire_as5600.h"

#define USER_CONTROL_HZ 20000.0f
#define USER_CONTROL_DT (1.0f / USER_CONTROL_HZ)
#define USER_VBUS_ADC_FULL_SCALE_V 52.29859719f
#define USER_VBUS_VOLTS_PER_COUNT (USER_VBUS_ADC_FULL_SCALE_V / 4096.0f)
#define USER_TWO_THIRDS 0.6666666667f
#define USER_ONE_THIRD 0.3333333333f

#define USER_APP_OFFSET 0u
#define USER_APP_READY 1u
#define USER_APP_RUN 2u

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
float freq_cmd_hz = 2.0f;
float freq_slew = 5.0f;
float vf_v_per_hz = 0.20f;
float vf_boost_V = 0.40f;
float mod_max = 0.15f;
uint16_t offset_ticks = 4000u;

uint16_t app_state;
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

static float user_sin01(float phase) {
  float x;
  float y;
  float correction;
  float sign = 1.0f;

  while (phase >= 1.0f) {
    phase -= 1.0f;
  }
  while (phase < 0.0f) {
    phase += 1.0f;
  }

  if (phase >= 0.5f) {
    phase -= 0.5f;
    sign = -1.0f;
  }

  x = phase * 2.0f;
  y = 4.0f * x * (1.0f - x);
  correction = 0.225f * ((y * y) - y);
  return sign * (y + correction);
}

void setup(void) {
  setup_count++;
  app_state = USER_APP_OFFSET;
  offset_count = 0uL;
  ia_offset = 0.0f;
  ib_offset = 0.0f;
  ic_offset = 0.0f;
  freq_run_hz = 0.0f;
  elec_phase = 0.0f;
  mod_cmd = 0.0f;
  app_duty_a = V2K_DUTY_NEUTRAL;
  app_duty_b = V2K_DUTY_NEUTRAL;
  app_duty_c = V2K_DUTY_NEUTRAL;
  v2k_io.out.duty_a = app_duty_a;
  v2k_io.out.duty_b = app_duty_b;
  v2k_io.out.duty_c = app_duty_c;
}

void control(void) {
  wire_as5600_sample_t encoder;
  uint16_t target_ticks;
  uint32_t next_count;
  float denom;
  float abs_a;
  float abs_b;
  float abs_c;
  float freq_delta;
  float slew_step;
  float v_phase;
  float sin_a;
  float sin_b;
  float sin_c;

  // EPWM1 SOCA has completed this seven-channel frame before ISR entry.
  adc_va_raw = ADC_readResult(myADC0_RESULT_BASE, myADC0_SOC0);
  adc_vb_raw = ADC_readResult(myADC0_RESULT_BASE, myADC0_SOC1);
  adc_vc_raw = ADC_readResult(myADC1_RESULT_BASE, myADC1_SOC0);
  adc_vbus_raw = ADC_readResult(myADC0_RESULT_BASE, myADC0_SOC3);
  adc_ia_raw = ADC_readResult(myADC1_RESULT_BASE, myADC1_SOC1);
  adc_ib_raw = ADC_readResult(myADC0_RESULT_BASE, myADC0_SOC2);
  adc_ic_raw = ADC_readResult(myADC2_RESULT_BASE, myADC2_SOC0);

  vbus_V = (float)adc_vbus_raw * USER_VBUS_VOLTS_PER_COUNT;

  enc_ok = wire_as5600_get_latest(&encoder);
  enc_raw = encoder.raw_angle;
  enc_status = encoder.status;
  enc_angle = encoder.angle_rad;
  enc_seq = encoder.sequence;

  target_ticks = offset_ticks;
  if (target_ticks == 0u) {
    target_ticks = 1u;
  }

  if (offset_count < (uint32_t)target_ticks) {
    next_count = offset_count + 1uL;
    denom = (float)next_count;
    ia_offset += (((float)adc_ia_raw - ia_offset) / denom);
    ib_offset += (((float)adc_ib_raw - ib_offset) / denom);
    ic_offset += (((float)adc_ic_raw - ic_offset) / denom);
    offset_count = next_count;

    ia_dev = (float)adc_ia_raw - ia_offset;
    ib_dev = (float)adc_ib_raw - ib_offset;
    ic_dev = (float)adc_ic_raw - ic_offset;
    abs_a = (ia_dev < 0.0f) ? -ia_dev : ia_dev;
    abs_b = (ib_dev < 0.0f) ? -ib_dev : ib_dev;
    abs_c = (ic_dev < 0.0f) ? -ic_dev : ic_dev;
    i_dev_abs = abs_a;
    if (abs_b > i_dev_abs) {
      i_dev_abs = abs_b;
    }
    if (abs_c > i_dev_abs) {
      i_dev_abs = abs_c;
    }

    app_state = USER_APP_OFFSET;
    freq_run_hz = 0.0f;
    elec_phase = 0.0f;
    mod_cmd = 0.0f;
    app_duty_a = V2K_DUTY_NEUTRAL;
    app_duty_b = V2K_DUTY_NEUTRAL;
    app_duty_c = V2K_DUTY_NEUTRAL;
    v2k_io.out.duty_a = app_duty_a;
    v2k_io.out.duty_b = app_duty_b;
    v2k_io.out.duty_c = app_duty_c;
    control_ticks++;
    return;
  }

  ia_dev = (float)adc_ia_raw - ia_offset;
  ib_dev = (float)adc_ib_raw - ib_offset;
  ic_dev = (float)adc_ic_raw - ic_offset;
  abs_a = (ia_dev < 0.0f) ? -ia_dev : ia_dev;
  abs_b = (ib_dev < 0.0f) ? -ib_dev : ib_dev;
  abs_c = (ic_dev < 0.0f) ? -ic_dev : ic_dev;
  i_dev_abs = abs_a;
  if (abs_b > i_dev_abs) {
    i_dev_abs = abs_b;
  }
  if (abs_c > i_dev_abs) {
    i_dev_abs = abs_c;
  }

  if (motor_enable == 0u) {
    app_state = USER_APP_READY;
    freq_run_hz = 0.0f;
    elec_phase = 0.0f;
    mod_cmd = 0.0f;
    app_duty_a = V2K_DUTY_NEUTRAL;
    app_duty_b = V2K_DUTY_NEUTRAL;
    app_duty_c = V2K_DUTY_NEUTRAL;
    v2k_io.out.duty_a = app_duty_a;
    v2k_io.out.duty_b = app_duty_b;
    v2k_io.out.duty_c = app_duty_c;
    control_ticks++;
    return;
  }

  app_state = USER_APP_RUN;

  freq_delta = freq_cmd_hz - freq_run_hz;
  if (freq_slew > 0.0f) {
    slew_step = freq_slew * USER_CONTROL_DT;
    if (freq_delta > slew_step) {
      freq_delta = slew_step;
    } else if (freq_delta < -slew_step) {
      freq_delta = -slew_step;
    }
  }
  freq_run_hz += freq_delta;

  v_phase = vf_boost_V +
            (vf_v_per_hz * ((freq_run_hz < 0.0f) ? -freq_run_hz : freq_run_hz));
  if (vbus_V > 0.0f) {
    mod_cmd = (2.0f * v_phase) / vbus_V;
  } else {
    mod_cmd = 0.0f;
  }
  if (mod_cmd < 0.0f) {
    mod_cmd = 0.0f;
  }
  if (mod_cmd > mod_max) {
    mod_cmd = mod_max;
  }

  elec_phase += freq_run_hz * USER_CONTROL_DT;
  while (elec_phase >= 1.0f) {
    elec_phase -= 1.0f;
  }
  while (elec_phase < 0.0f) {
    elec_phase += 1.0f;
  }

  sin_a = user_sin01(elec_phase);
  sin_b = user_sin01(elec_phase + USER_TWO_THIRDS);
  sin_c = user_sin01(elec_phase + USER_ONE_THIRD);

  app_duty_a = V2K_DUTY_NEUTRAL + (0.5f * mod_cmd * sin_a);
  app_duty_b = V2K_DUTY_NEUTRAL + (0.5f * mod_cmd * sin_b);
  app_duty_c = V2K_DUTY_NEUTRAL + (0.5f * mod_cmd * sin_c);
  v2k_io.out.duty_a = app_duty_a;
  v2k_io.out.duty_b = app_duty_b;
  v2k_io.out.duty_c = app_duty_c;
  control_ticks++;
}
