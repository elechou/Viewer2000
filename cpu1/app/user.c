//=============================================================================
// user.c - Phase 4.1 plain-C user application
//=============================================================================

#include "v2k.h"
#include "user_state.h"
#include "libraries/control/DCL/c28/include/DCLF32.h"

typedef struct
{
    float error_history[3];
    uint16_t write_index;
} user_trace_t;

float g_user_setpoint = 1.5f;

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

const float g_user_gain_table[4] = {1.0f, 0.75f, 0.50f, 0.25f};
float g_user_initial_offsets[3] = {0.10f, -0.05f, 0.025f};
uint32_t g_user_setup_count;
float g_user_last_error;
float g_user_last_output;
uint32_t g_user_control_ticks;
user_trace_t g_user_trace;

static float user_filter(float input)
{
    static float history;
    history += 0.125f * (input - history);
    return history;
}

void setup(void)
{
    g_user_setup_count++;
}

void control(void)
{
    float duty;
    float feedback = user_filter(v2k_io.in.vsense + g_user_initial_offsets[0]);

    g_user_last_error = g_user_setpoint - feedback;
    duty = DCL_runPI_C2(&g_user_pi,
                        g_user_setpoint,
                        feedback * g_user_gain_table[0]);

    v2k_io.out.duty_a = duty;
    g_user_last_output = duty;
    g_user_control_ticks++;
    g_user_trace.error_history[g_user_trace.write_index] = g_user_last_error;
    g_user_trace.write_index++;
    if (g_user_trace.write_index >= 3u)
    {
        g_user_trace.write_index = 0u;
    }
    user_secondary_step();
}
