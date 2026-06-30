//=============================================================================
// check_contracts.c - PC-side compile check for interface headers
//
// This compiles all interface headers with a CHAR_BIT=8 PC compiler, triggering
// the V2K_ASSERT_SIZE_BITS static assertions in each header. Those assertions
// are expressed in bits, so they are shared by PC and C28x builds. This file
// also asserts key field bit offsets to catch implicit padding that would break
// the "memory layout == on-wire format" mapping.
//
// Usage: gcc -std=c99 -Wall -Wextra -Werror -c check_contracts.c
// The same file is compiled on the cl2000 side, where the assertions exercise
// the C28x CHAR_BIT=16 layout.
//=============================================================================
#include <stddef.h>

#include "../contracts/v2k_common.h"
#include "../contracts/v2k_descriptor.h"
#include "../contracts/v2k_scope.h"
#include "../contracts/v2k_param.h"
#include "../contracts/v2k_command.h"
#include "../contracts/v2k_memmap.h"
#include "../common/v2k_planes.h"

// Bit-offset assertion. offsetof is in char units; multiply by CHAR_BIT to make
// it portable between PC and C28x.
#define V2K_ASSERT_OFFSET_BITS(t, field, bits) \
    V2K_STATIC_ASSERT((uint32_t)offsetof(t, field) * (uint32_t)CHAR_BIT == (bits))

// ---- Aggregate plane sizes. The CPU1 plane contains char[] fields, so PC and
// C28x char width differences prevent a cross-platform total-size assertion. ----
V2K_ASSERT_SIZE_BITS(v2k_cpu2_plane_t, 316u * 16u);

// ---- Compact catalog metadata and staging. ----
V2K_ASSERT_OFFSET_BITS(v2k_desc_entry_t, type,      32u);
V2K_ASSERT_OFFSET_BITS(v2k_desc_entry_t, kind,      48u);
V2K_ASSERT_OFFSET_BITS(v2k_desc_entry_t, prescaler, 64u);
V2K_ASSERT_OFFSET_BITS(v2k_desc_entry_t, reserved,  80u);

V2K_ASSERT_OFFSET_BITS(v2k_catalog_hdr_t, build_hash, 32u);
V2K_ASSERT_OFFSET_BITS(v2k_catalog_hdr_t, total_count, 80u);
V2K_ASSERT_OFFSET_BITS(v2k_catalog_req_t, req_seq, 48u);
V2K_ASSERT_OFFSET_BITS(v2k_catalog_resp_t, payload, 64u);
V2K_ASSERT_OFFSET_BITS(v2k_firmware_info_t, build_time_utc,
                       V2K_NAME_BITS(V2K_PROJECT_NAME_LEN));
V2K_ASSERT_OFFSET_BITS(v2k_user_desc_blob_t, firmware_info, 128u);
V2K_ASSERT_OFFSET_BITS(v2k_user_desc_blob_t, name_pool_octets,
                       128u + V2K_NAME_BITS(V2K_PROJECT_NAME_LEN) + 32u);
V2K_ASSERT_OFFSET_BITS(v2k_user_desc_blob_t, entries,
                       128u + V2K_NAME_BITS(V2K_PROJECT_NAME_LEN) + 96u);
V2K_ASSERT_OFFSET_BITS(v2k_user_desc_entry_t, name_offset, 96u);
V2K_ASSERT_OFFSET_BITS(v2k_user_desc_entry_t, name_len, 128u);

// ---- Scope block header and control blocks. ----
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

// ---- Parameter shadow and status. ----
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

// ---- Command/status plane. ----
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

// Prevent an empty translation-unit warning.
typedef int v2k_check_contracts_nonempty;
