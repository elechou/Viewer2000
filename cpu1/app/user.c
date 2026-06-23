//=============================================================================
// user.c - Phase 5.2 powered-neutral commissioning application
//=============================================================================

#include "board.h"
#include "driverlib.h"
#include "v2k.h"
#include "wire/wire_as5600.h"

#define USER_VBUS_ADC_FULL_SCALE_V   52.29859719f
#define USER_VBUS_VOLTS_PER_COUNT    (USER_VBUS_ADC_FULL_SCALE_V / 4096.0f)

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

void setup(void)
{
    setup_count++;
}

void control(void)
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

    // Phase 5.2 intentionally has no tunable actuation command. POWERED mode
    // may wake and verify the driver, but this application can only request the
    // three-phase neutral vector.
    v2k_io.out.duty_a = V2K_DUTY_NEUTRAL;
    v2k_io.out.duty_b = V2K_DUTY_NEUTRAL;
    v2k_io.out.duty_c = V2K_DUTY_NEUTRAL;
    control_ticks++;
}
