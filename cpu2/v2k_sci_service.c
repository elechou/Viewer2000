//=============================================================================
// v2k_sci_service.c - Viewer2000 wire v9 push transport over the CPU2 board pipe
//
// COBS/CRC, message handling, shared-plane service, and TX all run in the CPU2
// super-loop. Every on-wire field is serialized explicitly. C28x uint16_t
// buffers store octets in the low 8 bits only.
//=============================================================================

#include <string.h>
#include "../common/v2k_planes.h"
#include "../common/v2k_scope_consumer.h"
#include "v2k_cpu2_board.h"
#include "v2k_sci_service.h"

#define V2K_MAX_PAYLOAD       V2K_WIRE_MAX_PAYLOAD
#define V2K_RAW_MAX           (7u + V2K_MAX_PAYLOAD + 4u)
#define V2K_WIRE_MAX          (V2K_RAW_MAX + (V2K_RAW_MAX / 254u) + 2u)
#define V2K_RX_FRAME_WORDS    256u
#define V2K_STATUS_PUSH_PERIOD_MS 250uL
#define V2K_CPU1_STALE_TIMEOUT_MS 1000uL

#define V2K_MSG_HELLO         0x01u
#define V2K_MSG_STATUS        0x02u
#define V2K_MSG_ENUM          0x03u
#define V2K_MSG_CAL_WRITE     0x10u
#define V2K_MSG_CAL_COMMIT    0x11u
#define V2K_MSG_CAL_READ      0x12u
#define V2K_MSG_DAQ_CTRL      0x20u
#define V2K_MSG_CAPTURE_REPLAY_REQ 0x21u
#define V2K_MSG_DAQ_BIND      0x22u
#define V2K_MSG_CMD           0x30u
#define V2K_MSG_STATUS_PUSH   0x41u
#define V2K_MSG_SCOPE_BLOCK_PUSH 0x42u
#define V2K_MSG_CAPTURE_BATCH_PUSH 0x45u

#define V2K_ACK_OK            0u
#define V2K_ACK_BAD_PARAM     1u
#define V2K_ACK_BUSY          2u
#define V2K_ACK_BAD_STATE     3u
#define V2K_ACK_UNSUPPORTED   4u
#define V2K_ACK_INTERNAL      5u

#define V2K_CAPTURE_REPLAY_FLAG 0x01u

extern v2k_cpu2_plane_t g_v2k_cpu2_plane;
extern v2k_msg_2to1_t g_v2k_msg_2to1;

// The largest current request is 16 CAL_WRITE entries, 205 raw octets total.
// Responses still allow 1024 payload octets. On C28x each octet occupies one
// 16-bit word, so RX and TX buffers are budgeted separately.
static uint16_t s_rx_frame[V2K_RX_FRAME_WORDS];
static uint16_t s_rx_frame_len;
static uint16_t s_rx_discard;

static uint16_t s_tx_frame[V2K_WIRE_MAX];
static uint16_t s_response_frame[V2K_WIRE_MAX];
static uint16_t s_response_len;
static uint16_t s_response_pending;
static uint16_t s_last_response_req_type;
static uint16_t s_last_response_seq;
static uint16_t s_have_last_response;

static uint16_t s_raw[V2K_RAW_MAX];
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
static uint32_t s_cal_staged_commit_seq;
static uint32_t s_last_status_push_ms;
static uint16_t s_push_frame_seq;
static uint32_t s_cpu1_seen_heartbeat;
static uint32_t s_cpu1_seen_tick;
static uint32_t s_cpu1_last_change_ms;
static uint16_t s_cpu1_stale;

volatile uint32_t g_v2k_sci_rx_octets;
volatile uint32_t g_v2k_sci_tx_octets;
volatile uint32_t g_v2k_sci_rx_overflow;
volatile uint32_t g_v2k_sci_bad_frames;
volatile uint32_t g_v2k_sci_good_frames;
volatile uint32_t g_v2k_sci_tx_frames;
volatile uint32_t g_v2k_sci_tx_queue_full;
volatile uint32_t g_v2k_sci_tx_refill_isr;
volatile uint32_t g_v2k_sci_tx_refill_kick;
volatile uint32_t g_v2k_sci_tx_fifo_empty_refills;

static uint16_t v2k_get_u16(const uint16_t *buf, uint16_t off)
{
    return (uint16_t)((buf[off] & 0xFFu) |
                      ((buf[(uint16_t)(off + 1u)] & 0xFFu) << 8u));
}

static uint32_t v2k_get_u32(const uint16_t *buf, uint16_t off)
{
    uint32_t lo = v2k_get_u16(buf, off);
    uint32_t hi = v2k_get_u16(buf, (uint16_t)(off + 2u));
    return lo | (hi << 16u);
}

static void v2k_put_u16(uint16_t *buf, uint16_t off, uint16_t value)
{
    buf[off] = value & 0xFFu;
    buf[(uint16_t)(off + 1u)] = (value >> 8u) & 0xFFu;
}

static void v2k_put_u32(uint16_t *buf, uint16_t off, uint32_t value)
{
    v2k_put_u16(buf, off, (uint16_t)value);
    v2k_put_u16(buf, (uint16_t)(off + 2u), (uint16_t)(value >> 16u));
}

static uint32_t v2k_crc32c(const uint16_t *buf, uint16_t len)
{
    uint16_t i;
    uint16_t bit;
    uint32_t crc = 0xFFFFFFFFuL;
    for (i = 0u; i < len; i++)
    {
        crc ^= (uint32_t)(buf[i] & 0xFFu);
        for (bit = 0u; bit < 8u; bit++)
        {
            crc = (crc >> 1u) ^
                  ((crc & 1u) ? 0x82F63B78uL : 0uL);
        }
    }
    return crc ^ 0xFFFFFFFFuL;
}

static uint16_t v2k_cobs_decode_in_place(uint16_t *buf, uint16_t len)
{
    uint16_t read = 0u;
    uint16_t write = 0u;
    while (read < len)
    {
        uint16_t i;
        uint16_t code = buf[read++] & 0xFFu;
        if (code == 0u)
        {
            return 0u;
        }
        for (i = 1u; i < code; i++)
        {
            if (read >= len)
            {
                return 0u;
            }
            buf[write++] = buf[read++] & 0xFFu;
        }
        if ((code != 0xFFu) && (read < len))
        {
            buf[write++] = 0u;
        }
    }
    return write;
}

static uint16_t v2k_cobs_encode(const uint16_t *src, uint16_t len,
                                uint16_t *dst, uint16_t cap)
{
    uint16_t read;
    uint16_t write = 1u;
    uint16_t code_pos = 0u;
    uint16_t code = 1u;
    if (cap < 2u)
    {
        return 0u;
    }
    for (read = 0u; read < len; read++)
    {
        uint16_t octet = src[read] & 0xFFu;
        if (octet == 0u)
        {
            dst[code_pos] = code;
            code_pos = write++;
            code = 1u;
        }
        else
        {
            if (write >= cap)
            {
                return 0u;
            }
            dst[write++] = octet;
            code++;
            if (code == 0xFFu)
            {
                dst[code_pos] = code;
                code_pos = write++;
                code = 1u;
            }
        }
    }
    if (write >= cap)
    {
        return 0u;
    }
    dst[code_pos] = code;
    dst[write++] = 0u;
    return write;
}

static uint16_t v2k_frame_begin(uint16_t msg_type, uint16_t seq)
{
    s_raw[0] = V2K_WIRE_VER_MAGIC;
    s_raw[1] = msg_type & 0xFFu;
    s_raw[2] = 0u;
    v2k_put_u16(s_raw, 3u, seq);
    v2k_put_u16(s_raw, 5u, 0u);
    return 7u;
}

static uint16_t v2k_response_begin(uint16_t msg_type, uint16_t seq)
{
    return v2k_frame_begin((uint16_t)(msg_type | 0x80u), seq);
}

static uint16_t v2k_finish_frame(uint16_t payload_len,
                                 uint16_t *wire_buf,
                                 uint16_t *wire_len)
{
    uint16_t raw_len;
    uint32_t crc;

    if (payload_len > V2K_MAX_PAYLOAD)
    {
        return 0u;
    }
    v2k_put_u16(s_raw, 5u, payload_len);
    raw_len = (uint16_t)(7u + payload_len);
    crc = v2k_crc32c(s_raw, raw_len);
    v2k_put_u32(s_raw, raw_len, crc);
    raw_len = (uint16_t)(raw_len + 4u);
    *wire_len = v2k_cobs_encode(s_raw, raw_len, wire_buf, V2K_WIRE_MAX);
    return (*wire_len != 0u) ? 1u : 0u;
}

static void v2k_response_send(uint16_t payload_len)
{
    if (s_response_pending)
    {
        return;
    }
    if (v2k_finish_frame(payload_len, s_response_frame, &s_response_len))
    {
        s_response_pending = 1u;
        s_last_response_req_type = s_raw[1] & 0x7Fu;
        s_last_response_seq = v2k_get_u16(s_raw, 3u);
        s_have_last_response = 1u;
    }
}

static uint16_t v2k_start_push_frame(uint16_t payload_len, uint16_t prio)
{
    uint16_t wire_len;

    if (!v2k_cpu2_board_pipe_tx_can_submit(prio))
    {
        return 0u;
    }
    if (v2k_finish_frame(payload_len, s_tx_frame, &wire_len))
    {
        return v2k_cpu2_board_pipe_tx_submit_frame(
            s_tx_frame, wire_len, prio);
    }
    return 0u;
}

static void v2k_start_pending_response(void)
{
    if (s_response_pending &&
        v2k_cpu2_board_pipe_tx_can_submit(V2K_CPU2_BOARD_PIPE_TX_PRIO_HIGH) &&
        v2k_cpu2_board_pipe_tx_submit_frame(
            s_response_frame, s_response_len, V2K_CPU2_BOARD_PIPE_TX_PRIO_HIGH))
    {
        s_response_pending = 0u;
    }
}

static void v2k_send_ack(uint16_t request_type, uint16_t seq,
                         uint16_t status, uint32_t data)
{
    uint16_t off = v2k_response_begin(request_type, seq);
    s_raw[off++] = status & 0xFFu;
    s_raw[off++] = request_type & 0xFFu;
    v2k_put_u16(s_raw, off, 0u);
    off = (uint16_t)(off + 2u);
    v2k_put_u32(s_raw, off, data);
    off = (uint16_t)(off + 4u);
    v2k_response_send((uint16_t)(off - 7u));
}

static void v2k_handle_hello(uint16_t seq)
{
    static const char fw_name[16] = "viewer2000";
    uint16_t i;
    uint16_t off = v2k_response_begin(V2K_MSG_HELLO, seq);
    v2k_put_u16(s_raw, off, V2K_WIRE_VER);
    v2k_put_u16(s_raw, (uint16_t)(off + 2u), V2K_CONTRACT_VER);
    v2k_put_u32(s_raw, (uint16_t)(off + 4u),
                V2K_CPU1_PLANE_RO->desc_table.hdr.build_hash);
    v2k_put_u16(s_raw, (uint16_t)(off + 8u),
                V2K_CPU1_PLANE_RO->desc_table.hdr.entry_count);
    v2k_put_u16(s_raw, (uint16_t)(off + 10u), 0u);
    for (i = 0u; i < 16u; i++)
    {
        s_raw[(uint16_t)(off + 12u + i)] = ((uint16_t)fw_name[i]) & 0xFFu;
    }
    v2k_put_u32(s_raw, (uint16_t)(off + 28u),
                V2K_MSG_1TO2_RO->cpu1_status.tick_hz);
    v2k_put_u32(s_raw, (uint16_t)(off + 32u),
                V2K_CAPABILITIES_NATIVE);
    for (i = 0u; i < V2K_PROJECT_NAME_LEN; i++)
    {
        s_raw[(uint16_t)(off + 36u + i)] =
            ((uint16_t)V2K_CPU1_PLANE_RO->firmware_info.project_name[i]) & 0xFFu;
    }
    v2k_put_u32(s_raw, (uint16_t)(off + 68u),
                V2K_CPU1_PLANE_RO->firmware_info.build_time_utc);
    v2k_put_u16(s_raw, (uint16_t)(off + 72u), V2K_MCU_MODEL);
    v2k_put_u16(s_raw, (uint16_t)(off + 74u), V2K_SCOPE_MAX_CH);
    v2k_put_u16(s_raw, (uint16_t)(off + 76u), V2K_BLOCK_NTICKS_SCI);
    v2k_put_u16(s_raw, (uint16_t)(off + 78u), 0u);
    v2k_put_u32(s_raw, (uint16_t)(off + 80u), V2K_SCOPE_RING_WORDS);
    v2k_response_send(84u);
}

static uint16_t v2k_write_status_payload(uint16_t off)
{
    const volatile v2k_cpu1_status_t *cpu1 =
        &V2K_MSG_1TO2_RO->cpu1_status;
    const volatile v2k_param_status_t *cal = &V2K_CPU1_PLANE_RO->param_status;
    const volatile v2k_scope_prod_t *prod = &V2K_CPU1_PLANE_RO->scope_prod;
    uint16_t status_flags = cpu1->status_flags;

    if (s_cpu1_stale)
    {
        status_flags |= V2K_SF_CPU1_STALE;
    }

    v2k_put_u16(s_raw, off, cpu1->sys_state);
    v2k_put_u16(s_raw, (uint16_t)(off + 2u), cpu1->fault_code);
    v2k_put_u16(s_raw, (uint16_t)(off + 4u), status_flags);
    v2k_put_u32(s_raw, (uint16_t)(off + 6u), cpu1->tick);
    v2k_put_u32(s_raw, (uint16_t)(off + 10u), cpu1->heartbeat);
    v2k_put_u32(s_raw, (uint16_t)(off + 14u),
                g_v2k_msg_2to1.cpu2_status.heartbeat);
    v2k_put_u32(s_raw, (uint16_t)(off + 18u), cal->applied_seq);
    v2k_put_u16(s_raw, (uint16_t)(off + 22u), cal->result);
    v2k_put_u16(s_raw, (uint16_t)(off + 24u), cal->fail_idx);
    v2k_put_u32(s_raw, (uint16_t)(off + 26u),
                V2K_CPU1_PLANE_RO->desc_table.hdr.build_hash);
    s_raw[(uint16_t)(off + 30u)] = prod->mode & 0xFFu;
    s_raw[(uint16_t)(off + 31u)] = prod->flags & 0xFFu;
    v2k_put_u16(s_raw, (uint16_t)(off + 32u), 0u);
    v2k_put_u32(s_raw, (uint16_t)(off + 34u), cpu1->ack_seq);
    v2k_put_u16(s_raw, (uint16_t)(off + 38u), cpu1->cmd_result);
    v2k_put_u16(s_raw, (uint16_t)(off + 40u), 0u);
    v2k_put_u32(s_raw, (uint16_t)(off + 42u), cpu1->prof_seq);
    v2k_put_u32(s_raw, (uint16_t)(off + 46u), cpu1->cycle_budget);
    v2k_put_u32(s_raw, (uint16_t)(off + 50u), cpu1->load_avg);
    v2k_put_u32(s_raw, (uint16_t)(off + 54u), cpu1->load_peak);
    v2k_put_u32(s_raw, (uint16_t)(off + 58u), cpu1->ctrl_at_peak);
    v2k_put_u32(s_raw, (uint16_t)(off + 62u), cpu1->scope_at_peak);
    v2k_put_u16(s_raw, (uint16_t)(off + 66u), cpu1->lat_at_peak);
    v2k_put_u32(s_raw, (uint16_t)(off + 68u), cpu1->peak_tick);
    v2k_put_u32(s_raw, (uint16_t)(off + 72u), cpu1->budget_violations);
    v2k_put_u32(s_raw, (uint16_t)(off + 76u), cpu1->isr_overflows);
    v2k_put_u32(s_raw, (uint16_t)(off + 80u), cpu1->prof_seq);
    v2k_put_u16(s_raw, (uint16_t)(off + 84u), prod->state_seq);
    v2k_put_u16(s_raw, (uint16_t)(off + 86u), prod->frozen_count);
    v2k_put_u32(s_raw, (uint16_t)(off + 88u), prod->trig_tick);
    v2k_put_u16(s_raw, (uint16_t)(off + 92u), prod->bind_ack_seq);
    v2k_put_u16(s_raw, (uint16_t)(off + 94u), 0u);
    return 96u;
}

static void v2k_handle_status(uint16_t seq)
{
    uint16_t off = v2k_response_begin(V2K_MSG_STATUS, seq);
    v2k_response_send(v2k_write_status_payload(off));
}

static uint16_t v2k_start_status_push(void)
{
    uint32_t now_ms = v2k_cpu2_board_millis();
    uint16_t off;

    if ((uint32_t)(now_ms - s_last_status_push_ms) <
        V2K_STATUS_PUSH_PERIOD_MS)
    {
        return 0u;
    }
    off = v2k_frame_begin(V2K_MSG_STATUS_PUSH, 0u);
    if (v2k_start_push_frame(v2k_write_status_payload(off),
                             V2K_CPU2_BOARD_PIPE_TX_PRIO_NORMAL))
    {
        s_last_status_push_ms = now_ms;
        return 1u;
    }
    return 0u;
}

static void v2k_update_cpu1_stale_monitor(void)
{
    const volatile v2k_cpu1_status_t *cpu1 =
        &V2K_MSG_1TO2_RO->cpu1_status;
    uint32_t now_ms = v2k_cpu2_board_millis();
    uint32_t cpu1_heartbeat = cpu1->heartbeat;
    uint32_t cpu1_tick = cpu1->tick;

    if ((cpu1_heartbeat != s_cpu1_seen_heartbeat) ||
        (cpu1_tick != s_cpu1_seen_tick))
    {
        s_cpu1_seen_heartbeat = cpu1_heartbeat;
        s_cpu1_seen_tick = cpu1_tick;
        s_cpu1_last_change_ms = now_ms;
        s_cpu1_stale = 0u;
    }
    else if ((uint32_t)(now_ms - s_cpu1_last_change_ms) >=
             V2K_CPU1_STALE_TIMEOUT_MS)
    {
        s_cpu1_stale = 1u;
    }
}

static void v2k_handle_enum(uint16_t seq, const uint16_t *payload,
                            uint16_t payload_len)
{
    const volatile v2k_desc_table_t *table = &V2K_CPU1_PLANE_RO->desc_table;
    uint16_t start;
    uint16_t max_count;
    uint16_t count;
    uint16_t i;
    uint16_t off;
    if (payload_len != 4u)
    {
        v2k_send_ack(V2K_MSG_ENUM, seq, V2K_ACK_BAD_PARAM, 0uL);
        return;
    }
    start = v2k_get_u16(payload, 0u);
    max_count = payload[2] & 0xFFu;
    if ((max_count == 0u) || (max_count > 8u))
    {
        v2k_send_ack(V2K_MSG_ENUM, seq, V2K_ACK_BAD_PARAM, 0uL);
        return;
    }
    count = (start < table->hdr.entry_count) ?
            (uint16_t)(table->hdr.entry_count - start) : 0u;
    if (count > max_count)
    {
        count = max_count;
    }
    off = v2k_response_begin(V2K_MSG_ENUM, seq);
    v2k_put_u16(s_raw, off, table->hdr.entry_count);
    v2k_put_u16(s_raw, (uint16_t)(off + 2u), start);
    s_raw[(uint16_t)(off + 4u)] = count;
    s_raw[(uint16_t)(off + 5u)] = 0u;
    off = (uint16_t)(off + 6u);
    for (i = 0u; i < count; i++)
    {
        const volatile v2k_desc_entry_t *entry =
            &table->entries[(uint16_t)(start + i)];
        uint16_t n;
        for (n = 0u; n < V2K_NAME_LEN; n++)
        {
            s_raw[off++] = ((uint16_t)entry->name[n]) & 0xFFu;
        }
        v2k_put_u16(s_raw, off, entry->type);
        v2k_put_u16(s_raw, (uint16_t)(off + 2u), entry->kind);
        v2k_put_u32(s_raw, (uint16_t)(off + 4u), entry->addr);
        v2k_put_u16(s_raw, (uint16_t)(off + 8u), entry->prescaler);
        v2k_put_u16(s_raw, (uint16_t)(off + 10u), entry->reserved);
        off = (uint16_t)(off + 12u);
    }
    v2k_response_send((uint16_t)(off - 7u));
}

static int16_t v2k_find_staged(uint32_t addr)
{
    uint16_t i;
    for (i = 0u; i < g_v2k_cpu2_plane.param_shadow.count; i++)
    {
        if (g_v2k_cpu2_plane.param_shadow.writes[i].addr == addr)
        {
            return (int16_t)i;
        }
    }
    return -1;
}

static void v2k_handle_cal_write(uint16_t seq, const uint16_t *payload,
                                 uint16_t payload_len)
{
    uint16_t count;
    uint16_t i;
    if (payload_len < 2u)
    {
        v2k_send_ack(V2K_MSG_CAL_WRITE, seq, V2K_ACK_BAD_PARAM, 0uL);
        return;
    }
    if (g_v2k_cpu2_plane.param_shadow.commit_seq !=
        V2K_CPU1_PLANE_RO->param_status.applied_seq)
    {
        v2k_send_ack(V2K_MSG_CAL_WRITE, seq, V2K_ACK_BUSY, 0uL);
        return;
    }
    if (s_cal_staged_commit_seq != g_v2k_cpu2_plane.param_shadow.commit_seq)
    {
        g_v2k_cpu2_plane.param_shadow.count = 0u;
        s_cal_staged_commit_seq = g_v2k_cpu2_plane.param_shadow.commit_seq;
    }
    count = payload[0] & 0xFFu;
    if ((count == 0u) || (payload_len != (uint16_t)(2u + 12u * count)))
    {
        v2k_send_ack(V2K_MSG_CAL_WRITE, seq, V2K_ACK_BAD_PARAM, 0uL);
        return;
    }
    for (i = 0u; i < count; i++)
    {
        uint16_t in = (uint16_t)(2u + 12u * i);
        uint32_t addr = v2k_get_u32(payload, in);
        int16_t existing = v2k_find_staged(addr);
        uint16_t dst;
        if (existing >= 0)
        {
            dst = (uint16_t)existing;
        }
        else
        {
            if (g_v2k_cpu2_plane.param_shadow.count >= V2K_PARAM_BATCH_MAX)
            {
                v2k_send_ack(V2K_MSG_CAL_WRITE, seq,
                             V2K_ACK_BAD_PARAM, 0uL);
                return;
            }
            dst = g_v2k_cpu2_plane.param_shadow.count++;
        }
        g_v2k_cpu2_plane.param_shadow.writes[dst].addr = addr;
        g_v2k_cpu2_plane.param_shadow.writes[dst].value_bits =
            v2k_get_u32(payload, (uint16_t)(in + 4u));
        g_v2k_cpu2_plane.param_shadow.writes[dst].type =
            v2k_get_u16(payload, (uint16_t)(in + 8u));
        g_v2k_cpu2_plane.param_shadow.writes[dst].reserved = 0u;
    }
    v2k_send_ack(V2K_MSG_CAL_WRITE, seq, V2K_ACK_OK, 0uL);
}

static void v2k_handle_cal_commit(uint16_t seq, uint16_t payload_len)
{
    uint32_t commit_seq;
    if ((payload_len != 0u) || (g_v2k_cpu2_plane.param_shadow.count == 0u))
    {
        v2k_send_ack(V2K_MSG_CAL_COMMIT, seq, V2K_ACK_BAD_PARAM, 0uL);
        return;
    }
    if (g_v2k_cpu2_plane.param_shadow.commit_seq !=
        V2K_CPU1_PLANE_RO->param_status.applied_seq)
    {
        v2k_send_ack(V2K_MSG_CAL_COMMIT, seq, V2K_ACK_BUSY, 0uL);
        return;
    }
    commit_seq = g_v2k_cpu2_plane.param_shadow.commit_seq + 1uL;
    g_v2k_cpu2_plane.param_shadow.commit_seq = commit_seq;
    v2k_send_ack(V2K_MSG_CAL_COMMIT, seq, V2K_ACK_OK, commit_seq);
}

static void v2k_handle_cal_read(uint16_t seq, const uint16_t *payload,
                                uint16_t payload_len)
{
    volatile v2k_param_read_req_t *req = &g_v2k_cpu2_plane.param_read_req;
    const volatile v2k_param_read_resp_t *resp = &V2K_CPU1_PLANE_RO->param_read_resp;
    uint32_t read_seq;
    uint16_t count;
    uint16_t i;
    uint16_t off;
    uint16_t result;
    if (payload_len < 2u)
    {
        v2k_send_ack(V2K_MSG_CAL_READ, seq, V2K_ACK_BAD_PARAM, 0uL);
        return;
    }
    count = payload[0] & 0xFFu;
    if ((count == 0u) || (count > V2K_CAL_READ_MAX) ||
        (payload_len != (uint16_t)(2u + 8u * count)))
    {
        v2k_send_ack(V2K_MSG_CAL_READ, seq, V2K_ACK_BAD_PARAM, 0uL);
        return;
    }
    read_seq = req->read_seq + 1uL;
    req->count = count;
    req->reserved = 0u;
    for (i = 0u; i < count; i++)
    {
        uint16_t in = (uint16_t)(2u + 8u * i);
        req->refs[i].addr = v2k_get_u32(payload, in);
        req->refs[i].type = v2k_get_u16(payload, (uint16_t)(in + 4u));
        req->refs[i].reserved = 0u;
    }
    req->read_seq = read_seq;

    for (i = 0u; i < 3000u; i++)
    {
        if (resp->ack_seq == read_seq)
        {
            break;
        }
        v2k_cpu2_board_delay_us(1u);
    }
    if (resp->ack_seq != read_seq)
    {
        v2k_send_ack(V2K_MSG_CAL_READ, seq, V2K_ACK_INTERNAL, read_seq);
        return;
    }
    result = resp->result;
    if (result != V2K_CAL_OK)
    {
        v2k_send_ack(V2K_MSG_CAL_READ, seq, V2K_ACK_BAD_PARAM, read_seq);
        return;
    }
    if (resp->count != count)
    {
        v2k_send_ack(V2K_MSG_CAL_READ, seq, V2K_ACK_INTERNAL, read_seq);
        return;
    }
    off = v2k_response_begin(V2K_MSG_CAL_READ, seq);
    v2k_put_u32(s_raw, off, read_seq);
    s_raw[(uint16_t)(off + 4u)] = count;
    s_raw[(uint16_t)(off + 5u)] = 0u;
    v2k_put_u16(s_raw, (uint16_t)(off + 6u), 0u);
    off = (uint16_t)(off + 8u);
    for (i = 0u; i < count; i++)
    {
        v2k_put_u32(s_raw, off, resp->value_bits[i]);
        off = (uint16_t)(off + 4u);
    }
    v2k_response_send((uint16_t)(off - 7u));
}

static void v2k_handle_daq_ctrl(uint16_t seq, const uint16_t *payload,
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
        v2k_send_ack(V2K_MSG_DAQ_CTRL, seq, V2K_ACK_BAD_PARAM, 0uL);
        return;
    }
    ack_capture_id = v2k_get_u16(payload, 20u);
    flags = v2k_get_u16(payload, 22u);
    if (flags != 0u)
    {
        v2k_send_ack(V2K_MSG_DAQ_CTRL, seq, V2K_ACK_BAD_PARAM, 0uL);
        return;
    }
    (void)ack_capture_id;
    cfg = &g_v2k_cpu2_plane.scope_cfg;
    cfg_seq = (uint16_t)(cfg->cfg_seq + 1u);
    cfg->mode_req = v2k_get_u16(payload, 0u);
    cfg->trig_ch_slot = v2k_get_u16(payload, 2u);
    {
        union {
            uint32_t u32;
            float f32;
        } level;
        level.u32 = v2k_get_u32(payload, 4u);
        cfg->trig_level = level.f32;
        level.u32 = v2k_get_u32(payload, 8u);
        cfg->trig_hysteresis = level.f32;
    }
    cfg->trig_edge = v2k_get_u16(payload, 12u);
    cfg->pre_trig_pct = v2k_get_u16(payload, 14u);
    cfg->prescaler = v2k_get_u16(payload, 16u);
    cfg->record_points = v2k_get_u16(payload, 18u);
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
            v2k_send_ack(V2K_MSG_DAQ_CTRL, seq, ack, cfg_seq);
            return;
        }
        v2k_cpu2_board_delay_us(1u);
    }
    v2k_send_ack(V2K_MSG_DAQ_CTRL, seq, V2K_ACK_INTERNAL, cfg_seq);
}

static void v2k_handle_daq_bind(uint16_t seq, const uint16_t *payload,
                                uint16_t payload_len)
{
    uint16_t count;
    uint16_t i;
    uint16_t bind_seq;
    volatile v2k_scope_bind_t *bind;
    if (payload_len < 2u)
    {
        v2k_send_ack(V2K_MSG_DAQ_BIND, seq, V2K_ACK_BAD_PARAM, 0uL);
        return;
    }
    count = payload[0] & 0xFFu;
    if ((count == 0u) || (count > V2K_SCOPE_MAX_CH) ||
        (payload_len != (uint16_t)(2u + 8u * count)))
    {
        v2k_send_ack(V2K_MSG_DAQ_BIND, seq, V2K_ACK_BAD_PARAM, 0uL);
        return;
    }
    if (V2K_CPU1_PLANE_RO->scope_prod.mode != V2K_SCOPE_OFF)
    {
        v2k_send_ack(V2K_MSG_DAQ_BIND, seq, V2K_ACK_BAD_STATE, 0uL);
        return;
    }
    bind = &g_v2k_cpu2_plane.scope_bind;
    bind_seq = (uint16_t)(bind->bind_seq + 1u);
    bind->n_ch = count;
    for (i = 0u; i < count; i++)
    {
        uint16_t in = (uint16_t)(2u + 8u * i);
        bind->ch[i].addr = v2k_get_u32(payload, in);
        bind->ch[i].type = v2k_get_u16(payload, (uint16_t)(in + 4u));
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
            v2k_send_ack(V2K_MSG_DAQ_BIND, seq, ack, bind_seq);
            return;
        }
        v2k_cpu2_board_delay_us(1u);
    }
    v2k_send_ack(V2K_MSG_DAQ_BIND, seq, V2K_ACK_INTERNAL, bind_seq);
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
    view->word_count = (uint16_t)(8u +
        ((uint32_t)hdr->n_ticks * ((uint32_t)hdr->stride_octets / 2u)));
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
    s_raw[off++] = 0u;
    s_raw[off++] = 0u;
    v2k_put_u16(s_raw, off, prod->overrun_cnt);
    off = (uint16_t)(off + 2u);
    v2k_put_u16(s_raw, off, 0u);
    off = (uint16_t)(off + 2u);
    v2k_put_u16(s_raw, off, 0u);
    off = (uint16_t)(off + 2u);

    if (prod->mode != V2K_SCOPE_STREAM)
    {
        *count_out = 0u;
        return off;
    }
    while ((uint32_t)(off - 7u) < V2K_MAX_PAYLOAD)
    {
        v2k_scope_block_view_t view;
        uint16_t block_octets;
        uint16_t word;
        if (!v2k_scope_consumer_peek(prod, cons, &view))
        {
            break;
        }
        block_octets = (uint16_t)(view.word_count * 2u);
        if ((uint32_t)(off - 7u) + block_octets > V2K_MAX_PAYLOAD)
        {
            break;
        }
        for (word = 0u; word < view.word_count; word++)
        {
            uint16_t value = view.words[word];
            s_raw[off++] = value & 0xFFu;
            s_raw[off++] = (value >> 8u) & 0xFFu;
        }
        v2k_scope_consumer_release(cons);
        count++;
    }
    s_raw[count_off] = count;
    v2k_put_u16(s_raw, (uint16_t)(count_off + 4u),
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

    v2k_put_u16(s_raw, off, s_capture_id);
    off = (uint16_t)(off + 2u);
    v2k_put_u16(s_raw, off, s_capture_total_blocks);
    off = (uint16_t)(off + 2u);
    v2k_put_u16(s_raw, off, first_index);
    off = (uint16_t)(off + 2u);
    count_off = off;
    s_raw[off++] = 0u;
    s_raw[off++] = flags & 0xFFu;
    remaining_off = off;
    v2k_put_u16(s_raw, off, 0u);
    off = (uint16_t)(off + 2u);
    v2k_put_u16(s_raw, off, 0u);
    off = (uint16_t)(off + 2u);
    v2k_put_u32(s_raw, off, prod->trig_tick);
    off = (uint16_t)(off + 4u);

    while ((index < end_index) &&
           ((uint32_t)(off - 7u) < V2K_MAX_PAYLOAD))
    {
        v2k_scope_block_view_t view;
        uint16_t block_octets;
        uint16_t word;
        if (!v2k_scope_frozen_block_view(prod, index, &view))
        {
            break;
        }
        block_octets = (uint16_t)(view.word_count * 2u);
        if ((uint32_t)(off - 7u) + block_octets > V2K_MAX_PAYLOAD)
        {
            break;
        }
        for (word = 0u; word < view.word_count; word++)
        {
            uint16_t value = view.words[word];
            s_raw[off++] = value & 0xFFu;
            s_raw[off++] = (value >> 8u) & 0xFFu;
        }
        count++;
        index++;
    }

    s_raw[count_off] = count;
    v2k_put_u16(s_raw, remaining_off, (uint16_t)(end_index - index));
    *count_out = count;
    *next_index_out = index;
    return off;
}

static void v2k_handle_capture_replay_req(uint16_t seq,
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
        v2k_send_ack(V2K_MSG_CAPTURE_REPLAY_REQ, seq, V2K_ACK_BAD_PARAM, 0uL);
        return;
    }
    capture_id = v2k_get_u16(payload, 0u);
    first_index = v2k_get_u16(payload, 2u);
    max_blocks = payload[4u] & 0xFFu;
    if (max_blocks == 0u)
    {
        v2k_send_ack(V2K_MSG_CAPTURE_REPLAY_REQ, seq, V2K_ACK_BAD_PARAM, 0uL);
        return;
    }
    if ((prod->mode != V2K_SCOPE_CAPTURE_FROZEN) ||
        !s_capture_seen_valid ||
        (capture_id != s_capture_id) ||
        (s_capture_state_seq != prod->state_seq))
    {
        v2k_send_ack(V2K_MSG_CAPTURE_REPLAY_REQ, seq, V2K_ACK_BAD_STATE, 0uL);
        return;
    }
    if (first_index >= prod->frozen_count)
    {
        v2k_send_ack(V2K_MSG_CAPTURE_REPLAY_REQ, seq, V2K_ACK_BAD_PARAM, 0uL);
        return;
    }
    available = (uint16_t)(prod->frozen_count - first_index);
    count = (max_blocks < available) ? max_blocks : available;
    s_capture_replay_next_index = first_index;
    s_capture_replay_end_index = (uint16_t)(first_index + count);
    s_capture_replay_active = 1u;
    v2k_send_ack(V2K_MSG_CAPTURE_REPLAY_REQ, seq, V2K_ACK_OK, capture_id);
}

static void v2k_update_scope_state(void)
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

static uint16_t v2k_start_capture_batch_push(uint16_t replay)
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
        prio = V2K_CPU2_BOARD_PIPE_TX_PRIO_HIGH;
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
        prio = V2K_CPU2_BOARD_PIPE_TX_PRIO_NORMAL;
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
    if (!v2k_cpu2_board_pipe_tx_can_submit(prio))
    {
        return 0u;
    }
    off = v2k_frame_begin(V2K_MSG_CAPTURE_BATCH_PUSH, s_push_frame_seq);
    off = v2k_write_capture_batch_payload(off, prod, first_index, end_index,
                                          flags, &count, &next_index);
    if (count == 0u)
    {
        return 0u;
    }
    if (v2k_start_push_frame((uint16_t)(off - 7u), prio))
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

static uint16_t v2k_start_scope_stream_push(void)
{
    const volatile v2k_scope_prod_t *prod = &V2K_CPU1_PLANE_RO->scope_prod;
    volatile v2k_scope_cons_t *cons = &g_v2k_cpu2_plane.scope_cons;
    uint16_t off;
    uint16_t count;

    if (prod->mode != V2K_SCOPE_STREAM)
    {
        return 0u;
    }
    if (!v2k_cpu2_board_pipe_tx_can_submit(
            V2K_CPU2_BOARD_PIPE_TX_PRIO_NORMAL))
    {
        return 0u;
    }
    off = v2k_frame_begin(V2K_MSG_SCOPE_BLOCK_PUSH, s_push_frame_seq);
    off = v2k_write_stream_batch_payload(off, prod, cons, &count);
    if (count == 0u)
    {
        return 0u;
    }
    if (v2k_start_push_frame((uint16_t)(off - 7u),
                             V2K_CPU2_BOARD_PIPE_TX_PRIO_NORMAL))
    {
        s_push_frame_seq++;
        return 1u;
    }
    return 0u;
}

static uint16_t v2k_start_next_push(void)
{
    if ((g_v2k_msg_2to1.cpu2_status.link_state == 0u) ||
        s_response_pending)
    {
        return 0u;
    }
    if (v2k_start_capture_batch_push(1u))
    {
        return 1u;
    }
    if (v2k_start_capture_batch_push(0u))
    {
        return 1u;
    }
    if (v2k_start_status_push())
    {
        return 1u;
    }
    return v2k_start_scope_stream_push();
}

static void v2k_handle_cmd(uint16_t seq, const uint16_t *payload,
                           uint16_t payload_len)
{
    uint32_t cmd_seq;
    if (payload_len != 8u)
    {
        v2k_send_ack(V2K_MSG_CMD, seq, V2K_ACK_BAD_PARAM, 0uL);
        return;
    }
    if (g_v2k_msg_2to1.cmd_req.cmd_seq !=
        V2K_MSG_1TO2_RO->cpu1_status.ack_seq)
    {
        v2k_send_ack(V2K_MSG_CMD, seq, V2K_ACK_BUSY, 0uL);
        return;
    }
    cmd_seq = g_v2k_msg_2to1.cmd_req.cmd_seq + 1uL;
    g_v2k_msg_2to1.cmd_req.cmd_code = v2k_get_u16(payload, 0u);
    g_v2k_msg_2to1.cmd_req.arg0 = v2k_get_u16(payload, 2u);
    g_v2k_msg_2to1.cmd_req.arg1 = v2k_get_u32(payload, 4u);
    g_v2k_msg_2to1.cmd_req.cmd_seq = cmd_seq;
    v2k_send_ack(V2K_MSG_CMD, seq, V2K_ACK_OK, cmd_seq);
}

static void v2k_dispatch(uint16_t msg_type, uint16_t seq,
                         const uint16_t *payload, uint16_t payload_len)
{
    switch (msg_type)
    {
        case V2K_MSG_HELLO:
            if (payload_len == 0u) v2k_handle_hello(seq);
            else v2k_send_ack(msg_type, seq, V2K_ACK_BAD_PARAM, 0uL);
            break;
        case V2K_MSG_STATUS:
            if (payload_len == 0u) v2k_handle_status(seq);
            else v2k_send_ack(msg_type, seq, V2K_ACK_BAD_PARAM, 0uL);
            break;
        case V2K_MSG_ENUM:
            v2k_handle_enum(seq, payload, payload_len);
            break;
        case V2K_MSG_CAL_WRITE:
            v2k_handle_cal_write(seq, payload, payload_len);
            break;
        case V2K_MSG_CAL_COMMIT:
            v2k_handle_cal_commit(seq, payload_len);
            break;
        case V2K_MSG_CAL_READ:
            v2k_handle_cal_read(seq, payload, payload_len);
            break;
        case V2K_MSG_DAQ_CTRL:
            v2k_handle_daq_ctrl(seq, payload, payload_len);
            break;
        case V2K_MSG_CAPTURE_REPLAY_REQ:
            v2k_handle_capture_replay_req(seq, payload, payload_len);
            break;
        case V2K_MSG_DAQ_BIND:
            v2k_handle_daq_bind(seq, payload, payload_len);
            break;
        case V2K_MSG_CMD:
            v2k_handle_cmd(seq, payload, payload_len);
            break;
        default:
            v2k_send_ack(msg_type, seq, V2K_ACK_UNSUPPORTED, 0uL);
            break;
    }
}

static void v2k_process_encoded_frame(void)
{
    uint16_t raw_len;
    uint16_t payload_len;
    uint16_t expected_len;
    uint16_t seq;
    uint32_t received_crc;
    uint32_t calculated_crc;
    if (s_rx_frame_len == 0u)
    {
        return;
    }
    raw_len = v2k_cobs_decode_in_place(s_rx_frame, s_rx_frame_len);
    if (raw_len < 11u)
    {
        g_v2k_sci_bad_frames++;
        return;
    }
    payload_len = v2k_get_u16(s_rx_frame, 5u);
    expected_len = (uint16_t)(7u + payload_len + 4u);
    if ((s_rx_frame[0] != V2K_WIRE_VER_MAGIC) ||
        (s_rx_frame[2] != 0u) ||
        (payload_len > V2K_MAX_PAYLOAD) ||
        (raw_len != expected_len))
    {
        g_v2k_sci_bad_frames++;
        return;
    }
    received_crc = v2k_get_u32(s_rx_frame, (uint16_t)(raw_len - 4u));
    calculated_crc = v2k_crc32c(s_rx_frame, (uint16_t)(raw_len - 4u));
    if (received_crc != calculated_crc)
    {
        g_v2k_sci_bad_frames++;
        return;
    }
    seq = v2k_get_u16(s_rx_frame, 3u);
    g_v2k_sci_good_frames++;
    g_v2k_msg_2to1.cpu2_status.link_state = 1u;
    if (s_have_last_response &&
        ((s_rx_frame[1] & 0x7Fu) == s_last_response_req_type) &&
        (seq == s_last_response_seq))
    {
        // If the host retries the same frame after a timeout, replay the
        // original response and do not execute COMMIT/CMD-style services again.
        s_response_pending = 1u;
        return;
    }
    if (s_response_pending)
    {
        return;
    }
    v2k_dispatch(s_rx_frame[1] & 0x7Fu, seq,
                 &s_rx_frame[7], payload_len);
}

static void v2k_rx_service(void)
{
    uint16_t value;

    while (v2k_cpu2_board_pipe_read_octet(&value))
    {
        value &= 0xFFu;
        if (value == 0u)
        {
            if (!s_rx_discard)
            {
                v2k_process_encoded_frame();
            }
            s_rx_frame_len = 0u;
            s_rx_discard = 0u;
        }
        else if (!s_rx_discard)
        {
            if (s_rx_frame_len < V2K_RX_FRAME_WORDS)
            {
                s_rx_frame[s_rx_frame_len++] = value;
            }
            else
            {
                s_rx_discard = 1u;
                g_v2k_sci_bad_frames++;
            }
        }
    }
}

void v2k_sci_init(void)
{
    memset(s_rx_frame, 0, sizeof(s_rx_frame));
    memset(s_tx_frame, 0, sizeof(s_tx_frame));
    memset(s_response_frame, 0, sizeof(s_response_frame));
    memset(s_raw, 0, sizeof(s_raw));
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
    s_cal_staged_commit_seq = 0xFFFFFFFFuL;
    s_last_status_push_ms = 0uL;
    s_rx_frame_len = 0u;
    s_rx_discard = 0u;
    s_response_len = 0u;
    s_response_pending = 0u;
    s_last_response_req_type = 0u;
    s_last_response_seq = 0u;
    s_have_last_response = 0u;
    s_push_frame_seq = 0u;
    s_cpu1_seen_heartbeat = V2K_MSG_1TO2_RO->cpu1_status.heartbeat;
    s_cpu1_seen_tick = V2K_MSG_1TO2_RO->cpu1_status.tick;
    s_cpu1_stale = 0u;

    v2k_cpu2_board_pipe_init();
    s_cpu1_last_change_ms = v2k_cpu2_board_millis();
}

void v2k_sci_service(void)
{
    v2k_cpu2_board_pipe_service();
    v2k_start_pending_response();
    v2k_rx_service();
    v2k_update_cpu1_stale_monitor();
    v2k_update_scope_state();
    v2k_start_pending_response();
    while (v2k_start_next_push())
    {
        v2k_start_pending_response();
        if (s_response_pending)
        {
            break;
        }
    }
    v2k_start_pending_response();
    v2k_cpu2_board_pipe_service();
}
