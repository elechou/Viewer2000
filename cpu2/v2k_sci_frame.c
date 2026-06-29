//=============================================================================
// v2k_sci_frame.c - Viewer2000 SCI CRC/COBS frame adapter
//=============================================================================

#include "v2k_sci_frame.h"

#pragma CODE_SECTION(v2k_sci_crc32c, ".TI.ramfunc")
static const uint32_t s_crc32c_nibble[16] = {
    0x00000000uL, 0x105EC76FuL, 0x20BD8EDEuL, 0x30E349B1uL,
    0x417B1DBCuL, 0x5125DAD3uL, 0x61C69362uL, 0x7198540DuL,
    0x82F63B78uL, 0x92A8FC17uL, 0xA24BB5A6uL, 0xB21572C9uL,
    0xC38D26C4uL, 0xD3D3E1ABuL, 0xE330A81AuL, 0xF36E6F75uL,
};

static uint32_t v2k_sci_crc32c(const uint16_t *buf, uint16_t len)
{
    uint16_t i;
    uint32_t crc = 0xFFFFFFFFuL;

    for (i = 0u; i < len; i++)
    {
        uint16_t octet = buf[i] & 0xFFu;
        crc = s_crc32c_nibble[(crc ^ octet) & 0x0Fu] ^ (crc >> 4u);
        crc = s_crc32c_nibble[(crc ^ (octet >> 4u)) & 0x0Fu] ^
              (crc >> 4u);
    }

    return crc ^ 0xFFFFFFFFuL;
}

static uint16_t v2k_sci_cobs_decode_in_place(uint16_t *buf, uint16_t len)
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

static uint16_t v2k_sci_cobs_encode_frame(const uint16_t *message,
                                    uint16_t message_len,
                                    uint32_t crc,
                                    uint16_t *dst,
                                    uint16_t cap)
{
    uint16_t read;
    uint16_t len = (uint16_t)(message_len + V2K_SCI_FRAME_CRC_OCTETS);
    uint16_t write = 1u;
    uint16_t code_pos = 0u;
    uint16_t code = 1u;

    if (cap < 2u)
    {
        return 0u;
    }
    for (read = 0u; read < len; read++)
    {
        uint16_t octet;

        if (read < message_len)
        {
            octet = message[read] & 0xFFu;
        }
        else
        {
            uint16_t crc_off = (uint16_t)(read - message_len);
            octet = (uint16_t)(crc >> (crc_off * 8u)) & 0xFFu;
        }
        if (octet == 0u)
        {
            if (write >= cap)
            {
                return 0u;
            }
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
                if (write >= cap)
                {
                    return 0u;
                }
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

uint16_t v2k_sci_frame_encode(const uint16_t *message,
                              uint16_t message_len,
                              uint16_t *frame,
                              uint16_t frame_cap,
                              uint16_t *frame_len)
{
    uint32_t crc;

    if ((message == 0) || (frame == 0) || (frame_len == 0) ||
        (message_len < V2K_MESSAGE_HEADER_OCTETS) ||
        (message_len > V2K_MESSAGE_MAX_OCTETS))
    {
        return 0u;
    }

    crc = v2k_sci_crc32c(message, message_len);
    *frame_len = v2k_sci_cobs_encode_frame(
        message, message_len, crc, frame, frame_cap);
    return (*frame_len != 0u) ? 1u : 0u;
}

uint16_t v2k_sci_frame_decode_in_place(uint16_t *frame,
                                       uint16_t frame_len,
                                       uint16_t *message_len)
{
    uint16_t raw_len;
    uint16_t payload_len;
    uint16_t expected_message_len;
    uint32_t received_crc;
    uint32_t calculated_crc;

    if ((frame == 0) || (message_len == 0) || (frame_len == 0u))
    {
        return 0u;
    }
    raw_len = v2k_sci_cobs_decode_in_place(frame, frame_len);
    if (raw_len < (V2K_MESSAGE_HEADER_OCTETS + V2K_SCI_FRAME_CRC_OCTETS))
    {
        return 0u;
    }

    payload_len = v2k_message_get_u16(frame, 5u);
    expected_message_len =
        (uint16_t)(V2K_MESSAGE_HEADER_OCTETS + payload_len);
    if ((frame[0] != V2K_WIRE_VER_MAGIC) ||
        (frame[2] != 0u) ||
        (payload_len > V2K_WIRE_MAX_PAYLOAD) ||
        (raw_len != (uint16_t)(expected_message_len +
                               V2K_SCI_FRAME_CRC_OCTETS)))
    {
        return 0u;
    }

    received_crc = v2k_message_get_u32(frame, expected_message_len);
    calculated_crc = v2k_sci_crc32c(frame, expected_message_len);
    if (received_crc != calculated_crc)
    {
        return 0u;
    }

    *message_len = expected_message_len;
    return 1u;
}
