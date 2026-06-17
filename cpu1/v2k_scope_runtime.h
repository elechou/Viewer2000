//=============================================================================
// v2k_scope_runtime.h - CPU1 示波生产者与 CCS view
//=============================================================================
#ifndef V2K_SCOPE_RUNTIME_H
#define V2K_SCOPE_RUNTIME_H

#include "../contracts/v2k_common.h"

#define V2K_CCS_VIEW_SAMPLES 2048u

typedef struct {
    uint16_t channel_slot;
    uint16_t request_seq;
    uint16_t done_seq;
    uint16_t result;
    uint16_t count;
    v2k_tick_t start_tick;
    float data[V2K_CCS_VIEW_SAMPLES];
} v2k_ccs_view_t;

extern v2k_ccs_view_t g_v2k_ccs_view;
extern volatile uint32_t g_v2k_scope_overrun_total;

void v2k_scope_init(void);
void v2k_scope_service(void);
void v2k_scope_apply_ready(void);
void v2k_scope_sample_all(v2k_tick_t tick);
void v2k_scope_ccs_view_service(void);

#endif // V2K_SCOPE_RUNTIME_H
