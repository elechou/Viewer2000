MEMORY
{
   /* BEGIN is used for the "boot to SARAM" bootloader mode   */
   BEGIN            : origin = 0x000000, length = 0x000002
   BOOT_RSVD        : origin = 0x000002, length = 0x0001AF     /* Part of M0, BOOT rom will use this for stack */
   RAMM0            : origin = 0x0001B1, length = 0x00024F
   RAMM1            : origin = 0x000400, length = 0x000400

   // RAMD2            : origin = 0x008000, length = 0x002000  // Can be mapped to either CPU1 or CPU2. When configured to CPU1, use the address 0x01A000. User should comment/uncomment based on core selection
   // RAMD3            : origin = 0x00A000, length = 0x002000  // Can be mapped to either CPU1 or CPU2. When configured to CPU1, use the address 0x01C000. User should comment/uncomment based on core selection
   // RAMD4            : origin = 0x00C000, length = 0x002000  // Can be mapped to either CPU1 or CPU2. When configured to CPU1, use the address 0x01E000. User should comment/uncomment based on core selection
   // RAMD5            : origin = 0x00E000, length = 0x002000  // Can be mapped to either CPU1 or CPU2. When configured to CPU1, use the address 0x020000. User should comment/uncomment based on core selection

   RAMGS0           : origin = 0x010000, length = 0x002000
   RAMGS1           : origin = 0x012000, length = 0x002000
   RAMGS2           : origin = 0x014000, length = 0x002000
   RAMGS3           : origin = 0x016000, length = 0x002000
   /* GS4 归 CPU2（CPU1 启动时 MemCfg 划转，见 v2k_memmap.h）。头部 0x200 words
      切给 v2k GS4 平面（参数 shadow + 示波 cfg/bind/cons），余量给 CPU2 代码/数据。
      修改划分须与 contracts/v2k_memmap.h 及 FLASH .cmd 同步。 */
   RAMGS4_V2K       : origin = 0x018000, length = 0x000200
   RAMGS4           : origin = 0x018200, length = 0x001E00

   /* Flash Banks (128 sectors each) */
   // FLASH_BANK0     : origin = 0x080000, length = 0x20000  // Can be mapped to either CPU1 or CPU2. User should comment/uncomment based on core selection
   // FLASH_BANK1     : origin = 0x0A0000, length = 0x20000  // Can be mapped to either CPU1 or CPU2. User should comment/uncomment based on core selection
   // FLASH_BANK2     : origin = 0x0C0000, length = 0x20000  // Can be mapped to either CPU1 or CPU2. User should comment/uncomment based on core selection
   // FLASH_BANK3     : origin = 0x0E0000, length = 0x20000  // Can be mapped to either CPU1 or CPU2. User should comment/uncomment based on core selection
   // FLASH_BANK4     : origin = 0x100000, length = 0x20000  // Can be mapped to either CPU1 or CPU2. User should comment/uncomment based on core selection



   /* MSGRAM 区头 0x40 words 切给 v2k（基址=契约值,见 contracts/v2k_memmap.h）,
      余量给 TI driverlib 的 IPC 消息队列缓冲（ipc.obj 钉死在 MSGRAM_* section 名上） */
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
   .text            : > RAMGS4
   .cinit           : > RAMM0
   .switch          : > RAMM0
   .reset           : > RESET, TYPE = DSECT /* not used, */

   .stack           : > RAMM1
#if defined(__TI_EABI__)
   .bss             : > RAMGS4
   .bss:output      : > RAMGS4
   .init_array      : > RAMM0
   .const           : > RAMGS4
   .data            : > RAMGS4
   .sysmem          : > RAMGS4
#else
   .pinit           : > RAMM0
   .ebss            : > RAMGS4
   .econst          : > RAMGS4
   .esysmem         : > RAMGS4
#endif

   /* Viewer2000 GS4 平面：section 内只有一个聚合对象（common/v2k_planes.h），
      对象基址 == 0x018000 == V2K_GS4_BASE；运行期 v2k_assert_layout 自检兜底 */
   v2k_gs4_cpu2 : > RAMGS4_V2K, type=NOINIT

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
