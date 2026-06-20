//=============================================================================
// check_contracts.c — 接口头文件 PC 端编译检查
//
// 目的：在 CHAR_BIT=8 的 PC 编译器上编译全部接口头文件，
// 触发头文件内的 V2K_ASSERT_SIZE_BITS 静态断言（断言以 bit 计，双平台通用），
// 并额外断言关键字段的 bit 偏移（防止隐式填充破坏"内存布局=线上格式"映射）。
//
// 用法: gcc -std=c99 -Wall -Wextra -Werror -c check_contracts.c
// （cl2000 侧同一文件可直接编译，断言同样生效——Phase 1 接入 CCS 工程后验证）
//=============================================================================
#include <stddef.h>

#include "../contracts/v2k_common.h"
#include "../contracts/v2k_descriptor.h"
#include "../contracts/v2k_scope.h"
#include "../contracts/v2k_param.h"
#include "../contracts/v2k_command.h"
#include "../contracts/v2k_memmap.h"
#include "../common/v2k_planes.h"

// bit 偏移断言（offsetof 以 char 计 → ×CHAR_BIT 得 bit，双平台通用）
#define V2K_ASSERT_OFFSET_BITS(t, field, bits) \
    V2K_STATIC_ASSERT((uint32_t)offsetof(t, field) * (uint32_t)CHAR_BIT == (bits))

// ---- 聚合平面尺寸（GS0 含 char[]，PC/C28x char 宽度不同，不能跨平台总尺寸断言）----
V2K_ASSERT_SIZE_BITS(v2k_gs4_plane_t, 312u * 16u);

// ---- 描述符条目（name 之后不得有隐式填充）----
V2K_ASSERT_OFFSET_BITS(v2k_desc_entry_t, type,      V2K_NAME_BITS(V2K_NAME_LEN));
V2K_ASSERT_OFFSET_BITS(v2k_desc_entry_t, addr,      V2K_NAME_BITS(V2K_NAME_LEN) + 32u);
V2K_ASSERT_OFFSET_BITS(v2k_desc_entry_t, prescaler, V2K_NAME_BITS(V2K_NAME_LEN) + 64u);

// ---- 描述符表头 ----
V2K_ASSERT_OFFSET_BITS(v2k_desc_table_hdr_t, build_hash, 64u);
V2K_ASSERT_OFFSET_BITS(v2k_firmware_info_t, build_time_utc,
                       V2K_NAME_BITS(V2K_PROJECT_NAME_LEN));
V2K_ASSERT_OFFSET_BITS(v2k_user_desc_blob_t, firmware_info, 128u);
V2K_ASSERT_OFFSET_BITS(v2k_user_desc_blob_t, entries,
                       128u + V2K_NAME_BITS(V2K_PROJECT_NAME_LEN) + 32u);

// ---- 示波 block 头与控制块 ----
V2K_ASSERT_OFFSET_BITS(v2k_block_hdr_t, block_seq, 32u);
V2K_ASSERT_OFFSET_BITS(v2k_block_hdr_t, n_ch,      80u);
V2K_ASSERT_OFFSET_BITS(v2k_block_hdr_t, stride_octets, 112u);
V2K_ASSERT_OFFSET_BITS(v2k_scope_prod_t, prescaler, 80u);
V2K_ASSERT_OFFSET_BITS(v2k_scope_prod_t, ring_base, 128u);
V2K_ASSERT_OFFSET_BITS(v2k_scope_prod_t, wr_idx,   160u);
V2K_ASSERT_OFFSET_BITS(v2k_scope_prod_t, trig_tick, 192u);
V2K_ASSERT_OFFSET_BITS(v2k_scope_prod_t, bind_ack_seq, 272u);
V2K_ASSERT_OFFSET_BITS(v2k_scope_cfg_t,  trig_level, 32u);
V2K_ASSERT_OFFSET_BITS(v2k_scope_cfg_t,  trig_hysteresis, 64u);
V2K_ASSERT_OFFSET_BITS(v2k_scope_cfg_t,  record_points, 144u);
V2K_ASSERT_OFFSET_BITS(v2k_scope_cfg_t,  cfg_seq, 176u);
V2K_ASSERT_OFFSET_BITS(v2k_scope_ch_bind_t, type,    32u);
V2K_ASSERT_OFFSET_BITS(v2k_scope_bind_t,  ch,        32u);

// ---- 参数 shadow 与状态 ----
V2K_ASSERT_OFFSET_BITS(v2k_param_write_t,  value_bits, 32u);
V2K_ASSERT_OFFSET_BITS(v2k_param_write_t,  type,       64u);
V2K_ASSERT_OFFSET_BITS(v2k_param_read_ref_t, type,      32u);
V2K_ASSERT_OFFSET_BITS(v2k_param_shadow_t, commit_seq, 32u);
V2K_ASSERT_OFFSET_BITS(v2k_param_shadow_t, writes,     64u);
V2K_ASSERT_OFFSET_BITS(v2k_param_read_req_t, read_seq,  32u);
V2K_ASSERT_OFFSET_BITS(v2k_param_read_req_t, refs,      64u);
V2K_ASSERT_OFFSET_BITS(v2k_param_status_t, result,      32u);
V2K_ASSERT_OFFSET_BITS(v2k_param_read_resp_t, value_bits, 32u);
V2K_ASSERT_OFFSET_BITS(v2k_param_read_resp_t, ack_seq,  1056u);

// ---- 命令/状态平面 ----
V2K_ASSERT_OFFSET_BITS(v2k_cmd_req_t,      cmd_code,  32u);
V2K_ASSERT_OFFSET_BITS(v2k_cpu1_status_t,  ack_seq,   32u);
V2K_ASSERT_OFFSET_BITS(v2k_cpu1_status_t,  heartbeat, 128u);
V2K_ASSERT_OFFSET_BITS(v2k_cpu1_status_t,  tick,      160u);
V2K_ASSERT_OFFSET_BITS(v2k_cpu1_status_t,  tick_hz,   192u);
V2K_ASSERT_OFFSET_BITS(v2k_cpu1_status_t,  prof_seq,  224u);
V2K_ASSERT_OFFSET_BITS(v2k_cpu1_status_t,  cycle_budget, 256u);
V2K_ASSERT_OFFSET_BITS(v2k_cpu1_status_t,  load_avg,  288u);
V2K_ASSERT_OFFSET_BITS(v2k_cpu1_status_t,  load_peak, 320u);
V2K_ASSERT_OFFSET_BITS(v2k_cpu1_status_t,  ctrl_at_peak, 352u);
V2K_ASSERT_OFFSET_BITS(v2k_cpu1_status_t,  scope_at_peak, 384u);
V2K_ASSERT_OFFSET_BITS(v2k_cpu1_status_t,  lat_at_peak, 416u);
V2K_ASSERT_OFFSET_BITS(v2k_cpu1_status_t,  peak_tick, 448u);
V2K_ASSERT_OFFSET_BITS(v2k_cpu1_status_t,  budget_violations, 480u);
V2K_ASSERT_OFFSET_BITS(v2k_cpu1_status_t,  isr_overflows, 512u);

// 防"空 TU"告警
typedef int v2k_check_contracts_nonempty;
