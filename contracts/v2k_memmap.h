//=============================================================================
// v2k_memmap.h — 共享内存归属分配草案（各接口结构的物理落位）
//
// 物理资源（来自 28p65x_generic_*_lnk 链接脚本，已核实）：
//   RAMGS0..GS4 : 各 0x2000 words（16KB），两核同地址可见，
//                 每块可配置属主核（属主核读写，对侧核只读）
//   MSGRAM      : CPU1→CPU2 @0x3A000、CPU2→CPU1 @0x3B000，各 0x400 words，
//                 硬件单向写权限（天然单写者）
//   RAMD2..D5   : 各 0x2000 words，可整块划给任一核（注意 CPU2 侧地址不同），
//                 暂全留 CPU1（控制核代码/数据），将来可扩 snapshot 深度
//
// 分配原则：写者 = 属主核（GSx 硬件写保护把单写者约定变成硬性限制）。
//
// ┌────────┬──────┬─────────────────────────────────────────────┐
// │ 区块    │ 属主  │ 内容                                         │
// ├────────┼──────┼─────────────────────────────────────────────┤
// │ GS0    │ CPU1 │ v2k_desc_table_t（描述符表）   (1928 words)   │
// │        │      │ v2k_param_status_t            ( 134 words)   │
// │        │      │ v2k_scope_prod_t ×4           (  64 words)   │
// │        │      │ → 合计 ~2.1K / 8K words，余量大               │
// │ GS1-3  │ CPU1 │ 示波环数据区 24K words（分配见下）              │
// │ GS4    │ CPU2 │ v2k_param_shadow_t            (  68 words)   │
// │        │      │ v2k_scope_cfg_t ×4            (  32 words)   │
// │        │      │ v2k_scope_cons_t ×4           (   8 words)   │
// │        │      │ 余量 ~7.9K words 留 CPU2（EtherCAT 缓冲等）    │
// │ MSGRAM │ 硬件  │ 1→2: v2k_cpu1_status_t                       │
// │        │ 单向  │ 2→1: v2k_cmd_req_t + v2k_cpu2_status_t       │
// └────────┴──────┴─────────────────────────────────────────────┘
//
// 环深度核算（快组锚点）：
//   EtherCAT 档 N=50×8ch：block=406 words → GS1-3 容 60 块；
//   容量取 2 的幂 = 32 块（13K words）= 1600 tick = 16 ms @100kHz 抖动吸收；
//   余 11K words 给慢组环 + 将来加深（再不够时启用 RAMD2-5，接口不变）。
//   SCI 档 N=10×8ch：block=86 words，容量 64 块绰绰有余。
//
// Phase 1 落地方式：两核各自 .cmd 按本文件划分 GSx 归属与 SECTION；
// 共享结构体用 #pragma DATA_SECTION 钉进下列命名 section，地址由链接器分配，
// 但 section→物理块的映射以本文件为准（修改必须双 .cmd 同步 + 本文件同步）。
//=============================================================================
#ifndef V2K_MEMMAP_H
#define V2K_MEMMAP_H

//-----------------------------------------------------------------------------
// 物理基址（word 地址；两核同视角，RAMD 除外）
//-----------------------------------------------------------------------------
#define V2K_GS0_BASE   0x010000uL
#define V2K_GS1_BASE   0x012000uL
#define V2K_GS2_BASE   0x014000uL
#define V2K_GS3_BASE   0x016000uL
#define V2K_GS4_BASE   0x018000uL
#define V2K_GSX_WORDS  0x2000uL

#define V2K_MSGRAM_1TO2_BASE 0x03A000uL
#define V2K_MSGRAM_2TO1_BASE 0x03B000uL
#define V2K_MSGRAM_WORDS     0x400uL

//-----------------------------------------------------------------------------
// 共享结构体 → 链接 section 名（Phase 1 在两核 .cmd 中建立同名 SECTION 映射）
//-----------------------------------------------------------------------------
#define V2K_SECT_DESC_TABLE   "v2k_gs0_cpu1"    /* GS0: 描述符表+参数状态+示波生产块 */
#define V2K_SECT_SCOPE_RING   "v2k_gs13_ring"   /* GS1-3: 环数据区 */
#define V2K_SECT_CPU2_PLANE   "v2k_gs4_cpu2"    /* GS4: 参数 shadow+示波 cfg/cons */
#define V2K_SECT_MSG_1TO2     "MSGRAM_CPU1_TO_CPU2"
#define V2K_SECT_MSG_2TO1     "MSGRAM_CPU2_TO_CPU1"

//-----------------------------------------------------------------------------
// 单核调试开关（基本规则 3：所有核间接口可单核运行）
// 定义 V2K_SINGLE_CORE 后：消费端代码编译进 CPU1 后台循环，GSx 归属全划 CPU1，
// MSGRAM 两个方向退化为 CPU1 普通 RAM 中的两个 struct。接口布局不变。
//-----------------------------------------------------------------------------
// #define V2K_SINGLE_CORE 1

#endif // V2K_MEMMAP_H
