# BRINGUP.md — 实物验证记录

工作流约定：每一步记录**在实物上验证了什么、用什么方法验证**（示波器实测值、
CCS Graph 截图、Expressions 读数等）。验证知识不能只活在 commit message 里。

记录格式：日期 / 验证项 / 方法 / 实测结果 / 结论（+遗留问题）。

---

## Phase 1 — 双核骨架（待验证）

操作步骤见 `docs/phase1-sysconfig.md`。验证项清单：

- [ ] 两工程 RAM 配置 0 error 构建（含 v2k_check_contracts.c 契约断言在 cl2000 通过）
- [ ] CPU1 单独 Resume 后阻塞于 IPC_sync（红灯不闪，符合预期）
- [ ] CPU2 Resume 后双灯闪烁：红 LED4 1 Hz / 绿 LED5 2 Hz（目测或秒表）
- [ ] `v2k_assert_layout` 未触发（两核都没停在 ESTOP0）＝ .cmd 落位与 v2k_memmap.h 一致
- [ ] Expressions：g_ping_cnt / g_pong_cnt 同步递增；g_handshake_state == 3
- [ ] `g_v2k_gs0.desc_table.hdr.magic == 0x564B4454`（CPU2 会话从 0x10000 读同值更佳——双核同视角实证）
- [ ] 心跳监视：halt CPU2 → g_cpu2_alive 1→0、status_flags 置 CPU2_LOST、红灯照闪；resume 恢复（基本规则 1 实测）

记录区：

| 日期 | 验证项 | 方法 | 实测 | 结论 |
|---|---|---|---|---|
| | | | | |
