//=============================================================================
// v2k_cpu2_board.h - minimal CPU2 board seam
//
// This is the compatibility boundary for CPU2 physical bring-up. The SCI
// protocol service is intentionally not abstracted here yet; extracting a
// generic pipe interface is the next portability slice.
//=============================================================================
#ifndef V2K_CPU2_BOARD_H
#define V2K_CPU2_BOARD_H

#include "../common/v2k_planes.h"

void v2k_cpu2_board_init_device(void);
void v2k_cpu2_board_assert_layout(const volatile v2k_gs4_plane_t *gs4,
                                  const volatile v2k_msg_2to1_t *msg_2to1);
void v2k_cpu2_board_init_sci_pipe(void);
void v2k_cpu2_board_delay_100us(void);
void v2k_cpu2_board_toggle_status_led(void);
void v2k_cpu2_board_panic_halt(void);

#endif // V2K_CPU2_BOARD_H
