//=============================================================================
// v2k_sci_frame.h - Viewer2000 SCI CRC/COBS frame adapter
//=============================================================================
#ifndef V2K_SCI_FRAME_H
#define V2K_SCI_FRAME_H

#include <stdint.h>
#include "v2k_message_codec.h"

#define V2K_SCI_FRAME_CRC_OCTETS 4u
#define V2K_SCI_FRAME_RAW_MAX \
    (V2K_MESSAGE_MAX_OCTETS + V2K_SCI_FRAME_CRC_OCTETS)
#define V2K_SCI_FRAME_WIRE_MAX \
    (V2K_SCI_FRAME_RAW_MAX + (V2K_SCI_FRAME_RAW_MAX / 254u) + 2u)

uint16_t v2k_sci_frame_encode(const uint16_t *message,
                              uint16_t message_len,
                              uint16_t *frame,
                              uint16_t frame_cap,
                              uint16_t *frame_len);
uint16_t v2k_sci_frame_decode_in_place(uint16_t *frame,
                                       uint16_t frame_len,
                                       uint16_t *message_len);

#endif // V2K_SCI_FRAME_H
