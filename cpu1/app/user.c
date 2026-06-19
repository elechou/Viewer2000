//=============================================================================
// user.c - Phase 4 demo user application
//=============================================================================

#include "v2k.h"
#include "libraries/control/DCL/c28/include/DCLF32.h"

#pragma DATA_SECTION(g_user_setpoint, "v2k_user_data")
float g_user_setpoint = 1.5f;

#pragma DATA_SECTION(g_user_pi, "v2k_user_data")
DCL_PI g_user_pi = {
    0.35f,              // Kp
    0.015f,             // Ki
    0.0f,               // i10
    0.80f,              // Umax
    0.05f,              // Umin
    1.0f,               // i6
    0.0f,               // i11
    0.80f,              // Imax
    0.05f,              // Imin
    NULL_ADDR,          // sps
    NULL_ADDR           // css
};

#pragma DATA_SECTION(g_user_setup_count, "v2k_user_data")
uint32_t g_user_setup_count = 0u;

#pragma DATA_SECTION(g_user_last_error, "v2k_user_bss")
float g_user_last_error;

#pragma DATA_SECTION(g_user_last_output, "v2k_user_bss")
float g_user_last_output;

#pragma DATA_SECTION(g_user_control_ticks, "v2k_user_bss")
uint32_t g_user_control_ticks;

void setup(void)
{
    g_user_setup_count++;
}

void control(void)
{
    float duty;

    g_user_last_error = g_user_setpoint - v2k_io.in.vsense;
    duty = DCL_runPI_C2(&g_user_pi, g_user_setpoint, v2k_io.in.vsense);

    v2k_io.out.duty_a = duty;
    g_user_last_output = duty;
    g_user_control_ticks++;
}
