//=============================================================================
// v2k.h - Viewer2000 user-facing control interface
//=============================================================================
#ifndef V2K_H
#define V2K_H

#include "../contracts/v2k_common.h"

#define V2K_DUE_1KHZ  0x0001u
#define V2K_DUE_100HZ 0x0002u

typedef struct {
    v2k_tick_t tick;
    uint16_t due_mask;
    uint16_t state;
    uint16_t fault_code;
} v2k_sys_t;

#define V2K_SYS_TYPE_DEFINED 1
#include "board/include/v2k_board_profile_select.h"
#include V2K_BOARD_PROFILE_USER_API_HEADER

extern volatile v2k_io_t v2k_io;

void setup(void);
void control(void);

#endif // V2K_H
