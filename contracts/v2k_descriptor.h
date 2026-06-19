//=============================================================================
// v2k_descriptor.h — shared-memory interface: descriptor table
//                    (platform-quantity enumeration + default sampling hints)
//
// Role (variable-discovery architecture; revised 2026-06-19, see Phase 4.5):
// This table registers **platform quantities** (registered by wire/runtime:
// v2k_io physical quantities, duty cycles, status words, platform parameters)
// **plus user application variables** — the latter baked in at compile time by
// the build tool from the firmware .out DWARF (Phase 4.5). The student writes
// plain C: no registration API call, no hand-typed name strings, no mandated
// declaration style (the C symbol is the sole naming source, collected at build
// time, never hand-typed). Once the names are baked into the device the host
// enumerates them all (platform + user) via ENUM, with no .out needed (and no
// stale-ELF risk — the addresses come from the same build that was flashed, and
// build_hash still backstops the host cache).
// Responsibilities of this table:
//   1. Works out of the box: the host enumerates platform + user names and
//      immediately draws waveforms;
//   2. (Phase 3 once default-bound the first few observables in registration
//      order at boot; Phase 4 switched to on-demand binding.)
//
// 2026-06-17 semantic fix:
// * The on-wire value is the real value; the descriptor no longer carries
//   min/max/scale/offset semantics.
//
// CPU1 writes the table at startup (CPU1-owned region, see v2k_memmap.h);
// read-only thereafter.
//
// Key semantics:
// * entry.addr is a word address in CPU1 data space and may point into CPU1
//   private RAM. Only CPU1 may dereference it (sampling and parameter apply
//   both happen on the CPU1 side); CPU2 / host treat addr as an opaque id,
//   passing it through or ignoring it.
// * Registration order is desc_idx (0..count-1), the index key of the value
//   mirror.
// * After the table is written CPU1 fills hdr.entry_count and writes hdr.magic
//   last (publish barrier); CPU2 may read the table only once it sees magic
//   valid.
//
// The descriptor "add" primitive is an L1-internal function; user variables are
// baked into the table at Phase 4.5 build time and port names are registered by
// wire — there is no registration API exposed to L3 user code.
//=============================================================================
#ifndef V2K_DESCRIPTOR_H
#define V2K_DESCRIPTOR_H

#include "v2k_common.h"

//-----------------------------------------------------------------------------
// Capacity and size constants
//-----------------------------------------------------------------------------
#define V2K_NAME_LEN   16u   // Fixed name length (incl. NUL padding); ASCII, 1 octet/char on the wire
#define V2K_DESC_MAX   128u  // Table capacity cap (128 × 22 words ≈ 2.8K words, budget in memmap)
#define V2K_USER_DESC_MAX 96u // Build-time baked user-variable capacity; the first 32 slots remain for platform descriptors
#define V2K_PLATFORM_DESC_MAX (V2K_DESC_MAX - V2K_USER_DESC_MAX)

// Octets per descriptor on the wire (wire-spec §4.3 ENUM_RESP; mirrors the struct fields one-to-one)
#define V2K_DESC_WIRE_OCTETS 28u

//-----------------------------------------------------------------------------
// kind flag bits (tunable and observable are not mutually exclusive; one variable can be both)
//-----------------------------------------------------------------------------
#define V2K_KIND_PARAM  0x0001u  // Tunable parameter: participates in the parameter-plane write path (v2k_param.h)
#define V2K_KIND_SCOPE  0x0002u  // Observable signal: participates in the scope sampling path (v2k_scope.h); prescaler is the default sampling hint
// bit2..15 reserved, set to 0

#define V2K_DESC_ERROR_NONE              0u
#define V2K_DESC_ERROR_BLOB_HEADER       1u
#define V2K_DESC_ERROR_PLATFORM_CAPACITY 2u
#define V2K_DESC_ERROR_TABLE_CAPACITY    3u
#define V2K_DESC_ERROR_USER_ENTRY        4u

//-----------------------------------------------------------------------------
// Descriptor entry
//
// C28x layout (word offsets):
//   name@0..15, type@16, kind@17, addr@18, prescaler@20, reserved@21
//   → 22 words total, no padding
// PC layout (octet offsets):
//   name@0..15, type@16, kind@18, addr@20, prescaler@24, reserved@26
//   → 28 octets total, no padding
//-----------------------------------------------------------------------------
typedef struct {
    char     name[V2K_NAME_LEN]; // ASCII, NUL-padded; not guaranteed NUL-terminated (when exactly 16 chars)
    uint16_t type;               // V2K_TYPE_*
    uint16_t kind;               // V2K_KIND_* bit-or
    uint32_t addr;               // CPU1 data-space word address (opaque to CPU2/host)
    uint16_t prescaler;          // Default sampling-decimation hint; the actual runtime rate is governed by DAQ_CTRL
    uint16_t reserved;           // Set to 0; keeps the 28-octet descriptor entry aligned
} v2k_desc_entry_t;

V2K_ASSERT_SIZE_BITS(v2k_desc_entry_t, V2K_NAME_BITS(V2K_NAME_LEN) + 96u);

//-----------------------------------------------------------------------------
// Table header (precedes the entry array, in the same shared-RAM region)
//
// Publish protocol: CPU1 fills entries[] and the remaining header fields first,
// then writes magic last; CPU2 polls magic == V2K_DESC_MAGIC and treats the
// table as ready (single writer, one direction, no lock needed).
//-----------------------------------------------------------------------------
#define V2K_DESC_MAGIC 0x564B4454u   /* "VKDT" */
#define V2K_USER_DESC_MAGIC 0x564B5544u /* "VKUD" */
#define V2K_USER_DESC_VERSION 2u

typedef struct {
    uint32_t magic;              // V2K_DESC_MAGIC; written last = publish
    uint16_t contract_ver;       // = V2K_CONTRACT_VER, checked by CPU2 at handshake
    uint16_t entry_count;        // Number of registered entries ≤ V2K_DESC_MAX
    v2k_build_hash_t build_hash; // Firmware build hash (host re-enumeration trigger)
    uint16_t entry_stride_words; // = sizeof(v2k_desc_entry_t) (C28x words),
                                 //   lets CCS scripts/debug tools walk the table self-describingly
    uint16_t reserved;           // Set to 0
} v2k_desc_table_hdr_t;

V2K_ASSERT_SIZE_BITS(v2k_desc_table_hdr_t, 128u);

//-----------------------------------------------------------------------------
// Whole table (shared-RAM entity, CPU1-owned)
//-----------------------------------------------------------------------------
typedef struct {
    v2k_desc_table_hdr_t hdr;
    v2k_desc_entry_t     entries[V2K_DESC_MAX];
} v2k_desc_table_t;

typedef struct {
    uint32_t magic;       // V2K_USER_DESC_MAGIC when the reserved blob is initialized/patched
    uint16_t version;     // V2K_USER_DESC_VERSION
    uint16_t count;       // Valid baked entries in entries[]
    uint16_t capacity;    // = V2K_USER_DESC_MAX; checked by the baker and runtime
    uint16_t reserved;    // Set to 0
    v2k_build_hash_t build_hash; // Final linked image hash generated by the baker
    v2k_desc_entry_t entries[V2K_USER_DESC_MAX];
} v2k_user_desc_blob_t;

V2K_ASSERT_SIZE_BITS(v2k_user_desc_blob_t,
                     128u + (V2K_USER_DESC_MAX *
                            (V2K_NAME_BITS(V2K_NAME_LEN) + 96u)));
V2K_STATIC_ASSERT(V2K_PLATFORM_DESC_MAX + V2K_USER_DESC_MAX == V2K_DESC_MAX);

#endif // V2K_DESCRIPTOR_H
