//=============================================================================
// v2k_scope.h — 共享内存接口：示波平面（控制核 → 主机方向）
//
// 无锁 SPSC：CPU1 控制 ISR 是唯一生产者（采样在 ISR 上下文，所有通道天然同拍），
// CPU2（或单核调试时的 CPU1 后台循环，基本规则 3）是唯一消费者。
//
// ---- 通道组（≈ XCP event channel）----
// 组 = 同速率通道集合。组内所有通道共享同一 prescaler（注册时由 L1 强制校验，
// 描述符的 prescaler 字段必须等于组速率），因此一个 block 内各通道样本数相同。
// 组速率 = ISR tick 率 / prescaler。多速率 = 多个组（快组 8ch@1 + 慢组 N ch@100）。
//
// ---- block ----
// 环形缓冲的单元是 block，不是样本：
//   [v2k_block_hdr_t][int16 样本区: tick-major 交错, samples[n_ticks][n_ch]]
// block 即线上 BLOCK_DATA 的载荷单元（wire-spec §4.6），头+样本原样上线，
// 这是"内存布局=线上格式"在热路径上的体现——CPU2 不做任何重编码。
// n_ticks 是参数：SCI 用小 N（建议 10），EtherCAT 用 N=50（800 octets @8ch）。
//
// ---- 两种模式 ----
// LIVE：连续流。环满时生产者丢弃新 block 并递增 overrun_cnt（基本规则 1：
//   不阻塞、不等待消费者）。host 凭 block_seq 跳变画断口。
// SNAPSHOT：全速率连续覆盖写环（无视 rd_idx——消费者此时不读），触发命中后
//   再写 post 段然后冻结。环天然保存 pre-trigger 历史。冻结后 CPU2 按
//   frozen_* 字段直接索引排空，不走 rd_idx。排空完 host 重新 ARM。
//
// ---- SPSC 索引协议 ----
// wr_idx/rd_idx 为自由递增 uint16，按 ring_capacity（2 的幂）取模寻址：
//   空: wr_idx == rd_idx     满: (uint16_t)(wr_idx - rd_idx) == ring_capacity
// wr_idx 仅 CPU1 写（先写完 block 数据再发布 wr_idx）；rd_idx 仅 CPU2 写，
// 且两者位于各自核的属主 RAM 区（见 v2k_memmap.h）——单写者即免锁。
//=============================================================================
#ifndef V2K_SCOPE_H
#define V2K_SCOPE_H

#include "v2k_common.h"

//-----------------------------------------------------------------------------
// 容量常量
//-----------------------------------------------------------------------------
#define V2K_SCOPE_MAX_GROUPS  4u    // 通道组数上限（v1：组 0=快组，组 1=慢组，余备用）
#define V2K_SCOPE_MAX_CH      8u    // 单组通道数上限（v1 锚点：8ch 快组）
#define V2K_BLOCK_HDR_OCTETS  12u   // block 头线上尺寸

// block n_ticks 的两档基准（见 CLAUDE.md 通信架构）
#define V2K_BLOCK_NTICKS_SCI  10u   // SCI 哑泵：10 tick × 8ch × int16 = 160 octets
#define V2K_BLOCK_NTICKS_ECAT 50u   // EtherCAT：50 tick × 8ch × int16 = 800 octets

//-----------------------------------------------------------------------------
// 示波模式（prod.mode 字段；状态机由 CPU1 ISR epilogue 驱动）
//-----------------------------------------------------------------------------
#define V2K_SCOPE_OFF        0u
#define V2K_SCOPE_LIVE       1u
#define V2K_SCOPE_SNAP_ARMED 2u   // 覆盖写环 + 每拍触发判定
#define V2K_SCOPE_SNAP_TRIG  3u   // 已命中，补采 post-trigger 段
#define V2K_SCOPE_SNAP_FROZEN 4u  // 冻结，等 CPU2 排空 + host 重 ARM

// 触发边沿（cfg.trig_edge；2/3 预留给 >阈值 / <阈值，对齐 myway 语义）
#define V2K_TRIG_RISE 0u
#define V2K_TRIG_FALL 1u

//-----------------------------------------------------------------------------
// block 头（RAM 与线上同构，12 octets / 6 words）
//-----------------------------------------------------------------------------
typedef struct {
    v2k_tick_t start_tick;  // 本 block 首样本的 ISR tick（全平台唯一时间）
    uint16_t   block_seq;   // 组内递增序号，回绕；host 凭跳变检测丢块/断口
    uint16_t   group_id;
    uint16_t   n_ticks;     // 本 block 样本拍数 N
    uint16_t   n_ch;        // 通道数 M；样本区 = int16[N][M] tick-major 交错
} v2k_block_hdr_t;

V2K_ASSERT_SIZE_BITS(v2k_block_hdr_t, 96u);

//-----------------------------------------------------------------------------
// 生产者控制块（每组一个；CPU1 属主区，CPU2 只读）
//
// C28x word 偏移：mode@0 flags@1 cap@2 n_ticks@3 n_ch@4 res0@5 ring_base@6
//   wr_idx@8 overrun@9 trig_tick@10 frz_end@12 frz_cnt@13 state_seq@14 res1@15
//-----------------------------------------------------------------------------
typedef struct {
    uint16_t mode;            // V2K_SCOPE_*（CPU1 写；响应 cfg 请求后跃迁）
    uint16_t flags;           // 保留，置 0
    uint16_t ring_capacity;   // 环容量（block 数，2 的幂）
    uint16_t block_n_ticks;   // 当前生效的 N
    uint16_t n_ch;            // 当前生效的 M（= 组内已注册 SCOPE 通道数）
    uint16_t reserved0;
    uint32_t ring_base;       // 环数据区基址（CPU1 数据空间 word 地址）
    uint16_t wr_idx;          // 自由递增写索引（block 单位）；数据就绪后发布
    uint16_t overrun_cnt;     // LIVE 模式环满丢块累计
    v2k_tick_t trig_tick;     // SNAPSHOT 触发命中 tick（FROZEN 时有效）
    uint16_t frozen_end_idx;  // FROZEN：最后写入 block 的下一索引（自由递增值）
    uint16_t frozen_count;    // FROZEN：有效 block 数（≤ ring_capacity）
    uint16_t state_seq;       // mode 每次跃迁 +1（CPU2/host 检测状态变化）
    uint16_t reserved1;
} v2k_scope_prod_t;

V2K_ASSERT_SIZE_BITS(v2k_scope_prod_t, 256u);

//-----------------------------------------------------------------------------
// 消费者控制块（每组一个；CPU2 属主区，CPU1 只读）
//-----------------------------------------------------------------------------
typedef struct {
    uint16_t rd_idx;          // 自由递增读索引（LIVE 模式用；SNAPSHOT 排空不经此）
    uint16_t reserved;
} v2k_scope_cons_t;

V2K_ASSERT_SIZE_BITS(v2k_scope_cons_t, 32u);

//-----------------------------------------------------------------------------
// 示波配置请求（每组一个；CPU2 属主区 = host 经 DAQ_CTRL 写入，CPU1 应用）
//
// 提交协议（与参数平面同型的 shadow+commit 模式）：
//   CPU2 填全部字段 → 最后置 commit_flag=1 → CPU1 ISR 安全点读取、应用、
//   跃迁 mode、清 commit_flag。CPU1 清零即"已受理"，结果看 prod.mode/state_seq。
//-----------------------------------------------------------------------------
typedef struct {
    uint16_t mode_req;        // 目标模式：OFF / LIVE / SNAP_ARMED
    uint16_t trig_desc_idx;   // 触发源（描述符索引；须属本组且 kind&SCOPE）
    float    trig_level;      // 触发阈值（物理量纲，CPU1 按 scale/offset 折回 raw 比较）
    uint16_t trig_edge;       // V2K_TRIG_*
    uint16_t pre_trig_pct;    // pre-trigger 占环深百分比 0..100
    uint16_t prescaler;       // 组速率覆盖（0 = 维持注册值）
    uint16_t commit_flag;     // CPU2 置 1 → CPU1 应用后清 0
} v2k_scope_cfg_t;

V2K_ASSERT_SIZE_BITS(v2k_scope_cfg_t, 128u);

//-----------------------------------------------------------------------------
// 派生尺寸工具（环数据区按 block 步长平铺）
//-----------------------------------------------------------------------------
// 一个 block 占用的 word 数（C28x：头 6 words + 每样本 1 word）
#define V2K_BLOCK_WORDS(n_ticks, n_ch) (6u + (uint32_t)(n_ticks) * (n_ch))
// 一个 block 的线上 octet 数
#define V2K_BLOCK_OCTETS(n_ticks, n_ch) \
    (V2K_BLOCK_HDR_OCTETS + 2u * (uint32_t)(n_ticks) * (n_ch))

#endif // V2K_SCOPE_H
