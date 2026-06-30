//=============================================================================
// v2k_descriptor.h - shared-memory interface: CPU1-owned variable catalog
//
// v10 catalog model:
// * CPU1 owns the complete variable catalog. Platform names live in CPU1
//   firmware data, and baked user names live in the CPU1 Flash v2k_user_desc
//   blob. CPU2 never reads CPU1 Flash directly and never stores the dictionary.
// * Shared RAM carries only a small catalog header plus an ENUM request/response
//   staging window. Realtime paths remain keyed by (addr,type) and never carry
//   names.
// * The user catalog has explicit build-time guardrails: many variables and long
//   names are allowed, but over-budget catalogs fail in the baker with a clear
//   message before the linker/Flash capacity is the first signal.
//=============================================================================
#ifndef V2K_DESCRIPTOR_H
#define V2K_DESCRIPTOR_H

#include "v2k_common.h"

//-----------------------------------------------------------------------------
// Catalog capacity and resource guardrails
//-----------------------------------------------------------------------------
#define V2K_NAME_MAX 128u              // Max visible variable-name octets.
#define V2K_PROJECT_NAME_LEN 32u       // HELLO project name length.
#define V2K_DEFAULT_PROJECT_NAME "untitled"

#define V2K_PLATFORM_DESC_MAX 96u
#define V2K_USER_DESC_MAX 512u
#define V2K_DESC_MAX (V2K_PLATFORM_DESC_MAX + V2K_USER_DESC_MAX)

#define V2K_PLATFORM_NAME_POOL_OCTETS 4096u
#define V2K_USER_NAME_POOL_OCTETS 16384u

#define V2K_ENUM_MAX_COUNT 8u
#define V2K_ENUM_ENTRY_FIXED_OCTETS 12u
#define V2K_CATALOG_PAYLOAD_OCTETS V2K_WIRE_MAX_PAYLOAD

//-----------------------------------------------------------------------------
// kind flag bits (tunable and observable are not mutually exclusive)
//-----------------------------------------------------------------------------
#define V2K_KIND_PARAM  0x0001u
#define V2K_KIND_SCOPE  0x0002u
#define V2K_KIND_USER   0x0004u

#define V2K_DESC_ERROR_NONE              0u
#define V2K_DESC_ERROR_BLOB_HEADER       1u
#define V2K_DESC_ERROR_PLATFORM_CAPACITY 2u
#define V2K_DESC_ERROR_TABLE_CAPACITY    3u
#define V2K_DESC_ERROR_USER_ENTRY        4u
#define V2K_DESC_ERROR_NAME_POOL         5u

#define V2K_CATALOG_RESULT_OK        0u
#define V2K_CATALOG_RESULT_BAD_PARAM 1u
#define V2K_CATALOG_RESULT_NO_DATA   2u
#define V2K_CATALOG_RESULT_INTERNAL  3u

//-----------------------------------------------------------------------------
// Compact descriptor metadata
//
// This is the realtime-relevant identity of a variable. It deliberately has no
// name field. Names are serialized only during ENUM from the CPU1-owned catalog
// source.
//-----------------------------------------------------------------------------
typedef struct {
    uint32_t addr;       // CPU1 data-space word address.
    uint16_t type;       // V2K_TYPE_*
    uint16_t kind;       // V2K_KIND_* bit-or
    uint16_t prescaler;  // Default sampling-decimation hint.
    uint16_t reserved;   // Set to 0; keeps array stride 32-bit aligned.
} v2k_desc_entry_t;

V2K_ASSERT_SIZE_BITS(v2k_desc_entry_t, 96u);

//-----------------------------------------------------------------------------
// CPU1-published catalog header and ENUM staging
//-----------------------------------------------------------------------------
#define V2K_CATALOG_MAGIC 0x564B4341u       // "VKCA"
#define V2K_USER_DESC_MAGIC 0x564B5543u     // "VKUC"
#define V2K_USER_DESC_VERSION 5u

typedef struct {
    uint32_t magic;       // V2K_CATALOG_MAGIC; written last = publish.
    v2k_build_hash_t build_hash;
    uint16_t contract_ver;
    uint16_t total_count;
    uint16_t platform_count;
    uint16_t user_count;
    uint16_t max_name_len;
    uint16_t user_capacity;
    uint16_t platform_capacity;
    uint16_t user_name_pool_octets;
    uint16_t platform_name_pool_octets;
    uint16_t reserved;
} v2k_catalog_hdr_t;

V2K_ASSERT_SIZE_BITS(v2k_catalog_hdr_t, 224u);

typedef struct {
    uint16_t start_idx;
    uint16_t max_count;
    uint16_t reserved;
    uint16_t req_seq;    // CPU2 writes this last to publish.
} v2k_catalog_req_t;

V2K_ASSERT_SIZE_BITS(v2k_catalog_req_t, 64u);

typedef struct {
    uint16_t ack_seq;     // CPU1 writes this last to publish.
    uint16_t result;      // V2K_CATALOG_RESULT_*
    uint16_t payload_len; // ENUM response payload octets in payload[].
    uint16_t reserved;
    uint16_t payload[V2K_CATALOG_PAYLOAD_OCTETS]; // One wire octet per word.
} v2k_catalog_resp_t;

V2K_ASSERT_SIZE_BITS(v2k_catalog_resp_t,
                     64u + (V2K_CATALOG_PAYLOAD_OCTETS * 16u));

typedef struct {
    v2k_catalog_hdr_t hdr;
    v2k_catalog_resp_t enum_resp;
} v2k_catalog_shared_t;

//-----------------------------------------------------------------------------
// Firmware/project info
//-----------------------------------------------------------------------------
typedef struct {
    char     project_name[V2K_PROJECT_NAME_LEN];
    uint32_t build_time_utc;
} v2k_firmware_info_t;

V2K_ASSERT_SIZE_BITS(v2k_firmware_info_t,
                     V2K_NAME_BITS(V2K_PROJECT_NAME_LEN) + 32u);

//-----------------------------------------------------------------------------
// CPU1 Flash user catalog blob
//
// name_offset/name_len address bytes in name_pool_words as packed ASCII octets:
// even offset -> low byte of the word, odd offset -> high byte.
//-----------------------------------------------------------------------------
typedef struct {
    v2k_desc_entry_t desc;
    uint32_t name_offset;
    uint16_t name_len;
    uint16_t reserved;
} v2k_user_desc_entry_t;

V2K_ASSERT_SIZE_BITS(v2k_user_desc_entry_t, 160u);

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t count;
    uint16_t capacity;
    uint16_t reserved;
    v2k_build_hash_t build_hash;
    v2k_firmware_info_t firmware_info;
    uint16_t name_pool_octets;
    uint16_t name_pool_capacity;
    uint16_t reserved2;
    uint16_t reserved3;
    v2k_user_desc_entry_t entries[V2K_USER_DESC_MAX];
    uint16_t name_pool_words[(V2K_USER_NAME_POOL_OCTETS + 1u) / 2u];
} v2k_user_desc_blob_t;

V2K_ASSERT_SIZE_BITS(v2k_user_desc_blob_t,
                     128uL + V2K_NAME_BITS(V2K_PROJECT_NAME_LEN) + 32uL +
                     64uL + ((uint32_t)V2K_USER_DESC_MAX * 160uL) +
                     (((uint32_t)((V2K_USER_NAME_POOL_OCTETS + 1uL) / 2uL)) *
                      16uL));

#endif // V2K_DESCRIPTOR_H
