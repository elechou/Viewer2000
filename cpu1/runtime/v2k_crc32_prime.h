//=============================================================================
// v2k_crc32_prime.h - TI linker CRC32_PRIME-compatible CRC helper
//=============================================================================
#ifndef V2K_CRC32_PRIME_H
#define V2K_CRC32_PRIME_H

#include <stdint.h>

uint32_t v2k_crc32_prime(const uint16_t *src, uint32_t words);

#endif // V2K_CRC32_PRIME_H
