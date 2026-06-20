//=============================================================================
// v2k_profile.h - one-second CPU1 ISR load window
//=============================================================================
#ifndef V2K_PROFILE_H
#define V2K_PROFILE_H

#include "../../contracts/v2k_common.h"
#include "../../contracts/v2k_command.h"

extern volatile uint32_t g_v2k_prof_seq;
extern volatile uint32_t g_v2k_cycle_budget;
extern volatile uint32_t g_v2k_load_avg;
extern volatile uint32_t g_v2k_load_peak;
extern volatile uint32_t g_v2k_ctrl_at_peak;
extern volatile uint32_t g_v2k_scope_at_peak;
extern volatile uint16_t g_v2k_lat_at_peak;
extern volatile v2k_tick_t g_v2k_peak_tick;

void v2k_profile_init(void);
void v2k_profile_sample(uint32_t isr_cycles,
                        uint32_t control_cycles,
                        uint32_t scope_cycles,
                        uint16_t latency,
                        v2k_tick_t tick);
void v2k_profile_service(void);
void v2k_profile_publish_status(volatile v2k_cpu1_status_t *status);

#endif // V2K_PROFILE_H
