//=============================================================================
// v2k_registry.h - platform descriptor and parameter-transaction services
//=============================================================================
#ifndef V2K_REGISTRY_H
#define V2K_REGISTRY_H

#include "../../contracts/v2k_common.h"
#include "../../contracts/v2k_descriptor.h"

void v2k_registry_init(void);
void v2k_registry_add(const char *name, uint16_t type, uint16_t kind,
                      volatile void *addr, uint16_t prescaler);
void v2k_catalog_service(void);
void v2k_param_service(void);
void v2k_param_apply_ready(void);
void v2k_param_read_service(void);

#endif // V2K_REGISTRY_H
