//=============================================================================
// user_state.h - second-translation-unit state used by the Phase 4.1 demo
//=============================================================================
#ifndef USER_STATE_H
#define USER_STATE_H

#include <stdint.h>

extern float g_user_secondary_gain;
extern uint32_t g_user_secondary_ticks;
extern uint32_t g_user_secondary_last_local_count;

void user_secondary_step(void);

#endif // USER_STATE_H
