//=============================================================================
// v2k_message_codec.c - Transport-neutral Viewer2000 message codec
//
// C28x uint16_t buffers store one octet in each word's low 8 bits. This module
// owns only the common message header and little-endian field representation.
//=============================================================================

#include "v2k_message_codec.h"

uint16_t v2k_message_get_u16(const uint16_t *buf, uint16_t off)
{
    return (uint16_t)((buf[off] & 0xFFu) |
                      ((buf[(uint16_t)(off + 1u)] & 0xFFu) << 8u));
}

uint32_t v2k_message_get_u32(const uint16_t *buf, uint16_t off)
{
    uint32_t lo = v2k_message_get_u16(buf, off);
    uint32_t hi = v2k_message_get_u16(buf, (uint16_t)(off + 2u));
    return lo | (hi << 16u);
}

void v2k_message_put_u16(uint16_t *buf, uint16_t off, uint16_t value)
{
    buf[off] = value & 0xFFu;
    buf[(uint16_t)(off + 1u)] = (value >> 8u) & 0xFFu;
}

void v2k_message_put_u32(uint16_t *buf, uint16_t off, uint32_t value)
{
    v2k_message_put_u16(buf, off, (uint16_t)value);
    v2k_message_put_u16(buf, (uint16_t)(off + 2u),
                        (uint16_t)(value >> 16u));
}

uint16_t v2k_message_begin(uint16_t *message,
                           uint16_t msg_type,
                           uint16_t seq)
{
    message[0] = V2K_WIRE_VER_MAGIC;
    message[1] = msg_type & 0xFFu;
    message[2] = 0u;
    v2k_message_put_u16(message, 3u, seq);
    v2k_message_put_u16(message, 5u, 0u);
    return V2K_MESSAGE_HEADER_OCTETS;
}

uint16_t v2k_message_response_begin(uint16_t *message,
                                    uint16_t msg_type,
                                    uint16_t seq)
{
    return v2k_message_begin(message, (uint16_t)(msg_type | 0x80u), seq);
}

uint16_t v2k_message_finish(uint16_t *message,
                            uint16_t payload_len,
                            uint16_t *message_len)
{
    if ((message == 0) || (message_len == 0) ||
        (payload_len > V2K_WIRE_MAX_PAYLOAD))
    {
        return 0u;
    }

    v2k_message_put_u16(message, 5u, payload_len);
    *message_len = (uint16_t)(V2K_MESSAGE_HEADER_OCTETS + payload_len);
    return 1u;
}
