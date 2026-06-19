//=============================================================================
// cl2000-side compile of the contract static asserts (CHAR_BIT=16; for the PC
// side see the header note in tools/check_contracts.c — the same asserts are
// compiled once per platform, so "memory layout == on-wire format" only holds
// when it passes on both ends). Emits no code or data; compile-time only.
//=============================================================================
#include "../../tools/check_contracts.c"
#include "../../common/v2k_planes.h"
