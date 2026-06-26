//=============================================================================
// v2k_cpu2_board.h - CPU2 board seam
//
// CPU2 portable services own the shared-interface protocol. This seam owns the
// physical pipe, local boot resources, and diagnostic board I/O.
//=============================================================================
#ifndef V2K_CPU2_BOARD_H
#define V2K_CPU2_BOARD_H

#include "../common/v2k_planes.h"
#include "board/include/v2k_cpu2_board_profile_select.h"

void v2k_cpu2_board_init_device(void);
void v2k_cpu2_board_assert_layout(const volatile v2k_cpu2_plane_t *cpu2_plane,
                                  const volatile v2k_msg_2to1_t *msg_2to1);

// Local boot resources: NMI backstop, IPC rendezvous, and ping-pong ack. These
// own the interrupt vector and IPC registers, so they live below the seam,
// keeping cpu2.c (the portable super-loop orchestration) free of vendor register
// access — symmetric with cpu1/runtime/v2k_main.c.
void v2k_cpu2_board_boot_init_interrupts(void);
void v2k_cpu2_board_boot_sync(void);
uint16_t v2k_cpu2_board_ipc_pong_ack(void);

void v2k_cpu2_board_pipe_init(void);
void v2k_cpu2_board_pipe_service(void);
uint16_t v2k_cpu2_board_pipe_read_octet(uint16_t *octet);
uint16_t v2k_cpu2_board_pipe_can_write(void);
void v2k_cpu2_board_pipe_write_octet(uint16_t octet);
void v2k_cpu2_board_delay_us(uint16_t delay_us);
void v2k_cpu2_board_toggle_status_led(void);
void v2k_cpu2_board_panic_halt(void);

#endif // V2K_CPU2_BOARD_H
