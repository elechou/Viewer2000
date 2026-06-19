//=============================================================================
// user.c - Phase 4.1 plain-C user application
//=============================================================================

#include "v2k.h"
#include "user_state.h"
#include "libraries/control/DCL/c28/include/DCLF32.h"

typedef struct
{
    float err[3];
    uint16_t idx;
} user_trace_t;

float setpoint = 1.5f;

DCL_PI pi = {
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

const float gain[4] = {1.0f, 0.75f, 0.50f, 0.25f};
float offset[3] = {0.10f, -0.05f, 0.025f};
uint32_t setup_count;
float last_error;
float last_output;
uint32_t control_ticks;
user_trace_t trace;

static float user_filter(float input)
{
    static float history;
    history += 0.125f * (input - history);
    return history;
}

void setup(void)
{
    setup_count++;
}

void control(void)
{
    float duty;
    float feedback = user_filter(v2k_io.in.vsense + offset[0]);

    last_error = setpoint - feedback;
    duty = DCL_runPI_C2(&pi,
                        setpoint,
                        feedback * gain[0]);

    v2k_io.out.duty_a = duty;
    last_output = duty;
    control_ticks++;
    trace.err[trace.idx] = last_error;
    trace.idx++;
    if (trace.idx >= 3u)
    {
        trace.idx = 0u;
    }
    user_secondary_step();
}
