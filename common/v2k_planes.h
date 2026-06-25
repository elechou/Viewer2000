//=============================================================================
// v2k_planes.h - aggregate placement for the four shared-memory interfaces
//
// contracts/v2k_memmap.h defines which aggregate belongs in which named
// section. This file groups each plane into exactly one object, so the object
// base equals the mapped region base; runtime layout assertions backstop the
// linker scripts. Member order is the plane layout and is shared by CPU1, CPU2,
// and debug scripts. Reordering members is a contract change and requires
// V2K_CONTRACT_VER +1.
//
// Access rule:
//   owner core: defines the entity and pins it to a v2k_memmap.h section
//   peer core : reads through the volatile V2K_*_RO pointer at the target base
//=============================================================================
#ifndef V2K_PLANES_H
#define V2K_PLANES_H

#include "../contracts/v2k_memmap.h"
#include "../contracts/v2k_descriptor.h"
#include "../contracts/v2k_param.h"
#include "../contracts/v2k_scope.h"
#include "../contracts/v2k_command.h"

//-----------------------------------------------------------------------------
// CPU1-owned plane (section V2K_SECT_CPU1_PLANE)
//-----------------------------------------------------------------------------
typedef struct {
    v2k_desc_table_t   desc_table;    // Descriptor table; see v2k_descriptor.h for publish protocol.
    v2k_param_status_t param_status;  // Parameter apply status.
    v2k_param_read_resp_t param_read_resp; // CAL_READ on-demand read response.
    v2k_scope_prod_t   scope_prod;    // Scope producer control block.
    v2k_firmware_info_t firmware_info; // CPU1-generated project metadata forwarded by CPU2 HELLO.
} v2k_cpu1_plane_t;

//-----------------------------------------------------------------------------
// CPU2-owned plane (section V2K_SECT_CPU2_PLANE)
//-----------------------------------------------------------------------------
typedef struct {
    v2k_param_shadow_t param_shadow;  // Parameter double-buffer shadow region.
    v2k_param_read_req_t param_read_req; // CAL_READ on-demand read request.
    v2k_scope_cfg_t    scope_cfg;     // Scope configuration request.
    v2k_scope_bind_t   scope_bind;    // Channel binding request.
    v2k_scope_cons_t   scope_cons;    // Consumer read index.
} v2k_cpu2_plane_t;

//-----------------------------------------------------------------------------
// MSGRAM (hardware-directional write ownership, naturally single-writer)
//-----------------------------------------------------------------------------
typedef struct {
    v2k_cpu1_status_t  cpu1_status;   // CPU1 status + heartbeat.
} v2k_msg_1to2_t;

typedef struct {
    v2k_cmd_req_t      cmd_req;       // State-machine command request.
    v2k_cpu2_status_t  cpu2_status;   // CPU2 status + heartbeat.
} v2k_msg_2to1_t;

#if V2K_PLATFORM_C28X
// Plane occupancy checks. Unit = C28x word = 16 bits.
V2K_STATIC_ASSERT(sizeof(v2k_cpu1_plane_t) == 4006u);
V2K_STATIC_ASSERT(sizeof(v2k_cpu2_plane_t) == 312u);
V2K_STATIC_ASSERT(sizeof(v2k_cpu1_plane_t) <= V2K_CPU1_PLANE_WORDS);
V2K_STATIC_ASSERT(sizeof(v2k_cpu2_plane_t) <= V2K_CPU2_PLANE_WORDS);
V2K_STATIC_ASSERT(V2K_MSGRAM_V2K_WORDS <= V2K_MSGRAM_WORDS);
V2K_STATIC_ASSERT(sizeof(v2k_msg_1to2_t)  <= V2K_MSGRAM_V2K_WORDS);
V2K_STATIC_ASSERT(sizeof(v2k_msg_2to1_t)  <= V2K_MSGRAM_V2K_WORDS);
#endif

//-----------------------------------------------------------------------------
// Peer-core read-only access pointers. Owner cores use their linked entity
// symbols directly so CCS can observe them by name.
//-----------------------------------------------------------------------------
#define V2K_CPU1_PLANE_RO ((const volatile v2k_cpu1_plane_t *)V2K_CPU1_PLANE_BASE)
#define V2K_CPU2_PLANE_RO ((const volatile v2k_cpu2_plane_t *)V2K_CPU2_PLANE_BASE)
#define V2K_MSG_1TO2_RO ((const volatile v2k_msg_1to2_t *)V2K_MSGRAM_1TO2_BASE)
#define V2K_MSG_2TO1_RO ((const volatile v2k_msg_2to1_t *)V2K_MSGRAM_2TO1_BASE)

#endif // V2K_PLANES_H
