//=============================================================================
// v2k_param.h — 共享内存接口：参数平面（主机 → 控制核方向）
//
// 解决的问题：CCS 实时模式与线上链路写多字段参数都非原子（C2000 特有坑），
// 因此一切参数写入走 shadow 双缓冲 + commit 标志，由 CPU1 ISR 在每周期
// 固定安全点整组交换——一批参数要么全部生效于同一拍，要么全不生效。
//
// 写入目标按地址寻址（与示波通道绑定同一哲学：应用变量来自 viewer 解析
// .out/DWARF 的符号树，平台量来自描述符表枚举，固件不区分来源）。
//
// ---- 查表护栏（guard-if-registered，防呆的固件侧半边）----
// CPU1 应用每条写入前先在描述符表中查 addr：
//   * 命中且 kind&PARAM → 强制 min/max 范围检查，越界则整批拒绝；
//   * 未命中（应用变量）→ 放行原始写入，status.unguarded_cnt 累计 +1，
//     防呆责任移交 viewer（确认弹窗/写入前回显）。CCS 调参同理不设防。
//
// 写入路径（两条，机制相同）：
//   1. 线上：host CAL_WRITE(暂存) ×k → CAL_COMMIT → CPU2 填 shadow + 发布 seq
//   2. 调试：CCS 直接戳 shadow 区字段，最后把 commit_seq +1
//
// 应用协议（CPU1 ISR 安全点，user_step 之前；序号握手，无跨界写标志位）：
//   if (shadow.commit_seq != status.applied_seq) {
//       逐条护栏校验（见上）；全部通过 → 按 type 写入目标；任一越界 → 整批拒绝
//       status.result = 结果码; status.applied_seq = shadow.commit_seq;  // 应答
//   }
//
// 读回路径（值镜像）：CPU2/host 不能解引用 CPU1 私有地址（见 v2k_descriptor.h 注释），
// 平台参数现值由 CPU1 后台循环周期性（建议 10 Hz）刷进 status.value_mirror[]，
// 线上 CAL_READ 从镜像取。应用变量的"watch"= 绑到慢速示波组（自带时间戳）。
//=============================================================================
#ifndef V2K_PARAM_H
#define V2K_PARAM_H

#include "v2k_common.h"
#include "v2k_descriptor.h"

//-----------------------------------------------------------------------------
// 单条参数写入（shadow 区元素）
//-----------------------------------------------------------------------------
#define V2K_PARAM_BATCH_MAX 16u   // 一次 commit 的最大条数

typedef struct {
    uint32_t addr;         // 目标 CPU1 数据空间 word 地址（来源：描述符表或 DWARF）
    uint32_t value_bits;   // 新值位模式（约定见 v2k_common.h）
    uint16_t type;         // V2K_TYPE_*（决定写宽度与转换）
    uint16_t reserved;     // 置 0
} v2k_param_write_t;

V2K_ASSERT_SIZE_BITS(v2k_param_write_t, 96u);

//-----------------------------------------------------------------------------
// shadow 区（CPU2 属主：CPU2 与 CCS 写；CPU1 只读，应答走 status.applied_seq。
// 发布动作 = 最后写 commit_seq；GSx 对侧只读，不存在双写者字段）
//-----------------------------------------------------------------------------
typedef struct {
    uint16_t count;        // 本批条数 ≤ V2K_PARAM_BATCH_MAX
    uint16_t reserved;     // 置 0
    uint32_t commit_seq;   // 提交方每次 commit +1，最后写入（发布）
    v2k_param_write_t writes[V2K_PARAM_BATCH_MAX];
} v2k_param_shadow_t;

V2K_ASSERT_SIZE_BITS(v2k_param_shadow_t, 64u + 96u * V2K_PARAM_BATCH_MAX);

//-----------------------------------------------------------------------------
// 应用结果码（status.result）
//-----------------------------------------------------------------------------
#define V2K_CAL_OK         0u
#define V2K_CAL_BAD_TYPE   1u   // type 非法
#define V2K_CAL_OUT_RANGE  2u   // 注册参数越 min/max（仅护栏命中时可能）
#define V2K_CAL_BAD_COUNT  3u   // count 超上限
#define V2K_CAL_BAD_ADDR   4u   // 目标不在 CPU1 可写数据区或 32-bit 未对齐

//-----------------------------------------------------------------------------
// 状态 + 值镜像（CPU1 属主：CPU1 写，CPU2/host 只读）
//-----------------------------------------------------------------------------
typedef struct {
    uint32_t applied_seq;   // 最近应用完成的 commit_seq（序号握手应答侧）
    uint16_t result;        // V2K_CAL_*（对应 applied_seq 那一批）
    uint16_t fail_idx;      // 整批拒绝时首个非法条目的批内下标（result!=OK 时有效）
    uint16_t unguarded_cnt; // 未命中描述符表的"无护栏写入"累计（防呆可观测性）
    uint16_t reserved;      // 置 0
    uint32_t mirror_seq;    // 镜像每轮刷新 +1（host 判断数据新旧）
    uint32_t value_mirror[V2K_DESC_MAX]; // 描述符表现值位模式，CPU1 后台刷新
} v2k_param_status_t;

V2K_ASSERT_SIZE_BITS(v2k_param_status_t, 128u + 32u * V2K_DESC_MAX);

#endif // V2K_PARAM_H
