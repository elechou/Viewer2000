//=============================================================================
// v2k_memmap_f28p65x.h - F28P65x physical shared-memory map
//
// Word addresses match the current 28p65x_generic_* linker command files.
// Keep this file synchronized with both CPU linker scripts and with the runtime
// layout assertions.
//=============================================================================
#ifndef V2K_MEMMAP_F28P65X_H
#define V2K_MEMMAP_F28P65X_H

#define V2K_GS0_BASE   0x010000uL
#define V2K_GS1_BASE   0x012000uL
#define V2K_GS2_BASE   0x014000uL
#define V2K_GS3_BASE   0x016000uL
#define V2K_GS4_BASE   0x018000uL
#define V2K_GSX_WORDS  0x2000uL

#define V2K_GS0_PLANE_BASE    V2K_GS0_BASE
#define V2K_SCOPE_RING_BASE   0x011000uL

#define V2K_MSGRAM_1TO2_BASE 0x03A000uL
#define V2K_MSGRAM_2TO1_BASE 0x03B000uL
#define V2K_MSGRAM_WORDS     0x400uL

#endif // V2K_MEMMAP_F28P65X_H
