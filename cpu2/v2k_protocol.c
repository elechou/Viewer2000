//=============================================================================
// v2k_protocol.c - Transport-neutral Viewer2000 protocol service
//
// This module owns message semantics and shared-plane transactions. Physical
// links provide validated requests and an endpoint for outgoing messages.
//=============================================================================

#include <string.h>
#include "../common/v2k_planes.h"
#include "v2k_cpu2_board.h"
#include "v2k_message_codec.h"
#include "v2k_protocol.h"
#include "v2k_protocol_internal.h"
#include "v2k_scope_protocol.h"

#define V2K_STATUS_PUSH_PERIOD_MS 250uL
#define V2K_CPU1_STALE_TIMEOUT_MS 1000uL
extern v2k_cpu2_plane_t g_v2k_cpu2_plane;
extern v2k_msg_2to1_t g_v2k_msg_2to1;

uint16_t g_v2k_protocol_message[V2K_MESSAGE_MAX_OCTETS];
#define s_message g_v2k_protocol_message
static uint16_t s_response_len;
static uint16_t s_response_ready;
static uint32_t s_cal_staged_commit_seq;
static uint32_t s_last_status_push_ms;
static uint32_t s_cpu1_seen_heartbeat;
static uint32_t s_cpu1_seen_tick;
static uint32_t s_cpu1_last_change_ms;
static uint16_t s_cpu1_stale;
static uint16_t s_enum_req_seq;
static const v2k_protocol_endpoint_t *s_request_endpoint;
static const v2k_protocol_endpoint_t *s_push_endpoint;

uint16_t v2k_protocol_response_begin(uint16_t msg_type, uint16_t seq)
{
    return v2k_message_response_begin(s_message, msg_type, seq);
}

void v2k_protocol_finish_response(uint16_t payload_len)
{
    if (v2k_message_finish(s_message, payload_len, &s_response_len))
    {
        s_response_ready = 1u;
    }
}

uint16_t v2k_protocol_push_begin(uint16_t msg_type, uint16_t seq)
{
    return v2k_message_begin(s_message, msg_type, seq);
}

void v2k_protocol_send_ack(uint16_t request_type,
                           uint16_t seq,
                           uint16_t status,
                           uint32_t data)
{
    uint16_t off = v2k_protocol_response_begin(request_type, seq);
    s_message[off++] = status & 0xFFu;
    s_message[off++] = request_type & 0xFFu;
    v2k_message_put_u16(s_message, off, 0u);
    off = (uint16_t)(off + 2u);
    v2k_message_put_u32(s_message, off, data);
    off = (uint16_t)(off + 4u);
    v2k_protocol_finish_response((uint16_t)(off -
                                            V2K_MESSAGE_HEADER_OCTETS));
}

uint16_t v2k_protocol_tx_can_submit(uint16_t prio)
{
    if ((s_push_endpoint == 0) ||
        (s_push_endpoint->tx_can_submit == 0))
    {
        return 0u;
    }
    return s_push_endpoint->tx_can_submit(prio);
}

uint16_t v2k_protocol_submit_push(uint16_t payload_len, uint16_t prio)
{
    uint16_t message_len;

    if (!v2k_protocol_tx_can_submit(prio) ||
        (s_push_endpoint->tx_submit_message == 0) ||
        !v2k_message_finish(s_message, payload_len, &message_len))
    {
        return 0u;
    }
    return s_push_endpoint->tx_submit_message(s_message, message_len, prio);
}

static void v2k_handle_hello(uint16_t seq)
{
    static const char fw_name[16] = "viewer2000";
    uint16_t i;
    uint16_t off = v2k_protocol_response_begin(V2K_MSG_HELLO, seq);
    v2k_message_put_u16(s_message, off, V2K_WIRE_VER);
    v2k_message_put_u16(s_message, (uint16_t)(off + 2u), V2K_CONTRACT_VER);
    v2k_message_put_u32(s_message, (uint16_t)(off + 4u),
                V2K_CPU1_PLANE_RO->catalog.hdr.build_hash);
    v2k_message_put_u16(s_message, (uint16_t)(off + 8u),
                V2K_CPU1_PLANE_RO->catalog.hdr.total_count);
    v2k_message_put_u16(s_message, (uint16_t)(off + 10u), 0u);
    for (i = 0u; i < 16u; i++)
    {
        s_message[(uint16_t)(off + 12u + i)] = ((uint16_t)fw_name[i]) & 0xFFu;
    }
    v2k_message_put_u32(s_message, (uint16_t)(off + 28u),
                V2K_MSG_1TO2_RO->cpu1_status.tick_hz);
    v2k_message_put_u32(s_message, (uint16_t)(off + 32u),
                V2K_CAPABILITIES_NATIVE);
    for (i = 0u; i < V2K_PROJECT_NAME_LEN; i++)
    {
        s_message[(uint16_t)(off + 36u + i)] =
            ((uint16_t)V2K_CPU1_PLANE_RO->firmware_info.project_name[i]) & 0xFFu;
    }
    v2k_message_put_u32(s_message, (uint16_t)(off + 68u),
                V2K_CPU1_PLANE_RO->firmware_info.build_time_utc);
    v2k_message_put_u16(s_message, (uint16_t)(off + 72u), V2K_MCU_MODEL);
    v2k_message_put_u16(s_message, (uint16_t)(off + 74u), V2K_SCOPE_MAX_CH);
    v2k_message_put_u16(s_message, (uint16_t)(off + 76u),
                        s_request_endpoint->scope_block_nticks);
    v2k_message_put_u16(s_message, (uint16_t)(off + 78u), 0u);
    v2k_message_put_u32(s_message, (uint16_t)(off + 80u), V2K_SCOPE_RING_WORDS);
    v2k_protocol_finish_response(84u);
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

    v2k_message_put_u16(s_message, off, cpu1->sys_state);
    v2k_message_put_u16(s_message, (uint16_t)(off + 2u), cpu1->fault_code);
    v2k_message_put_u16(s_message, (uint16_t)(off + 4u), status_flags);
    v2k_message_put_u32(s_message, (uint16_t)(off + 6u), cpu1->tick);
    v2k_message_put_u32(s_message, (uint16_t)(off + 10u), cpu1->heartbeat);
    v2k_message_put_u32(s_message, (uint16_t)(off + 14u),
                g_v2k_msg_2to1.cpu2_status.heartbeat);
    v2k_message_put_u32(s_message, (uint16_t)(off + 18u), cal->applied_seq);
    v2k_message_put_u16(s_message, (uint16_t)(off + 22u), cal->result);
    v2k_message_put_u16(s_message, (uint16_t)(off + 24u), cal->fail_idx);
    v2k_message_put_u32(s_message, (uint16_t)(off + 26u),
                V2K_CPU1_PLANE_RO->catalog.hdr.build_hash);
    s_message[(uint16_t)(off + 30u)] = prod->mode & 0xFFu;
    s_message[(uint16_t)(off + 31u)] = prod->flags & 0xFFu;
    v2k_message_put_u16(s_message, (uint16_t)(off + 32u), 0u);
    v2k_message_put_u32(s_message, (uint16_t)(off + 34u), cpu1->ack_seq);
    v2k_message_put_u16(s_message, (uint16_t)(off + 38u), cpu1->cmd_result);
    v2k_message_put_u16(s_message, (uint16_t)(off + 40u), 0u);
    v2k_message_put_u32(s_message, (uint16_t)(off + 42u), cpu1->prof_seq);
    v2k_message_put_u32(s_message, (uint16_t)(off + 46u), cpu1->cycle_budget);
    v2k_message_put_u32(s_message, (uint16_t)(off + 50u), cpu1->load_avg);
    v2k_message_put_u32(s_message, (uint16_t)(off + 54u), cpu1->load_peak);
    v2k_message_put_u32(s_message, (uint16_t)(off + 58u), cpu1->ctrl_at_peak);
    v2k_message_put_u32(s_message, (uint16_t)(off + 62u), cpu1->scope_at_peak);
    v2k_message_put_u16(s_message, (uint16_t)(off + 66u), cpu1->lat_at_peak);
    v2k_message_put_u32(s_message, (uint16_t)(off + 68u), cpu1->peak_tick);
    v2k_message_put_u32(s_message, (uint16_t)(off + 72u), cpu1->budget_violations);
    v2k_message_put_u32(s_message, (uint16_t)(off + 76u), cpu1->isr_overflows);
    v2k_message_put_u32(s_message, (uint16_t)(off + 80u), cpu1->prof_seq);
    v2k_message_put_u16(s_message, (uint16_t)(off + 84u), prod->state_seq);
    v2k_message_put_u16(s_message, (uint16_t)(off + 86u), prod->frozen_count);
    v2k_message_put_u32(s_message, (uint16_t)(off + 88u), prod->trig_tick);
    v2k_message_put_u16(s_message, (uint16_t)(off + 92u), prod->bind_ack_seq);
    v2k_message_put_u16(s_message, (uint16_t)(off + 94u), 0u);
    return 96u;
}

static void v2k_handle_status(uint16_t seq)
{
    uint16_t off = v2k_protocol_response_begin(V2K_MSG_STATUS, seq);
    v2k_protocol_finish_response(v2k_write_status_payload(off));
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
    off = v2k_protocol_push_begin(V2K_MSG_STATUS_PUSH, 0u);
    if (v2k_protocol_submit_push(v2k_write_status_payload(off),
                             V2K_PROTOCOL_TX_PRIO_NORMAL))
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
    volatile v2k_catalog_req_t *req = &g_v2k_cpu2_plane.catalog_req;
    const volatile v2k_catalog_resp_t *resp =
        &V2K_CPU1_PLANE_RO->catalog.enum_resp;
    uint16_t start;
    uint16_t max_count;
    uint16_t req_seq;
    uint16_t i;
    uint16_t off;
    if (payload_len != 4u)
    {
        v2k_protocol_send_ack(V2K_MSG_ENUM, seq, V2K_ACK_BAD_PARAM, 0uL);
        return;
    }
    start = v2k_message_get_u16(payload, 0u);
    max_count = payload[2] & 0xFFu;

    req_seq = (uint16_t)(s_enum_req_seq + 1u);
    if (req_seq == 0u)
    {
        req_seq = 1u;
    }
    s_enum_req_seq = req_seq;
    req->start_idx = start;
    req->max_count = max_count;
    req->reserved = 0u;
    req->req_seq = req_seq;

    for (i = 0u; i < 6000u; i++)
    {
        if (resp->ack_seq == req_seq)
        {
            break;
        }
        v2k_cpu2_board_delay_us(1u);
    }
    if (resp->ack_seq != req_seq)
    {
        v2k_protocol_send_ack(V2K_MSG_ENUM, seq, V2K_ACK_INTERNAL, req_seq);
        return;
    }
    if (resp->result == V2K_CATALOG_RESULT_BAD_PARAM)
    {
        v2k_protocol_send_ack(V2K_MSG_ENUM, seq, V2K_ACK_BAD_PARAM, req_seq);
        return;
    }
    if ((resp->result != V2K_CATALOG_RESULT_OK) ||
        (resp->payload_len > V2K_WIRE_MAX_PAYLOAD))
    {
        v2k_protocol_send_ack(V2K_MSG_ENUM, seq, V2K_ACK_INTERNAL, req_seq);
        return;
    }
    off = v2k_protocol_response_begin(V2K_MSG_ENUM, seq);
    for (i = 0u; i < resp->payload_len; i++)
    {
        s_message[(uint16_t)(off + i)] = resp->payload[i] & 0xFFu;
    }
    v2k_protocol_finish_response(resp->payload_len);
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
    // Volatile shadow pointer: CPU1 copies this region when commit_seq
    // advances, so the staged writes must be ordered before that publish
    // (the commit itself is in v2k_handle_cal_commit).
    volatile v2k_param_shadow_t *shadow = &g_v2k_cpu2_plane.param_shadow;
    uint16_t count;
    uint16_t i;
    if (payload_len < 2u)
    {
        v2k_protocol_send_ack(V2K_MSG_CAL_WRITE, seq, V2K_ACK_BAD_PARAM, 0uL);
        return;
    }
    if (shadow->commit_seq !=
        V2K_CPU1_PLANE_RO->param_status.applied_seq)
    {
        v2k_protocol_send_ack(V2K_MSG_CAL_WRITE, seq, V2K_ACK_BUSY, 0uL);
        return;
    }
    if (s_cal_staged_commit_seq != shadow->commit_seq)
    {
        shadow->count = 0u;
        s_cal_staged_commit_seq = shadow->commit_seq;
    }
    count = payload[0] & 0xFFu;
    if ((count == 0u) || (payload_len != (uint16_t)(2u + 12u * count)))
    {
        v2k_protocol_send_ack(V2K_MSG_CAL_WRITE, seq, V2K_ACK_BAD_PARAM, 0uL);
        return;
    }
    for (i = 0u; i < count; i++)
    {
        uint16_t in = (uint16_t)(2u + 12u * i);
        uint32_t addr = v2k_message_get_u32(payload, in);
        int16_t existing = v2k_find_staged(addr);
        uint16_t dst;
        if (existing >= 0)
        {
            dst = (uint16_t)existing;
        }
        else
        {
            if (shadow->count >= V2K_PARAM_BATCH_MAX)
            {
                v2k_protocol_send_ack(V2K_MSG_CAL_WRITE, seq,
                             V2K_ACK_BAD_PARAM, 0uL);
                return;
            }
            dst = shadow->count++;
        }
        shadow->writes[dst].addr = addr;
        shadow->writes[dst].value_bits =
            v2k_message_get_u32(payload, (uint16_t)(in + 4u));
        shadow->writes[dst].type =
            v2k_message_get_u16(payload, (uint16_t)(in + 8u));
        shadow->writes[dst].reserved = 0u;
    }
    v2k_protocol_send_ack(V2K_MSG_CAL_WRITE, seq, V2K_ACK_OK, 0uL);
}

static void v2k_handle_cal_commit(uint16_t seq, uint16_t payload_len)
{
    volatile v2k_param_shadow_t *shadow = &g_v2k_cpu2_plane.param_shadow;
    uint32_t commit_seq;
    if ((payload_len != 0u) || (shadow->count == 0u))
    {
        v2k_protocol_send_ack(V2K_MSG_CAL_COMMIT, seq, V2K_ACK_BAD_PARAM, 0uL);
        return;
    }
    if (shadow->commit_seq !=
        V2K_CPU1_PLANE_RO->param_status.applied_seq)
    {
        v2k_protocol_send_ack(V2K_MSG_CAL_COMMIT, seq, V2K_ACK_BUSY, 0uL);
        return;
    }
    commit_seq = shadow->commit_seq + 1uL;
    shadow->commit_seq = commit_seq;
    v2k_protocol_send_ack(V2K_MSG_CAL_COMMIT, seq, V2K_ACK_OK, commit_seq);
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
        v2k_protocol_send_ack(V2K_MSG_CAL_READ, seq, V2K_ACK_BAD_PARAM, 0uL);
        return;
    }
    count = payload[0] & 0xFFu;
    if ((count == 0u) || (count > V2K_CAL_READ_MAX) ||
        (payload_len != (uint16_t)(2u + 8u * count)))
    {
        v2k_protocol_send_ack(V2K_MSG_CAL_READ, seq, V2K_ACK_BAD_PARAM, 0uL);
        return;
    }
    read_seq = req->read_seq + 1uL;
    req->count = count;
    req->reserved = 0u;
    for (i = 0u; i < count; i++)
    {
        uint16_t in = (uint16_t)(2u + 8u * i);
        req->refs[i].addr = v2k_message_get_u32(payload, in);
        req->refs[i].type = v2k_message_get_u16(payload, (uint16_t)(in + 4u));
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
        v2k_protocol_send_ack(V2K_MSG_CAL_READ, seq, V2K_ACK_INTERNAL, read_seq);
        return;
    }
    result = resp->result;
    if (result != V2K_CAL_OK)
    {
        v2k_protocol_send_ack(V2K_MSG_CAL_READ, seq, V2K_ACK_BAD_PARAM, read_seq);
        return;
    }
    if (resp->count != count)
    {
        v2k_protocol_send_ack(V2K_MSG_CAL_READ, seq, V2K_ACK_INTERNAL, read_seq);
        return;
    }
    off = v2k_protocol_response_begin(V2K_MSG_CAL_READ, seq);
    v2k_message_put_u32(s_message, off, read_seq);
    s_message[(uint16_t)(off + 4u)] = count;
    s_message[(uint16_t)(off + 5u)] = 0u;
    v2k_message_put_u16(s_message, (uint16_t)(off + 6u), 0u);
    off = (uint16_t)(off + 8u);
    for (i = 0u; i < count; i++)
    {
        v2k_message_put_u32(s_message, off, resp->value_bits[i]);
        off = (uint16_t)(off + 4u);
    }
    v2k_protocol_finish_response((uint16_t)(off - 7u));
}


static void v2k_handle_cmd(uint16_t seq, const uint16_t *payload,
                           uint16_t payload_len)
{
    // Volatile request pointer: cmd_seq is the publish, so the code/arg
    // stores must not be reorderable past it (CPU1 polls at 1 kHz and would
    // execute a command with stale arguments).
    volatile v2k_cmd_req_t *req = &g_v2k_msg_2to1.cmd_req;
    uint32_t cmd_seq;
    if (payload_len != 8u)
    {
        v2k_protocol_send_ack(V2K_MSG_CMD, seq, V2K_ACK_BAD_PARAM, 0uL);
        return;
    }
    if (req->cmd_seq != V2K_MSG_1TO2_RO->cpu1_status.ack_seq)
    {
        v2k_protocol_send_ack(V2K_MSG_CMD, seq, V2K_ACK_BUSY, 0uL);
        return;
    }
    cmd_seq = req->cmd_seq + 1uL;
    req->cmd_code = v2k_message_get_u16(payload, 0u);
    req->arg0 = v2k_message_get_u16(payload, 2u);
    req->arg1 = v2k_message_get_u32(payload, 4u);
    req->cmd_seq = cmd_seq;
    v2k_protocol_send_ack(V2K_MSG_CMD, seq, V2K_ACK_OK, cmd_seq);
}

static void v2k_dispatch(uint16_t msg_type, uint16_t seq,
                         const uint16_t *payload, uint16_t payload_len)
{
    switch (msg_type)
    {
        case V2K_MSG_HELLO:
            if (payload_len == 0u) v2k_handle_hello(seq);
            else v2k_protocol_send_ack(msg_type, seq, V2K_ACK_BAD_PARAM, 0uL);
            break;
        case V2K_MSG_STATUS:
            if (payload_len == 0u) v2k_handle_status(seq);
            else v2k_protocol_send_ack(msg_type, seq, V2K_ACK_BAD_PARAM, 0uL);
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
            v2k_scope_protocol_handle_daq_ctrl(seq, payload, payload_len);
            break;
        case V2K_MSG_CAPTURE_REPLAY_REQ:
            v2k_scope_protocol_handle_capture_replay(seq, payload, payload_len);
            break;
        case V2K_MSG_DAQ_BIND:
            v2k_scope_protocol_handle_daq_bind(seq, payload, payload_len);
            break;
        case V2K_MSG_CMD:
            v2k_handle_cmd(seq, payload, payload_len);
            break;
        default:
            v2k_protocol_send_ack(msg_type, seq, V2K_ACK_UNSUPPORTED, 0uL);
            break;
    }
}


uint16_t v2k_protocol_handle_request(
    const v2k_protocol_endpoint_t *endpoint,
    uint16_t msg_type,
    uint16_t seq,
    const uint16_t *payload,
    uint16_t payload_len,
    const uint16_t **response,
    uint16_t *response_len)
{
    if ((endpoint == 0) || (response == 0) || (response_len == 0))
    {
        return 0u;
    }

    s_request_endpoint = endpoint;
    s_response_len = 0u;
    s_response_ready = 0u;
    v2k_dispatch(msg_type, seq, payload, payload_len);
    s_request_endpoint = 0;

    if (!s_response_ready)
    {
        return 0u;
    }
    *response = s_message;
    *response_len = s_response_len;
    return 1u;
}

void v2k_protocol_init(void)
{
    memset(s_message, 0, sizeof(s_message));
    s_response_len = 0u;
    s_response_ready = 0u;
    s_cal_staged_commit_seq = 0xFFFFFFFFuL;
    s_enum_req_seq = V2K_CPU1_PLANE_RO->catalog.enum_resp.ack_seq;
    s_last_status_push_ms = 0uL;
    s_cpu1_seen_heartbeat = V2K_MSG_1TO2_RO->cpu1_status.heartbeat;
    s_cpu1_seen_tick = V2K_MSG_1TO2_RO->cpu1_status.tick;
    s_cpu1_stale = 0u;
    s_cpu1_last_change_ms = v2k_cpu2_board_millis();
    s_request_endpoint = 0;
    s_push_endpoint = 0;
    v2k_scope_protocol_init();
}

void v2k_protocol_service(const v2k_protocol_endpoint_t *push_endpoint,
                          uint16_t response_pending)
{
    s_push_endpoint = push_endpoint;
    v2k_update_cpu1_stale_monitor();
    v2k_scope_protocol_update();

    if ((push_endpoint != 0) &&
        (g_v2k_msg_2to1.cpu2_status.link_state != 0u) &&
        !response_pending)
    {
        while (v2k_scope_protocol_start_capture_push(1u) ||
               v2k_scope_protocol_start_capture_push(0u) ||
               v2k_start_status_push() ||
               v2k_scope_protocol_start_stream_push())
        {
        }
    }
    s_push_endpoint = 0;
}
