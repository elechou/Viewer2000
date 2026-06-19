//=============================================================================
// v2k_user_runtime.h - resettable user lifecycle support
//=============================================================================
#ifndef V2K_USER_RUNTIME_H
#define V2K_USER_RUNTIME_H

#include "../v2k.h"

extern volatile uint16_t g_v2k_app_enabled;
extern volatile uint32_t g_v2k_user_reset_count;
extern volatile uint16_t g_v2k_user_reset_error;
extern volatile uint16_t g_v2k_user_reset_active;

void v2k_user_runtime_init(void);
uint16_t v2k_user_prepare_start(void);
void v2k_user_disable(void);
uint16_t v2k_user_is_enabled(void);
uint16_t v2k_user_reset_is_active(void);
void v2k_user_control_tick(void);

#endif // V2K_USER_RUNTIME_H
