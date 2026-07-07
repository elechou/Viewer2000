//=============================================================================
// v2k_board_user_api.h - public three-phase profile user I/O surface
//=============================================================================
#ifndef V2K_BOARD_USER_API_F28P65X_LAUNCHXL_DRV8323RS_AS5600_H
#define V2K_BOARD_USER_API_F28P65X_LAUNCHXL_DRV8323RS_AS5600_H

#ifndef V2K_SYS_TYPE_DEFINED
#error "Include cpu1/v2k.h instead of including a profile user API directly"
#endif

#define V2K_DUTY_NEUTRAL  0.50f
#define V2K_DUTY_SAFE_MIN 0.02f
#define V2K_DUTY_SAFE_MAX 0.98f

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

void v2k_pwm_apply(float duty_a, float duty_b, float duty_c);

#endif // V2K_BOARD_USER_API_F28P65X_LAUNCHXL_DRV8323RS_AS5600_H
