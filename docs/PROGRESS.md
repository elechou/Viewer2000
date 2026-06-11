# PROGRESS — Phase 进度跟踪

> **文档分工**：路线图与各 Phase 的定义在 `CLAUDE.md`（唯一基准，不在此重复）；
> 本文档只记录**完成状态、验证方式、遗留尾巴**。硬件实测细节（示波器读数、
> CCS 截图）自 Phase 2 起记入 `BRINGUP.md`。
>
> **维护约定**：每个 Phase 收口时更新本文档（与收口 commit 同次提交）；
> "完成"的标准是该 Phase 的验证命令/实测可复现，不是"代码写完了"。

## 状态总览

| Phase | 内容 | 状态 |
|---|---|---|
| 0 | 接口 + 协议定稿 | ✅ 完成（2026-06） |
| 1 | 双核骨架 | ⬜ 未开始 |
| 2 | 时基证明 + 保护 | ⬜ 未开始 |
| 3 | 执行器 + 可观测性 | ⬜ 未开始 |
| 3.5 | SCI 哑数据泵 | ⬜ 未开始 |
| 4 | 控制库 | ⬜ 未开始 |
| 5 | 电机 bring-up（平台验收） | ⬜ 未开始 |
| 6 | EtherCAT 链路成型 | ⬜ 未开始 |

## Phase 0 — 接口 + 协议定稿 ✅

交付物与验证：

| 交付物 | 落点 | 验证方式 |
|---|---|---|
| 四个共享内存接口头文件 | `contracts/v2k_{descriptor,param,scope,command}.h` + `v2k_common.h` | PC 端编译检查（见下） |
| 内存映射（GSx/MSGRAM 归属 + section 命名） | `contracts/v2k_memmap.h` | 物理基址已对照链接脚本核实 |
| wire spec v1 | `docs/wire-spec.md` | 与 vectors 互为基准（不一致以 vectors 为准） |
| golden test vectors（23 个，含 bad-CRC 负样本） | `contracts/vectors/*.txt` | `python3 tools/gen_vectors.py --check` → `CHECK OK` |
| 布局静态断言（尺寸 + bit 偏移） | `tools/check_contracts.c` | `gcc -std=c99 -Wall -Wextra -Werror -c tools/check_contracts.c` 通过 |

两条验证命令在收口时（2026-06-11）均通过，任何改动 contracts/ 或
gen_vectors.py 后必须重跑。

**有意留到后续 Phase 的尾巴**（不阻塞 Phase 0 收口）：

- cl2000 侧编译同一份 `check_contracts.c` 验证断言 → Phase 1 接入 CCS 工程后做；
- flash bank 划分与 CPU2 镜像存放 → Phase 1（双 .cmd）决策；
- Rust 端 conformance test（同一组 vectors）→ 上位机 repo，随 `V2kSource`
  在 Phase 3.5 落地；
- ISR 频率 20–100 kHz 定档、示波通道组/降采样比档位 → 运行时配置，
  协议布局不依赖，Phase 2/3 实测后定；
- ESC 过程数据 RAM 与 SM 上限 TRM 核对 → 只影响 block 尺寸天花板（N 是参数），
  Phase 6 前完成即可，但优先级已标高（见 CLAUDE.md）。

## Phase 1 — 双核骨架 ⬜

完成标志（来自 CLAUDE.md）：两核各自闪灯 + 共享 RAM 握手成功。

- [ ] 两个 CCS 工程 + 两份 linker .cmd（按 `v2k_memmap.h` 划 GSx 归属与 SECTION）
- [ ] flash bank 划分与 CPU2 镜像存放决策（回填 CLAUDE.md 待决策项）
- [ ] CPU1 引导 CPU2、IPC ping-pong、共享 RAM 握手
- [ ] CCS 双核调试会话
- [ ] cl2000 编译 `check_contracts.c`（双平台断言闭环）
