//=============================================================================
// v2k_board_profile_select.h - selected CPU1 board profile
//=============================================================================
#ifndef V2K_BOARD_PROFILE_SELECT_H
#define V2K_BOARD_PROFILE_SELECT_H

#include "v2k_board_api.h"

#ifndef V2K_BOARD_PROFILE_HEADER
#define V2K_BOARD_PROFILE_HEADER \
    "../profiles/f28p65x_launchxl_drv8323rs_as5600/v2k_board_profile.h"
#endif

#include V2K_BOARD_PROFILE_HEADER

#ifndef V2K_BOARD_PROFILE_API_VERSION
#error "Selected CPU1 board profile must define V2K_BOARD_PROFILE_API_VERSION"
#endif

#if V2K_BOARD_PROFILE_API_VERSION != V2K_BOARD_API_VERSION
#error "Selected CPU1 board profile API version does not match v2k_board.h"
#endif

#ifndef V2K_BOARD_PROFILE_CONTROL_HZ
#error "Selected CPU1 board profile must define V2K_BOARD_PROFILE_CONTROL_HZ"
#endif

#ifndef V2K_BOARD_PROFILE_EPWMCLK_HZ
#error "Selected CPU1 board profile must define V2K_BOARD_PROFILE_EPWMCLK_HZ"
#endif

#ifndef V2K_BOARD_PROFILE_CYCLE_COUNTER_HZ
#error "Selected CPU1 board profile must define V2K_BOARD_PROFILE_CYCLE_COUNTER_HZ"
#endif

#ifndef V2K_BOARD_PROFILE_ID_TEXT
#error "Selected CPU1 board profile must define V2K_BOARD_PROFILE_ID_TEXT"
#endif

#ifndef V2K_BOARD_PROFILE_CPU_TOPOLOGY
#error "Selected CPU1 board profile must define V2K_BOARD_PROFILE_CPU_TOPOLOGY"
#endif

#ifndef V2K_BOARD_PROFILE_POWER_CAPS
#error "Selected CPU1 board profile must define V2K_BOARD_PROFILE_POWER_CAPS"
#endif

#ifndef V2K_BOARD_PROFILE_SENSOR_CAPS
#error "Selected CPU1 board profile must define V2K_BOARD_PROFILE_SENSOR_CAPS"
#endif

#endif // V2K_BOARD_PROFILE_SELECT_H
