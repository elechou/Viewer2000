//=============================================================================
// user_state.c - second plain-C user translation unit
//=============================================================================

#include "user_state.h"

float secondary_gain = 0.25f;
uint32_t secondary_ticks;
uint32_t secondary_last;

void user_secondary_step(void)
{
    static uint32_t local_count;

    local_count++;
    secondary_last = local_count;
    secondary_ticks += (uint32_t)(secondary_gain * 4.0f);
}
