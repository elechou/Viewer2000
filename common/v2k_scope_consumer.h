//=============================================================================
// v2k_scope_consumer.h - CPU2 数据泵使用的 SPSC consumer API
//=============================================================================
#ifndef V2K_SCOPE_CONSUMER_H
#define V2K_SCOPE_CONSUMER_H

#include "../contracts/v2k_scope.h"

typedef struct {
    const volatile uint16_t *words;
    uint16_t word_count;
    uint16_t block_index;
} v2k_scope_block_view_t;

static inline uint16_t v2k_scope_consumer_peek(
    const volatile v2k_scope_prod_t *prod,
    volatile v2k_scope_cons_t *cons,
    v2k_scope_block_view_t *view)
{
    uint16_t end;
    uint16_t idx = cons->rd_idx;
    const volatile v2k_block_hdr_t *hdr;

    if ((prod->ring_capacity == 0u) || (prod->block_slot_words == 0u))
    {
        return 0u;
    }
    end = (prod->mode == V2K_SCOPE_SNAP_FROZEN) ?
          prod->frozen_end_idx : prod->wr_idx;
    if (idx == end)
    {
        return 0u;
    }
    view->block_index = idx;
    view->words = (const volatile uint16_t *)prod->ring_base +
                  ((uint32_t)(idx & (prod->ring_capacity - 1u)) *
                   prod->block_slot_words);
    hdr = (const volatile v2k_block_hdr_t *)view->words;
    view->word_count = (uint16_t)(8u +
        ((uint32_t)hdr->n_ticks * ((uint32_t)hdr->stride_octets / 2u)));
    return 1u;
}

static inline void v2k_scope_consumer_release(
    volatile v2k_scope_cons_t *cons)
{
    cons->rd_idx++;
}

static inline void v2k_scope_consumer_begin_snapshot(
    const volatile v2k_scope_prod_t *prod,
    volatile v2k_scope_cons_t *cons)
{
    cons->rd_idx = (uint16_t)(prod->frozen_end_idx - prod->frozen_count);
}

#endif // V2K_SCOPE_CONSUMER_H
