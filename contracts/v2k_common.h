//=============================================================================
// v2k_common.h - Viewer2000 shared-interface base definitions
//
// This file and the other headers under contracts/ are the only authority for
// the four shared-memory interfaces. They must be compiled or understood by:
//   1. CPU1 (cl2000, C28x, CHAR_BIT=16)
//   2. CPU2 (cl2000, same ISA, shared structs have no ABI mismatch)
//   3. PC-side unit tests (gcc/clang, CHAR_BIT=8). These are serializer tests;
//      the PC does not directly access on-chip memory layout. Only the on-wire
//      format must match across platforms.
//
// ---- Struct definition rules ----
// 1. Shared structs may contain only these member types:
//      int16_t / uint16_t / int32_t / uint32_t / float (32-bit) / char[] (names only)
//    Forbidden: uint8_t (not available on C28x), uint64_t/double (different
//    alignment rules across platforms), bitfields, enum members directly
//    (width is implementation-defined; use uint16_t + #define), and pointers.
// 2. 32-bit members must land on 32-bit boundaries. Arrange fields manually by
//    declaration order; do not rely on implicit padding.
// 3. String fields: on C28x, each 16-bit char stores one ASCII character as a
//    natural C string. The wire serializer emits one octet per character.
//    Structs containing char[] therefore have different bit sizes on C28x and
//    PC. That is expected: memory layout only has to match between CPU1/CPU2,
//    while the on-wire layout is fixed by docs/wire-spec.md.
// 4. Serialization must never memcpy a struct. Serialize every field explicitly.
//
// ---- Unit convention ----
// * "word"  = C28x 16-bit addressable unit (on-chip addresses and sizeof unit)
// * "octet" = on-wire 8-bit byte (wire spec and SCI/EtherCAT payload unit)
// Use the explicit word or octet term in comments and field names. Avoid bare
// "byte" where the width would be ambiguous.
//=============================================================================
#ifndef V2K_COMMON_H
#define V2K_COMMON_H

#include <stdint.h>
#include <limits.h>

//-----------------------------------------------------------------------------
// Platform detection
//-----------------------------------------------------------------------------
#if defined(__TMS320C28XX__)
#define V2K_PLATFORM_C28X 1
#else
#define V2K_PLATFORM_C28X 0
#endif

//-----------------------------------------------------------------------------
// Static assert, C99-compatible negative-array-size trick. Works with cl2000
// and gcc/clang. Use extern declarations instead of typedefs: if different
// headers collide on __LINE__, repeated same-name same-type extern declarations
// are legal C99 and allocate no storage, while typedef redefinition is illegal.
//-----------------------------------------------------------------------------
#define V2K_CONCAT_(a, b) a##b
#define V2K_CONCAT(a, b)  V2K_CONCAT_(a, b)
#define V2K_STATIC_ASSERT(cond) \
    extern char V2K_CONCAT(v2k_static_assert_, __LINE__)[(cond) ? 1 : -1]

// Cross-platform size asserts expressed in bits.
// C28x: sizeof counts 16-bit words, CHAR_BIT=16.
// PC: sizeof counts octets, CHAR_BIT=8.
// sizeof(t) * CHAR_BIT gives the bit count on both platforms, so one assert can
// cover both. Structs with char[] use V2K_NAME_BITS(n) for the name field,
// because that part varies by platform.
#define V2K_SIZEOF_BITS(t)        ((uint32_t)sizeof(t) * (uint32_t)CHAR_BIT)
#define V2K_ASSERT_SIZE_BITS(t, bits) V2K_STATIC_ASSERT(V2K_SIZEOF_BITS(t) == (bits))
#define V2K_NAME_BITS(n)          ((uint32_t)(n) * (uint32_t)CHAR_BIT)

// Base type width self-checks. On C28x, int16_t and char are both 16 bits.
V2K_ASSERT_SIZE_BITS(uint16_t, 16);
V2K_ASSERT_SIZE_BITS(uint32_t, 32);
V2K_ASSERT_SIZE_BITS(float, 32);

//-----------------------------------------------------------------------------
// Protocol and interface versions
//-----------------------------------------------------------------------------
// Wire protocol version: low nibble of frame header ver_magic. The high nibble
// is fixed at 0x5 for resync checking. Increment only for incompatible frame or
// message layout changes. Variable-catalog content changes are covered by
// build_hash-triggered re-enumeration.
#define V2K_WIRE_VER        0xAu
#define V2K_WIRE_VER_MAGIC  (0x50u | V2K_WIRE_VER)   /* = 0x5A, first frame octet */
#define V2K_WIRE_MAX_PAYLOAD 1024u

// Shared-memory layout version. Increment for any shared-struct field change.
// CPU1/CPU2 handshake checks this when firmware images are flashed out of sync.
// See the handshake flow in v2k_command.h.
#define V2K_CONTRACT_VER    14u

//-----------------------------------------------------------------------------
// Public MCU family identifiers reported by HELLO. These identify only the MCU
// family/profile class; private board identity must stay out of the protocol.
//-----------------------------------------------------------------------------
#define V2K_MCU_MODEL_UNKNOWN  0u
#define V2K_MCU_MODEL_F28P65X  1u
#define V2K_MCU_MODEL_F28379D  2u

//-----------------------------------------------------------------------------
// Device capability bits (HELLO capabilities). Append only; never reuse bits.
//-----------------------------------------------------------------------------
#define V2K_CAP_ENUM          (1uL << 0)
#define V2K_CAP_CAL           (1uL << 1)
#define V2K_CAP_SCOPE_STREAM  (1uL << 2)
#define V2K_CAP_SCOPE_CAPTURE (1uL << 3)
#define V2K_CAP_PRE_TRIGGER   (1uL << 4)
#define V2K_CAP_SYSTEM_CMD    (1uL << 5)
#define V2K_CAP_NATIVE_BLOCK  (1uL << 6)

#define V2K_CAPABILITIES_NATIVE \
    (V2K_CAP_ENUM | V2K_CAP_CAL | V2K_CAP_SCOPE_STREAM | \
     V2K_CAP_SCOPE_CAPTURE | V2K_CAP_PRE_TRIGGER | \
     V2K_CAP_SYSTEM_CMD | V2K_CAP_NATIVE_BLOCK)

//-----------------------------------------------------------------------------
// Shared platform types
//-----------------------------------------------------------------------------
// ISR tick: the platform's sole control time, incremented by the CPU1 control
// ISR. uint32 wraps after about 11.9 hours at 100 kHz. The host must process
// unsigned wraparound deltas; long recordings reconstruct absolute time from
// block sequence plus wrap count.
typedef uint32_t v2k_tick_t;

// Firmware build hash: 32-bit truncation injected at link/bake time. If the
// host sees it change after reconnect, it must force descriptor re-enumeration.
// See v2k_descriptor.h.
typedef uint32_t v2k_build_hash_t;

//-----------------------------------------------------------------------------
// Variable type codes used by catalog entry type fields. This is the
// minimum subset shared with the descriptor baker and inspector.
//-----------------------------------------------------------------------------
#define V2K_TYPE_I16  0u
#define V2K_TYPE_U16  1u
#define V2K_TYPE_I32  2u
#define V2K_TYPE_U32  3u
#define V2K_TYPE_F32  4u
#define V2K_TYPE_COUNT 5u

// On-wire type width in octets. On chip, all values are moved through 32-bit
// slots. 16-bit types use the low half; the high half is zero for unsigned
// values and sign-extended for signed values.

//-----------------------------------------------------------------------------
// value_bits convention: parameter and mirror values move as uint32_t bit patterns.
//-----------------------------------------------------------------------------
// * F32     : IEEE-754 bit pattern of float
// * I32/U32 : original value bit pattern
// * I16     : sign-extended to 32 bits; U16 is zero-extended
// CPU1 truncates/converts when writing the target variable according to the
// descriptor type. CPU2 and the host do not interpret the bit pattern.

#endif // V2K_COMMON_H
