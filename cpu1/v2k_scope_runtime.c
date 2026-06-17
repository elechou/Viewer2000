//=============================================================================
// v2k_scope_runtime.c - Stream/Capture 共用示波生产者
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

#pragma DATA_SECTION(g_v2k_scope_ring, "v2k_scope_ring")
volatile uint16_t g_v2k_scope_ring[V2K_SCOPE_RING_WORDS];

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
} v2k_scope_runtime_t;

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

static v2k_scope_runtime_t s_scope;
static v2k_scope_cfg_t s_active_cfg;
static v2k_scope_cfg_pending_t s_cfg_pending;
static v2k_scope_bind_pending_t s_bind_pending;
static uint16_t s_cfg_seen;
static uint16_t s_bind_seen;
static volatile uint16_t s_scope_active;
static volatile uint16_t s_cons_rd_cache;

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

static uint16_t v2k_scope_layout(uint16_t n_ticks, uint16_t stride_words,
                                 uint16_t *slot_words, uint16_t *capacity)
{
    uint32_t slot;
    uint32_t block_octets;
    uint16_t cap;

    if ((n_ticks == 0u) || (stride_words == 0u))
    {
        return V2K_SCOPE_RESULT_NO_CAPACITY;
    }
    block_octets = V2K_BLOCK_OCTETS(n_ticks, (uint16_t)(stride_words * 2u));
    if (block_octets > (V2K_WIRE_MAX_PAYLOAD - V2K_BLOCK_DATA_PREFIX_OCTETS))
    {
        return V2K_SCOPE_RESULT_NO_CAPACITY;
    }
    slot = V2K_BLOCK_WORDS(n_ticks, stride_words);
    if ((slot & 1u) != 0u)
    {
        slot++;
    }
    if ((slot > 0xFFFFuL) || (slot > V2K_SCOPE_RING_WORDS))
    {
        return V2K_SCOPE_RESULT_NO_CAPACITY;
    }
    cap = v2k_floor_pow2(V2K_SCOPE_RING_WORDS / slot);
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

static void v2k_default_bind(void)
{
    const v2k_desc_table_t *table = &g_v2k_gs0.desc_table;
    v2k_scope_prod_t *prod = &g_v2k_gs0.scope_prod;
    uint16_t i;
    uint16_t n = 0u;
    uint16_t slot_words = 0u;
    uint16_t capacity = 0u;

    for (i = 0u;
         (i < table->hdr.entry_count) && (n < V2K_SCOPE_DEFAULT_CH);
         i++)
    {
        const v2k_desc_entry_t *entry = &table->entries[i];
        if ((entry->kind & V2K_KIND_SCOPE) != 0u)
        {
            s_scope.bind[n].addr = entry->addr;
            s_scope.bind[n].type = entry->type;
            s_scope.bind[n].reserved = 0u;
            n++;
        }
    }
    s_scope.stride_words = v2k_stride_words(s_scope.bind, n);
    prod->n_ch = n;
    if (v2k_scope_layout(prod->block_n_ticks, s_scope.stride_words,
                         &slot_words, &capacity) == V2K_SCOPE_RESULT_OK)
    {
        prod->block_slot_words = slot_words;
        prod->ring_capacity = capacity;
    }
}

void v2k_scope_init(void)
{
    v2k_scope_prod_t *prod = &g_v2k_gs0.scope_prod;

    memset((void *)g_v2k_scope_ring, 0, sizeof(g_v2k_scope_ring));
    memset(&s_scope, 0, sizeof(s_scope));
    memset(&s_active_cfg, 0, sizeof(s_active_cfg));
    memset(&s_cfg_pending, 0, sizeof(s_cfg_pending));
    memset(&s_bind_pending, 0, sizeof(s_bind_pending));
    memset((void *)&s_scope_active, 0, sizeof(s_scope_active));
    memset((void *)&s_cons_rd_cache, 0, sizeof(s_cons_rd_cache));
    memset(&g_v2k_ccs_view, 0, sizeof(g_v2k_ccs_view));

    memset(prod, 0, sizeof(*prod));
    prod->mode = V2K_SCOPE_OFF;
    prod->block_n_ticks = V2K_BLOCK_NTICKS_SCI;
    prod->prescaler = 1u;
    prod->ring_base = (uint32_t)g_v2k_scope_ring;
    prod->cfg_result = V2K_SCOPE_RESULT_OK;
    prod->bind_result = V2K_SCOPE_RESULT_OK;
    s_active_cfg.trig_edge = V2K_TRIG_RISE;
    s_active_cfg.block_n_ticks = V2K_BLOCK_NTICKS_SCI;
    v2k_default_bind();
}

static uint16_t v2k_validate_bind(const v2k_scope_bind_t *bind)
{
    uint16_t i;
    uint16_t stride;
    uint16_t slot_words;
    uint16_t capacity;

    if ((bind->n_ch == 0u) || (bind->n_ch > V2K_SCOPE_MAX_CH))
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
    return v2k_scope_layout(g_v2k_gs0.scope_prod.block_n_ticks, stride,
                            &slot_words, &capacity);
}

static uint16_t v2k_validate_cfg(const v2k_scope_cfg_t *cfg)
{
    const v2k_scope_prod_t *prod = &g_v2k_gs0.scope_prod;
    uint16_t n_ticks;
    uint16_t prescaler;
    uint16_t slot_words;
    uint16_t capacity;

    if ((cfg->mode_req != V2K_SCOPE_OFF) &&
        (cfg->mode_req != V2K_SCOPE_STREAM) &&
        (cfg->mode_req != V2K_SCOPE_CAPTURE_ARMED))
    {
        return V2K_SCOPE_RESULT_BAD_PARAM;
    }
    if (cfg->mode_req == V2K_SCOPE_OFF)
    {
        return V2K_SCOPE_RESULT_OK;
    }
    if (prod->n_ch == 0u)
    {
        return V2K_SCOPE_RESULT_BAD_PARAM;
    }
    if (cfg->mode_req == V2K_SCOPE_CAPTURE_ARMED)
    {
        if ((cfg->pre_trig_pct > 100u) ||
            (cfg->trig_ch_slot >= prod->n_ch) ||
            ((cfg->trig_edge != V2K_TRIG_RISE) &&
             (cfg->trig_edge != V2K_TRIG_FALL)))
        {
            return V2K_SCOPE_RESULT_BAD_PARAM;
        }
    }
    n_ticks = (cfg->block_n_ticks == 0u) ?
              prod->block_n_ticks : cfg->block_n_ticks;
    prescaler = (cfg->prescaler == 0u) ? prod->prescaler : cfg->prescaler;
    if (prescaler == 0u)
    {
        return V2K_SCOPE_RESULT_BAD_PARAM;
    }
    return v2k_scope_layout(n_ticks, s_scope.stride_words,
                            &slot_words, &capacity);
}

void v2k_scope_service(void)
{
    const volatile v2k_scope_cfg_t *cfg = &V2K_GS4_RO->scope_cfg;
    const volatile v2k_scope_bind_t *bind = &V2K_GS4_RO->scope_bind;
    uint16_t seq_before;
    uint16_t seq_after;

    // ISR 不直接读取 CPU2 属主 GS4；缓存稍旧只会保守地多丢新块。
    s_cons_rd_cache = V2K_GS4_RO->scope_cons.rd_idx;

    if (!s_cfg_pending.pending)
    {
        seq_before = cfg->cfg_seq;
        if ((seq_before != s_cfg_seen) &&
            (seq_before != g_v2k_gs0.scope_prod.cfg_ack_seq))
        {
            s_cfg_pending.cfg = *cfg;
            seq_after = cfg->cfg_seq;
            if (seq_before == seq_after)
            {
                s_cfg_seen = seq_after;
                s_cfg_pending.result =
                    v2k_validate_cfg(&s_cfg_pending.cfg);
                s_cfg_pending.pending = 1u;
            }
        }
    }

    if (!s_bind_pending.pending)
    {
        seq_before = bind->bind_seq;
        if ((seq_before != s_bind_seen) &&
            (seq_before != g_v2k_gs0.scope_prod.bind_ack_seq))
        {
            s_bind_pending.bind = *bind;
            seq_after = bind->bind_seq;
            if (seq_before == seq_after)
            {
                s_bind_seen = seq_after;
                s_bind_pending.result =
                    v2k_validate_bind(&s_bind_pending.bind);
                s_bind_pending.pending = 1u;
            }
        }
    }
}

static void v2k_apply_bind(void)
{
    v2k_scope_prod_t *prod = &g_v2k_gs0.scope_prod;
    uint16_t result = s_bind_pending.result;
    uint16_t slot_words = 0u;
    uint16_t capacity = 0u;

    if (prod->mode != V2K_SCOPE_OFF)
    {
        result = V2K_SCOPE_RESULT_BAD_STATE;
    }
    if (result == V2K_SCOPE_RESULT_OK)
    {
        uint16_t i;
        for (i = 0u; i < s_bind_pending.bind.n_ch; i++)
        {
            s_scope.bind[i] = s_bind_pending.bind.ch[i];
        }
        s_scope.stride_words =
            v2k_stride_words(s_scope.bind, s_bind_pending.bind.n_ch);
        result = v2k_scope_layout(prod->block_n_ticks, s_scope.stride_words,
                                  &slot_words, &capacity);
        if (result == V2K_SCOPE_RESULT_OK)
        {
            prod->n_ch = s_bind_pending.bind.n_ch;
            s_scope.active_bind_seq = s_bind_pending.bind.bind_seq;
            prod->block_slot_words = slot_words;
            prod->ring_capacity = capacity;
            prod->wr_idx = 0u;
            s_scope.sample_in_block = 0u;
            s_scope.published_count = 0u;
        }
    }
    prod->bind_result = result;
    prod->bind_ack_seq = s_bind_pending.bind.bind_seq;
    s_bind_pending.pending = 0u;
}

static void v2k_apply_cfg(void)
{
    v2k_scope_prod_t *prod = &g_v2k_gs0.scope_prod;
    uint16_t result = s_cfg_pending.result;
    uint16_t n_ticks;
    uint16_t prescaler;
    uint16_t slot_words = 0u;
    uint16_t capacity = 0u;

    if ((prod->mode == V2K_SCOPE_CAPTURE_POST) &&
        (s_cfg_pending.cfg.mode_req != V2K_SCOPE_OFF))
    {
        result = V2K_SCOPE_RESULT_BAD_STATE;
    }
    if ((result == V2K_SCOPE_RESULT_OK) &&
        (s_cfg_pending.cfg.mode_req == V2K_SCOPE_OFF))
    {
        // 先从 ISR 热路径摘除，再修改运行态。
        s_scope_active = 0u;
        s_scope.sample_in_block = 0u;
        s_scope.drop_block = 0u;
        s_scope.post_remaining = 0u;
        v2k_scope_transition(prod, V2K_SCOPE_OFF);
        prod->cfg_result = V2K_SCOPE_RESULT_OK;
        prod->cfg_ack_seq = s_cfg_pending.cfg.cfg_seq;
        s_cfg_pending.pending = 0u;
        return;
    }

    n_ticks = (s_cfg_pending.cfg.block_n_ticks == 0u) ?
              prod->block_n_ticks : s_cfg_pending.cfg.block_n_ticks;
    prescaler = (s_cfg_pending.cfg.prescaler == 0u) ?
                prod->prescaler : s_cfg_pending.cfg.prescaler;
    if (result == V2K_SCOPE_RESULT_OK)
    {
        result = v2k_scope_layout(n_ticks, s_scope.stride_words,
                                  &slot_words, &capacity);
    }
    if (result == V2K_SCOPE_RESULT_OK)
    {
        prod->prescaler = prescaler;
        prod->block_n_ticks = n_ticks;
        prod->block_slot_words = slot_words;
        prod->ring_capacity = capacity;
        s_scope.sample_in_block = 0u;
        s_scope.drop_block = 0u;
        s_scope.prescale_count = 1u;
        s_scope.prev_valid = 0u;
        s_scope.post_remaining = 0u;
        s_scope.published_count = 0u;
        prod->frozen_count = 0u;
        prod->frozen_end_idx = 0u;
        s_active_cfg = s_cfg_pending.cfg;
        s_active_cfg.prescaler = prescaler;
        s_active_cfg.block_n_ticks = n_ticks;
        if (s_cfg_pending.cfg.mode_req == V2K_SCOPE_STREAM)
        {
            prod->wr_idx = s_cons_rd_cache;
        }
        else if (s_cfg_pending.cfg.mode_req == V2K_SCOPE_CAPTURE_ARMED)
        {
            prod->wr_idx = 0u;
        }
        v2k_scope_transition(prod, s_cfg_pending.cfg.mode_req);
        // 所有运行态字段就绪后，最后发布给 ISR。
        s_scope_active = 1u;
    }
    prod->cfg_result = result;
    prod->cfg_ack_seq = s_cfg_pending.cfg.cfg_seq;
    s_cfg_pending.pending = 0u;
}

void v2k_scope_apply_ready(void)
{
    if (s_bind_pending.pending)
    {
        v2k_apply_bind();
    }
    if (s_cfg_pending.pending)
    {
        v2k_apply_cfg();
    }
}

static volatile uint16_t *v2k_block_words(uint16_t wr_idx)
{
    v2k_scope_prod_t *prod = &g_v2k_gs0.scope_prod;
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

static void v2k_copy_sample(volatile uint16_t *dst)
{
    uint16_t ch;
    for (ch = 0u; ch < g_v2k_gs0.scope_prod.n_ch; ch++)
    {
        const v2k_scope_ch_bind_t *bind = &s_scope.bind[ch];
        const volatile uint16_t *src = (const volatile uint16_t *)bind->addr;
        *dst++ = *src++;
        if ((bind->type != V2K_TYPE_I16) && (bind->type != V2K_TYPE_U16))
        {
            *dst++ = *src;
        }
    }
}

static void v2k_publish_block(uint16_t n_ticks)
{
    v2k_scope_prod_t *prod = &g_v2k_gs0.scope_prod;
    volatile v2k_block_hdr_t *hdr =
        (volatile v2k_block_hdr_t *)v2k_block_words(prod->wr_idx);

    hdr->n_ticks = n_ticks;
    prod->wr_idx++;
    s_scope.block_seq++;
    if (s_scope.published_count < prod->ring_capacity)
    {
        s_scope.published_count++;
    }
    s_scope.sample_in_block = 0u;
}

static void v2k_freeze(void)
{
    v2k_scope_prod_t *prod = &g_v2k_gs0.scope_prod;
    if (s_scope.sample_in_block != 0u)
    {
        v2k_publish_block(s_scope.sample_in_block);
    }
    prod->frozen_end_idx = prod->wr_idx;
    prod->frozen_count = s_scope.published_count;
    v2k_scope_transition(prod, V2K_SCOPE_CAPTURE_FROZEN);
    s_scope_active = 0u;
}

static void v2k_scope_sample_one(v2k_tick_t tick)
{
    v2k_scope_prod_t *prod = &g_v2k_gs0.scope_prod;
    uint16_t mode_before = prod->mode;
    volatile uint16_t *block;
    volatile v2k_block_hdr_t *hdr;
    uint16_t hit = 0u;

    if (s_scope.prescale_count > 1u)
    {
        s_scope.prescale_count--;
        return;
    }
    s_scope.prescale_count = prod->prescaler;

    if (s_scope.sample_in_block == 0u)
    {
        s_scope.drop_block = 0u;
        if ((prod->mode == V2K_SCOPE_STREAM) &&
            ((uint16_t)(prod->wr_idx - s_cons_rd_cache) >=
             prod->ring_capacity))
        {
            s_scope.drop_block = 1u;
        }
        if (!s_scope.drop_block)
        {
            block = v2k_block_words(prod->wr_idx);
            hdr = (volatile v2k_block_hdr_t *)block;
            hdr->start_tick = tick;
            hdr->block_seq = s_scope.block_seq;
            hdr->flags = 0u;
            hdr->n_ticks = prod->block_n_ticks;
            hdr->n_ch = prod->n_ch;
            hdr->bind_seq = s_scope.active_bind_seq;
            hdr->stride_octets = (uint16_t)(s_scope.stride_words * 2u);
        }
    }

    if (!s_scope.drop_block)
    {
        block = v2k_block_words(prod->wr_idx);
        v2k_copy_sample(block + 8u +
            ((uint32_t)s_scope.sample_in_block * s_scope.stride_words));
    }
    s_scope.sample_in_block++;

    if (mode_before == V2K_SCOPE_CAPTURE_ARMED)
    {
        const v2k_scope_cfg_t *cfg = &s_active_cfg;
        float current = v2k_source_float(&s_scope.bind[cfg->trig_ch_slot]);
        if (s_scope.prev_valid)
        {
            if ((cfg->trig_edge == V2K_TRIG_RISE) &&
                (s_scope.prev_trigger < cfg->trig_level) &&
                (current >= cfg->trig_level))
            {
                hit = 1u;
            }
            else if ((cfg->trig_edge == V2K_TRIG_FALL) &&
                     (s_scope.prev_trigger > cfg->trig_level) &&
                     (current <= cfg->trig_level))
            {
                hit = 1u;
            }
        }
        s_scope.prev_trigger = current;
        s_scope.prev_valid = 1u;
    }

    if (s_scope.sample_in_block >= prod->block_n_ticks)
    {
        if (s_scope.drop_block)
        {
            s_scope.sample_in_block = 0u;
            s_scope.block_seq++;
            prod->overrun_cnt++;
            g_v2k_scope_overrun_total++;
        }
        else
        {
            v2k_publish_block(prod->block_n_ticks);
        }
    }

    if (hit)
    {
        uint32_t total = (uint32_t)prod->ring_capacity * prod->block_n_ticks;
        uint32_t pre = (total * s_active_cfg.pre_trig_pct) / 100u;
        uint32_t post = total - pre;
        if (post == 0u)
        {
            post = 1u;
        }
        prod->trig_tick = tick;
        s_scope.post_remaining = post - 1u;
        v2k_scope_transition(prod, V2K_SCOPE_CAPTURE_POST);
        if (s_scope.post_remaining == 0u)
        {
            v2k_freeze();
        }
    }
    else if (mode_before == V2K_SCOPE_CAPTURE_POST)
    {
        if (s_scope.post_remaining > 0u)
        {
            s_scope.post_remaining--;
        }
        if (s_scope.post_remaining == 0u)
        {
            v2k_freeze();
        }
    }
}

void v2k_scope_sample_all(v2k_tick_t tick)
{
    if (s_scope_active != 0u)
    {
        v2k_scope_sample_one(tick);
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
    uint16_t slot = g_v2k_ccs_view.channel_slot;
    uint16_t request = g_v2k_ccs_view.request_seq;
    const v2k_scope_prod_t *prod = &g_v2k_gs0.scope_prod;
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
    if ((prod->mode != V2K_SCOPE_CAPTURE_FROZEN) || (slot >= prod->n_ch))
    {
        g_v2k_ccs_view.done_seq = request;
        return;
    }
    for (ch = 0u; ch < slot; ch++)
    {
        offset = (uint16_t)(offset +
            (((s_scope.bind[ch].type == V2K_TYPE_I16) ||
              (s_scope.bind[ch].type == V2K_TYPE_U16)) ? 1u : 2u));
    }

    idx = (uint16_t)(prod->frozen_end_idx - prod->frozen_count);
    end = prod->frozen_end_idx;
    while ((idx != end) && (count < V2K_CCS_VIEW_SAMPLES))
    {
        const volatile uint16_t *words = v2k_block_words(idx);
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
                ((uint32_t)sample * s_scope.stride_words) + offset;
            g_v2k_ccs_view.data[count++] =
                v2k_words_float(src, s_scope.bind[slot].type);
        }
        idx++;
    }
    g_v2k_ccs_view.count = count;
    g_v2k_ccs_view.result = V2K_SCOPE_RESULT_OK;
    g_v2k_ccs_view.done_seq = request;
}
