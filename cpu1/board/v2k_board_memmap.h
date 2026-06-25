//=============================================================================
// v2k_board_memmap.h - public F28P65x board-profile shared-memory map
//
// Word addresses match the current 28p65x_generic_* linker command files.
// Keep this file synchronized with both CPU linker scripts and with the runtime
// layout assertions.
//=============================================================================
#ifndef V2K_BOARD_MEMMAP_H
#define V2K_BOARD_MEMMAP_H

#define V2K_F28P65X_RAMGS0_BASE   0x010000uL
#define V2K_F28P65X_RAMGS1_BASE   0x012000uL
#define V2K_F28P65X_RAMGS2_BASE   0x014000uL
#define V2K_F28P65X_RAMGS3_BASE   0x016000uL
#define V2K_F28P65X_RAMGS4_BASE   0x018000uL
#define V2K_F28P65X_RAMGS_WORDS   0x2000uL

#define V2K_CPU1_PLANE_BASE   V2K_F28P65X_RAMGS0_BASE
#define V2K_SCOPE_RING_BASE   0x011000uL
#define V2K_CPU2_PLANE_BASE   V2K_F28P65X_RAMGS4_BASE

#define V2K_MSGRAM_1TO2_BASE 0x03A000uL
#define V2K_MSGRAM_2TO1_BASE 0x03B000uL
#define V2K_MSGRAM_WORDS     0x400uL

#endif // V2K_BOARD_MEMMAP_H
