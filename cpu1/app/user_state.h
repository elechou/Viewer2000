//=============================================================================
// user_state.h - second-translation-unit state used by the Phase 4.1 demo
//=============================================================================
#ifndef USER_STATE_H
#define USER_STATE_H

#include <stdint.h>

extern float secondary_gain;
extern uint32_t secondary_ticks;
extern uint32_t secondary_last;

void user_secondary_step(void);

#endif // USER_STATE_H
