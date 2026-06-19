//=============================================================================
// v2k_param.h — shared-memory interface: parameter plane (host → control-core)
//
// Problem solved: both CCS real-time mode and the on-wire link write
// multi-field parameters non-atomically (a C2000-specific trap), so every
// parameter write goes through a shadow double buffer + commit flag, and the
// CPU1 ISR swaps the whole set at a fixed safe point each cycle — a batch of
// parameters either all take effect on the same tick or none do.
//
// Write targets are addressed by address (same philosophy as scope channel
// binding: platform quantities and user application variables both come from
// descriptor-table enumeration; user variables are baked in from DWARF at
// Phase 4.5 build time, and the firmware does not distinguish the source).
//
// ---- Write validation (mechanical consistency, no range/unit semantics) ----
// Before applying each write CPU1 does only mechanical checks:
//   * type is legal;
//   * addr lies in a writable CPU1 data region, with 32-bit types aligned;
//   * on a descriptor-table hit, kind&PARAM must be set and type must match.
// The firmware does no min/max range check, no clamp, no scale/offset inverse.
// The on-wire value is the target variable's true native bit pattern.
//
// Write paths (two, same mechanism):
//   1. On-wire: host CAL_WRITE (staged) ×k → CAL_COMMIT → CPU2 fills shadow + publishes seq
//   2. Debug:   CCS pokes the shadow fields directly, then bumps commit_seq by 1
//
// Apply protocol (CPU1 ISR safe point, before control(); sequence handshake, no cross-domain write flag):
//   if (shadow.commit_seq != status.applied_seq) {
//       mechanically validate each entry (see above); all pass → write targets by type; any illegal → reject the whole batch
//       status.result = result code; status.applied_seq = shadow.commit_seq;  // acknowledge
//   }
//
// Read-back path: on-wire CAL_READ uses the same addressing philosophy as
// CAL_WRITE — the host sends an (addr,type) list, CPU2 publishes it to the read
// request, and a CPU1 background poll point reads it once on demand and
// publishes the read response. Reads never enter the ISR; high-rate,
// timestamped data still goes through the scope ring.
//=============================================================================
#ifndef V2K_PARAM_H
#define V2K_PARAM_H

#include "v2k_common.h"
#include "v2k_descriptor.h"

//-----------------------------------------------------------------------------
// Single parameter write (shadow-region element)
//-----------------------------------------------------------------------------
#define V2K_PARAM_BATCH_MAX 16u   // Max entries per commit
#define V2K_CAL_READ_MAX    32u   // Max entries per CAL_READ

typedef struct {
    uint32_t addr;         // Target CPU1 data-space word address (source: descriptor table, incl. Phase 4.5 build-baked user variables)
    uint32_t value_bits;   // New value bit pattern (convention in v2k_common.h)
    uint16_t type;         // V2K_TYPE_* (determines write width and conversion)
    uint16_t reserved;     // Set to 0
} v2k_param_write_t;

V2K_ASSERT_SIZE_BITS(v2k_param_write_t, 96u);

typedef struct {
    uint32_t addr;         // Target CPU1 data-space word address
    uint16_t type;         // V2K_TYPE_* (determines read width and sign extension)
    uint16_t reserved;     // Set to 0
} v2k_param_read_ref_t;

V2K_ASSERT_SIZE_BITS(v2k_param_read_ref_t, 64u);

//-----------------------------------------------------------------------------
// Shadow region (CPU2-owned: written by CPU2 and CCS; CPU1 reads only and acks
// via status.applied_seq. The publish action = writing commit_seq last; the
// GSx far side is read-only, so there is no dual-writer field)
//-----------------------------------------------------------------------------
typedef struct {
    uint16_t count;        // Entries in this batch ≤ V2K_PARAM_BATCH_MAX
    uint16_t reserved;     // Set to 0
    uint32_t commit_seq;   // The committer bumps this by 1 per commit, written last (publish)
    v2k_param_write_t writes[V2K_PARAM_BATCH_MAX];
} v2k_param_shadow_t;

V2K_ASSERT_SIZE_BITS(v2k_param_shadow_t, 64u + 96u * V2K_PARAM_BATCH_MAX);

//-----------------------------------------------------------------------------
// Read-request region (CPU2-owned: CPU2 writes, CPU1 reads only. Publish action = writing read_seq last)
//-----------------------------------------------------------------------------
typedef struct {
    uint16_t count;        // Entries to read this time, 1..V2K_CAL_READ_MAX
    uint16_t reserved;     // Set to 0
    uint32_t read_seq;     // CPU2 bumps this by 1 per read, written last (publish)
    v2k_param_read_ref_t refs[V2K_CAL_READ_MAX];
} v2k_param_read_req_t;

V2K_ASSERT_SIZE_BITS(v2k_param_read_req_t, 64u + 64u * V2K_CAL_READ_MAX);

//-----------------------------------------------------------------------------
// Apply result codes (status.result)
//-----------------------------------------------------------------------------
#define V2K_CAL_OK         0u
#define V2K_CAL_BAD_TYPE   1u   // type is illegal
#define V2K_CAL_BAD_COUNT  2u   // count exceeds the cap
#define V2K_CAL_BAD_ADDR   3u   // target not in a CPU1-writable data region, or 32-bit misaligned

//-----------------------------------------------------------------------------
// Write status (CPU1-owned: CPU1 writes, CPU2/host read only)
//-----------------------------------------------------------------------------
typedef struct {
    uint32_t applied_seq;   // Most recently applied commit_seq (acknowledge side of the sequence handshake)
    uint16_t result;        // V2K_CAL_* (for the batch named by applied_seq)
    uint16_t fail_idx;      // In-batch index of the first illegal entry on whole-batch rejection (valid when result!=OK)
} v2k_param_status_t;

V2K_ASSERT_SIZE_BITS(v2k_param_status_t, 64u);

//-----------------------------------------------------------------------------
// Read-response region (CPU1-owned: CPU1 writes, CPU2/host read only. Publish action = writing ack_seq last)
//-----------------------------------------------------------------------------
typedef struct {
    uint16_t result;        // V2K_CAL_OK / BAD_TYPE / BAD_COUNT / BAD_ADDR
    uint16_t count;         // Valid when result==OK; should equal the requested count
    uint32_t value_bits[V2K_CAL_READ_MAX];
    uint32_t ack_seq;       // Most recently processed read_seq, written last (publish)
} v2k_param_read_resp_t;

V2K_ASSERT_SIZE_BITS(v2k_param_read_resp_t, 64u + 32u * V2K_CAL_READ_MAX);

#endif // V2K_PARAM_H
