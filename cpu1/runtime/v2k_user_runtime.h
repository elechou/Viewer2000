//=============================================================================
// v2k_user_runtime.h - resettable user lifecycle support
//=============================================================================
#ifndef V2K_USER_RUNTIME_H
#define V2K_USER_RUNTIME_H

#include "../v2k.h"

#define V2K_USER_RESET_OK                 0u
#define V2K_USER_RESET_ERR_LAYOUT         1u
#define V2K_USER_RESET_ERR_CRC_TABLE      2u
#define V2K_USER_RESET_ERR_GOLDEN_CRC     3u
#define V2K_USER_RESET_ERR_RUN_CRC        4u

extern volatile uint16_t g_v2k_app_enabled;
extern volatile uint32_t g_v2k_user_reset_count;
extern volatile uint16_t g_v2k_user_reset_error;
extern volatile uint16_t g_v2k_user_reset_active;
extern volatile uint32_t g_v2k_user_crc_expected;
extern volatile uint32_t g_v2k_user_crc_actual;

void v2k_user_runtime_init(void);
uint16_t v2k_user_prepare_start(void);
void v2k_user_disable(void);
uint16_t v2k_user_is_enabled(void);
uint16_t v2k_user_reset_is_active(void);
void v2k_user_control_tick(void);

#endif // V2K_USER_RUNTIME_H
