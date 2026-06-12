//=============================================================================
// v2k_planes.h — 四个共享内存接口在物理 RAM 区块上的聚合落位（Phase 1 起用）
//
// contracts/v2k_memmap.h 规定"哪个 struct 落在哪个区块"；本文件把每个区块
// 的全部内容聚合成单个 struct——每个链接 section 内只有一个对象，因此
// 对象基址 == 区块基址可以保证（运行期 v2k_assert_layout 自检兜底）。
// 聚合内的成员顺序即区块内布局，三方（CPU1/CPU2/CCS 脚本）共同依赖，
// 调整顺序 = 契约变更，必须 V2K_CONTRACT_VER +1。
//
// 访问规则（GSx 硬件写保护：属主核读写，对侧核只读）：
//   属主核 : 定义实体，#pragma DATA_SECTION 钉进 v2k_memmap.h 的命名 section
//   对侧核 : 经下方 V2K_*_RO 只读 volatile 指针访问（地址 = memmap 基址）
//=============================================================================
#ifndef V2K_PLANES_H
#define V2K_PLANES_H

#include "../contracts/v2k_memmap.h"
#include "../contracts/v2k_descriptor.h"
#include "../contracts/v2k_param.h"
#include "../contracts/v2k_scope.h"
#include "../contracts/v2k_command.h"

//-----------------------------------------------------------------------------
// GS0 平面（CPU1 属主，section V2K_SECT_DESC_TABLE = "v2k_gs0_cpu1"）
//-----------------------------------------------------------------------------
typedef struct {
    v2k_desc_table_t   desc_table;    // 描述符表（magic 发布协议见 v2k_descriptor.h）
    v2k_param_status_t param_status;  // 参数应用状态 + 值镜像
    v2k_scope_prod_t   scope_prod[V2K_SCOPE_MAX_GROUPS]; // 示波生产者控制块
} v2k_gs0_plane_t;

//-----------------------------------------------------------------------------
// GS4 平面（CPU2 属主，section V2K_SECT_CPU2_PLANE = "v2k_gs4_cpu2"）
//-----------------------------------------------------------------------------
typedef struct {
    v2k_param_shadow_t param_shadow;  // 参数双缓冲 shadow 区
    v2k_scope_cfg_t    scope_cfg[V2K_SCOPE_MAX_GROUPS];  // 示波配置请求
    v2k_scope_bind_t   scope_bind[V2K_SCOPE_MAX_GROUPS]; // 通道绑定请求
    v2k_scope_cons_t   scope_cons[V2K_SCOPE_MAX_GROUPS]; // 消费者读索引
} v2k_gs4_plane_t;

//-----------------------------------------------------------------------------
// MSGRAM（硬件单向写权限，天然单写者）
//-----------------------------------------------------------------------------
typedef struct {
    v2k_cpu1_status_t  cpu1_status;   // CPU1 状态 + 心跳
} v2k_msg_1to2_t;

typedef struct {
    v2k_cmd_req_t      cmd_req;       // 状态机命令请求
    v2k_cpu2_status_t  cpu2_status;   // CPU2 状态 + 心跳
} v2k_msg_2to1_t;

#if V2K_PLATFORM_C28X
// 区块占用自检（单位 word = 16 bit；与 v2k_memmap.h 头部核算表一致）
V2K_STATIC_ASSERT(sizeof(v2k_gs0_plane_t) == 2136u);
V2K_STATIC_ASSERT(sizeof(v2k_gs4_plane_t) == 276u);
V2K_STATIC_ASSERT(sizeof(v2k_gs0_plane_t) <= V2K_GSX_WORDS);
V2K_STATIC_ASSERT(sizeof(v2k_gs4_plane_t) <= 0x200u);  // ≤ RAMGS4_V2K 子区（cpu2 .cmd）
V2K_STATIC_ASSERT(sizeof(v2k_msg_1to2_t)  <= V2K_MSGRAM_V2K_WORDS);
V2K_STATIC_ASSERT(sizeof(v2k_msg_2to1_t)  <= V2K_MSGRAM_V2K_WORDS);
#endif

//-----------------------------------------------------------------------------
// 对侧核只读访问指针（属主核请直接用自己链接出的实体符号，便于 CCS 观测）
//-----------------------------------------------------------------------------
#define V2K_GS0_RO      ((const volatile v2k_gs0_plane_t *)V2K_GS0_BASE)
#define V2K_GS4_RO      ((const volatile v2k_gs4_plane_t *)V2K_GS4_BASE)
#define V2K_MSG_1TO2_RO ((const volatile v2k_msg_1to2_t *)V2K_MSGRAM_1TO2_BASE)
#define V2K_MSG_2TO1_RO ((const volatile v2k_msg_2to1_t *)V2K_MSGRAM_2TO1_BASE)

#endif // V2K_PLANES_H
