//=============================================================================
// v2k_scope_protocol.h - Viewer2000 DAQ/capture protocol services
//=============================================================================
#ifndef V2K_SCOPE_PROTOCOL_H
#define V2K_SCOPE_PROTOCOL_H

#include <stdint.h>

void v2k_scope_protocol_init(void);
void v2k_scope_protocol_update(void);
void v2k_scope_protocol_handle_daq_ctrl(uint16_t seq,
                                        const uint16_t *payload,
                                        uint16_t payload_len);
void v2k_scope_protocol_handle_daq_bind(uint16_t seq,
                                        const uint16_t *payload,
                                        uint16_t payload_len);
void v2k_scope_protocol_handle_capture_replay(
    uint16_t seq,
    const uint16_t *payload,
    uint16_t payload_len);
uint16_t v2k_scope_protocol_start_capture_push(uint16_t replay);
uint16_t v2k_scope_protocol_start_stream_push(void);

#endif // V2K_SCOPE_PROTOCOL_H
