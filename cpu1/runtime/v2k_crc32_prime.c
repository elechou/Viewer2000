//=============================================================================
// v2k_crc32_prime.c - TI linker CRC32_PRIME-compatible CRC helper
//=============================================================================

#include "v2k_crc32_prime.h"

#define V2K_CRC32_PRIME_POLY 0x04C11DB7uL

// Match the TI linker CRC32_PRIME model: seed 0, polynomial 0x04C11DB7,
// non-reflected input/output, low byte first for each C28x 16-bit word.
uint32_t v2k_crc32_prime(const uint16_t *src, uint32_t words)
{
    uint32_t crc = 0uL;
    uint32_t i;
    uint16_t byte_index;
    uint16_t bit;

    for (i = 0uL; i < words; i++)
    {
        uint16_t word = src[i];
        for (byte_index = 0u; byte_index < 2u; byte_index++)
        {
            uint16_t octet = (uint16_t)((word >> (byte_index * 8u)) & 0x00FFu);
            crc ^= (uint32_t)octet << 24u;
            for (bit = 0u; bit < 8u; bit++)
            {
                if ((crc & 0x80000000uL) != 0uL)
                {
                    crc = (crc << 1u) ^ V2K_CRC32_PRIME_POLY;
                }
                else
                {
                    crc <<= 1u;
                }
            }
        }
    }
    return crc;
}
