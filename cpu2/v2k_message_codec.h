//=============================================================================
// v2k_message_codec.h - Transport-neutral Viewer2000 message codec
//=============================================================================
#ifndef V2K_MESSAGE_CODEC_H
#define V2K_MESSAGE_CODEC_H

#include <stdint.h>
#include "../contracts/v2k_common.h"

#define V2K_MESSAGE_HEADER_OCTETS 7u
#define V2K_MESSAGE_MAX_OCTETS \
    (V2K_MESSAGE_HEADER_OCTETS + V2K_WIRE_MAX_PAYLOAD)

uint16_t v2k_message_get_u16(const uint16_t *buf, uint16_t off);
uint32_t v2k_message_get_u32(const uint16_t *buf, uint16_t off);
void v2k_message_put_u16(uint16_t *buf, uint16_t off, uint16_t value);
void v2k_message_put_u32(uint16_t *buf, uint16_t off, uint32_t value);
uint16_t v2k_message_begin(uint16_t *message,
                           uint16_t msg_type,
                           uint16_t seq);
uint16_t v2k_message_response_begin(uint16_t *message,
                                    uint16_t msg_type,
                                    uint16_t seq);
uint16_t v2k_message_finish(uint16_t *message,
                            uint16_t payload_len,
                            uint16_t *message_len);

#endif // V2K_MESSAGE_CODEC_H
