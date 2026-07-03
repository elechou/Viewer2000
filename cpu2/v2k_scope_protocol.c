//=============================================================================
// v2k_scope_protocol.c - Viewer2000 DAQ/capture protocol services
//=============================================================================

#include "../common/v2k_planes.h"
#include "../common/v2k_scope_consumer.h"
#include "v2k_cpu2_board.h"
#include "v2k_message_codec.h"
#include "v2k_protocol_internal.h"
#include "v2k_scope_protocol.h"

#define V2K_MAX_PAYLOAD V2K_WIRE_MAX_PAYLOAD
#define V2K_CAPTURE_REPLAY_FLAG 0x01u

extern v2k_cpu2_plane_t g_v2k_cpu2_plane;

static uint16_t s_capture_state_seq;
static uint16_t s_capture_seen_valid;
static uint16_t s_capture_id;
static uint16_t s_capture_total_blocks;
static uint16_t s_capture_initial_next_index;
static uint16_t s_capture_initial_active;
static uint16_t s_capture_replay_next_index;
static uint16_t s_capture_replay_end_index;
static uint16_t s_capture_replay_active;
static uint16_t s_scope_seen_state_seq;
static uint16_t s_scope_seen_valid;
static uint16_t s_push_frame_seq;

void v2k_scope_protocol_handle_daq_ctrl(uint16_t seq, const uint16_t *payload,
                                uint16_t payload_len)
{
    uint16_t i;
    uint16_t cfg_seq;
    uint16_t result;
    uint16_t ack;
    uint16_t ack_capture_id;
    uint16_t flags;
    volatile v2k_scope_cfg_t *cfg;
    if (payload_len != 24u)
    {
        v2k_protocol_send_ack(V2K_MSG_DAQ_CTRL, seq, V2K_ACK_BAD_PARAM, 0uL);
        return;
    }
    ack_capture_id = v2k_message_get_u16(payload, 20u);
    flags = v2k_message_get_u16(payload, 22u);
    if (flags != 0u)
    {
        v2k_protocol_send_ack(V2K_MSG_DAQ_CTRL, seq, V2K_ACK_BAD_PARAM, 0uL);
        return;
    }
    (void)ack_capture_id;
    cfg = &g_v2k_cpu2_plane.scope_cfg;
    cfg_seq = (uint16_t)(cfg->cfg_seq + 1u);
    cfg->mode_req = v2k_message_get_u16(payload, 0u);
    cfg->trig_ch_slot = v2k_message_get_u16(payload, 2u);
    {
        union {
            uint32_t u32;
            float f32;
        } level;
        level.u32 = v2k_message_get_u32(payload, 4u);
        cfg->trig_level = level.f32;
        level.u32 = v2k_message_get_u32(payload, 8u);
        cfg->trig_hysteresis = level.f32;
    }
    cfg->trig_edge = v2k_message_get_u16(payload, 12u);
    cfg->pre_trig_pct = v2k_message_get_u16(payload, 14u);
    cfg->prescaler = v2k_message_get_u16(payload, 16u);
    cfg->record_points = v2k_message_get_u16(payload, 18u);
    cfg->reserved = 0u;
    cfg->cfg_seq = cfg_seq;
    for (i = 0u; i < 3000u; i++)
    {
        if (V2K_CPU1_PLANE_RO->scope_prod.cfg_ack_seq == cfg_seq)
        {
            result = V2K_CPU1_PLANE_RO->scope_prod.cfg_result;
            ack = (result == V2K_SCOPE_RESULT_OK) ?
                  V2K_ACK_OK :
                  ((result == V2K_SCOPE_RESULT_BAD_STATE) ?
                   V2K_ACK_BAD_STATE : V2K_ACK_BAD_PARAM);
            v2k_protocol_send_ack(V2K_MSG_DAQ_CTRL, seq, ack, cfg_seq);
            return;
        }
        v2k_cpu2_board_delay_us(1u);
    }
    v2k_protocol_send_ack(V2K_MSG_DAQ_CTRL, seq, V2K_ACK_INTERNAL, cfg_seq);
}

void v2k_scope_protocol_handle_daq_bind(uint16_t seq, const uint16_t *payload,
                                uint16_t payload_len)
{
    uint16_t count;
    uint16_t i;
    uint16_t bind_seq;
    volatile v2k_scope_bind_t *bind;
    if (payload_len < 2u)
    {
        v2k_protocol_send_ack(V2K_MSG_DAQ_BIND, seq, V2K_ACK_BAD_PARAM, 0uL);
        return;
    }
    count = payload[0] & 0xFFu;
    if ((count == 0u) || (count > V2K_SCOPE_MAX_CH) ||
        (payload_len != (uint16_t)(2u + 8u * count)))
    {
        v2k_protocol_send_ack(V2K_MSG_DAQ_BIND, seq, V2K_ACK_BAD_PARAM, 0uL);
        return;
    }
    if (V2K_CPU1_PLANE_RO->scope_prod.mode != V2K_SCOPE_OFF)
    {
        v2k_protocol_send_ack(V2K_MSG_DAQ_BIND, seq, V2K_ACK_BAD_STATE, 0uL);
        return;
    }
    bind = &g_v2k_cpu2_plane.scope_bind;
    bind_seq = (uint16_t)(bind->bind_seq + 1u);
    bind->n_ch = count;
    for (i = 0u; i < count; i++)
    {
        uint16_t in = (uint16_t)(2u + 8u * i);
        bind->ch[i].addr = v2k_message_get_u32(payload, in);
        bind->ch[i].type = v2k_message_get_u16(payload, (uint16_t)(in + 4u));
        bind->ch[i].reserved = 0u;
    }
    bind->bind_seq = bind_seq;
    for (i = 0u; i < 3000u; i++)
    {
        if (V2K_CPU1_PLANE_RO->scope_prod.bind_ack_seq == bind_seq)
        {
            uint16_t result = V2K_CPU1_PLANE_RO->scope_prod.bind_result;
            uint16_t ack = (result == V2K_SCOPE_RESULT_OK) ?
                           V2K_ACK_OK :
                           ((result == V2K_SCOPE_RESULT_BAD_STATE) ?
                            V2K_ACK_BAD_STATE : V2K_ACK_BAD_PARAM);
            v2k_protocol_send_ack(V2K_MSG_DAQ_BIND, seq, ack, bind_seq);
            return;
        }
        v2k_cpu2_board_delay_us(1u);
    }
    v2k_protocol_send_ack(V2K_MSG_DAQ_BIND, seq, V2K_ACK_INTERNAL, bind_seq);
}

static uint16_t v2k_stream_remaining(
    const volatile v2k_scope_prod_t *prod,
    const volatile v2k_scope_cons_t *cons)
{
    return (uint16_t)(prod->wr_idx - cons->rd_idx);
}

static uint16_t v2k_scope_frozen_block_view(
    const volatile v2k_scope_prod_t *prod,
    uint16_t block_index,
    v2k_scope_block_view_t *view)
{
    uint16_t ring_index;
    uint16_t n_ticks;
    uint16_t stride_octets;
    uint32_t word_count;
    const volatile v2k_block_hdr_t *hdr;

    if ((prod->mode != V2K_SCOPE_CAPTURE_FROZEN) ||
        (prod->ring_capacity == 0u) ||
        (prod->block_slot_words == 0u) ||
        (block_index >= prod->frozen_count))
    {
        return 0u;
    }
    ring_index = (uint16_t)(prod->frozen_end_idx - prod->frozen_count +
                            block_index);
    view->block_index = block_index;
    view->words = (const volatile uint16_t *)prod->ring_base +
                  ((uint32_t)(ring_index & (prod->ring_capacity - 1u)) *
                   prod->block_slot_words);
    hdr = (const volatile v2k_block_hdr_t *)view->words;
    // Same header-geometry guard as v2k_scope_consumer_peek: a block that
    // does not fit its own slot was never produced by v2k_publish_block, so
    // refuse the view instead of forwarding an oversized count downstream.
    n_ticks = hdr->n_ticks;
    stride_octets = hdr->stride_octets;
    word_count = 8u + ((uint32_t)n_ticks * ((uint32_t)stride_octets / 2u));
    if ((n_ticks == 0u) ||
        (n_ticks > prod->block_n_ticks) ||
        (stride_octets == 0u) ||
        (word_count > prod->block_slot_words))
    {
        return 0u;
    }
    view->word_count = (uint16_t)word_count;
    return 1u;
}

static uint16_t v2k_write_stream_batch_payload(
    uint16_t off,
    const volatile v2k_scope_prod_t *prod,
    volatile v2k_scope_cons_t *cons,
    uint16_t *count_out)
{
    uint16_t count = 0u;
    uint16_t count_off;

    count_off = off;
    g_v2k_protocol_message[off++] = 0u;
    g_v2k_protocol_message[off++] = 0u;
    v2k_message_put_u16(g_v2k_protocol_message, off, prod->overrun_cnt);
    off = (uint16_t)(off + 2u);
    v2k_message_put_u16(g_v2k_protocol_message, off, 0u);
    off = (uint16_t)(off + 2u);
    v2k_message_put_u16(g_v2k_protocol_message, off, 0u);
    off = (uint16_t)(off + 2u);

    if (prod->mode != V2K_SCOPE_STREAM)
    {
        *count_out = 0u;
        return off;
    }
    while ((uint32_t)(off - 7u) < V2K_MAX_PAYLOAD)
    {
        v2k_scope_block_view_t view;
        uint32_t block_octets;
        uint16_t word;
        if (!v2k_scope_consumer_peek(prod, cons, &view))
        {
            break;
        }
        // 32-bit size math: a 16-bit product would wrap for a corrupt
        // word_count and slip past this payload bound check.
        block_octets = (uint32_t)view.word_count * 2u;
        if (((uint32_t)(off - 7u) + block_octets) > V2K_MAX_PAYLOAD)
        {
            break;
        }
        for (word = 0u; word < view.word_count; word++)
        {
            uint16_t value = view.words[word];
            g_v2k_protocol_message[off++] = value & 0xFFu;
            g_v2k_protocol_message[off++] = (value >> 8u) & 0xFFu;
        }
        v2k_scope_consumer_release(cons);
        count++;
    }
    g_v2k_protocol_message[count_off] = count;
    v2k_message_put_u16(g_v2k_protocol_message, (uint16_t)(count_off + 4u),
                v2k_stream_remaining(prod, cons));
    *count_out = count;
    return off;
}

static uint16_t v2k_write_capture_batch_payload(
    uint16_t off,
    const volatile v2k_scope_prod_t *prod,
    uint16_t first_index,
    uint16_t end_index,
    uint16_t flags,
    uint16_t *count_out,
    uint16_t *next_index_out)
{
    uint16_t count = 0u;
    uint16_t count_off;
    uint16_t remaining_off;
    uint16_t index = first_index;

    v2k_message_put_u16(g_v2k_protocol_message, off, s_capture_id);
    off = (uint16_t)(off + 2u);
    v2k_message_put_u16(g_v2k_protocol_message, off, s_capture_total_blocks);
    off = (uint16_t)(off + 2u);
    v2k_message_put_u16(g_v2k_protocol_message, off, first_index);
    off = (uint16_t)(off + 2u);
    count_off = off;
    g_v2k_protocol_message[off++] = 0u;
    g_v2k_protocol_message[off++] = flags & 0xFFu;
    remaining_off = off;
    v2k_message_put_u16(g_v2k_protocol_message, off, 0u);
    off = (uint16_t)(off + 2u);
    v2k_message_put_u16(g_v2k_protocol_message, off, 0u);
    off = (uint16_t)(off + 2u);
    v2k_message_put_u32(g_v2k_protocol_message, off, prod->trig_tick);
    off = (uint16_t)(off + 4u);

    while ((index < end_index) &&
           ((uint32_t)(off - 7u) < V2K_MAX_PAYLOAD))
    {
        v2k_scope_block_view_t view;
        uint32_t block_octets;
        uint16_t word;
        if (!v2k_scope_frozen_block_view(prod, index, &view))
        {
            break;
        }
        // 32-bit size math: a 16-bit product would wrap for a corrupt
        // word_count and slip past this payload bound check.
        block_octets = (uint32_t)view.word_count * 2u;
        if (((uint32_t)(off - 7u) + block_octets) > V2K_MAX_PAYLOAD)
        {
            break;
        }
        for (word = 0u; word < view.word_count; word++)
        {
            uint16_t value = view.words[word];
            g_v2k_protocol_message[off++] = value & 0xFFu;
            g_v2k_protocol_message[off++] = (value >> 8u) & 0xFFu;
        }
        count++;
        index++;
    }

    g_v2k_protocol_message[count_off] = count;
    v2k_message_put_u16(g_v2k_protocol_message, remaining_off, (uint16_t)(end_index - index));
    *count_out = count;
    *next_index_out = index;
    return off;
}

void v2k_scope_protocol_handle_capture_replay(uint16_t seq,
                                           const uint16_t *payload,
                                           uint16_t payload_len)
{
    const volatile v2k_scope_prod_t *prod = &V2K_CPU1_PLANE_RO->scope_prod;
    uint16_t capture_id;
    uint16_t first_index;
    uint16_t max_blocks;
    uint16_t available;
    uint16_t count;

    if ((payload_len != 8u) ||
        ((payload[5u] & 0xFFu) != 0u) ||
        ((payload[6u] & 0xFFu) != 0u) ||
        ((payload[7u] & 0xFFu) != 0u))
    {
        v2k_protocol_send_ack(V2K_MSG_CAPTURE_REPLAY_REQ, seq, V2K_ACK_BAD_PARAM, 0uL);
        return;
    }
    capture_id = v2k_message_get_u16(payload, 0u);
    first_index = v2k_message_get_u16(payload, 2u);
    max_blocks = payload[4u] & 0xFFu;
    if (max_blocks == 0u)
    {
        v2k_protocol_send_ack(V2K_MSG_CAPTURE_REPLAY_REQ, seq, V2K_ACK_BAD_PARAM, 0uL);
        return;
    }
    if ((prod->mode != V2K_SCOPE_CAPTURE_FROZEN) ||
        !s_capture_seen_valid ||
        (capture_id != s_capture_id) ||
        (s_capture_state_seq != prod->state_seq))
    {
        v2k_protocol_send_ack(V2K_MSG_CAPTURE_REPLAY_REQ, seq, V2K_ACK_BAD_STATE, 0uL);
        return;
    }
    if (first_index >= prod->frozen_count)
    {
        v2k_protocol_send_ack(V2K_MSG_CAPTURE_REPLAY_REQ, seq, V2K_ACK_BAD_PARAM, 0uL);
        return;
    }
    available = (uint16_t)(prod->frozen_count - first_index);
    count = (max_blocks < available) ? max_blocks : available;
    s_capture_replay_next_index = first_index;
    s_capture_replay_end_index = (uint16_t)(first_index + count);
    s_capture_replay_active = 1u;
    v2k_protocol_send_ack(V2K_MSG_CAPTURE_REPLAY_REQ, seq, V2K_ACK_OK, capture_id);
}

void v2k_scope_protocol_update(void)
{
    const volatile v2k_scope_prod_t *prod = &V2K_CPU1_PLANE_RO->scope_prod;
    volatile v2k_scope_cons_t *cons = &g_v2k_cpu2_plane.scope_cons;

    if (!s_scope_seen_valid || (s_scope_seen_state_seq != prod->state_seq))
    {
        s_scope_seen_valid = 1u;
        s_scope_seen_state_seq = prod->state_seq;
        if (prod->mode == V2K_SCOPE_STREAM)
        {
            cons->rd_idx = prod->wr_idx;
        }
    }

    if (prod->mode == V2K_SCOPE_CAPTURE_FROZEN)
    {
        if (!s_capture_seen_valid || (s_capture_state_seq != prod->state_seq))
        {
            s_capture_seen_valid = 1u;
            s_capture_state_seq = prod->state_seq;
            s_capture_id = prod->state_seq;
            s_capture_total_blocks = prod->frozen_count;
            s_capture_initial_next_index = 0u;
            s_capture_initial_active = (prod->frozen_count != 0u) ? 1u : 0u;
            s_capture_replay_active = 0u;
        }
    }
    else
    {
        s_capture_initial_active = 0u;
        s_capture_replay_active = 0u;
        s_capture_total_blocks = 0u;
        s_capture_seen_valid = 0u;
    }
}

uint16_t v2k_scope_protocol_start_capture_push(uint16_t replay)
{
    const volatile v2k_scope_prod_t *prod = &V2K_CPU1_PLANE_RO->scope_prod;
    uint16_t off;
    uint16_t count;
    uint16_t first_index;
    uint16_t end_index;
    uint16_t next_index;
    uint16_t prio;
    uint16_t flags;

    if (prod->mode != V2K_SCOPE_CAPTURE_FROZEN)
    {
        return 0u;
    }
    if (replay)
    {
        if (!s_capture_replay_active)
        {
            return 0u;
        }
        first_index = s_capture_replay_next_index;
        end_index = s_capture_replay_end_index;
        prio = V2K_PROTOCOL_TX_PRIO_HIGH;
        flags = V2K_CAPTURE_REPLAY_FLAG;
    }
    else
    {
        if (!s_capture_initial_active)
        {
            return 0u;
        }
        first_index = s_capture_initial_next_index;
        end_index = s_capture_total_blocks;
        prio = V2K_PROTOCOL_TX_PRIO_NORMAL;
        flags = 0u;
    }
    if (first_index >= end_index)
    {
        if (replay)
        {
            s_capture_replay_active = 0u;
        }
        else
        {
            s_capture_initial_active = 0u;
        }
        return 0u;
    }
    if (!v2k_protocol_tx_can_submit(prio))
    {
        return 0u;
    }
    off = v2k_protocol_push_begin(V2K_MSG_CAPTURE_BATCH_PUSH, s_push_frame_seq);
    off = v2k_write_capture_batch_payload(off, prod, first_index, end_index,
                                          flags, &count, &next_index);
    if (count == 0u)
    {
        return 0u;
    }
    if (v2k_protocol_submit_push((uint16_t)(off - 7u), prio))
    {
        s_push_frame_seq++;
        if (replay)
        {
            s_capture_replay_next_index = next_index;
            if (next_index >= end_index)
            {
                s_capture_replay_active = 0u;
            }
        }
        else
        {
            s_capture_initial_next_index = next_index;
            if (next_index >= end_index)
            {
                s_capture_initial_active = 0u;
            }
        }
        return 1u;
    }
    return 0u;
}

uint16_t v2k_scope_protocol_start_stream_push(void)
{
    const volatile v2k_scope_prod_t *prod = &V2K_CPU1_PLANE_RO->scope_prod;
    volatile v2k_scope_cons_t *cons = &g_v2k_cpu2_plane.scope_cons;
    uint16_t off;
    uint16_t count;

    if (prod->mode != V2K_SCOPE_STREAM)
    {
        return 0u;
    }
    if (!v2k_protocol_tx_can_submit(
            V2K_PROTOCOL_TX_PRIO_NORMAL))
    {
        return 0u;
    }
    off = v2k_protocol_push_begin(V2K_MSG_SCOPE_BLOCK_PUSH, s_push_frame_seq);
    off = v2k_write_stream_batch_payload(off, prod, cons, &count);
    if (count == 0u)
    {
        return 0u;
    }
    if (v2k_protocol_submit_push((uint16_t)(off - 7u),
                             V2K_PROTOCOL_TX_PRIO_NORMAL))
    {
        s_push_frame_seq++;
        return 1u;
    }
    return 0u;
}


void v2k_scope_protocol_init(void)
{
    s_capture_state_seq = 0u;
    s_capture_seen_valid = 0u;
    s_capture_id = 0u;
    s_capture_total_blocks = 0u;
    s_capture_initial_next_index = 0u;
    s_capture_initial_active = 0u;
    s_capture_replay_next_index = 0u;
    s_capture_replay_end_index = 0u;
    s_capture_replay_active = 0u;
    s_scope_seen_state_seq = 0u;
    s_scope_seen_valid = 0u;
    s_push_frame_seq = 0u;
}
