//=============================================================================
// v2k_registry.h - 平台描述符与参数事务服务
//=============================================================================
#ifndef V2K_REGISTRY_H
#define V2K_REGISTRY_H

#include "../contracts/v2k_common.h"

void v2k_registry_init(v2k_build_hash_t build_hash);
void v2k_param_service(void);
void v2k_param_apply_ready(void);
void v2k_param_read_service(void);

#endif // V2K_REGISTRY_H
