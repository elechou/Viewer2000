//=============================================================================
// v2k_param.h — 共享内存接口：参数平面（主机 → 控制核方向）
//
// 解决的问题：CCS 实时模式与线上链路写多字段参数都非原子（C2000 特有坑），
// 因此一切参数写入走 shadow 双缓冲 + commit 标志，由 CPU1 ISR 在每周期
// 固定安全点整组交换——一批参数要么全部生效于同一拍，要么全不生效。
//
// 写入路径（两条，机制相同）：
//   1. 线上：host CAL_WRITE(暂存) ×k → CAL_COMMIT → CPU2 填 shadow + 置 commit
//   2. 调试：CCS 直接戳 shadow 区字段，最后写 commit_flag=1
//
// 应用协议（CPU1 ISR 安全点，user_step 之前）：
//   if (shadow.commit_flag) {
//       逐条校验 desc_idx 合法 && kind&PARAM && min/max 范围（物理量纲）
//       全部合法 → 按描述符 type 写入目标变量；任一非法 → 整批拒绝
//       status.applied_seq = shadow.commit_seq; status.result = 结果码
//       shadow.commit_flag = 0;   // 清零即受理完成
//   }
//
// 读回路径（值镜像）：CPU2/host 不能解引用 CPU1 私有地址（见 v2k_descriptor.h 注释），
// 参数现值由 CPU1 后台循环周期性（建议 10 Hz）刷进 status.value_mirror[]，
// 线上 CAL_READ 从镜像取——这同时免费提供了 myway inspector 式的慢速 watch。
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
    uint16_t desc_idx;     // 目标参数（描述符索引）
    uint16_t reserved;     // 置 0
    uint32_t value_bits;   // 新值位模式（约定见 v2k_common.h）
} v2k_param_write_t;

V2K_ASSERT_SIZE_BITS(v2k_param_write_t, 64u);

//-----------------------------------------------------------------------------
// shadow 区（CPU2 属主：CPU2 与 CCS 写，CPU1 只在安全点读 + 清 commit_flag。
// commit_flag 是唯一的双写者字段：CPU2 置 1 / CPU1 清 0，单 bit 方向互斥，
// 16-bit 对齐写在 C28x 上原子）
//-----------------------------------------------------------------------------
typedef struct {
    uint16_t commit_flag;  // 1 = 有待应用批次；CPU1 应用后清 0
    uint16_t count;        // 本批条数 ≤ V2K_PARAM_BATCH_MAX
    uint32_t commit_seq;   // 提交方每次 commit 前 +1（host 凭此对账 ACK）
    v2k_param_write_t writes[V2K_PARAM_BATCH_MAX];
} v2k_param_shadow_t;

V2K_ASSERT_SIZE_BITS(v2k_param_shadow_t, 64u + 64u * V2K_PARAM_BATCH_MAX);

//-----------------------------------------------------------------------------
// 应用结果码（status.result）
//-----------------------------------------------------------------------------
#define V2K_CAL_OK         0u
#define V2K_CAL_BAD_IDX    1u   // desc_idx 越界或非 PARAM
#define V2K_CAL_OUT_RANGE  2u   // 值超出 min/max
#define V2K_CAL_BAD_COUNT  3u   // count 超上限

//-----------------------------------------------------------------------------
// 状态 + 值镜像（CPU1 属主：CPU1 写，CPU2/host 只读）
//-----------------------------------------------------------------------------
typedef struct {
    uint32_t applied_seq;  // 最近应用完成的 commit_seq（host 轮询 STATUS 对账）
    uint16_t result;       // V2K_CAL_*（对应 applied_seq 那一批）
    uint16_t fail_idx;     // 整批拒绝时首个非法条目的批内下标（result!=OK 时有效）
    uint32_t mirror_seq;   // 镜像每轮刷新 +1（host 判断数据新旧）
    uint32_t value_mirror[V2K_DESC_MAX]; // 全部描述符现值位模式，CPU1 后台刷新
} v2k_param_status_t;

V2K_ASSERT_SIZE_BITS(v2k_param_status_t, 96u + 32u * V2K_DESC_MAX);

#endif // V2K_PARAM_H
