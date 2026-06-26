//=============================================================================
// v2k_sci_service.h - Phase 3.5 Viewer2000 protocol data pump
//=============================================================================
#ifndef V2K_SCI_SERVICE_H
#define V2K_SCI_SERVICE_H

#include <stdint.h>

// Baud rate, frame format, FIFO enable, and the physical pipe instance are
// board-profile configuration. This service owns only the Viewer2000 wire
// protocol and shared-plane transactions.

extern volatile uint32_t g_v2k_sci_rx_octets;
extern volatile uint32_t g_v2k_sci_tx_octets;
extern volatile uint32_t g_v2k_sci_rx_overflow;
extern volatile uint32_t g_v2k_sci_bad_frames;
extern volatile uint32_t g_v2k_sci_good_frames;

void v2k_sci_init(void);
void v2k_sci_service(void);

#endif // V2K_SCI_SERVICE_H
