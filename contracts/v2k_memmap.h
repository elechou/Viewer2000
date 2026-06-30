//=============================================================================
// v2k_memmap.h - logical shared-memory layout contract
//
// This header defines the portable allocation contract: which shared interface
// object is linked into which named section, and how much logical capacity each
// plane reserves. Concrete device base addresses live in the selected target
// memory-map header. A downstream build may override V2K_TARGET_MEMMAP_HEADER
// to point at its private profile header without adding that target to mainline.
//
// Ownership rule: writer = owning core. Target shared-RAM write protection
// turns the single-writer convention into an enforced property where available;
// MSGRAM is naturally directional.
//
// Logical allocation:
//
//   CPU1-owned front plane:
//     v2k_catalog_shared_t
//     v2k_param_status_t
//     v2k_param_read_resp_t
//     v2k_scope_prod_t
//     v2k_firmware_info_t
//
//   CPU1-owned scope ring:
//     shared Stream/Capture scope ring
//
//   CPU2-owned front plane:
//     v2k_catalog_req_t
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

#ifndef V2K_TARGET_MEMMAP_HEADER
#define V2K_TARGET_MEMMAP_HEADER "../cpu1/board/v2k_board_memmap.h"
#endif

#include "v2k_common.h"
#include V2K_TARGET_MEMMAP_HEADER

#ifndef V2K_CPU1_PLANE_BASE
#error "Target memmap must define V2K_CPU1_PLANE_BASE"
#endif
#ifndef V2K_CPU2_PLANE_BASE
#error "Target memmap must define V2K_CPU2_PLANE_BASE"
#endif
#ifndef V2K_SCOPE_RING_BASE
#error "Target memmap must define V2K_SCOPE_RING_BASE"
#endif
#ifndef V2K_MSGRAM_1TO2_BASE
#error "Target memmap must define V2K_MSGRAM_1TO2_BASE"
#endif
#ifndef V2K_MSGRAM_2TO1_BASE
#error "Target memmap must define V2K_MSGRAM_2TO1_BASE"
#endif
#ifndef V2K_MSGRAM_WORDS
#error "Target memmap must define V2K_MSGRAM_WORDS"
#endif
#ifndef V2K_MCU_MODEL
#error "Target memmap must define V2K_MCU_MODEL"
#endif

// Logical plane sizes, in C28x 16-bit words. A downstream target header may
// override these before this header supplies the public-profile defaults.
#ifndef V2K_CPU1_PLANE_WORDS
#define V2K_CPU1_PLANE_WORDS  0x1000uL
#endif
#ifndef V2K_CPU2_PLANE_WORDS
#define V2K_CPU2_PLANE_WORDS  0x0200uL
#endif
#ifndef V2K_SCOPE_RING_WORDS
#define V2K_SCOPE_RING_WORDS  0x7000uL
#endif

// v2k owns the header slice of each MSGRAM direction. The remaining words stay
// available to TI driverlib's IPC message queues, whose ipc.obj is pinned to the
// conventional MSGRAM_CPU1_TO_CPU2/MSGRAM_CPU2_TO_CPU1 section names.
#ifndef V2K_MSGRAM_V2K_WORDS
#define V2K_MSGRAM_V2K_WORDS 0x40uL
#endif

// Shared object -> linker section names. Both CPU linker command files map
// these sections to the selected physical memory regions.
#define V2K_SECT_CPU1_PLANE   "v2k_cpu1_plane"
#define V2K_SECT_SCOPE_RING   "v2k_scope_ring"
#define V2K_SECT_CPU2_PLANE   "v2k_cpu2_plane"
#define V2K_SECT_MSG_1TO2     "v2k_msg_1to2"
#define V2K_SECT_MSG_2TO1     "v2k_msg_2to1"

#endif // V2K_MEMMAP_H
