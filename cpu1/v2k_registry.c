//=============================================================================
// v2k_registry.c - 描述符表与后台验证/ISR 应用参数批次
//=============================================================================

#include <string.h>
#include "v2k_registry.h"
#include "v2k_shared.h"
#include "v2k_platform.h"
#include "v2k_timebase.h"
#include "v2k_fault.h"

extern uint16_t V2K_BssStart;
extern uint16_t V2K_BssEnd;
extern uint16_t V2K_BssOutputStart;
extern uint16_t V2K_BssOutputEnd;
extern uint16_t V2K_DataStart;
extern uint16_t V2K_DataEnd;

extern volatile float g_v2k_adc_a0_v;
extern volatile float g_v2k_pwm_duty_cmd;
extern volatile float g_v2k_pwm_duty_applied;
extern volatile uint32_t g_v2k_isr_cycles;
extern volatile uint32_t g_v2k_isr_cycles_max;
extern volatile uint32_t g_v2k_control_cycles_max;
extern volatile uint32_t g_v2k_scope_cycles_max;
extern volatile uint32_t g_v2k_isr_budget_violation_cnt;
extern volatile uint32_t g_v2k_scope_overrun_total;
extern volatile uint16_t g_v2k_due_mask;
extern volatile uint32_t g_v2k_tz_int_cnt;
extern uint16_t g_cpu2_alive;

typedef union {
    uint32_t u32;
    int32_t i32;
    float f32;
} v2k_value_u;

typedef struct {
    uint32_t seq;
    uint16_t count;
    uint16_t unguarded;
    v2k_param_write_t writes[V2K_PARAM_BATCH_MAX];
} v2k_param_ready_t;

static v2k_param_ready_t s_ready;
static volatile uint16_t s_ready_valid;
static uint32_t s_shadow_seen;

static uint32_t v2k_addr(const volatile void *ptr)
{
    return (uint32_t)ptr;
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
    uint16_t words;

    if (type >= V2K_TYPE_COUNT)
    {
        return 0u;
    }
    words = ((type == V2K_TYPE_I16) || (type == V2K_TYPE_U16)) ? 1u : 2u;
    if ((words == 2u) && ((addr & 1u) != 0u))
    {
        return 0u;
    }
    return (uint16_t)(
        v2k_addr_in_range(addr, words, &V2K_BssStart, &V2K_BssEnd) ||
        v2k_addr_in_range(addr, words, &V2K_BssOutputStart, &V2K_BssOutputEnd) ||
        v2k_addr_in_range(addr, words, &V2K_DataStart, &V2K_DataEnd));
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

static void v2k_desc_add(const char *name, uint16_t type, uint16_t kind,
                         volatile void *addr, float min_val, float max_val,
                         float scale, float offset, uint16_t prescaler,
                         uint16_t group)
{
    v2k_desc_table_t *table = &g_v2k_gs0.desc_table;
    uint16_t idx = table->hdr.entry_count;
    v2k_desc_entry_t *entry;

    if (idx >= V2K_DESC_MAX)
    {
        return;
    }
    entry = &table->entries[idx];
    memset(entry, 0, sizeof(*entry));
    v2k_desc_name(entry->name, name);
    entry->type = type;
    entry->kind = kind;
    entry->addr = v2k_addr(addr);
    entry->min_val = min_val;
    entry->max_val = max_val;
    entry->scale = scale;
    entry->offset = offset;
    entry->prescaler = prescaler;
    entry->group = group;
    table->hdr.entry_count = (uint16_t)(idx + 1u);
}

void v2k_registry_init(v2k_build_hash_t build_hash)
{
    v2k_desc_table_t *table = &g_v2k_gs0.desc_table;
    uint16_t slow_div = (uint16_t)(V2K_ISR_HZ / 1000u);

    memset(table, 0, sizeof(*table));
    table->hdr.contract_ver = V2K_CONTRACT_VER;
    table->hdr.build_hash = build_hash;
    table->hdr.entry_stride_words = (uint16_t)sizeof(v2k_desc_entry_t);

    v2k_desc_add("adc_a0_raw", V2K_TYPE_U16, V2K_KIND_SCOPE,
                 &g_v2k_adc_a0, 0.0f, 4095.0f, 1.0f, 0.0f, 1u, 0u);
    v2k_desc_add("adc_a0_v", V2K_TYPE_F32, V2K_KIND_SCOPE,
                 &g_v2k_adc_a0_v, 0.0f, 3.0f, 1.0f, 0.0f, 1u, 0u);
    v2k_desc_add("pwm1_duty_cmd", V2K_TYPE_F32,
                 V2K_KIND_PARAM | V2K_KIND_SCOPE,
                 &g_v2k_pwm_duty_cmd, 0.02f, 0.98f, 1.0f, 0.0f, 1u, 0u);
    v2k_desc_add("pwm1_duty", V2K_TYPE_F32, V2K_KIND_SCOPE,
                 &g_v2k_pwm_duty_applied, 0.0f, 1.0f, 1.0f, 0.0f, 1u, 0u);
    v2k_desc_add("isr_cycles", V2K_TYPE_U32, V2K_KIND_SCOPE,
                 &g_v2k_isr_cycles, 0.0f, 0.0f, 1.0f, 0.0f, 1u, 0u);
    v2k_desc_add("isr_latency", V2K_TYPE_U16, V2K_KIND_SCOPE,
                 &g_v2k_isr_lat, 0.0f, 0.0f, 1.0f, 0.0f, 1u, 0u);
    v2k_desc_add("due_mask", V2K_TYPE_U16, V2K_KIND_SCOPE,
                 &g_v2k_due_mask, 0.0f, 0.0f, 1.0f, 0.0f, 1u, 0u);
    v2k_desc_add("sys_state", V2K_TYPE_U16, V2K_KIND_SCOPE,
                 &g_v2k_sm_state, 0.0f, 0.0f, 1.0f, 0.0f, 1u, 0u);

    v2k_desc_add("fault_code", V2K_TYPE_U16, V2K_KIND_SCOPE,
                 &g_v2k_fault_code, 0.0f, 0.0f, 1.0f, 0.0f, slow_div, 1u);
    v2k_desc_add("cpu2_alive", V2K_TYPE_U16, V2K_KIND_SCOPE,
                 &g_cpu2_alive, 0.0f, 1.0f, 1.0f, 0.0f, slow_div, 1u);
    v2k_desc_add("isr_overflow", V2K_TYPE_U32, V2K_KIND_SCOPE,
                 &g_v2k_isr_ovf_cnt, 0.0f, 0.0f, 1.0f, 0.0f, slow_div, 1u);
    v2k_desc_add("isr_budget", V2K_TYPE_U32, V2K_KIND_SCOPE,
                 &g_v2k_isr_budget_violation_cnt, 0.0f, 0.0f,
                 1.0f, 0.0f, slow_div, 1u);
    v2k_desc_add("isr_cycles_max", V2K_TYPE_U32, V2K_KIND_SCOPE,
                 &g_v2k_isr_cycles_max, 0.0f, 0.0f,
                 1.0f, 0.0f, slow_div, 1u);
    v2k_desc_add("ctrl_cycles_max", V2K_TYPE_U32, V2K_KIND_SCOPE,
                 &g_v2k_control_cycles_max, 0.0f, 0.0f,
                 1.0f, 0.0f, slow_div, 1u);
    v2k_desc_add("scope_cyc_max", V2K_TYPE_U32, V2K_KIND_SCOPE,
                 &g_v2k_scope_cycles_max, 0.0f, 0.0f,
                 1.0f, 0.0f, slow_div, 1u);
    v2k_desc_add("scope_overrun", V2K_TYPE_U32, V2K_KIND_SCOPE,
                 &g_v2k_scope_overrun_total, 0.0f, 0.0f,
                 1.0f, 0.0f, slow_div, 1u);
    v2k_desc_add("tz_trip_cnt", V2K_TYPE_U32, V2K_KIND_SCOPE,
                 &g_v2k_tz_int_cnt, 0.0f, 0.0f, 1.0f, 0.0f, slow_div, 1u);

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

static float v2k_value_as_float(uint16_t type, uint32_t bits)
{
    v2k_value_u value;
    value.u32 = bits;
    switch (type)
    {
        case V2K_TYPE_I16: return (float)(int16_t)bits;
        case V2K_TYPE_U16: return (float)(uint16_t)bits;
        case V2K_TYPE_I32: return (float)value.i32;
        case V2K_TYPE_U32: return (float)value.u32;
        default: return value.f32;
    }
}

static uint16_t v2k_validate_write(const v2k_param_write_t *write,
                                   uint16_t *unguarded)
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
        (*unguarded)++;
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
    {
        float value = v2k_value_as_float(write->type, write->value_bits);
        if (!((value >= entry->min_val) && (value <= entry->max_val)))
        {
            return V2K_CAL_OUT_RANGE;
        }
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
    candidate.unguarded = 0u;
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
            result = v2k_validate_write(&candidate.writes[i],
                                        &candidate.unguarded);
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
    g_v2k_gs0.param_status.unguarded_cnt =
        (uint16_t)(g_v2k_gs0.param_status.unguarded_cnt + s_ready.unguarded);
    g_v2k_gs0.param_status.result = V2K_CAL_OK;
    g_v2k_gs0.param_status.fail_idx = 0u;
    g_v2k_gs0.param_status.applied_seq = s_ready.seq;
    s_ready_valid = 0u;
}

static uint32_t v2k_read_value(const v2k_desc_entry_t *entry)
{
    const volatile uint16_t *src16 = (const volatile uint16_t *)entry->addr;

    if ((entry->type == V2K_TYPE_I16) || (entry->type == V2K_TYPE_U16))
    {
        uint16_t value = *src16;
        if ((entry->type == V2K_TYPE_I16) && ((value & 0x8000u) != 0u))
        {
            return 0xFFFF0000uL | value;
        }
        return value;
    }
    return *(const volatile uint32_t *)entry->addr;
}

void v2k_param_refresh_mirror(void)
{
    v2k_desc_table_t *table = &g_v2k_gs0.desc_table;
    uint16_t i;

    for (i = 0u; i < table->hdr.entry_count; i++)
    {
        g_v2k_gs0.param_status.value_mirror[i] =
            v2k_read_value(&table->entries[i]);
    }
    g_v2k_gs0.param_status.mirror_seq++;
}
