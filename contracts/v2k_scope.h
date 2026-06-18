//=============================================================================
// v2k_scope.h — 共享内存接口：示波 Stream/Capture（控制核 → 主机方向）
//
// 无锁 SPSC：CPU1 控制 ISR 是唯一生产者（采样在 ISR 上下文，所有通道天然同拍），
// CPU2 是唯一消费者；CCS Graph 通过 CPU1 后台生成的只读 view 查看 Capture
// 冻结后的窗口。
//
// ---- 两个入口，一个热路径 ----
// Host 面对两个明确入口：
//   * Stream：连续 block 流，host 持续 BLOCK_REQ 消费，环满丢新块并报 overrun；
//   * Capture：设备侧触发冻结窗口，CPU1 覆盖写环、判定触发、补 post 段，
//     冻结后由 host 慢速排空。
// 两个入口复用同一套通道绑定、ring 与 block 格式，因此不再暴露固定 8 通道
// group，也不维护两份示波热路径。
//
// ---- 通道绑定（host 运行时选通道，不重烧）----
// 通道成员由 host 经 DAQ_BIND 在运行时下发（v2k_scope_bind_t），通道 =
// (addr, type) 二元组：
//   * addr 来源任意——平台量取自描述符表枚举，应用变量取自 viewer 解析 .out
//     (DWARF) 的符号树，固件不区分也不需要"识别"；
//   * 样本按原生宽度无损直拷：I16/U16 占 2 octets，I32/U32/F32 占 4 octets
//     （位模式原样上线，固件不做任何量化/转换；物理量换算是纯 host 侧显示元数据）；
//   * 绑定仅在 mode==OFF 时可应用（换通道先停采）；
//   * 开机默认绑定由 L1 写入前 V2K_SCOPE_DEFAULT_CH 个平台可观测量，开箱即有波形。
//
// ---- block ----
// 环形缓冲的单元是 block，不是样本：
//   [v2k_block_hdr_t][样本区: tick-major 交错，每 tick 内按绑定顺序排列，
//    各通道按原生宽度连续存放，每 tick 共 stride_octets]
// block 即线上 BLOCK_DATA 的载荷单元（wire-spec §4.6），头+样本原样上线，
// 这是"内存布局=线上格式"在热路径上的体现——CPU2 不做任何重编码。
// 头部 bind_seq 标记产生本块的绑定代号：host 换绑后据此丢弃残留旧块；
// stride_octets 让块自描述（不依赖绑定知识也能定界）。
//
// ---- 运行状态 ----
// STREAM：连续流。环满时生产者丢弃新 block 并递增 overrun_cnt（基本规则 1：
//   不阻塞、不等待消费者）。host 凭 block_seq 跳变画断口。
// CAPTURE_ARMED/CAPTURE_POST/CAPTURE_FROZEN：Capture 入口。CAPTURE_ARMED 覆盖
//   写环（无视 rd_idx），逐拍判定触发；命中后进入 CAPTURE_POST 补采 post 段；
//   CAPTURE_FROZEN 后 CPU2 按 frozen_* 字段直接索引排空，不走 rd_idx。排空完 host
//   重新 ARM 或切回 STREAM。
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
#define V2K_SCOPE_MAX_CH       16u   // 单次 Stream/Capture 通道数上限
#define V2K_SCOPE_DEFAULT_CH    8u   // 开机默认绑定的平台快量数量
#define V2K_BLOCK_HDR_OCTETS   16u   // block 头线上尺寸
#define V2K_BLOCK_DATA_PREFIX_OCTETS 12u

// block n_ticks 基准（SCI 哑泵：10 tick × 8ch × f32 = 320 octets 样本区）
#define V2K_BLOCK_NTICKS_SCI  10u

// 类型的样本宽度（octet）：I16/U16=2，I32/U32/F32=4（原生宽度无损直拷）
#define V2K_TYPE_SAMPLE_OCTETS(t) (((t) == V2K_TYPE_I16 || (t) == V2K_TYPE_U16) ? 2u : 4u)

//-----------------------------------------------------------------------------
// 示波状态（prod.mode 字段；状态机由 CPU1 ISR epilogue 驱动）
//-----------------------------------------------------------------------------
#define V2K_SCOPE_OFF     0u
#define V2K_SCOPE_STREAM  1u   // Stream 入口：连续流，无触发冻结
#define V2K_SCOPE_CAPTURE_ARMED  2u   // Capture：覆盖写环 + 每拍触发判定
#define V2K_SCOPE_CAPTURE_POST   3u   // Capture：已触发，补采 post-trigger 段
#define V2K_SCOPE_CAPTURE_FROZEN 4u   // Capture：冻结，等 CPU2 排空 + host 重启

// 触发边沿（cfg.trig_edge；2/3 预留给 >阈值 / <阈值）
#define V2K_TRIG_RISE 0u
#define V2K_TRIG_FALL 1u

// 配置与绑定结果码（prod.cfg_result / prod.bind_result）
#define V2K_SCOPE_RESULT_OK          0u
#define V2K_SCOPE_RESULT_BAD_STATE   1u
#define V2K_SCOPE_RESULT_BAD_PARAM   2u
#define V2K_SCOPE_RESULT_BAD_TYPE    3u
#define V2K_SCOPE_RESULT_BAD_ADDR    4u
#define V2K_SCOPE_RESULT_NO_CAPACITY 5u

//-----------------------------------------------------------------------------
// block 头（RAM 与线上同构，16 octets / 8 words）
//-----------------------------------------------------------------------------
typedef struct {
    v2k_tick_t start_tick;    // 本 block 首样本的 ISR tick（全平台唯一时间）
    uint16_t   block_seq;     // 递增序号，回绕；host 凭跳变检测丢块/断口
    uint16_t   flags;         // 保留，置 0
    uint16_t   n_ticks;       // 本 block 样本拍数 N
    uint16_t   n_ch;          // 通道数 M（每拍内按绑定顺序、原生宽度排列）
    uint16_t   bind_seq;      // 产生本块的绑定代号；host 换绑后丢弃不匹配的旧块
    uint16_t   stride_octets; // 每 tick 样本区 octet 数 = Σ 通道宽度（块自描述）
} v2k_block_hdr_t;

V2K_ASSERT_SIZE_BITS(v2k_block_hdr_t, 128u);

//-----------------------------------------------------------------------------
// 生产者控制块（CPU1 属主区，CPU2 只读）
//
// C28x word 偏移：mode@0 flags@1 cap@2 n_ticks@3 n_ch@4 prescaler@5
//   cfg_ack@6 cfg_result@7 ring_base@8 wr_idx@10 overrun@11 trig_tick@12
//   frz_end@14 frz_cnt@15 state_seq@16 bind_ack@17 bind_result@18 slot_words@19
//-----------------------------------------------------------------------------
typedef struct {
    uint16_t mode;            // V2K_SCOPE_*（CPU1 写；响应 cfg 请求后跃迁）
    uint16_t flags;           // 保留，置 0
    uint16_t ring_capacity;   // 环容量（block 数，2 的幂）
    uint16_t block_n_ticks;   // 当前生效的 N
    uint16_t n_ch;            // 当前生效的 M（= 已应用绑定的通道数）
    uint16_t prescaler;       // 当前生效的 Scope 采样分频
    uint16_t cfg_ack_seq;     // 已处理的 cfg_seq（序号握手应答侧）
    uint16_t cfg_result;      // V2K_SCOPE_RESULT_*（对应 cfg_ack_seq）
    uint32_t ring_base;       // 环数据区基址（CPU1 数据空间 word 地址）
    uint16_t wr_idx;          // 自由递增写索引（block 单位）；数据就绪后发布
    uint16_t overrun_cnt;     // STREAM 环满丢块累计
    v2k_tick_t trig_tick;     // 触发命中 tick（CAPTURE_FROZEN 时有效）
    uint16_t frozen_end_idx;  // CAPTURE_FROZEN：最后写入 block 的下一索引（自由递增值）
    uint16_t frozen_count;    // CAPTURE_FROZEN：有效 block 数（≤ ring_capacity）
    uint16_t state_seq;       // mode 每次跃迁 +1（CPU2/host 检测状态变化）
    uint16_t bind_ack_seq;    // 已处理的 bind_seq（序号握手应答侧）
    uint16_t bind_result;     // V2K_SCOPE_RESULT_*（对应 bind_ack_seq）
    uint16_t block_slot_words;// 环内每 block 槽位 word 数 = 8 + n_ticks×stride_words
} v2k_scope_prod_t;

V2K_ASSERT_SIZE_BITS(v2k_scope_prod_t, 320u);

//-----------------------------------------------------------------------------
// 消费者控制块（CPU2 属主区，CPU1 只读）
//-----------------------------------------------------------------------------
typedef struct {
    uint16_t rd_idx;          // 自由递增读索引（STREAM 用；CAPTURE_FROZEN 排空前重置）
    uint16_t reserved;
} v2k_scope_cons_t;

V2K_ASSERT_SIZE_BITS(v2k_scope_cons_t, 32u);

//-----------------------------------------------------------------------------
// 示波配置请求（CPU2 属主区 = host 经 DAQ_CTRL 写入，CPU1 应用）
//
// 序号握手：CPU2 填其余字段 → 最后写 cfg_seq = 旧值+1（发布）。
// CPU1 后台稳定复制、验证并准备运行态；所有字段完成后，最后发布 active 标志给 ISR。
//-----------------------------------------------------------------------------
typedef struct {
    uint16_t mode_req;        // 目标模式：OFF / STREAM / CAPTURE_ARMED
    uint16_t trig_ch_slot;    // 触发源 = 当前绑定的通道槽位 0..n_ch-1
    float    trig_level;      // 触发阈值，源值域；固件无物理换算知识
    float    trig_hysteresis; // 触发迟滞半宽，源值域绝对值；0 = 裸阈值
    uint16_t trig_edge;       // V2K_TRIG_*
    uint16_t pre_trig_pct;    // pre-trigger 占环深百分比 0..100
    uint16_t prescaler;       // Scope 速率覆盖（0 = 维持当前值）
    uint16_t record_points;   // Capture 目标样本点数；STREAM/OFF 忽略
    uint16_t reserved;        // 置 0
    uint16_t cfg_seq;         // CPU2 最后写（发布）；应答在 prod.cfg_ack_seq
} v2k_scope_cfg_t;

V2K_ASSERT_SIZE_BITS(v2k_scope_cfg_t, 192u);

//-----------------------------------------------------------------------------
// 通道绑定请求（CPU2 属主区 = host 经 DAQ_BIND 写入，CPU1 应用）
//
// 约束：仅 mode==OFF 时 CPU1 才应用，否则 bind_result=BAD_STATE。
//-----------------------------------------------------------------------------
typedef struct {
    uint32_t addr;            // CPU1 数据空间 word 地址（来源：描述符表或 DWARF）
    uint16_t type;            // V2K_TYPE_*（决定样本宽度，见 V2K_TYPE_SAMPLE_OCTETS）
    uint16_t reserved;        // 置 0
} v2k_scope_ch_bind_t;

V2K_ASSERT_SIZE_BITS(v2k_scope_ch_bind_t, 64u);

typedef struct {
    uint16_t n_ch;            // 1..V2K_SCOPE_MAX_CH
    uint16_t bind_seq;        // CPU2 最后写（发布）；应答在 prod.bind_ack_seq
    v2k_scope_ch_bind_t ch[V2K_SCOPE_MAX_CH];
} v2k_scope_bind_t;

V2K_ASSERT_SIZE_BITS(v2k_scope_bind_t, 32u + 64u * V2K_SCOPE_MAX_CH);

//-----------------------------------------------------------------------------
// 派生尺寸工具（环数据区按 block 槽位平铺；stride = Σ 通道宽度）
//-----------------------------------------------------------------------------
// 一个 block 占用的 word 数（C28x：头 8 words + 样本区；stride_words = stride_octets/2）
#define V2K_BLOCK_WORDS(n_ticks, stride_words) \
    (8u + (uint32_t)(n_ticks) * (stride_words))
// 一个 block 的线上 octet 数
#define V2K_BLOCK_OCTETS(n_ticks, stride_octets) \
    (V2K_BLOCK_HDR_OCTETS + (uint32_t)(n_ticks) * (stride_octets))

#endif // V2K_SCOPE_H
