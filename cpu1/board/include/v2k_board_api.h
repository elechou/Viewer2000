//=============================================================================
// v2k_board_api.h - CPU1 board-profile API version and capability bits
//=============================================================================
#ifndef V2K_BOARD_API_H
#define V2K_BOARD_API_H

#include <stdint.h>

#define V2K_BOARD_API_VERSION 2u

// Every CPU1 profile publishes its control-time limits through the selected
// profile header. Runtime derives scheduling and profiling constants from
// these values and contains no target clock literal.

#define V2K_BOARD_CPU_TOPOLOGY_DUAL_C28X 0x0001u

#define V2K_BOARD_POWER_CAP_THREE_PHASE_PWM        0x0001u
#define V2K_BOARD_POWER_CAP_CURRENT_TRIP           0x0002u
#define V2K_BOARD_POWER_CAP_EXTERNAL_GATE_FAULT    0x0004u
#define V2K_BOARD_POWER_CAP_GATE_FAULT_CLEAR       0x0008u
#define V2K_BOARD_POWER_CAP_GATE_CONFIG_TRANSACTION 0x0010u
#define V2K_BOARD_POWER_CAP_DRY_RUN_ONLY           0x8000u

#define V2K_BOARD_SENSOR_CAP_NONE   0x0000u
#define V2K_BOARD_SENSOR_CAP_AS5600 0x0001u

#endif // V2K_BOARD_API_H
