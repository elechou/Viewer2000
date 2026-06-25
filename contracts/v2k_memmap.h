//=============================================================================
// v2k_memmap.h - logical shared-memory layout contract
//
// This header defines the portable allocation contract: which shared interface
// object is linked into which named section, and how much logical capacity each
// plane reserves. Concrete device base addresses live in the selected physical
// memory-map header. Session 1 still selects the F28P65x map directly here; a
// later board-profile build selector should choose the physical map instead.
//
// Ownership rule: writer = owning core. GSx hardware write protection turns the
// single-writer convention into an enforced property; MSGRAM is naturally
// directional.
//
// Logical allocation:
//
//   GS0 front half, CPU1-owned:
//     v2k_desc_table_t
//     v2k_param_status_t
//     v2k_param_read_resp_t
//     v2k_scope_prod_t
//     v2k_firmware_info_t
//
//   GS0 back half + GS1..GS3, CPU1-owned:
//     shared Stream/Capture scope ring
//
//   GS4 front slice, CPU2-owned:
//     v2k_param_shadow_t
//     v2k_param_read_req_t
//     v2k_scope_cfg_t
//     v2k_scope_bind_t
//     v2k_scope_cons_t
//
//   MSGRAM:
//     CPU1->CPU2: v2k_cpu1_status_t
//     CPU2->CPU1: v2k_cmd_req_t + v2k_cpu2_status_t
//
// Scope ring sizing, native sample width, f32 8ch = 16 words/tick:
//   28K words ~= 1792 ticks of jitter absorption
//   89.6 ms @20 kHz / 17.9 ms @100 kHz
//   N=50 f32 8ch block = 808 words, rounded capacity = 32 blocks.
//=============================================================================
#ifndef V2K_MEMMAP_H
#define V2K_MEMMAP_H

// Logical plane sizes, in C28x 16-bit words.
#define V2K_GS0_PLANE_WORDS   0x1000uL
#define V2K_SCOPE_RING_WORDS  0x7000uL

// v2k owns the header slice of each MSGRAM direction. The remaining words stay
// available to TI driverlib's IPC message queues, whose ipc.obj is pinned to the
// conventional MSGRAM_CPU1_TO_CPU2/MSGRAM_CPU2_TO_CPU1 section names.
#define V2K_MSGRAM_V2K_WORDS 0x40uL

// Shared object -> linker section names. Both CPU linker command files map
// these sections to the selected physical memory regions.
#define V2K_SECT_DESC_TABLE   "v2k_gs0_cpu1"
#define V2K_SECT_SCOPE_RING   "v2k_scope_ring"
#define V2K_SECT_CPU2_PLANE   "v2k_gs4_cpu2"
#define V2K_SECT_MSG_1TO2     "v2k_msg_1to2"
#define V2K_SECT_MSG_2TO1     "v2k_msg_2to1"

#include "v2k_memmap_f28p65x.h"

#endif // V2K_MEMMAP_H
