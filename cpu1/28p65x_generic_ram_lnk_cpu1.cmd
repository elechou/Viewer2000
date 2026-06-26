/*
 * Deprecated RAM build linker command file.
 *
 * Viewer2000's supported firmware build configuration is FLASH from Phase 5.0
 * onward. This file remains only so historical Phase 1-4 bring-up records and
 * old debug notes can still be interpreted. Do not use it as a capacity target,
 * powered-commissioning artifact, or current acceptance build.
 */

MEMORY
{
   /* BEGIN is used for the "boot to SARAM" bootloader mode   */
   BEGIN            : origin = 0x000000, length = 0x000002
   BOOT_RSVD        : origin = 0x000002, length = 0x0001AF     /* Part of M0, BOOT rom will use this for stack */
   RAMM0            : origin = 0x0001B1, length = 0x00024F
   RAMM1            : origin = 0x000400, length = 0x000400

   RAMD0            : origin = 0x00C000, length = 0x002000
   RAMD1            : origin = 0x00E000, length = 0x002000
   RAMD2            : origin = 0x01A000, length = 0x002000  // Can be mapped to either CPU1 or CPU2. When configured to CPU2, use the address 0x8000. User should comment/uncomment based on core selection
   RAMD3            : origin = 0x01C000, length = 0x002000  // Can be mapped to either CPU1 or CPU2. When configured to CPU2, use the address 0xA000. User should comment/uncomment based on core selection
   RAMD4            : origin = 0x01E000, length = 0x002000  // Can be mapped to either CPU1 or CPU2. When configured to CPU2, use the address 0xC000. User should comment/uncomment based on core selection
   USER_GOLDEN_RAM  : origin = 0x020000, length = 0x000800  // Phase 4.1 initialized-user-state golden image; CPU1-owned and non-adjacent to USER_RUN
   RAMD5_FREE       : origin = 0x020800, length = 0x001800  // Remaining CPU1-owned D5 capacity

   RAMLS0           : origin = 0x008000, length = 0x000800
   RAMLS1           : origin = 0x008800, length = 0x000800
   RAMLS2           : origin = 0x009000, length = 0x000800
   RAMLS3           : origin = 0x009800, length = 0x000800
   RAMLS4           : origin = 0x00A000, length = 0x000800
   RAMLS5           : origin = 0x00A800, length = 0x000800
   USER_RUN         : origin = 0x00B000, length = 0x000800  // Combined initialized data + BSS for user-owned objects
   USER_CONST_RAM   : origin = 0x00B800, length = 0x000800  // Constrained RAM-build storage for immutable user objects
   RAMLS8           : origin = 0x022000, length = 0x002000  // When configured as CLA program use the address 0x4000
   RAMLS9           : origin = 0x024000, length = 0x002000  // When configured as CLA program use the address 0x6000

   // RAMLS8_CLA    : origin = 0x004000, length = 0x002000  // Use only if configured as CLA program memory
   // RAMLS9_CLA    : origin = 0x006000, length = 0x002000  // Use only if configured as CLA program memory

   RAMGS0_PLANE     : origin = 0x010000, length = 0x001000
   RAMGS_SCOPE      : origin = 0x011000, length = 0x007000
   RAMGS4           : origin = 0x018000, length = 0x002000

   /* Flash Banks (128 sectors each) */
   FLASH_BANK0     : origin = 0x080000, length = 0x20000  // Can be mapped to either CPU1 or CPU2. User should comment/uncomment based on core selection
   FLASH_BANK1     : origin = 0x0A0000, length = 0x20000  // Can be mapped to either CPU1 or CPU2. User should comment/uncomment based on core selection
   FLASH_BANK2     : origin = 0x0C0000, length = 0x20000  // Can be mapped to either CPU1 or CPU2. User should comment/uncomment based on core selection
   FLASH_BANK3     : origin = 0x0E0000, length = 0x20000  // Can be mapped to either CPU1 or CPU2. User should comment/uncomment based on core selection
   FLASH_BANK4     : origin = 0x100000, length = 0x20000  // Can be mapped to either CPU1 or CPU2. User should comment/uncomment based on core selection



   /* The first 0x40 words of the MSGRAM region go to v2k (base = contract value,
      see contracts/v2k_memmap.h); the remainder goes to TI driverlib's IPC
      message-queue buffers (ipc.obj is pinned to the MSGRAM_* section names) */
   CPU1TOCPU2RAM_V2K : origin = 0x03A000, length = 0x000040
   CPU1TOCPU2RAM     : origin = 0x03A040, length = 0x0003C0
   CPU2TOCPU1RAM_V2K : origin = 0x03B000, length = 0x000040
   CPU2TOCPU1RAM     : origin = 0x03B040, length = 0x0003C0

   CLATOCPURAM      : origin = 0x001480,   length = 0x000080
   CPUTOCLARAM      : origin = 0x001500,   length = 0x000080
   CLATODMARAM      : origin = 0x001680,   length = 0x000080
   DMATOCLARAM      : origin = 0x001700,   length = 0x000080

   CANA_MSG_RAM     : origin = 0x049000, length = 0x000800
   CANB_MSG_RAM     : origin = 0x04B000, length = 0x000800
   RESET            : origin = 0x3FFFC0, length = 0x000002
}


SECTIONS
{
   codestart        : > BEGIN
   .text            : >> RAMD0 | RAMD1 | RAMLS0 | RAMLS1 | RAMLS2 | RAMLS3
   .cinit           : > RAMM0
   .switch          : > RAMM0
   .reset           : > RESET, TYPE = DSECT /* not used, */

   .stack           : > RAMM1
#if defined(__TI_EABI__)
   .bss             : > RAMLS5, START(V2K_BssStart), END(V2K_BssEnd)
   .bss:output      : > RAMLS3, START(V2K_BssOutputStart), END(V2K_BssOutputEnd)
   .init_array      : > RAMM0
	   /* Keep assertion/DriverLib string tables out of the contiguous writable
	      state range; Phase 5.0 no longer fits .const + .bss in RAMLS5. */
	   .const           : > RAMLS4
	   .data            : > RAMLS5, START(V2K_DataStart), END(V2K_DataEnd)
	   .sysmem          : > RAMLS5
	   .TI.crctab       : > RAMM0, ALIGN(2)
	   dclfuncs         : > RAMM0
#else
	   .pinit           : > RAMM0
	   .ebss            : > RAMLS5
   .econst          : > RAMLS5
   .esysmem         : > RAMLS5
#endif

   /* Viewer2000 shared-memory planes. Layout reference = contracts/v2k_memmap.h;
      any change must stay in sync across both cores' .cmd files (4 total,
      RAM+FLASH) and the memmap header. Each section holds exactly one aggregate
      object (common/v2k_planes.h), so object base == region base; the runtime
      v2k_assert_layout self-check backstops it. */
   v2k_cpu1_plane   : > RAMGS0_PLANE, type=NOINIT /* descriptor table + param status + scope producer block */
   v2k_scope_ring : > RAMGS_SCOPE, type=NOINIT  /* Stream/Capture shared scope ring */
   v2k_ccs_view    : > RAMD2, type=NOINIT        /* post-freeze float[2048] de-interleaved view */
   v2k_user_desc   : > RAMD5_FREE, ALIGN(2)      /* Phase 4.5 post-link baked user descriptor blob */

   v2k_msg_1to2 : > CPU1TOCPU2RAM_V2K, type=NOINIT
   v2k_msg_2to1 : > CPU2TOCPU1RAM_V2K, type=NOINIT

   MSGRAM_CPU1_TO_CPU2 > CPU1TOCPU2RAM, type=NOINIT
   MSGRAM_CPU2_TO_CPU1 > CPU2TOCPU1RAM, type=NOINIT

    .TI.ramfunc : {} > RAMM0

}

/*
//===========================================================================
// End of file.
//===========================================================================
*/
