//=============================================================================
// v2k_sci_service.c - Viewer2000 SCI frame and byte-stream adapter
//
// This layer owns the COBS delimiter stream, CRC validation, response replay,
// and the SCI board-pipe queue. It passes validated messages to v2k_protocol.
//=============================================================================

#include <string.h>
#include "../contracts/v2k_scope.h"
#include "v2k_cpu2_board.h"
#include "v2k_message_codec.h"
#include "v2k_protocol.h"
#include "v2k_sci_frame.h"
#include "v2k_sci_service.h"

#define V2K_SCI_RX_FRAME_WORDS 256u

extern v2k_msg_2to1_t g_v2k_msg_2to1;

// The largest current request is 16 CAL_WRITE entries, 205 raw octets total.
// On C28x each octet occupies one 16-bit word.
static uint16_t s_rx_frame[V2K_SCI_RX_FRAME_WORDS];
static uint16_t s_rx_frame_len;
static uint16_t s_rx_discard;
static uint16_t s_tx_frame[V2K_SCI_FRAME_WIRE_MAX];
static uint16_t s_response_frame[V2K_SCI_FRAME_WIRE_MAX];
static uint16_t s_response_len;
static uint16_t s_response_pending;
static uint16_t s_last_response_req_type;
static uint16_t s_last_response_seq;
static uint16_t s_have_last_response;

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

static uint16_t v2k_sci_board_prio(uint16_t protocol_prio,
                                   uint16_t *board_prio)
{
    if (protocol_prio == V2K_PROTOCOL_TX_PRIO_HIGH)
    {
        *board_prio = V2K_CPU2_BOARD_PIPE_TX_PRIO_HIGH;
        return 1u;
    }
    if (protocol_prio == V2K_PROTOCOL_TX_PRIO_NORMAL)
    {
        *board_prio = V2K_CPU2_BOARD_PIPE_TX_PRIO_NORMAL;
        return 1u;
    }
    return 0u;
}

static uint16_t v2k_sci_tx_can_submit(uint16_t prio)
{
    uint16_t board_prio;

    return v2k_sci_board_prio(prio, &board_prio) &&
           v2k_cpu2_board_pipe_tx_can_submit(board_prio);
}

static uint16_t v2k_sci_tx_submit_message(const uint16_t *message,
                                          uint16_t message_len,
                                          uint16_t prio)
{
    uint16_t board_prio;
    uint16_t frame_len;

    if (!v2k_sci_board_prio(prio, &board_prio) ||
        !v2k_sci_frame_encode(message, message_len,
                              s_tx_frame, V2K_SCI_FRAME_WIRE_MAX,
                              &frame_len))
    {
        return 0u;
    }
    return v2k_cpu2_board_pipe_tx_submit_frame(
        s_tx_frame, frame_len, board_prio);
}

static const v2k_protocol_endpoint_t s_endpoint = {
    V2K_BLOCK_NTICKS_SCI,
    v2k_sci_tx_can_submit,
    v2k_sci_tx_submit_message,
};

const v2k_protocol_endpoint_t *v2k_sci_endpoint(void)
{
    return &s_endpoint;
}

static void v2k_sci_queue_response(uint16_t request_type,
                                   uint16_t seq,
                                   const uint16_t *message,
                                   uint16_t message_len)
{
    if (s_response_pending)
    {
        return;
    }
    if (v2k_sci_frame_encode(message, message_len,
                             s_response_frame, V2K_SCI_FRAME_WIRE_MAX,
                             &s_response_len))
    {
        s_response_pending = 1u;
        s_last_response_req_type = request_type;
        s_last_response_seq = seq;
        s_have_last_response = 1u;
    }
}

static void v2k_sci_start_pending_response(void)
{
    if (s_response_pending &&
        v2k_cpu2_board_pipe_tx_can_submit(
            V2K_CPU2_BOARD_PIPE_TX_PRIO_HIGH) &&
        v2k_cpu2_board_pipe_tx_submit_frame(
            s_response_frame, s_response_len,
            V2K_CPU2_BOARD_PIPE_TX_PRIO_HIGH))
    {
        s_response_pending = 0u;
    }
}

static void v2k_sci_process_encoded_frame(uint16_t *frame,
                                          uint16_t frame_len)
{
    uint16_t message_len;
    uint16_t payload_len;
    uint16_t request_type;
    uint16_t seq;
    const uint16_t *response;
    uint16_t response_len;

    if (!v2k_sci_frame_decode_in_place(frame, frame_len, &message_len))
    {
        g_v2k_sci_bad_frames++;
        return;
    }

    request_type = frame[1] & 0x7Fu;
    seq = v2k_message_get_u16(frame, 3u);
    payload_len = v2k_message_get_u16(frame, 5u);
    g_v2k_sci_good_frames++;
    g_v2k_msg_2to1.cpu2_status.link_state = 1u;

    if (s_have_last_response &&
        (request_type == s_last_response_req_type) &&
        (seq == s_last_response_seq))
    {
        // A host timeout retry reuses the original response without executing
        // COMMIT/CMD-style services a second time.
        s_response_pending = 1u;
        return;
    }
    if (s_response_pending)
    {
        return;
    }
    if (v2k_protocol_handle_request(
            &s_endpoint, request_type, seq, &frame[7], payload_len,
            &response, &response_len))
    {
        v2k_sci_queue_response(request_type, seq, response, response_len);
    }
}

static void v2k_sci_rx_service(void)
{
    uint16_t value;

    while (v2k_cpu2_board_pipe_read_octet(&value))
    {
        value &= 0xFFu;
        if (value == 0u)
        {
            if (!s_rx_discard && (s_rx_frame_len != 0u))
            {
                v2k_sci_process_encoded_frame(s_rx_frame, s_rx_frame_len);
            }
            s_rx_frame_len = 0u;
            s_rx_discard = 0u;
        }
        else if (!s_rx_discard)
        {
            if (s_rx_frame_len < V2K_SCI_RX_FRAME_WORDS)
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
    s_rx_frame_len = 0u;
    s_rx_discard = 0u;
    s_response_len = 0u;
    s_response_pending = 0u;
    s_last_response_req_type = 0u;
    s_last_response_seq = 0u;
    s_have_last_response = 0u;
    v2k_cpu2_board_pipe_init();
}

void v2k_sci_poll(void)
{
    v2k_cpu2_board_pipe_service();
    v2k_sci_start_pending_response();
    v2k_sci_rx_service();
}

void v2k_sci_flush(void)
{
    v2k_sci_start_pending_response();
    v2k_cpu2_board_pipe_service();
}

uint16_t v2k_sci_response_pending(void)
{
    return s_response_pending;
}
