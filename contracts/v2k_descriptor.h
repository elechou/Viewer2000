//=============================================================================
// v2k_descriptor.h — 共享内存接口：描述符表（平台量枚举 + 默认采样提示）
//
// 角色定位（变量发现架构；2026-06-19 修订，见 Phase 4.5）：
// 本表登记**平台量**（L0/L1 注册：plat_in/plat_out 物理量、占空比、状态字、
// 平台参数）**加上用户应用变量**——后者由构建工具在编译期从固件 .out 的
// DWARF 烤入（Phase 4.5）。学生写纯 C：不调注册 API、不手打名字字符串、无
// 规定写法（C 符号是唯一命名来源，构建期采集、非手打）。名字烤进设备后，
// host 经 ENUM 枚举全部（平台 + 用户），无需 .out（且无陈旧 ELF 风险——地址
// 来自被烧的同一次构建，build_hash 仍兜 host 缓存）。
// 本表职责：
//   1. 开箱即用：host 枚举平台 + 用户名字并立即出波形；
//   2. （Phase 3 曾开机按注册序默认绑定前几个可观测量；Phase 4 改按需绑定。）
//
// 2026-06-17 语义修正：
// * 线上值就是真实值；描述符不再承载 min/max/scale/offset 语义。
//
// CPU1 启动时写表（CPU1 属主区，见 v2k_memmap.h），此后只读。
//
// 关键语义：
// * entry.addr 是 CPU1 数据空间的 word 地址，可能指向 CPU1 私有 RAM。
//   只有 CPU1 允许解引用它（采样、参数应用都在 CPU1 侧完成）；
//   CPU2 / host 仅把 addr 当不透明 id 透传或忽略。
// * 注册顺序即 desc_idx（0..count-1），是值镜像的索引键。
// * 表写入完成后由 CPU1 填 hdr.entry_count 并最后写 hdr.magic（发布屏障），
//   CPU2 见 magic 有效才允许读表。
//
// 描述符 add 原语为 L1 内部函数；用户变量经 Phase 4.5 构建期烤录入表、
// 端口名经 L0 注册——无面向 L3 用户代码的注册 API。
//=============================================================================
#ifndef V2K_DESCRIPTOR_H
#define V2K_DESCRIPTOR_H

#include "v2k_common.h"

//-----------------------------------------------------------------------------
// 容量与尺寸常量
//-----------------------------------------------------------------------------
#define V2K_NAME_LEN   16u   // 名字定长（含 NUL 填充）；ASCII，每字符 1 octet 上线
#define V2K_DESC_MAX   64u   // 表容量上限（64 × 22 words ≈ 1.4K words，预算见 memmap）

// 线上一条描述符的 octet 数（wire-spec §4.3 ENUM_RESP；与 struct 字段一一镜像）
#define V2K_DESC_WIRE_OCTETS 28u

//-----------------------------------------------------------------------------
// kind 标志位（可调与可观测不互斥，同一变量可两者皆是）
//-----------------------------------------------------------------------------
#define V2K_KIND_PARAM  0x0001u  // 可调参数：参与参数平面写入路径（v2k_param.h）
#define V2K_KIND_SCOPE  0x0002u  // 可观测信号：参与示波采样路径（v2k_scope.h），prescaler 为默认采样建议
// bit2..15 保留，置 0

//-----------------------------------------------------------------------------
// 描述符条目
//
// C28x 布局（word 偏移）:
//   name@0..15, type@16, kind@17, addr@18, prescaler@20, reserved@21
//   → 共 22 words，无填充
// PC 布局（octet 偏移）:
//   name@0..15, type@16, kind@18, addr@20, prescaler@24, reserved@26
//   → 共 28 octets，无填充
//-----------------------------------------------------------------------------
typedef struct {
    char     name[V2K_NAME_LEN]; // ASCII，NUL 填充；不保证 NUL 结尾（恰好 16 字符时）
    uint16_t type;               // V2K_TYPE_*
    uint16_t kind;               // V2K_KIND_* 位或
    uint32_t addr;               // CPU1 数据空间 word 地址（CPU2/host 视为不透明）
    uint16_t prescaler;          // 默认采样分频建议；运行时实际速率以 DAQ_CTRL 为准
    uint16_t reserved;           // 置 0；保留 28-octet 描述符条目对齐
} v2k_desc_entry_t;

V2K_ASSERT_SIZE_BITS(v2k_desc_entry_t, V2K_NAME_BITS(V2K_NAME_LEN) + 96u);

//-----------------------------------------------------------------------------
// 表头（位于表数组之前，同一共享 RAM 区）
//
// 发布协议：CPU1 先填 entries[] 与其余头字段，最后写 magic；
// CPU2 轮询 magic == V2K_DESC_MAGIC 即视为表就绪（单写者单方向，无需锁）。
//-----------------------------------------------------------------------------
#define V2K_DESC_MAGIC 0x564B4454u   /* "VKDT" */

typedef struct {
    uint32_t magic;              // V2K_DESC_MAGIC；最后写入=发布
    uint16_t contract_ver;       // = V2K_CONTRACT_VER，CPU2 握手时校验
    uint16_t entry_count;        // 已注册条目数 ≤ V2K_DESC_MAX
    v2k_build_hash_t build_hash; // 固件 build hash（host 重枚举依据）
    uint16_t entry_stride_words; // = sizeof(v2k_desc_entry_t)（C28x words），
                                 //   供 CCS 脚本/调试工具自描述遍历
    uint16_t reserved;           // 置 0
} v2k_desc_table_hdr_t;

V2K_ASSERT_SIZE_BITS(v2k_desc_table_hdr_t, 128u);

//-----------------------------------------------------------------------------
// 整表（共享 RAM 实体，CPU1 属主）
//-----------------------------------------------------------------------------
typedef struct {
    v2k_desc_table_hdr_t hdr;
    v2k_desc_entry_t     entries[V2K_DESC_MAX];
} v2k_desc_table_t;

#endif // V2K_DESCRIPTOR_H
