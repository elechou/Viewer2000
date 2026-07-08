//=============================================================================
// v2k_registry.c - CPU1 catalog + background validation / ISR-applied parameter batches
//=============================================================================

#include <string.h>
#include "v2k_registry.h"
#include "v2k_shared.h"
#include "../v2k.h"
#include "../board/v2k_board.h"
#include "v2k_timebase.h"
#include "v2k_fault.h"
#include "v2k_profile.h"
#include "v2k_user_runtime.h"

extern uint16_t V2K_BssStart;
extern uint16_t V2K_BssEnd;
extern uint16_t V2K_BssOutputStart;
extern uint16_t V2K_BssOutputEnd;
extern uint16_t V2K_DataStart;
extern uint16_t V2K_DataEnd;
extern uint16_t V2K_UserDataRunStart;
extern uint16_t V2K_UserDataRunEnd;
extern uint16_t V2K_UserBssStart;
extern uint16_t V2K_UserBssEnd;
extern uint16_t V2K_UserConstStart;
extern uint16_t V2K_UserConstEnd;

extern volatile uint32_t g_v2k_isr_cycles;
extern volatile uint32_t g_v2k_isr_cycles_max;
extern volatile uint32_t g_v2k_control_cycles_max;
extern volatile uint32_t g_v2k_scope_cycles_max;
extern volatile uint32_t g_v2k_isr_budget_violation_cnt;
extern volatile uint32_t g_v2k_scope_overrun_total;
extern volatile uint16_t g_v2k_due_mask;
extern volatile uint32_t g_v2k_tz_int_cnt;
extern uint16_t g_cpu2_alive;

typedef struct {
    uint32_t seq;
    uint16_t count;
    v2k_param_write_t writes[V2K_PARAM_BATCH_MAX];
} v2k_param_ready_t;

typedef struct {
    const char *name;
    uint16_t name_len;
    v2k_desc_entry_t desc;
} v2k_platform_catalog_entry_t;

// The background service publishes this batch to the control ISR. Keep the
// storage static to avoid placing a full parameter batch on the foreground
// stack, and volatile so neither side caches fields across the ready flag.
static volatile v2k_param_ready_t s_ready;
static volatile uint16_t s_ready_valid;
static uint32_t s_shadow_seen;
static v2k_param_read_ref_t s_read_refs[V2K_CAL_READ_MAX];
static v2k_platform_catalog_entry_t s_platform_catalog[V2K_PLATFORM_DESC_MAX];
static uint16_t s_platform_count;
static uint16_t s_platform_name_pool_octets;
static uint16_t s_user_catalog_valid;
static uint16_t s_catalog_req_seen;
volatile uint16_t g_v2k_desc_error;

// Fixed-size post-link patch target; its initialized header is validated before baking.
#pragma DATA_SECTION(g_v2k_user_desc_blob, "v2k_user_desc")
const volatile v2k_user_desc_blob_t g_v2k_user_desc_blob = {
    V2K_USER_DESC_MAGIC,
    V2K_USER_DESC_VERSION,
    0u,
    V2K_USER_DESC_MAX,
    0u,
    0uL,
    { V2K_DEFAULT_PROJECT_NAME, 0uL },
    0u,
    V2K_USER_NAME_POOL_OCTETS,
    0u,
    0u,
    {{0}},
    {0}
};

static uint32_t v2k_addr(const volatile void *ptr)
{
    return (uint32_t)ptr;
}

static uint16_t v2k_type_words(uint16_t type)
{
    if (type >= V2K_TYPE_COUNT)
    {
        return 0u;
    }
    return ((type == V2K_TYPE_I16) || (type == V2K_TYPE_U16)) ? 1u : 2u;
}

static uint16_t v2k_addr_in_range(uint32_t addr, uint16_t words,
                                  const uint16_t *start, const uint16_t *end)
{
    uint32_t first = (uint32_t)start;
    uint32_t limit = (uint32_t)end;
    return ((addr >= first) && ((addr + words) <= limit)) ? 1u : 0u;
}

static uint16_t v2k_addr_is_writable(uint32_t addr, uint16_t type)
{
    uint16_t words = v2k_type_words(type);

    if (words == 0u)
    {
        return 0u;
    }
    if ((words == 2u) && ((addr & 1u) != 0u))
    {
        return 0u;
    }
    return (uint16_t)(
        v2k_addr_in_range(addr, words, &V2K_BssStart, &V2K_BssEnd) ||
        v2k_addr_in_range(addr, words, &V2K_BssOutputStart, &V2K_BssOutputEnd) ||
        v2k_addr_in_range(addr, words, &V2K_DataStart, &V2K_DataEnd) ||
        v2k_addr_in_range(addr, words,
                          &V2K_UserDataRunStart, &V2K_UserDataRunEnd) ||
        v2k_addr_in_range(addr, words, &V2K_UserBssStart, &V2K_UserBssEnd));
}

static uint16_t v2k_addr_is_readable(uint32_t addr, uint16_t type)
{
    uint16_t words = v2k_type_words(type);

    if (words == 0u)
    {
        return 0u;
    }
    if ((words == 2u) && ((addr & 1u) != 0u))
    {
        return 0u;
    }
    return (uint16_t)(
        v2k_addr_is_writable(addr, type) ||
        v2k_addr_in_range(addr, words, &V2K_UserConstStart, &V2K_UserConstEnd));
}

// Wire staging writes go through volatile pointers: the ENUM response lives
// in the CPU1 plane that CPU2 busy-polls, and volatile keeps the payload
// stores ordered before the ack_seq publish below.
static void v2k_put_u16(volatile uint16_t *buf, uint16_t off, uint16_t value)
{
    buf[off] = value & 0xFFu;
    buf[(uint16_t)(off + 1u)] = (value >> 8u) & 0xFFu;
}

static void v2k_put_u32(volatile uint16_t *buf, uint16_t off, uint32_t value)
{
    v2k_put_u16(buf, off, (uint16_t)value);
    v2k_put_u16(buf, (uint16_t)(off + 2u), (uint16_t)(value >> 16u));
}

static uint16_t v2k_name_len(const char *name, uint16_t max_len,
                             uint16_t *len)
{
    uint16_t i;

    for (i = 0u; i <= max_len; i++)
    {
        uint16_t c = (uint16_t)name[i];

        if (c == 0u)
        {
            *len = i;
            return (i > 0u) ? 1u : 0u;
        }
        if ((i == max_len) || (c < 0x20u) || (c > 0x7Eu))
        {
            return 0u;
        }
    }
    return 0u;
}

static uint16_t v2k_project_name_valid(
    const volatile char name[V2K_PROJECT_NAME_LEN])
{
    uint16_t i;

    for (i = 0u; i < V2K_PROJECT_NAME_LEN; i++)
    {
        uint16_t c = (uint16_t)name[i];
        if (c == 0u)
        {
            return (i > 0u) ? 1u : 0u;
        }
        if ((c < 0x20u) || (c > 0x7Eu))
        {
            return 0u;
        }
    }
    return 1u;
}

static void v2k_default_project_name(char dst[V2K_PROJECT_NAME_LEN])
{
    static const char default_name[V2K_PROJECT_NAME_LEN] =
        V2K_DEFAULT_PROJECT_NAME;
    uint16_t i;

    for (i = 0u; i < V2K_PROJECT_NAME_LEN; i++)
    {
        dst[i] = default_name[i];
    }
}

static void v2k_publish_firmware_info(void)
{
    v2k_firmware_info_t *info = &g_v2k_cpu1_plane.firmware_info;
    uint16_t i;

    memset(info, 0, sizeof(*info));
    if (v2k_project_name_valid(g_v2k_user_desc_blob.firmware_info.project_name))
    {
        for (i = 0u; i < V2K_PROJECT_NAME_LEN; i++)
        {
            info->project_name[i] =
                g_v2k_user_desc_blob.firmware_info.project_name[i];
        }
        info->build_time_utc =
            g_v2k_user_desc_blob.firmware_info.build_time_utc;
    }
    else
    {
        v2k_default_project_name(info->project_name);
        info->build_time_utc = 0uL;
    }
}

void v2k_registry_add(const char *name, uint16_t type, uint16_t kind,
                      volatile void *addr, uint16_t prescaler)
{
    uint16_t idx = s_platform_count;
    uint16_t len = 0u;
    v2k_platform_catalog_entry_t *entry;

    if (idx >= V2K_PLATFORM_DESC_MAX)
    {
        g_v2k_desc_error = V2K_DESC_ERROR_PLATFORM_CAPACITY;
        return;
    }
    if (!v2k_name_len(name, V2K_NAME_MAX, &len) ||
        ((uint32_t)s_platform_name_pool_octets + (uint32_t)len >
         V2K_PLATFORM_NAME_POOL_OCTETS) ||
        (v2k_type_words(type) == 0u) ||
        (prescaler == 0u))
    {
        g_v2k_desc_error = V2K_DESC_ERROR_NAME_POOL;
        return;
    }
    entry = &s_platform_catalog[idx];
    memset(entry, 0, sizeof(*entry));
    entry->name = name;
    entry->name_len = len;
    entry->desc.addr = v2k_addr(addr);
    entry->desc.type = type;
    entry->desc.kind = kind;
    entry->desc.prescaler = prescaler;
    entry->desc.reserved = 0u;
    s_platform_name_pool_octets =
        (uint16_t)(s_platform_name_pool_octets + len);
    s_platform_count = (uint16_t)(idx + 1u);
}

static uint16_t v2k_user_desc_blob_valid(void)
{
    const volatile v2k_user_desc_blob_t *blob = &g_v2k_user_desc_blob;

    return (uint16_t)(
        (blob->magic == V2K_USER_DESC_MAGIC) &&
        (blob->version == V2K_USER_DESC_VERSION) &&
        (blob->capacity == V2K_USER_DESC_MAX) &&
        (blob->count <= blob->capacity) &&
        (blob->reserved == 0u) &&
        (blob->build_hash != 0uL) &&
        (blob->name_pool_capacity == V2K_USER_NAME_POOL_OCTETS) &&
        (blob->name_pool_octets <= blob->name_pool_capacity) &&
        (blob->reserved2 == 0u) &&
        (blob->reserved3 == 0u) &&
        v2k_project_name_valid(blob->firmware_info.project_name));
}

static uint16_t v2k_user_name_octet(uint32_t offset)
{
    uint16_t word = g_v2k_user_desc_blob.name_pool_words[offset >> 1u];

    if ((offset & 1u) == 0u)
    {
        return word & 0xFFu;
    }
    return (word >> 8u) & 0xFFu;
}

static uint16_t v2k_user_desc_entry_valid(
    const volatile v2k_user_desc_entry_t *entry)
{
    uint16_t i;
    uint16_t words;
    const volatile v2k_desc_entry_t *desc = &entry->desc;

    words = v2k_type_words(desc->type);
    if ((words == 0u) ||
        ((words == 2u) && ((desc->addr & 1u) != 0u)) ||
        (desc->prescaler == 0u) ||
        (desc->reserved != 0u) ||
        (entry->reserved != 0u) ||
        (entry->name_len == 0u) ||
        (entry->name_len > V2K_NAME_MAX) ||
        ((entry->name_offset + entry->name_len) >
         g_v2k_user_desc_blob.name_pool_octets))
    {
        return 0u;
    }
    for (i = 0u; i < entry->name_len; i++)
    {
        uint16_t c = v2k_user_name_octet(entry->name_offset + i);

        if ((c < 0x20u) || (c > 0x7Eu))
        {
            return 0u;
        }
    }
    if (desc->kind ==
        (V2K_KIND_USER | V2K_KIND_PARAM | V2K_KIND_SCOPE))
    {
        return (uint16_t)(
            v2k_addr_in_range(desc->addr, words,
                              &V2K_UserDataRunStart, &V2K_UserDataRunEnd) ||
            v2k_addr_in_range(desc->addr, words,
                              &V2K_UserBssStart, &V2K_UserBssEnd));
    }
    if (desc->kind == (V2K_KIND_USER | V2K_KIND_SCOPE))
    {
        return v2k_addr_in_range(desc->addr, words,
                                 &V2K_UserConstStart, &V2K_UserConstEnd);
    }
    return 0u;
}

static void v2k_registry_validate_baked_user(void)
{
    const volatile v2k_user_desc_blob_t *blob = &g_v2k_user_desc_blob;
    uint16_t i;

    s_user_catalog_valid = 0u;
    if (!v2k_user_desc_blob_valid())
    {
        g_v2k_desc_error = V2K_DESC_ERROR_BLOB_HEADER;
        return;
    }
    for (i = 0u; i < blob->count; i++)
    {
        if (!v2k_user_desc_entry_valid(&blob->entries[i]))
        {
            g_v2k_desc_error = V2K_DESC_ERROR_USER_ENTRY;
            return;
        }
    }
    if (blob->count > (V2K_DESC_MAX - s_platform_count))
    {
        g_v2k_desc_error = V2K_DESC_ERROR_TABLE_CAPACITY;
        return;
    }
    s_user_catalog_valid = 1u;
}

static uint16_t v2k_user_count(void)
{
    return s_user_catalog_valid ? g_v2k_user_desc_blob.count : 0u;
}

static uint16_t v2k_catalog_total_count(void)
{
    return (uint16_t)(s_platform_count + v2k_user_count());
}

static void v2k_publish_catalog_header(void)
{
    // Volatile publish: CPU2 gates its startup on hdr->magic, so the field
    // fills must not be reorderable past the magic store below.
    volatile v2k_catalog_hdr_t *hdr = &g_v2k_cpu1_plane.catalog.hdr;

    hdr->build_hash = s_user_catalog_valid ?
        g_v2k_user_desc_blob.build_hash : 0uL;
    hdr->contract_ver = V2K_CONTRACT_VER;
    hdr->total_count = v2k_catalog_total_count();
    hdr->platform_count = s_platform_count;
    hdr->user_count = v2k_user_count();
    hdr->max_name_len = V2K_NAME_MAX;
    hdr->user_capacity = V2K_USER_DESC_MAX;
    hdr->platform_capacity = V2K_PLATFORM_DESC_MAX;
    hdr->user_name_pool_octets = V2K_USER_NAME_POOL_OCTETS;
    hdr->platform_name_pool_octets = V2K_PLATFORM_NAME_POOL_OCTETS;
    hdr->reserved = 0u;
    hdr->magic = V2K_CATALOG_MAGIC;
}

void v2k_registry_init(void)
{
    uint16_t slow_div = (uint16_t)(V2K_ISR_HZ / 1000u);

    g_v2k_desc_error = V2K_DESC_ERROR_NONE;
    memset(&g_v2k_cpu1_plane.catalog, 0, sizeof(g_v2k_cpu1_plane.catalog));
    memset(s_platform_catalog, 0, sizeof(s_platform_catalog));
    s_platform_count = 0u;
    s_platform_name_pool_octets = 0u;
    s_user_catalog_valid = 0u;
    s_catalog_req_seen = 0u;
    v2k_publish_firmware_info();

    v2k_registry_add("desc_error", V2K_TYPE_U16, V2K_KIND_SCOPE,
                     &g_v2k_desc_error, 1u);
    v2k_board_register_ports(1u);
    v2k_registry_add("isr_cycles", V2K_TYPE_U32, V2K_KIND_SCOPE,
                     &g_v2k_isr_cycles, 1u);
    v2k_registry_add("isr_latency", V2K_TYPE_U16, V2K_KIND_SCOPE,
                     &g_v2k_isr_lat, 1u);
    v2k_registry_add("due_mask", V2K_TYPE_U16, V2K_KIND_SCOPE,
                     &g_v2k_due_mask, 1u);
    v2k_registry_add("sys_state", V2K_TYPE_U16, V2K_KIND_SCOPE,
                     &g_v2k_sm_state, 1u);

    // The rest are low-rate health/protection quantities; prescaler is only
    // Scope2000's default sampling hint. Stream/Capture share one hot path, and
    // the actual sampling decimation is issued uniformly by the host via DAQ_CTRL.
    v2k_registry_add("fault_code", V2K_TYPE_U16, V2K_KIND_SCOPE,
                     &g_v2k_fault_code, slow_div);
    v2k_registry_add("app_enabled", V2K_TYPE_U16, V2K_KIND_SCOPE,
                     &g_v2k_app_enabled, slow_div);
    v2k_registry_add("user_rst_busy", V2K_TYPE_U16, V2K_KIND_SCOPE,
                     &g_v2k_user_reset_active, slow_div);
    v2k_registry_add("user_resets", V2K_TYPE_U32, V2K_KIND_SCOPE,
                     &g_v2k_user_reset_count, slow_div);
    v2k_registry_add("user_reset_err", V2K_TYPE_U16, V2K_KIND_SCOPE,
                     &g_v2k_user_reset_error, slow_div);
    v2k_registry_add("user_crc_expect", V2K_TYPE_U32, V2K_KIND_SCOPE,
                     &g_v2k_user_crc_expected, slow_div);
    v2k_registry_add("user_crc_actual", V2K_TYPE_U32, V2K_KIND_SCOPE,
                     &g_v2k_user_crc_actual, slow_div);
    v2k_registry_add("cpu2_alive", V2K_TYPE_U16, V2K_KIND_SCOPE,
                     &g_cpu2_alive, slow_div);
    v2k_registry_add("isr_overflow", V2K_TYPE_U32, V2K_KIND_SCOPE,
                     &g_v2k_isr_ovf_cnt, slow_div);
    v2k_registry_add("isr_budget", V2K_TYPE_U32, V2K_KIND_SCOPE,
                     &g_v2k_isr_budget_violation_cnt, slow_div);
    v2k_registry_add("isr_cycles_max", V2K_TYPE_U32, V2K_KIND_SCOPE,
                     &g_v2k_isr_cycles_max, slow_div);
    v2k_registry_add("ctrl_cycles_max", V2K_TYPE_U32, V2K_KIND_SCOPE,
                     &g_v2k_control_cycles_max, slow_div);
    // Expose the five profiler signal values as system Variables (scope-bindable
    // / CAL-readable). prof_seq (a publication sequence), cycle_budget (a build
    // constant), and peak_tick (a hidden bring-up correlation id) ride in STATUS
    // only: they are not meaningful to plot or bind, so registering them would
    // spend scarce platform descriptor slots for nothing.
    v2k_registry_add("load_avg", V2K_TYPE_U32, V2K_KIND_SCOPE,
                     &g_v2k_load_avg, slow_div);
    v2k_registry_add("load_peak", V2K_TYPE_U32, V2K_KIND_SCOPE,
                     &g_v2k_load_peak, slow_div);
    v2k_registry_add("ctrl_at_peak", V2K_TYPE_U32, V2K_KIND_SCOPE,
                     &g_v2k_ctrl_at_peak, slow_div);
    v2k_registry_add("scope_at_peak", V2K_TYPE_U32, V2K_KIND_SCOPE,
                     &g_v2k_scope_at_peak, slow_div);
    v2k_registry_add("lat_at_peak", V2K_TYPE_U16, V2K_KIND_SCOPE,
                     &g_v2k_lat_at_peak, slow_div);
    // scope_cyc_max is intentionally not registered, freeing this slot for the
    // protection signal tz_trip_cnt (g_v2k_scope_cycles_max stays viewable in
    // CCS, and the profiler's scope_at_peak carries the on-wire scope figure).
    // Note: scope can no longer be back-derived as isr_cycles_max − ctrl_cycles_max,
    // because ctrl now times only the user control() body (Phase 4.6).
    v2k_registry_add("scope_overrun", V2K_TYPE_U32, V2K_KIND_SCOPE,
                     &g_v2k_scope_overrun_total, slow_div);
    v2k_registry_add("tz_trip_cnt", V2K_TYPE_U32, V2K_KIND_SCOPE,
                     &g_v2k_tz_int_cnt, slow_div);
    v2k_board_register_diagnostics(slow_div);
    v2k_registry_validate_baked_user();
    v2k_publish_catalog_header();
}

static const volatile v2k_desc_entry_t *v2k_catalog_desc_at(uint16_t idx)
{
    uint16_t user_idx;

    if (idx < s_platform_count)
    {
        return &s_platform_catalog[idx].desc;
    }
    user_idx = (uint16_t)(idx - s_platform_count);
    if (s_user_catalog_valid && (user_idx < g_v2k_user_desc_blob.count))
    {
        return &g_v2k_user_desc_blob.entries[user_idx].desc;
    }
    return (const volatile v2k_desc_entry_t *)0;
}

static uint16_t v2k_catalog_name_len_at(uint16_t idx)
{
    uint16_t user_idx;

    if (idx < s_platform_count)
    {
        return s_platform_catalog[idx].name_len;
    }
    user_idx = (uint16_t)(idx - s_platform_count);
    if (s_user_catalog_valid && (user_idx < g_v2k_user_desc_blob.count))
    {
        return g_v2k_user_desc_blob.entries[user_idx].name_len;
    }
    return 0u;
}

static uint16_t v2k_catalog_name_octet_at(uint16_t idx, uint16_t name_pos)
{
    uint16_t user_idx;

    if (idx < s_platform_count)
    {
        return ((uint16_t)s_platform_catalog[idx].name[name_pos]) & 0xFFu;
    }
    user_idx = (uint16_t)(idx - s_platform_count);
    if (s_user_catalog_valid && (user_idx < g_v2k_user_desc_blob.count))
    {
        return v2k_user_name_octet(
            g_v2k_user_desc_blob.entries[user_idx].name_offset + name_pos);
    }
    return 0u;
}

static uint16_t v2k_catalog_pack_entry(uint16_t idx,
                                       volatile uint16_t *payload,
                                       uint16_t *off)
{
    const volatile v2k_desc_entry_t *desc = v2k_catalog_desc_at(idx);
    uint16_t name_len = v2k_catalog_name_len_at(idx);
    uint16_t i;

    if ((desc == (const volatile v2k_desc_entry_t *)0) ||
        ((*off + V2K_ENUM_ENTRY_FIXED_OCTETS + name_len) >
         V2K_CATALOG_PAYLOAD_OCTETS))
    {
        return 0u;
    }
    v2k_put_u32(payload, *off, desc->addr);
    v2k_put_u16(payload, (uint16_t)(*off + 4u), desc->type);
    v2k_put_u16(payload, (uint16_t)(*off + 6u), desc->kind);
    v2k_put_u16(payload, (uint16_t)(*off + 8u), desc->prescaler);
    payload[(uint16_t)(*off + 10u)] = name_len & 0xFFu;
    payload[(uint16_t)(*off + 11u)] = 0u;
    *off = (uint16_t)(*off + V2K_ENUM_ENTRY_FIXED_OCTETS);
    for (i = 0u; i < name_len; i++)
    {
        payload[*off] = v2k_catalog_name_octet_at(idx, i);
        *off = (uint16_t)(*off + 1u);
    }
    return 1u;
}

void v2k_catalog_service(void)
{
    const volatile v2k_catalog_req_t *req = &V2K_CPU2_PLANE_RO->catalog_req;
    // Volatile response pointer: CPU2 busy-polls ack_seq at microsecond
    // granularity, so payload/result stores must be ordered before it.
    volatile v2k_catalog_resp_t *resp = &g_v2k_cpu1_plane.catalog.enum_resp;
    uint16_t seq_before = req->req_seq;
    uint16_t seq_after;
    uint16_t start;
    uint16_t max_count;
    uint16_t total;
    uint16_t count = 0u;
    uint16_t off = 6u;
    uint16_t idx;
    uint16_t result = V2K_CATALOG_RESULT_OK;

    if ((seq_before == s_catalog_req_seen) ||
        (seq_before == resp->ack_seq))
    {
        return;
    }
    start = req->start_idx;
    max_count = req->max_count;
    seq_after = req->req_seq;
    if (seq_before != seq_after)
    {
        return;
    }
    s_catalog_req_seen = seq_after;
    total = v2k_catalog_total_count();

    if ((max_count == 0u) || (max_count > V2K_ENUM_MAX_COUNT))
    {
        result = V2K_CATALOG_RESULT_BAD_PARAM;
        off = 0u;
    }
    else
    {
        v2k_put_u16(resp->payload, 0u, total);
        v2k_put_u16(resp->payload, 2u, start);
        resp->payload[4] = 0u;
        resp->payload[5] = 0u;

        idx = start;
        while ((idx < total) && (count < max_count))
        {
            uint16_t before = off;

            if (!v2k_catalog_pack_entry(idx, resp->payload, &off))
            {
                if (count == 0u)
                {
                    result = V2K_CATALOG_RESULT_INTERNAL;
                    off = 0u;
                }
                break;
            }
            if (off == before)
            {
                result = V2K_CATALOG_RESULT_INTERNAL;
                off = 0u;
                break;
            }
            count++;
            idx++;
        }
        if (result == V2K_CATALOG_RESULT_OK)
        {
            resp->payload[4] = count & 0xFFu;
        }
    }

    resp->result = result;
    resp->payload_len = (result == V2K_CATALOG_RESULT_OK) ? off : 0u;
    resp->reserved = 0u;
    resp->ack_seq = seq_before;
}

static const volatile v2k_desc_entry_t *v2k_desc_find(uint32_t addr)
{
    uint16_t i;
    uint16_t total = v2k_catalog_total_count();

    for (i = 0u; i < total; i++)
    {
        const volatile v2k_desc_entry_t *entry = v2k_catalog_desc_at(i);

        if ((entry != (const volatile v2k_desc_entry_t *)0) &&
            (entry->addr == addr))
        {
            return entry;
        }
    }
    return (const volatile v2k_desc_entry_t *)0;
}

static uint16_t v2k_validate_write(const volatile v2k_param_write_t *write)
{
    const volatile v2k_desc_entry_t *entry;

    if (write->type >= V2K_TYPE_COUNT)
    {
        return V2K_CAL_BAD_TYPE;
    }
    if (!v2k_addr_is_writable(write->addr, write->type))
    {
        return V2K_CAL_BAD_ADDR;
    }

    entry = v2k_desc_find(write->addr);
    if (entry == (const volatile v2k_desc_entry_t *)0)
    {
        return V2K_CAL_OK;
    }
    if ((entry->kind & V2K_KIND_PARAM) == 0u)
    {
        return V2K_CAL_BAD_ADDR;
    }
    if (entry->type != write->type)
    {
        return V2K_CAL_BAD_TYPE;
    }
    return V2K_CAL_OK;
}

void v2k_param_service(void)
{
    const volatile v2k_param_shadow_t *shadow = &V2K_CPU2_PLANE_RO->param_shadow;
    // Volatile status pointer: CPU2 reads applied_seq to gate the next
    // CAL_WRITE/COMMIT, so result/fail_idx must be ordered before it.
    volatile v2k_param_status_t *status = &g_v2k_cpu1_plane.param_status;
    uint32_t seq_before;
    uint32_t seq_after;
    uint16_t i;
    uint16_t result = V2K_CAL_OK;

    if (s_ready_valid)
    {
        return;
    }
    seq_before = shadow->commit_seq;
    if ((seq_before == s_shadow_seen) ||
        (seq_before == status->applied_seq))
    {
        return;
    }

    s_ready.seq = seq_before;
    s_ready.count = shadow->count;
    if (s_ready.count <= V2K_PARAM_BATCH_MAX)
    {
        for (i = 0u; i < s_ready.count; i++)
        {
            s_ready.writes[i].addr = shadow->writes[i].addr;
            s_ready.writes[i].value_bits = shadow->writes[i].value_bits;
            s_ready.writes[i].type = shadow->writes[i].type;
            s_ready.writes[i].reserved = shadow->writes[i].reserved;
        }
    }
    seq_after = shadow->commit_seq;
    if (seq_before != seq_after)
    {
        return;
    }
    s_shadow_seen = seq_after;

    if (s_ready.count > V2K_PARAM_BATCH_MAX)
    {
        result = V2K_CAL_BAD_COUNT;
        i = 0u;
    }
    else
    {
        for (i = 0u; i < s_ready.count; i++)
        {
            result = v2k_validate_write(&s_ready.writes[i]);
            if (result != V2K_CAL_OK)
            {
                break;
            }
        }
    }

    if (result != V2K_CAL_OK)
    {
        status->result = result;
        status->fail_idx = i;
        status->applied_seq = s_ready.seq;
        return;
    }

    s_ready_valid = 1u;
}

static void v2k_write_value(const volatile v2k_param_write_t *write)
{
    volatile uint16_t *dst16 = (volatile uint16_t *)write->addr;

    if ((write->type == V2K_TYPE_I16) || (write->type == V2K_TYPE_U16))
    {
        *dst16 = (uint16_t)write->value_bits;
    }
    else
    {
        volatile uint32_t *dst32 = (volatile uint32_t *)write->addr;
        *dst32 = write->value_bits;
    }
}

void v2k_param_apply_ready(void)
{
    volatile v2k_param_status_t *status = &g_v2k_cpu1_plane.param_status;
    uint16_t i;

    if (!s_ready_valid)
    {
        return;
    }
    for (i = 0u; i < s_ready.count; i++)
    {
        v2k_write_value(&s_ready.writes[i]);
    }
    status->result = V2K_CAL_OK;
    status->fail_idx = 0u;
    status->applied_seq = s_ready.seq;
    s_ready_valid = 0u;
}

static uint16_t v2k_validate_read(const v2k_param_read_ref_t *ref)
{
    if (ref->type >= V2K_TYPE_COUNT)
    {
        return V2K_CAL_BAD_TYPE;
    }
    if (!v2k_addr_is_readable(ref->addr, ref->type))
    {
        return V2K_CAL_BAD_ADDR;
    }
    return V2K_CAL_OK;
}

static uint32_t v2k_read_addr(uint32_t addr, uint16_t type)
{
    const volatile uint16_t *src16 = (const volatile uint16_t *)addr;

    if ((type == V2K_TYPE_I16) || (type == V2K_TYPE_U16))
    {
        uint16_t value = *src16;
        if ((type == V2K_TYPE_I16) && ((value & 0x8000u) != 0u))
        {
            return 0xFFFF0000uL | value;
        }
        return value;
    }
    return *(const volatile uint32_t *)addr;
}

void v2k_param_read_service(void)
{
    const volatile v2k_param_read_req_t *req = &V2K_CPU2_PLANE_RO->param_read_req;
    // Volatile response pointer: CPU2 busy-polls ack_seq, so result/count/
    // value stores must be ordered before the ack publish.
    volatile v2k_param_read_resp_t *resp = &g_v2k_cpu1_plane.param_read_resp;
    uint32_t seq_before;
    uint32_t seq_after;
    uint16_t i;
    uint16_t count;
    uint16_t result = V2K_CAL_OK;

    seq_before = req->read_seq;
    if (seq_before == resp->ack_seq)
    {
        return;
    }
    count = req->count;
    if ((count > 0u) && (count <= V2K_CAL_READ_MAX))
    {
        for (i = 0u; i < count; i++)
        {
            s_read_refs[i] = req->refs[i];
        }
    }
    seq_after = req->read_seq;
    if (seq_before != seq_after)
    {
        return;
    }

    if ((count == 0u) || (count > V2K_CAL_READ_MAX))
    {
        result = V2K_CAL_BAD_COUNT;
        count = 0u;
    }
    else
    {
        for (i = 0u; i < count; i++)
        {
            result = v2k_validate_read(&s_read_refs[i]);
            if (result != V2K_CAL_OK)
            {
                break;
            }
        }
    }

    resp->result = result;
    resp->count = (result == V2K_CAL_OK) ? count : 0u;
    if (result == V2K_CAL_OK)
    {
        for (i = 0u; i < count; i++)
        {
            resp->value_bits[i] =
                v2k_read_addr(s_read_refs[i].addr, s_read_refs[i].type);
        }
    }
    resp->ack_seq = seq_before;
}
