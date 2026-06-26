//=============================================================================
// v2k_cpu2_board_profile.h - LAUNCHXL-F28P65X XDS110 SCI CPU2 profile
//=============================================================================
#ifndef V2K_CPU2_BOARD_PROFILE_F28P65X_LAUNCHXL_SCI_H
#define V2K_CPU2_BOARD_PROFILE_F28P65X_LAUNCHXL_SCI_H

#include "../../include/v2k_cpu2_board_api.h"

#define V2K_CPU2_BOARD_PROFILE_API_VERSION V2K_CPU2_BOARD_API_VERSION
#define V2K_CPU2_BOARD_PROFILE_ID_TEXT "f28p65x_launchxl_sci"
#define V2K_CPU2_BOARD_PROFILE_CPU_TOPOLOGY \
    V2K_CPU2_BOARD_CPU_TOPOLOGY_DUAL_C28X
#define V2K_CPU2_BOARD_PROFILE_PIPE_CAPS V2K_CPU2_BOARD_PIPE_CAP_SCI_XDS110

#endif // V2K_CPU2_BOARD_PROFILE_F28P65X_LAUNCHXL_SCI_H
