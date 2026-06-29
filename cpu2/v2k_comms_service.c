//=============================================================================
// v2k_comms_service.c - CPU2 communication endpoint orchestrator
//
// Inbound transports call the shared protocol service. The selected push
// endpoint owns stream/status delivery; EtherCAT can be added here without
// moving request semantics back into a physical-link module.
//=============================================================================

#include "v2k_comms_service.h"
#include "v2k_protocol.h"
#include "v2k_sci_service.h"

void v2k_comms_init(void)
{
    v2k_sci_init();
    v2k_protocol_init();
}

void v2k_comms_service(void)
{
    v2k_sci_poll();
    v2k_protocol_service(v2k_sci_endpoint(),
                         v2k_sci_response_pending());
    v2k_sci_flush();
}
