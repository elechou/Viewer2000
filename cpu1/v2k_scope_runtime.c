//=============================================================================
// v2k_scope_runtime.c - LIVE/SNAPSHOT 示波生产者
//=============================================================================

#include <string.h>
#include "v2k_scope_runtime.h"
#include "v2k_shared.h"
#include "v2k_timebase.h"

extern uint16_t V2K_BssStart;
extern uint16_t V2K_BssEnd;
extern uint16_t V2K_BssOutputStart;
extern uint16_t V2K_BssOutputEnd;
extern uint16_t V2K_DataStart;
extern uint16_t V2K_DataEnd;

#pragma DATA_SECTION(g_v2k_scope_slow, "v2k_scope_slow")
volatile uint16_t g_v2k_scope_slow[V2K_SCOPE_SLOW_WORDS];

#pragma DATA_SECTION(g_v2k_scope_fast, "v2k_scope_fast")
volatile uint16_t g_v2k_scope_fast[V2K_SCOPE_FAST_WORDS];

#pragma DATA_SECTION(g_v2k_ccs_view, "v2k_ccs_view")
v2k_ccs_view_t g_v2k_ccs_view;
volatile uint32_t g_v2k_scope_overrun_total;

typedef struct {
    v2k_scope_ch_bind_t bind[V2K_SCOPE_MAX_CH];
    uint16_t stride_words;
    uint16_t prescale_count;
    uint16_t sample_in_block;
    uint16_t block_seq;
    uint16_t active_bind_seq;
    uint16_t drop_block;
    uint16_t prev_valid;
    float prev_trigger;
    uint32_t post_remaining;
    uint16_t published_count;
} v2k_scope_group_t;

typedef struct {
    v2k_scope_cfg_t cfg;
    uint16_t pending;
    uint16_t result;
} v2k_scope_cfg_pending_t;

typedef struct {
    v2k_scope_bind_t bind;
    uint16_t pending;
    uint16_t result;
} v2k_scope_bind_pending_t;

static v2k_scope_group_t s_group[V2K_SCOPE_MAX_GROUPS];
static v2k_scope_cfg_t s_active_cfg[V2K_SCOPE_MAX_GROUPS];
static v2k_scope_cfg_pending_t s_cfg_pending[V2K_SCOPE_MAX_GROUPS];
static v2k_scope_bind_pending_t s_bind_pending[V2K_SCOPE_MAX_GROUPS];
static uint16_t s_cfg_seen[V2K_SCOPE_MAX_GROUPS];
static uint16_t s_bind_seen[V2K_SCOPE_MAX_GROUPS];
static volatile uint16_t s_group_active[V2K_SCOPE_MAX_GROUPS];
static volatile uint16_t s_cons_rd_cache[V2K_SCOPE_MAX_GROUPS];

static uint16_t v2k_addr_in_range(uint32_t addr, uint16_t words,
                                  const uint16_t *start, const uint16_t *end)
{
    uint32_t first = (uint32_t)start;
    uint32_t limit = (uint32_t)end;
    return ((addr >= first) && ((addr + words) <= limit)) ? 1u : 0u;
}

static uint16_t v2k_scope_addr_valid(uint32_t addr, uint16_t type)
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

static uint16_t v2k_floor_pow2(uint32_t value)
{
    uint16_t result;
    uint16_t next = 1u;
    if (value == 0u)
    {
        return 0u;
    }
    result = 1u;
    while (((uint32_t)next << 1u) <= value)
    {
        next = (uint16_t)(next << 1u);
        result = next;
    }
    return result;
}

static uint32_t v2k_group_pool_words(uint16_t group)
{
    if (group == 0u)
    {
        return V2K_SCOPE_FAST_WORDS;
    }
    if (group == 1u)
    {
        return V2K_SCOPE_SLOW_WORDS;
    }
    return 0u;
}

static uint32_t v2k_group_pool_base(uint16_t group)
{
    if (group == 0u)
    {
        return (uint32_t)g_v2k_scope_fast;
    }
    if (group == 1u)
    {
        return (uint32_t)g_v2k_scope_slow;
    }
    return 0u;
}

static uint16_t v2k_stride_words(const v2k_scope_ch_bind_t *bind,
                                 uint16_t n_ch)
{
    uint16_t i;
    uint16_t stride = 0u;
    for (i = 0u; i < n_ch; i++)
    {
        stride = (uint16_t)(stride +
            (((bind[i].type == V2K_TYPE_I16) ||
              (bind[i].type == V2K_TYPE_U16)) ? 1u : 2u));
    }
    return stride;
}

static uint16_t v2k_scope_layout(uint16_t group, uint16_t n_ticks,
                                 uint16_t stride_words, uint16_t *slot_words,
                                 uint16_t *capacity)
{
    uint32_t pool_words = v2k_group_pool_words(group);
    uint32_t slot;
    uint16_t cap;

    if ((pool_words == 0u) || (n_ticks == 0u) || (stride_words == 0u))
    {
        return V2K_SCOPE_RESULT_NO_CAPACITY;
    }
    slot = V2K_BLOCK_WORDS(n_ticks, stride_words);
    if ((slot & 1u) != 0u)
    {
        slot++;
    }
    if ((slot > 0xFFFFuL) || (slot > pool_words))
    {
        return V2K_SCOPE_RESULT_NO_CAPACITY;
    }
    cap = v2k_floor_pow2(pool_words / slot);
    if (cap == 0u)
    {
        return V2K_SCOPE_RESULT_NO_CAPACITY;
    }
    *slot_words = (uint16_t)slot;
    *capacity = cap;
    return V2K_SCOPE_RESULT_OK;
}

static void v2k_scope_transition(v2k_scope_prod_t *prod, uint16_t mode)
{
    if (prod->mode != mode)
    {
        prod->mode = mode;
        prod->state_seq++;
    }
}

static void v2k_default_bind(uint16_t group)
{
    const v2k_desc_table_t *table = &g_v2k_gs0.desc_table;
    v2k_scope_group_t *runtime = &s_group[group];
    v2k_scope_prod_t *prod = &g_v2k_gs0.scope_prod[group];
    uint16_t i;
    uint16_t n = 0u;
    uint16_t slot_words = 0u;
    uint16_t capacity = 0u;

    for (i = 0u; (i < table->hdr.entry_count) && (n < V2K_SCOPE_MAX_CH); i++)
    {
        const v2k_desc_entry_t *entry = &table->entries[i];
        if (((entry->kind & V2K_KIND_SCOPE) != 0u) && (entry->group == group))
        {
            runtime->bind[n].addr = entry->addr;
            runtime->bind[n].type = entry->type;
            runtime->bind[n].reserved = 0u;
            n++;
        }
    }
    runtime->stride_words = v2k_stride_words(runtime->bind, n);
    prod->n_ch = n;
    if (v2k_scope_layout(group, prod->block_n_ticks, runtime->stride_words,
                         &slot_words, &capacity) == V2K_SCOPE_RESULT_OK)
    {
        prod->block_slot_words = slot_words;
        prod->ring_capacity = capacity;
    }
}

void v2k_scope_init(void)
{
    uint16_t group;
    uint16_t slow_div = (uint16_t)(V2K_ISR_HZ / 1000u);

    memset((void *)g_v2k_scope_fast, 0, sizeof(g_v2k_scope_fast));
    memset((void *)g_v2k_scope_slow, 0, sizeof(g_v2k_scope_slow));
    memset(s_group, 0, sizeof(s_group));
    memset(s_active_cfg, 0, sizeof(s_active_cfg));
    memset(s_cfg_pending, 0, sizeof(s_cfg_pending));
    memset(s_bind_pending, 0, sizeof(s_bind_pending));
    memset((void *)s_group_active, 0, sizeof(s_group_active));
    memset((void *)s_cons_rd_cache, 0, sizeof(s_cons_rd_cache));
    memset(&g_v2k_ccs_view, 0, sizeof(g_v2k_ccs_view));

    for (group = 0u; group < V2K_SCOPE_MAX_GROUPS; group++)
    {
        v2k_scope_prod_t *prod = &g_v2k_gs0.scope_prod[group];
        memset(prod, 0, sizeof(*prod));
        prod->mode = V2K_SCOPE_OFF;
        prod->block_n_ticks = V2K_BLOCK_NTICKS_SCI;
        prod->prescaler = (group == 1u) ? slow_div : 1u;
        prod->ring_base = v2k_group_pool_base(group);
        prod->cfg_result = V2K_SCOPE_RESULT_OK;
        prod->bind_result = V2K_SCOPE_RESULT_OK;
        s_active_cfg[group].trig_edge = V2K_TRIG_RISE;
        s_active_cfg[group].block_n_ticks = V2K_BLOCK_NTICKS_SCI;
        if (group < 2u)
        {
            v2k_default_bind(group);
        }
    }
}

static uint16_t v2k_validate_bind(uint16_t group,
                                  const v2k_scope_bind_t *bind)
{
    uint16_t i;
    uint16_t stride;
    uint16_t slot_words;
    uint16_t capacity;

    if ((group >= V2K_SCOPE_MAX_GROUPS) || (bind->n_ch == 0u) ||
        (bind->n_ch > V2K_SCOPE_MAX_CH))
    {
        return V2K_SCOPE_RESULT_BAD_PARAM;
    }
    for (i = 0u; i < bind->n_ch; i++)
    {
        if (bind->ch[i].type >= V2K_TYPE_COUNT)
        {
            return V2K_SCOPE_RESULT_BAD_TYPE;
        }
        if (!v2k_scope_addr_valid(bind->ch[i].addr, bind->ch[i].type))
        {
            return V2K_SCOPE_RESULT_BAD_ADDR;
        }
    }
    stride = v2k_stride_words(bind->ch, bind->n_ch);
    return v2k_scope_layout(group,
        g_v2k_gs0.scope_prod[group].block_n_ticks, stride,
        &slot_words, &capacity);
}

static uint16_t v2k_validate_cfg(uint16_t group, const v2k_scope_cfg_t *cfg)
{
    const v2k_scope_prod_t *prod;
    uint16_t n_ticks;
    uint16_t slot_words;
    uint16_t capacity;

    if (group >= V2K_SCOPE_MAX_GROUPS)
    {
        return V2K_SCOPE_RESULT_BAD_PARAM;
    }
    prod = &g_v2k_gs0.scope_prod[group];
    if ((cfg->mode_req != V2K_SCOPE_OFF) &&
        (cfg->mode_req != V2K_SCOPE_LIVE) &&
        (cfg->mode_req != V2K_SCOPE_SNAP_ARMED))
    {
        return V2K_SCOPE_RESULT_BAD_PARAM;
    }
    if (cfg->mode_req == V2K_SCOPE_OFF)
    {
        return V2K_SCOPE_RESULT_OK;
    }
    if ((prod->n_ch == 0u) || (cfg->pre_trig_pct > 100u) ||
        (cfg->trig_ch_slot >= prod->n_ch) ||
        ((cfg->trig_edge != V2K_TRIG_RISE) &&
         (cfg->trig_edge != V2K_TRIG_FALL)))
    {
        return V2K_SCOPE_RESULT_BAD_PARAM;
    }
    n_ticks = (cfg->block_n_ticks == 0u) ?
              prod->block_n_ticks : cfg->block_n_ticks;
    return v2k_scope_layout(group, n_ticks, s_group[group].stride_words,
                            &slot_words, &capacity);
}

void v2k_scope_service(void)
{
    uint16_t group;
    for (group = 0u; group < V2K_SCOPE_MAX_GROUPS; group++)
    {
        const volatile v2k_scope_cfg_t *cfg = &V2K_GS4_RO->scope_cfg[group];
        const volatile v2k_scope_bind_t *bind = &V2K_GS4_RO->scope_bind[group];
        uint16_t seq_before;
        uint16_t seq_after;

        // ISR 不直接读取 CPU2 属主 GS4；缓存稍旧只会保守地多丢新块。
        s_cons_rd_cache[group] = V2K_GS4_RO->scope_cons[group].rd_idx;

        if (!s_cfg_pending[group].pending)
        {
            seq_before = cfg->cfg_seq;
            if ((seq_before != s_cfg_seen[group]) &&
                (seq_before != g_v2k_gs0.scope_prod[group].cfg_ack_seq))
            {
                s_cfg_pending[group].cfg = *cfg;
                seq_after = cfg->cfg_seq;
                if (seq_before == seq_after)
                {
                    s_cfg_seen[group] = seq_after;
                    s_cfg_pending[group].result =
                        v2k_validate_cfg(group, &s_cfg_pending[group].cfg);
                    s_cfg_pending[group].pending = 1u;
                }
            }
        }

        if (!s_bind_pending[group].pending)
        {
            seq_before = bind->bind_seq;
            if ((seq_before != s_bind_seen[group]) &&
                (seq_before != g_v2k_gs0.scope_prod[group].bind_ack_seq))
            {
                s_bind_pending[group].bind = *bind;
                seq_after = bind->bind_seq;
                if (seq_before == seq_after)
                {
                    s_bind_seen[group] = seq_after;
                    s_bind_pending[group].result =
                        v2k_validate_bind(group, &s_bind_pending[group].bind);
                    s_bind_pending[group].pending = 1u;
                }
            }
        }
    }
}

static void v2k_apply_bind(uint16_t group)
{
    v2k_scope_bind_pending_t *pending = &s_bind_pending[group];
    v2k_scope_prod_t *prod = &g_v2k_gs0.scope_prod[group];
    v2k_scope_group_t *runtime = &s_group[group];
    uint16_t result = pending->result;
    uint16_t slot_words = 0u;
    uint16_t capacity = 0u;

    if (prod->mode != V2K_SCOPE_OFF)
    {
        result = V2K_SCOPE_RESULT_BAD_STATE;
    }
    if (result == V2K_SCOPE_RESULT_OK)
    {
        uint16_t i;
        for (i = 0u; i < pending->bind.n_ch; i++)
        {
            runtime->bind[i] = pending->bind.ch[i];
        }
        runtime->stride_words =
            v2k_stride_words(runtime->bind, pending->bind.n_ch);
        result = v2k_scope_layout(group, prod->block_n_ticks,
                                  runtime->stride_words,
                                  &slot_words, &capacity);
        if (result == V2K_SCOPE_RESULT_OK)
        {
            prod->n_ch = pending->bind.n_ch;
            runtime->active_bind_seq = pending->bind.bind_seq;
            prod->block_slot_words = slot_words;
            prod->ring_capacity = capacity;
            prod->wr_idx = 0u;
            runtime->sample_in_block = 0u;
            runtime->published_count = 0u;
        }
    }
    prod->bind_result = result;
    prod->bind_ack_seq = pending->bind.bind_seq;
    pending->pending = 0u;
}

static void v2k_apply_cfg(uint16_t group)
{
    v2k_scope_cfg_pending_t *pending = &s_cfg_pending[group];
    v2k_scope_prod_t *prod = &g_v2k_gs0.scope_prod[group];
    v2k_scope_group_t *runtime = &s_group[group];
    uint16_t result = pending->result;
    uint16_t n_ticks;
    uint16_t prescaler;
    uint16_t slot_words = 0u;
    uint16_t capacity = 0u;

    if ((prod->mode == V2K_SCOPE_SNAP_TRIG) &&
        (pending->cfg.mode_req != V2K_SCOPE_OFF))
    {
        result = V2K_SCOPE_RESULT_BAD_STATE;
    }
    if ((result == V2K_SCOPE_RESULT_OK) &&
        (pending->cfg.mode_req == V2K_SCOPE_OFF))
    {
        // 先从 ISR 热路径摘除，再修改组内运行态。
        s_group_active[group] = 0u;
        runtime->sample_in_block = 0u;
        runtime->drop_block = 0u;
        runtime->post_remaining = 0u;
        v2k_scope_transition(prod, V2K_SCOPE_OFF);
        prod->cfg_result = V2K_SCOPE_RESULT_OK;
        prod->cfg_ack_seq = pending->cfg.cfg_seq;
        pending->pending = 0u;
        return;
    }

    n_ticks = (pending->cfg.block_n_ticks == 0u) ?
              prod->block_n_ticks : pending->cfg.block_n_ticks;
    prescaler = (pending->cfg.prescaler == 0u) ?
                prod->prescaler : pending->cfg.prescaler;
    if (result == V2K_SCOPE_RESULT_OK)
    {
        result = v2k_scope_layout(group, n_ticks, runtime->stride_words,
                                  &slot_words, &capacity);
    }
    if (result == V2K_SCOPE_RESULT_OK)
    {
        prod->prescaler = prescaler;
        prod->block_n_ticks = n_ticks;
        prod->block_slot_words = slot_words;
        prod->ring_capacity = capacity;
        runtime->sample_in_block = 0u;
        runtime->drop_block = 0u;
        runtime->prescale_count = 1u;
        runtime->prev_valid = 0u;
        runtime->post_remaining = 0u;
        runtime->published_count = 0u;
        prod->frozen_count = 0u;
        prod->frozen_end_idx = 0u;
        s_active_cfg[group] = pending->cfg;
        s_active_cfg[group].prescaler = prescaler;
        s_active_cfg[group].block_n_ticks = n_ticks;
        if (pending->cfg.mode_req == V2K_SCOPE_LIVE)
        {
            prod->wr_idx = s_cons_rd_cache[group];
        }
        else if (pending->cfg.mode_req == V2K_SCOPE_SNAP_ARMED)
        {
            prod->wr_idx = 0u;
        }
        v2k_scope_transition(prod, pending->cfg.mode_req);
        // 所有运行态字段就绪后，最后发布给 ISR。
        s_group_active[group] = 1u;
    }
    prod->cfg_result = result;
    prod->cfg_ack_seq = pending->cfg.cfg_seq;
    pending->pending = 0u;
}

void v2k_scope_apply_ready(void)
{
    uint16_t group;
    for (group = 0u; group < V2K_SCOPE_MAX_GROUPS; group++)
    {
        if (s_bind_pending[group].pending)
        {
            v2k_apply_bind(group);
        }
        if (s_cfg_pending[group].pending)
        {
            v2k_apply_cfg(group);
        }
    }
}

static volatile uint16_t *v2k_block_words(uint16_t group, uint16_t wr_idx)
{
    v2k_scope_prod_t *prod = &g_v2k_gs0.scope_prod[group];
    return (volatile uint16_t *)prod->ring_base +
           ((uint32_t)(wr_idx & (prod->ring_capacity - 1u)) *
            prod->block_slot_words);
}

static float v2k_source_float(const v2k_scope_ch_bind_t *bind)
{
    const volatile uint16_t *src16 = (const volatile uint16_t *)bind->addr;
    union {
        uint32_t u32;
        int32_t i32;
        float f32;
    } value;

    switch (bind->type)
    {
        case V2K_TYPE_I16: return (float)*(const volatile int16_t *)src16;
        case V2K_TYPE_U16: return (float)*src16;
        case V2K_TYPE_I32:
            value.u32 = *(const volatile uint32_t *)bind->addr;
            return (float)value.i32;
        case V2K_TYPE_U32:
            value.u32 = *(const volatile uint32_t *)bind->addr;
            return (float)value.u32;
        default:
            value.u32 = *(const volatile uint32_t *)bind->addr;
            return value.f32;
    }
}

static void v2k_copy_sample(uint16_t group,
                            const v2k_scope_group_t *runtime,
                            volatile uint16_t *dst)
{
    uint16_t ch;
    for (ch = 0u; ch < g_v2k_gs0.scope_prod[group].n_ch; ch++)
    {
        const v2k_scope_ch_bind_t *bind = &runtime->bind[ch];
        const volatile uint16_t *src = (const volatile uint16_t *)bind->addr;
        *dst++ = *src++;
        if ((bind->type != V2K_TYPE_I16) && (bind->type != V2K_TYPE_U16))
        {
            *dst++ = *src;
        }
    }
}

static void v2k_publish_block(uint16_t group, uint16_t n_ticks)
{
    v2k_scope_prod_t *prod = &g_v2k_gs0.scope_prod[group];
    v2k_scope_group_t *runtime = &s_group[group];
    volatile v2k_block_hdr_t *hdr =
        (volatile v2k_block_hdr_t *)v2k_block_words(group, prod->wr_idx);

    hdr->n_ticks = n_ticks;
    prod->wr_idx++;
    runtime->block_seq++;
    if (runtime->published_count < prod->ring_capacity)
    {
        runtime->published_count++;
    }
    runtime->sample_in_block = 0u;
}

static void v2k_freeze(uint16_t group)
{
    v2k_scope_prod_t *prod = &g_v2k_gs0.scope_prod[group];
    v2k_scope_group_t *runtime = &s_group[group];
    if (runtime->sample_in_block != 0u)
    {
        v2k_publish_block(group, runtime->sample_in_block);
    }
    prod->frozen_end_idx = prod->wr_idx;
    prod->frozen_count = runtime->published_count;
    v2k_scope_transition(prod, V2K_SCOPE_SNAP_FROZEN);
    s_group_active[group] = 0u;
}

static void v2k_scope_sample_group(uint16_t group, v2k_tick_t tick)
{
    v2k_scope_prod_t *prod = &g_v2k_gs0.scope_prod[group];
    v2k_scope_group_t *runtime = &s_group[group];
    uint16_t mode_before = prod->mode;
    volatile uint16_t *block;
    volatile v2k_block_hdr_t *hdr;
    uint16_t hit = 0u;

    if (runtime->prescale_count > 1u)
    {
        runtime->prescale_count--;
        return;
    }
    runtime->prescale_count = prod->prescaler;

    if (runtime->sample_in_block == 0u)
    {
        runtime->drop_block = 0u;
        if ((prod->mode == V2K_SCOPE_LIVE) &&
            ((uint16_t)(prod->wr_idx -
              s_cons_rd_cache[group]) >= prod->ring_capacity))
        {
            runtime->drop_block = 1u;
        }
        if (!runtime->drop_block)
        {
            block = v2k_block_words(group, prod->wr_idx);
            hdr = (volatile v2k_block_hdr_t *)block;
            hdr->start_tick = tick;
            hdr->block_seq = runtime->block_seq;
            hdr->group_id = group;
            hdr->n_ticks = prod->block_n_ticks;
            hdr->n_ch = prod->n_ch;
            hdr->bind_seq = runtime->active_bind_seq;
            hdr->stride_octets = (uint16_t)(runtime->stride_words * 2u);
        }
    }

    if (!runtime->drop_block)
    {
        block = v2k_block_words(group, prod->wr_idx);
        v2k_copy_sample(group, runtime, block + 8u +
            ((uint32_t)runtime->sample_in_block * runtime->stride_words));
    }
    runtime->sample_in_block++;

    if (mode_before == V2K_SCOPE_SNAP_ARMED)
    {
        const v2k_scope_cfg_t *cfg = &s_active_cfg[group];
        float current = v2k_source_float(&runtime->bind[cfg->trig_ch_slot]);
        if (runtime->prev_valid)
        {
            if ((cfg->trig_edge == V2K_TRIG_RISE) &&
                (runtime->prev_trigger < cfg->trig_level) &&
                (current >= cfg->trig_level))
            {
                hit = 1u;
            }
            else if ((cfg->trig_edge == V2K_TRIG_FALL) &&
                     (runtime->prev_trigger > cfg->trig_level) &&
                     (current <= cfg->trig_level))
            {
                hit = 1u;
            }
        }
        runtime->prev_trigger = current;
        runtime->prev_valid = 1u;
    }

    if (runtime->sample_in_block >= prod->block_n_ticks)
    {
        if (runtime->drop_block)
        {
            runtime->sample_in_block = 0u;
            runtime->block_seq++;
            prod->overrun_cnt++;
            g_v2k_scope_overrun_total++;
        }
        else
        {
            v2k_publish_block(group, prod->block_n_ticks);
        }
    }

    if (hit)
    {
        uint32_t total = (uint32_t)prod->ring_capacity * prod->block_n_ticks;
        uint32_t pre = (total * s_active_cfg[group].pre_trig_pct) / 100u;
        uint32_t post = total - pre;
        if (post == 0u)
        {
            post = 1u;
        }
        prod->trig_tick = tick;
        runtime->post_remaining = post - 1u;
        v2k_scope_transition(prod, V2K_SCOPE_SNAP_TRIG);
        if (runtime->post_remaining == 0u)
        {
            v2k_freeze(group);
        }
    }
    else if (mode_before == V2K_SCOPE_SNAP_TRIG)
    {
        if (runtime->post_remaining > 0u)
        {
            runtime->post_remaining--;
        }
        if (runtime->post_remaining == 0u)
        {
            v2k_freeze(group);
        }
    }
}

void v2k_scope_sample_all(v2k_tick_t tick)
{
    // Phase 3 只启用快/慢两组；group 2/3 不进入 ISR 热路径。
    if (s_group_active[0] != 0u)
    {
        v2k_scope_sample_group(0u, tick);
    }
    if (s_group_active[1] != 0u)
    {
        v2k_scope_sample_group(1u, tick);
    }
}

static float v2k_words_float(const volatile uint16_t *src, uint16_t type)
{
    union {
        uint32_t u32;
        int32_t i32;
        float f32;
    } value;
    if (type == V2K_TYPE_I16)
    {
        return (float)*(const volatile int16_t *)src;
    }
    if (type == V2K_TYPE_U16)
    {
        return (float)*src;
    }
    value.u32 = *(const volatile uint32_t *)src;
    if (type == V2K_TYPE_I32)
    {
        return (float)value.i32;
    }
    if (type == V2K_TYPE_U32)
    {
        return (float)value.u32;
    }
    return value.f32;
}

void v2k_scope_ccs_view_service(void)
{
    uint16_t group = g_v2k_ccs_view.group;
    uint16_t slot = g_v2k_ccs_view.channel_slot;
    uint16_t request = g_v2k_ccs_view.request_seq;
    const v2k_scope_prod_t *prod;
    const v2k_scope_group_t *runtime;
    uint16_t idx;
    uint16_t end;
    uint16_t offset = 0u;
    uint16_t ch;
    uint16_t count = 0u;

    if (request == g_v2k_ccs_view.done_seq)
    {
        return;
    }
    g_v2k_ccs_view.result = V2K_SCOPE_RESULT_BAD_STATE;
    g_v2k_ccs_view.count = 0u;
    if (group >= V2K_SCOPE_MAX_GROUPS)
    {
        g_v2k_ccs_view.result = V2K_SCOPE_RESULT_BAD_PARAM;
        g_v2k_ccs_view.done_seq = request;
        return;
    }
    prod = &g_v2k_gs0.scope_prod[group];
    runtime = &s_group[group];
    if ((prod->mode != V2K_SCOPE_SNAP_FROZEN) || (slot >= prod->n_ch))
    {
        g_v2k_ccs_view.done_seq = request;
        return;
    }
    for (ch = 0u; ch < slot; ch++)
    {
        offset = (uint16_t)(offset +
            (((runtime->bind[ch].type == V2K_TYPE_I16) ||
              (runtime->bind[ch].type == V2K_TYPE_U16)) ? 1u : 2u));
    }

    idx = (uint16_t)(prod->frozen_end_idx - prod->frozen_count);
    end = prod->frozen_end_idx;
    while ((idx != end) && (count < V2K_CCS_VIEW_SAMPLES))
    {
        const volatile uint16_t *words = v2k_block_words(group, idx);
        const volatile v2k_block_hdr_t *hdr =
            (const volatile v2k_block_hdr_t *)words;
        uint16_t sample;
        if (count == 0u)
        {
            g_v2k_ccs_view.start_tick = hdr->start_tick;
        }
        for (sample = 0u;
             (sample < hdr->n_ticks) && (count < V2K_CCS_VIEW_SAMPLES);
             sample++)
        {
            const volatile uint16_t *src = words + 8u +
                ((uint32_t)sample * runtime->stride_words) + offset;
            g_v2k_ccs_view.data[count++] =
                v2k_words_float(src, runtime->bind[slot].type);
        }
        idx++;
    }
    g_v2k_ccs_view.count = count;
    g_v2k_ccs_view.result = V2K_SCOPE_RESULT_OK;
    g_v2k_ccs_view.done_seq = request;
}
