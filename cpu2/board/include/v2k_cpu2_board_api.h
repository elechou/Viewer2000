//=============================================================================
// v2k_cpu2_board_api.h - CPU2 board-profile API version and capability bits
//=============================================================================
#ifndef V2K_CPU2_BOARD_API_H
#define V2K_CPU2_BOARD_API_H

#include <stdint.h>

#define V2K_CPU2_BOARD_API_VERSION 1u

#define V2K_CPU2_BOARD_CPU_TOPOLOGY_DUAL_C28X 0x0001u

#define V2K_CPU2_BOARD_PIPE_CAP_SCI_XDS110 0x0001u
#define V2K_CPU2_BOARD_PIPE_CAP_LOOPBACK   0x0002u
#define V2K_CPU2_BOARD_PIPE_CAP_ETHERCAT   0x0004u

#endif // V2K_CPU2_BOARD_API_H
