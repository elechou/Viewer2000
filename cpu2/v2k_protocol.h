//=============================================================================
// v2k_protocol.h - Transport-neutral Viewer2000 protocol service
//=============================================================================
#ifndef V2K_PROTOCOL_H
#define V2K_PROTOCOL_H

#include <stdint.h>

#define V2K_PROTOCOL_TX_PRIO_NORMAL 0u
#define V2K_PROTOCOL_TX_PRIO_HIGH   1u

// Each physical link supplies its HELLO-facing block size and synchronous
// push callbacks. tx_submit_message must copy or consume the message before it
// returns; the protocol core reuses its scratch buffer for the next message.
typedef struct {
    uint16_t scope_block_nticks;
    uint16_t (*tx_can_submit)(uint16_t prio);
    uint16_t (*tx_submit_message)(const uint16_t *message,
                                  uint16_t message_len,
                                  uint16_t prio);
} v2k_protocol_endpoint_t;

void v2k_protocol_init(void);
// The returned response is valid until the next protocol request or service
// call. The ingress adapter must frame/copy it synchronously.
uint16_t v2k_protocol_handle_request(
    const v2k_protocol_endpoint_t *endpoint,
    uint16_t msg_type,
    uint16_t seq,
    const uint16_t *payload,
    uint16_t payload_len,
    const uint16_t **response,
    uint16_t *response_len);
void v2k_protocol_service(const v2k_protocol_endpoint_t *push_endpoint,
                          uint16_t response_pending);

#endif // V2K_PROTOCOL_H
