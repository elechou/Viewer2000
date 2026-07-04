//=============================================================================
// v2k_sci_service.h - Viewer2000 SCI frame and byte-stream adapter
//=============================================================================
#ifndef V2K_SCI_SERVICE_H
#define V2K_SCI_SERVICE_H

#include <stdint.h>
#include "v2k_protocol.h"

extern volatile uint32_t g_v2k_sci_rx_octets;
extern volatile uint32_t g_v2k_sci_tx_octets;
extern volatile uint32_t g_v2k_sci_rx_overflow;
extern volatile uint32_t g_v2k_sci_rx_errors;
extern volatile uint32_t g_v2k_sci_bad_frames;
extern volatile uint32_t g_v2k_sci_good_frames;
extern volatile uint32_t g_v2k_sci_tx_frames;
extern volatile uint32_t g_v2k_sci_tx_queue_full;
extern volatile uint32_t g_v2k_sci_tx_refill_isr;
extern volatile uint32_t g_v2k_sci_tx_refill_kick;
extern volatile uint32_t g_v2k_sci_tx_fifo_empty_refills;

void v2k_sci_init(void);
void v2k_sci_poll(void);
void v2k_sci_flush(void);
uint16_t v2k_sci_response_pending(void);
const v2k_protocol_endpoint_t *v2k_sci_endpoint(void);

#endif // V2K_SCI_SERVICE_H
