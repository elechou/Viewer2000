//=============================================================================
// v2k_protocol_internal.h - Shared protocol/scope implementation boundary
//=============================================================================
#ifndef V2K_PROTOCOL_INTERNAL_H
#define V2K_PROTOCOL_INTERNAL_H

#include <stdint.h>
#include "v2k_protocol.h"

#define V2K_MSG_HELLO              0x01u
#define V2K_MSG_STATUS             0x02u
#define V2K_MSG_ENUM               0x03u
#define V2K_MSG_CAL_WRITE          0x10u
#define V2K_MSG_CAL_COMMIT         0x11u
#define V2K_MSG_CAL_READ           0x12u
#define V2K_MSG_DAQ_CTRL           0x20u
#define V2K_MSG_CAPTURE_REPLAY_REQ 0x21u
#define V2K_MSG_DAQ_BIND           0x22u
#define V2K_MSG_CMD                0x30u
#define V2K_MSG_STATUS_PUSH        0x41u
#define V2K_MSG_SCOPE_BLOCK_PUSH   0x42u
#define V2K_MSG_CAPTURE_BATCH_PUSH 0x45u

#define V2K_ACK_OK          0u
#define V2K_ACK_BAD_PARAM   1u
#define V2K_ACK_BUSY        2u
#define V2K_ACK_BAD_STATE   3u
#define V2K_ACK_UNSUPPORTED 4u
#define V2K_ACK_INTERNAL    5u

extern uint16_t g_v2k_protocol_message[];

uint16_t v2k_protocol_response_begin(uint16_t msg_type, uint16_t seq);
void v2k_protocol_finish_response(uint16_t payload_len);
void v2k_protocol_send_ack(uint16_t request_type,
                           uint16_t seq,
                           uint16_t status,
                           uint32_t data);
uint16_t v2k_protocol_push_begin(uint16_t msg_type, uint16_t seq);
uint16_t v2k_protocol_tx_can_submit(uint16_t prio);
uint16_t v2k_protocol_submit_push(uint16_t payload_len, uint16_t prio);

#endif // V2K_PROTOCOL_INTERNAL_H
