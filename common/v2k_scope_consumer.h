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
    uint16_t n_ticks;
    uint16_t stride_octets;
    uint32_t word_count;
    const volatile v2k_block_hdr_t *hdr;

    if ((prod->ring_capacity == 0u) || (prod->block_slot_words == 0u))
    {
        return 0u;
    }
    end = (prod->mode == V2K_SCOPE_CAPTURE_FROZEN) ?
          prod->frozen_end_idx : prod->wr_idx;
    if (idx == end)
    {
        return 0u;
    }
    // A rd_idx outside [0, 2*ring_capacity) is stale state from an older ring
    // geometry; treat it as a desync and resync to the producer's index
    // instead of deriving an out-of-ring block address from it.
    if (idx >= (uint16_t)(prod->ring_capacity << 1u))
    {
        cons->rd_idx = end;
        return 0u;
    }
    view->block_index = idx;
    view->words = (const volatile uint16_t *)prod->ring_base +
                  ((uint32_t)v2k_ring_pos(idx, prod->ring_capacity) *
                   prod->block_slot_words);
    hdr = (const volatile v2k_block_hdr_t *)view->words;
    // Never trust ring geometry read back from shared RAM. After an index
    // desync (a reconfigure racing the producer) this "header" can be sample
    // data or a torn write, and an oversized count would defeat the payload
    // bound checks downstream once truncated to 16 bits. Validate a single
    // snapshot of the geometry fields: no block published by
    // v2k_publish_block can exceed its own slot, so treat a misfit header as
    // desync, drop up to the producer's published index, and resync.
    n_ticks = hdr->n_ticks;
    stride_octets = hdr->stride_octets;
    word_count = 8u + ((uint32_t)n_ticks * ((uint32_t)stride_octets / 2u));
    if ((n_ticks == 0u) ||
        (n_ticks > prod->block_n_ticks) ||
        (stride_octets == 0u) ||
        (word_count > prod->block_slot_words))
    {
        cons->rd_idx = end;
        return 0u;
    }
    view->word_count = (uint16_t)word_count;
    return 1u;
}

static inline void v2k_scope_consumer_release(
    const volatile v2k_scope_prod_t *prod,
    volatile v2k_scope_cons_t *cons)
{
    cons->rd_idx = v2k_ring_next(cons->rd_idx, prod->ring_capacity);
}

static inline void v2k_scope_consumer_begin_frozen(
    const volatile v2k_scope_prod_t *prod,
    volatile v2k_scope_cons_t *cons)
{
    cons->rd_idx = v2k_ring_back(prod->frozen_end_idx, prod->frozen_count,
                                 prod->ring_capacity);
}

#endif // V2K_SCOPE_CONSUMER_H
