//=============================================================================
// v2k_registry.c - descriptor table + background validation / ISR-applied parameter batches
//=============================================================================

#include <string.h>
#include "v2k_registry.h"
#include "v2k_shared.h"
#include "../v2k.h"
#include "../wire/wire.h"
#include "v2k_timebase.h"
#include "v2k_fault.h"
#include "v2k_user_runtime.h"

extern uint16_t V2K_BssStart;
extern uint16_t V2K_BssEnd;
extern uint16_t V2K_BssOutputStart;
extern uint16_t V2K_BssOutputEnd;
extern uint16_t V2K_DataStart;
extern uint16_t V2K_DataEnd;
extern uint16_t V2K_UserDataStart;
extern uint16_t V2K_UserDataEnd;
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

static v2k_param_ready_t s_ready;
static volatile uint16_t s_ready_valid;
static uint32_t s_shadow_seen;
static v2k_param_read_ref_t s_read_refs[V2K_CAL_READ_MAX];
volatile uint16_t g_v2k_desc_error;

// Fixed-size post-link patch target; its initialized header is validated before baking.
#pragma DATA_SECTION(g_v2k_user_desc_blob, "v2k_user_desc")
const v2k_user_desc_blob_t g_v2k_user_desc_blob = {
    V2K_USER_DESC_MAGIC,
    V2K_USER_DESC_VERSION,
    0u,
    V2K_USER_DESC_MAX,
    0u,
    0uL,
    {{0}}
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
        v2k_addr_in_range(addr, words, &V2K_UserDataStart, &V2K_UserDataEnd) ||
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

static void v2k_desc_name(char dst[V2K_NAME_LEN], const char *src)
{
    uint16_t i;
    for (i = 0u; i < V2K_NAME_LEN; i++)
    {
        char c = src[i];
        dst[i] = c;
        if (c == '\0')
        {
            i++;
            break;
        }
    }
    while (i < V2K_NAME_LEN)
    {
        dst[i++] = '\0';
    }
}

void v2k_registry_add(const char *name, uint16_t type, uint16_t kind,
                      volatile void *addr, uint16_t prescaler)
{
    v2k_desc_table_t *table = &g_v2k_gs0.desc_table;
    uint16_t idx = table->hdr.entry_count;
    v2k_desc_entry_t *entry;

    if (idx >= V2K_PLATFORM_DESC_MAX)
    {
        g_v2k_desc_error = V2K_DESC_ERROR_PLATFORM_CAPACITY;
        return;
    }
    entry = &table->entries[idx];
    memset(entry, 0, sizeof(*entry));
    v2k_desc_name(entry->name, name);
    entry->type = type;
    entry->kind = kind;
    entry->addr = v2k_addr(addr);
    entry->prescaler = prescaler;
    entry->reserved = 0u;
    table->hdr.entry_count = (uint16_t)(idx + 1u);
}

static uint16_t v2k_user_desc_blob_valid(void)
{
    const v2k_user_desc_blob_t *blob = &g_v2k_user_desc_blob;

    return (uint16_t)(
        (blob->magic == V2K_USER_DESC_MAGIC) &&
        (blob->version == V2K_USER_DESC_VERSION) &&
        (blob->capacity == V2K_USER_DESC_MAX) &&
        (blob->count <= blob->capacity) &&
        (blob->build_hash != 0uL));
}

static uint16_t v2k_user_desc_entry_valid(const v2k_desc_entry_t *entry)
{
    uint16_t i;
    uint16_t words;
    uint16_t terminated = 0u;

    words = v2k_type_words(entry->type);
    if ((words == 0u) ||
        ((words == 2u) && ((entry->addr & 1u) != 0u)) ||
        (entry->prescaler == 0u) ||
        (entry->reserved != 0u))
    {
        return 0u;
    }
    for (i = 0u; i < V2K_NAME_LEN; i++)
    {
        if (entry->name[i] == '\0')
        {
            terminated = (i > 0u) ? 1u : 0u;
            break;
        }
        if ((uint16_t)entry->name[i] > 0x7Fu)
        {
            return 0u;
        }
    }
    if (!terminated)
    {
        return 0u;
    }
    if (entry->kind ==
        (V2K_KIND_USER | V2K_KIND_PARAM | V2K_KIND_SCOPE))
    {
        return (uint16_t)(
            v2k_addr_in_range(entry->addr, words,
                              &V2K_UserDataStart, &V2K_UserDataEnd) ||
            v2k_addr_in_range(entry->addr, words,
                              &V2K_UserBssStart, &V2K_UserBssEnd));
    }
    if (entry->kind == (V2K_KIND_USER | V2K_KIND_SCOPE))
    {
        return v2k_addr_in_range(entry->addr, words,
                                 &V2K_UserConstStart, &V2K_UserConstEnd);
    }
    return 0u;
}

static void v2k_registry_add_baked_user(void)
{
    const v2k_user_desc_blob_t *blob = &g_v2k_user_desc_blob;
    v2k_desc_table_t *table = &g_v2k_gs0.desc_table;
    uint16_t i;

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
    if (blob->count > (V2K_DESC_MAX - table->hdr.entry_count))
    {
        g_v2k_desc_error = V2K_DESC_ERROR_TABLE_CAPACITY;
        return;
    }
    for (i = 0u; i < blob->count; i++)
    {
        table->entries[table->hdr.entry_count] = blob->entries[i];
        table->hdr.entry_count++;
    }
}

void v2k_registry_init(void)
{
    v2k_desc_table_t *table = &g_v2k_gs0.desc_table;
    uint16_t slow_div = (uint16_t)(V2K_ISR_HZ / 1000u);

    g_v2k_desc_error = V2K_DESC_ERROR_NONE;
    memset(table, 0, sizeof(*table));
    table->hdr.contract_ver = V2K_CONTRACT_VER;
    if (v2k_user_desc_blob_valid())
    {
        table->hdr.build_hash = g_v2k_user_desc_blob.build_hash;
    }
    else
    {
        g_v2k_desc_error = V2K_DESC_ERROR_BLOB_HEADER;
    }
    table->hdr.entry_stride_words = (uint16_t)sizeof(v2k_desc_entry_t);

    v2k_registry_add("desc_error", V2K_TYPE_U16, V2K_KIND_SCOPE,
                     &g_v2k_desc_error, 1u);
    wire_register_ports(1u);
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
    // scope_cyc_max is intentionally not registered: the scope-segment cycle
    // count can be derived as isr_cycles_max − ctrl_cycles_max, freeing this
    // slot for the protection signal tz_trip_cnt (g_v2k_scope_cycles_max is
    // still directly viewable in CCS).
    v2k_registry_add("scope_overrun", V2K_TYPE_U32, V2K_KIND_SCOPE,
                     &g_v2k_scope_overrun_total, slow_div);
    v2k_registry_add("tz_trip_cnt", V2K_TYPE_U32, V2K_KIND_SCOPE,
                     &g_v2k_tz_int_cnt, slow_div);
    v2k_registry_add_baked_user();

    table->hdr.magic = V2K_DESC_MAGIC;
}

static const v2k_desc_entry_t *v2k_desc_find(uint32_t addr)
{
    const v2k_desc_table_t *table = &g_v2k_gs0.desc_table;
    uint16_t i;

    for (i = 0u; i < table->hdr.entry_count; i++)
    {
        if (table->entries[i].addr == addr)
        {
            return &table->entries[i];
        }
    }
    return (const v2k_desc_entry_t *)0;
}

static uint16_t v2k_validate_write(const v2k_param_write_t *write)
{
    const v2k_desc_entry_t *entry;

    if (write->type >= V2K_TYPE_COUNT)
    {
        return V2K_CAL_BAD_TYPE;
    }
    if (!v2k_addr_is_writable(write->addr, write->type))
    {
        return V2K_CAL_BAD_ADDR;
    }

    entry = v2k_desc_find(write->addr);
    if (entry == (const v2k_desc_entry_t *)0)
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
    const volatile v2k_param_shadow_t *shadow = &V2K_GS4_RO->param_shadow;
    v2k_param_ready_t candidate;
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
        (seq_before == g_v2k_gs0.param_status.applied_seq))
    {
        return;
    }

    candidate.seq = seq_before;
    candidate.count = shadow->count;
    if (candidate.count <= V2K_PARAM_BATCH_MAX)
    {
        for (i = 0u; i < candidate.count; i++)
        {
            candidate.writes[i] = shadow->writes[i];
        }
    }
    seq_after = shadow->commit_seq;
    if (seq_before != seq_after)
    {
        return;
    }
    s_shadow_seen = seq_after;

    if (candidate.count > V2K_PARAM_BATCH_MAX)
    {
        result = V2K_CAL_BAD_COUNT;
        i = 0u;
    }
    else
    {
        for (i = 0u; i < candidate.count; i++)
        {
            result = v2k_validate_write(&candidate.writes[i]);
            if (result != V2K_CAL_OK)
            {
                break;
            }
        }
    }

    if (result != V2K_CAL_OK)
    {
        g_v2k_gs0.param_status.result = result;
        g_v2k_gs0.param_status.fail_idx = i;
        g_v2k_gs0.param_status.applied_seq = candidate.seq;
        return;
    }

    s_ready = candidate;
    s_ready_valid = 1u;
}

static void v2k_write_value(const v2k_param_write_t *write)
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
    uint16_t i;

    if (!s_ready_valid)
    {
        return;
    }
    for (i = 0u; i < s_ready.count; i++)
    {
        v2k_write_value(&s_ready.writes[i]);
    }
    g_v2k_gs0.param_status.result = V2K_CAL_OK;
    g_v2k_gs0.param_status.fail_idx = 0u;
    g_v2k_gs0.param_status.applied_seq = s_ready.seq;
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
    const volatile v2k_param_read_req_t *req = &V2K_GS4_RO->param_read_req;
    uint32_t seq_before;
    uint32_t seq_after;
    uint16_t i;
    uint16_t count;
    uint16_t result = V2K_CAL_OK;

    seq_before = req->read_seq;
    if (seq_before == g_v2k_gs0.param_read_resp.ack_seq)
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

    g_v2k_gs0.param_read_resp.result = result;
    g_v2k_gs0.param_read_resp.count =
        (result == V2K_CAL_OK) ? count : 0u;
    if (result == V2K_CAL_OK)
    {
        for (i = 0u; i < count; i++)
        {
            g_v2k_gs0.param_read_resp.value_bits[i] =
                v2k_read_addr(s_read_refs[i].addr, s_read_refs[i].type);
        }
    }
    g_v2k_gs0.param_read_resp.ack_seq = seq_before;
}
