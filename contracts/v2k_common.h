//=============================================================================
// v2k_common.h — Viewer2000 共享接口基础定义
//
// 本文件与 contracts/ 下其余头文件构成四个共享内存接口的唯一基准，
// 必须同时被以下三方编译/理解：
//   1. CPU1 (cl2000, C28x, CHAR_BIT=16)
//   2. CPU2 (cl2000, 同 ISA, 共享 struct 无 ABI 问题)
//   3. PC 端单元测试 (gcc/clang, CHAR_BIT=8) —— 仅用于序列化器测试，
//      PC 不直接访问片上内存布局，跨平台只需"线上格式"一致。
//
// ---- struct 定义规则（保证三方读出同一布局）----
// 1. 共享 struct 只允许下列成员类型：
//      int16_t / uint16_t / int32_t / uint32_t / float (32-bit) / char[]（仅限名字）
//    禁止: uint8_t（C28x 上不存在）、uint64_t/double（两平台对齐规则不同）、
//          位域、enum 直接做成员（宽度由实现定义，用 uint16_t + #define 代替）、指针。
// 2. 32-bit 成员必须落在 32-bit 边界上（按声明顺序手工排布，禁止依赖隐式填充）。
// 3. 字符串字段：C28x 上每个 16-bit char 存一个 ASCII 字符（自然 C 字符串），
//    线上序列化为每字符 1 octet。因此含 char[] 的 struct 在两平台 bit 尺寸不同，
//    这是预期行为——内存布局只需 CPU1/CPU2 一致，线上布局由 wire-spec 钉死。
// 4. 序列化禁止 struct memcpy（CLAUDE.md 明文规定），必须显式逐字段序列化。
//
// ---- 单位约定 ----
// * "word"  = C28x 16-bit 可寻址单元（片上地址、sizeof 的单位）
// * "octet" = 线上 8-bit 字节（wire-spec、SCI/EtherCAT 载荷的单位）
// 两者出现在注释/字段名中时必须显式写明，禁止裸用 "byte"。
//=============================================================================
#ifndef V2K_COMMON_H
#define V2K_COMMON_H

#include <stdint.h>
#include <limits.h>

//-----------------------------------------------------------------------------
// 平台检测
//-----------------------------------------------------------------------------
#if defined(__TMS320C28XX__)
#define V2K_PLATFORM_C28X 1
#else
#define V2K_PLATFORM_C28X 0
#endif

//-----------------------------------------------------------------------------
// 静态断言（C99 兼容：负数组尺寸技巧；cl2000 与 gcc/clang 均可用）
// 用 extern 声明而非 typedef：不同头文件撞 __LINE__ 时，同名同类型的
// 重复 extern 声明在 C99 合法（typedef 重定义则非法）；不占存储。
//-----------------------------------------------------------------------------
#define V2K_CONCAT_(a, b) a##b
#define V2K_CONCAT(a, b)  V2K_CONCAT_(a, b)
#define V2K_STATIC_ASSERT(cond) \
    extern char V2K_CONCAT(v2k_static_assert_, __LINE__)[(cond) ? 1 : -1]

// 跨平台尺寸断言：以 bit 为单位表达。
// C28x: sizeof 计 16-bit word，CHAR_BIT=16；PC: sizeof 计 octet，CHAR_BIT=8。
// sizeof(t) * CHAR_BIT 在两边都得到 bit 数，因此同一断言双平台通用。
// 含 char[] 的 struct 用 V2K_NAME_BITS(n) 表达名字部分（随平台变化）。
#define V2K_SIZEOF_BITS(t)        ((uint32_t)sizeof(t) * (uint32_t)CHAR_BIT)
#define V2K_ASSERT_SIZE_BITS(t, bits) V2K_STATIC_ASSERT(V2K_SIZEOF_BITS(t) == (bits))
#define V2K_NAME_BITS(n)          ((uint32_t)(n) * (uint32_t)CHAR_BIT)

// 基础类型宽度自检（C28x 上 int16_t == char 宽度 == 16 bit）
V2K_ASSERT_SIZE_BITS(uint16_t, 16);
V2K_ASSERT_SIZE_BITS(uint32_t, 32);
V2K_ASSERT_SIZE_BITS(float, 32);

//-----------------------------------------------------------------------------
// 协议/接口版本
//-----------------------------------------------------------------------------
// 线上协议版本：帧头 ver_magic 低 nibble。高 nibble 固定 0x5 作 resync 校验。
// 版本语义：不兼容的帧格式/消息布局变更才允许 +1（描述符表内容变化不算——
// 那由 build_hash 强制重枚举机制覆盖）。
#define V2K_WIRE_VER        0x5u
#define V2K_WIRE_VER_MAGIC  (0x50u | V2K_WIRE_VER)   /* = 0x55, 帧首 octet */
#define V2K_WIRE_MAX_PAYLOAD 1024u

// 共享内存布局版本：任何共享 struct 字段变更必须 +1（CPU1/CPU2 固件不同期
// 烧录时的握手自检用，见 v2k_command.h 握手流程）。
#define V2K_CONTRACT_VER    7u

//-----------------------------------------------------------------------------
// 设备能力位（HELLO capabilities；只追加，不复用）
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
// 全平台公共类型
//-----------------------------------------------------------------------------
// ISR tick：全平台唯一时间，由 CPU1 控制 ISR 递增（基本规则 5）。
// uint32 @ 100 kHz 约 11.9 小时回绕；host 端须按无符号回绕差值处理，
// 长时间录盘以 block 序号 + 回绕计数重建绝对时间。
typedef uint32_t v2k_tick_t;

// firmware build hash：git 短哈希的 32-bit 截断，链接时注入。
// host 重连后发现变更 → 强制重新枚举描述符表（见 v2k_descriptor.h）。
typedef uint32_t v2k_build_hash_t;

//-----------------------------------------------------------------------------
// 变量类型码（描述符表 type 字段；对齐 .def/inspector 的最小必要子集）
//-----------------------------------------------------------------------------
#define V2K_TYPE_I16  0u
#define V2K_TYPE_U16  1u
#define V2K_TYPE_I32  2u
#define V2K_TYPE_U32  3u
#define V2K_TYPE_F32  4u
#define V2K_TYPE_COUNT 5u

// 类型的线上宽度（octet）。片上一律按 32-bit 槽位存取（见 value_bits 约定）。
// 16-bit 类型占用 32-bit 槽位的低半，高半为 0（无符号）或符号扩展（有符号）。

//-----------------------------------------------------------------------------
// value_bits 约定：参数/镜像值统一以 uint32_t 位模式搬运
//-----------------------------------------------------------------------------
// * F32  : float 的 IEEE754 位模式
// * I32/U32 : 原值位模式
// * I16  : 符号扩展到 32 bit 后的位模式；U16: 零扩展
// 写入目标变量时由 CPU1 按描述符 type 截断/转换——CPU2 与 host 不解释位模式。

#endif // V2K_COMMON_H
