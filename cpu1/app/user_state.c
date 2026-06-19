//=============================================================================
// user_state.c - second plain-C user translation unit
//=============================================================================

#include "user_state.h"

float g_user_secondary_gain = 0.25f;
uint32_t g_user_secondary_ticks;
uint32_t g_user_secondary_last_local_count;

void user_secondary_step(void)
{
    static uint32_t local_count;

    local_count++;
    g_user_secondary_last_local_count = local_count;
    g_user_secondary_ticks += (uint32_t)(g_user_secondary_gain * 4.0f);
}
