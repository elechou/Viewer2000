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
// ---- 写入校验（机械一致性，不做范围/单位语义）----
// CPU1 应用每条写入前只做机械检查：
//   * type 合法；
//   * addr 位于允许写入的 CPU1 数据区，32-bit 类型地址对齐；
//   * 命中描述符表时必须 kind&PARAM 且 type 一致。
// 固件不做 min/max 范围检查、不 clamp、不做 scale/offset 反算。线上值就是
// 目标变量的真实原生位模式。
//
// 写入路径（两条，机制相同）：
//   1. 线上：host CAL_WRITE(暂存) ×k → CAL_COMMIT → CPU2 填 shadow + 发布 seq
//   2. 调试：CCS 直接戳 shadow 区字段，最后把 commit_seq +1
//
// 应用协议（CPU1 ISR 安全点，user_step 之前；序号握手，无跨界写标志位）：
//   if (shadow.commit_seq != status.applied_seq) {
//       逐条机械校验（见上）；全部通过 → 按 type 写入目标；任一非法 → 整批拒绝
//       status.result = 结果码; status.applied_seq = shadow.commit_seq;  // 应答
//   }
//
// 读回路径：线上 CAL_READ 与 CAL_WRITE 使用同一寻址哲学，host 发送
// (addr,type) 列表，CPU2 发布到 read request，CPU1 后台 poll point 按需读取一次
// 并发布 read response。读取不进入 ISR；高速、带时间戳的数据仍走 scope ring。
//=============================================================================
#ifndef V2K_PARAM_H
#define V2K_PARAM_H

#include "v2k_common.h"
#include "v2k_descriptor.h"

//-----------------------------------------------------------------------------
// 单条参数写入（shadow 区元素）
//-----------------------------------------------------------------------------
#define V2K_PARAM_BATCH_MAX 16u   // 一次 commit 的最大条数
#define V2K_CAL_READ_MAX    32u   // 一次 CAL_READ 的最大条数

typedef struct {
    uint32_t addr;         // 目标 CPU1 数据空间 word 地址（来源：描述符表或 DWARF）
    uint32_t value_bits;   // 新值位模式（约定见 v2k_common.h）
    uint16_t type;         // V2K_TYPE_*（决定写宽度与转换）
    uint16_t reserved;     // 置 0
} v2k_param_write_t;

V2K_ASSERT_SIZE_BITS(v2k_param_write_t, 96u);

typedef struct {
    uint32_t addr;         // 目标 CPU1 数据空间 word 地址
    uint16_t type;         // V2K_TYPE_*（决定读宽度与符号扩展）
    uint16_t reserved;     // 置 0
} v2k_param_read_ref_t;

V2K_ASSERT_SIZE_BITS(v2k_param_read_ref_t, 64u);

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
// read request 区（CPU2 属主：CPU2 写，CPU1 只读。发布动作 = 最后写 read_seq）
//-----------------------------------------------------------------------------
typedef struct {
    uint16_t count;        // 本次读取条数，1..V2K_CAL_READ_MAX
    uint16_t reserved;     // 置 0
    uint32_t read_seq;     // CPU2 每次 read +1，最后写入（发布）
    v2k_param_read_ref_t refs[V2K_CAL_READ_MAX];
} v2k_param_read_req_t;

V2K_ASSERT_SIZE_BITS(v2k_param_read_req_t, 64u + 64u * V2K_CAL_READ_MAX);

//-----------------------------------------------------------------------------
// 应用结果码（status.result）
//-----------------------------------------------------------------------------
#define V2K_CAL_OK         0u
#define V2K_CAL_BAD_TYPE   1u   // type 非法
#define V2K_CAL_BAD_COUNT  2u   // count 超上限
#define V2K_CAL_BAD_ADDR   3u   // 目标不在 CPU1 可写数据区或 32-bit 未对齐

//-----------------------------------------------------------------------------
// 写入状态（CPU1 属主：CPU1 写，CPU2/host 只读）
//-----------------------------------------------------------------------------
typedef struct {
    uint32_t applied_seq;   // 最近应用完成的 commit_seq（序号握手应答侧）
    uint16_t result;        // V2K_CAL_*（对应 applied_seq 那一批）
    uint16_t fail_idx;      // 整批拒绝时首个非法条目的批内下标（result!=OK 时有效）
} v2k_param_status_t;

V2K_ASSERT_SIZE_BITS(v2k_param_status_t, 64u);

//-----------------------------------------------------------------------------
// read response 区（CPU1 属主：CPU1 写，CPU2/host 只读。发布动作 = 最后写 ack_seq）
//-----------------------------------------------------------------------------
typedef struct {
    uint16_t result;        // V2K_CAL_OK / BAD_TYPE / BAD_COUNT / BAD_ADDR
    uint16_t count;         // result==OK 时有效，应等于请求 count
    uint32_t value_bits[V2K_CAL_READ_MAX];
    uint32_t ack_seq;       // 最近处理完成的 read_seq，最后写入（发布）
} v2k_param_read_resp_t;

V2K_ASSERT_SIZE_BITS(v2k_param_read_resp_t, 64u + 32u * V2K_CAL_READ_MAX);

#endif // V2K_PARAM_H
