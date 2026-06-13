//=============================================================================
// v2k_fault.c — Phase 2 保护（实现）
//=============================================================================

#include "driverlib.h"
#include "device.h"
#include "v2k_fault.h"

volatile uint16_t g_v2k_sm_state = V2K_STATE_INIT;
volatile uint16_t g_v2k_fault_code = V2K_FAULT_NONE;
volatile uint32_t g_v2k_tz_int_cnt;

static uint32_t s_cmd_handled;   // 已受理的 cmd_seq

//-----------------------------------------------------------------------------
// TZ 中断（仅 OST 源；EPWM 级 TZEINT.OST 与 PIE 级中断同步，只在 RUNNING 开）。
// 真实 trip → 锁 FAULT。退出本 ISR 时同时关两级中断、只留 OST 锁存：
//   · 关 PIE 级（Interrupt_disable）：止中断风暴——latched-OST 会让 TZFLG.INT
//     反复重置，PIE 开着就精准卡死在本 ISR（2026-06-13 实测连 CPU2 都引导不了）；
//   · 关 EPWM 级 TZEINT.OST：本次调试根因修复——否则 FAULT 期间 latched-OST 持续
//     把 TZFLG.INT 转发进 PIEIFR 并锁存（PIE 关着不触发、标志位却攒着，
//     EPWM_clearTripZoneFlag 清不掉 PIEIFR），下次 APP_START 使能 PIE 的瞬间立刻
//     补发一次伪 trip。Phase 2 实测正是此故障：每次 START 都先被陈旧 PIEIFR 伪触发
//     本 ISR（置 FAULT/fault_code=1 并关中断），紧接着 START 又把 sys_state 覆写回
//     RUNNING、fault_code=1 残留、TZ 中断已关死 → 之后真插跳线只剩硬件 OST 封波形、
//     状态机却卡 RUNNING、命令全 BAD_STATE。
// OST 锁存保留 = 输出仍封；退出 FAULT 的重新评估在 CLEAR_FAULT→IDLE、再 APP_START。
//-----------------------------------------------------------------------------
static __interrupt void v2k_tz_isr(void)
{
    g_v2k_tz_int_cnt++;
    g_v2k_sm_state   = V2K_STATE_FAULT;
    g_v2k_fault_code = V2K_FAULT_TZ1_EXT;     // Phase 2 唯一中断 trip 源是 TZ1
    Interrupt_disable(INT_EPWM1_TZ);                                   // 关 PIE 级
    EPWM_disableTripZoneInterrupt(EPWM1_BASE, EPWM_TZ_INTERRUPT_OST);  // 关 EPWM 级
    EPWM_clearTripZoneFlag(EPWM1_BASE, EPWM_TZ_INTERRUPT);
    Interrupt_clearACKGroup(INTERRUPT_ACK_GROUP2);
}

void v2k_fault_arm(void)
{
    //
    // Board_init 之前抢先锁存 OST（保护先行第一道）。
    // ⚠ 关键：此刻 EPWM1 外设时钟尚未打开——cpu1.c 只调 Device_init，而
    // Device_init 不含外设时钟使能（那在从未被调用的 Device_enableAllPeripherals
    // 里），EPWM1 时钟要到 Board_init→SYSCTL_init 才开。若直接写 TZFRC，在无
    // 时钟下该写被丢弃、OST 不锁存——这正是“开机即出波形、IDLE 形同虚设”的根因
    // （2026-06-13 实测确证）。故必须在此先显式开 EPWM1 时钟，等几个周期生效，
    // 再强制 OST。此刻 TZCTL 仍是复位值（Hi-Z）、TZSEL 未配，本次只求“尽早封”；
    // 配上 force-low 动作并经 TZSEL 后的权威锁存在 v2k_fault_init 完成 + 读回断言。
    //
    SysCtl_enablePeripheral(SYSCTL_PERIPH_CLK_EPWM1);
    asm(" RPT #5 || NOP");   // 外设时钟使能后需数周期生效，再访问其寄存器
    EPWM_forceTripZoneEvent(EPWM1_BASE, EPWM_TZ_FORCE_EVENT_OST);
}

void v2k_fault_init(void)
{
    //
    // 权威封锁点（保护先行第二道，必定生效）：此刻 Board_init 已落地——EPWM1
    // 时钟在、TZSEL=OSHT1、TZA/TZB=force-low 都配好——在这里强制 OST 必定锁存且
    // 按 force-low 生效。arm() 那次是“尽早封”的尽力而为，这次才是保证（不依赖
    // arm 那次是否赶上时钟）。
    //
    EPWM_forceTripZoneEvent(EPWM1_BASE, EPWM_TZ_FORCE_EVENT_OST);
    EPWM_clearTripZoneFlag(EPWM1_BASE, EPWM_TZ_INTERRUPT);
    Interrupt_register(INT_EPWM1_TZ, &v2k_tz_isr);
    //
    // ⚠ 两级 TZ 中断都在 IDLE 保持关闭，只在 RUNNING（APP_START 放行后）才开：
    //   · PIE 级：OST 已锁存（输出封死），同时开 PIE 级 TZ 中断的话 latched-OST
    //     会让 TZFLG.INT 反复重置 → 中断风暴（实测卡死、引导不了 CPU2）；
    //   · EPWM 级 TZEINT.OST：本次调试根因——IDLE/FAULT 期间开着它，latched-OST
    //     就持续把 TZFLG.INT 转发进 PIEIFR 并锁存（PIE 关着不触发、标志位却攒着），
    //     下次 START 使能 PIE 的瞬间补发一次伪 trip（详见 v2k_tz_isr 上方注释）。
    //     故 TZEINT.OST 也只在 RUNNING 开：START 开 / STOP 关 / ISR 进 FAULT 时关。
    // 此刻 OST 已锁存 + 两级中断皆关 → 输出封死、既无风暴也无陈旧 PIEIFR。
    //
    Interrupt_disable(INT_EPWM1_TZ);

    //
    // 读回断言：把“保护先行”变成上电即验证的不变量。OST 必须真的锁存（输出被
    // 封死），否则 = 封锁失败、tb_start 放行后会带电输出——立即停机，绝不静默
    // 放行。这次“开机即出波形”正是缺了这道把关（同 v2k_tb_check / v2k_assert_layout
    // 的对账哲学：不信“我以为封了”，只信寄存器读回）。
    //
    if ((EPWM_getTripZoneFlagStatus(EPWM1_BASE) & EPWM_TZ_FLAG_OST) == 0u)
    {
        for (;;) { ESTOP0; }
    }

    // OST 已锁存 → 输出常封，唯一放行路径是 APP_START 清 OST
    g_v2k_sm_state = V2K_STATE_IDLE;
}

//-----------------------------------------------------------------------------
// 慢环轮询：受理命令（cmd_seq 前进 = 新命令）+ 状态/故障码同步到 MSGRAM。
// 命令来自 CPU2 侧 MSGRAM（Phase 2 经 CPU2 调试会话用 Expressions 戳入：
// 先填 cmd_code/arg，最后写 cmd_seq = 旧值+1 —— 契约的发布协议）。
//-----------------------------------------------------------------------------
void v2k_fault_poll(volatile v2k_cpu1_status_t *st)
{
    const volatile v2k_cmd_req_t *req = &V2K_MSG_2TO1_RO->cmd_req;
    uint32_t seq = req->cmd_seq;

    if (seq != s_cmd_handled)
    {
        uint16_t code   = req->cmd_code;
        uint16_t result = V2K_CMDR_OK;

        switch (code)
        {
            case V2K_CMD_NOP:
                break;

            case V2K_CMD_APP_START:
                if (g_v2k_sm_state == V2K_STATE_IDLE)
                {
                    // 放行 + 武装 TZ 中断。顺序修掉两个 Phase 2 调试根因：
                    //   ① 先清 OST 放行输出 + 清 INT；
                    //   ② 先把 state 置 RUNNING，再开中断——若 trip 源仍在
                    //      （GPIO3 低），开 TZEINT.OST/PIE 会同步进 ISR 重入 FAULT，
                    //      FAULT 写在 RUNNING 之后才不被覆盖。旧码在 enable 之后才
                    //      写 RUNNING，把 ISR 刚置的 FAULT 覆写掉 → 带 trip 的 START
                    //      永远进不了 FAULT（伪 trip 同样被覆写成卡死的 RUNNING）；
                    //   ③ 先清 OST 再开 TZEINT.OST：无 trip 源时清 OST 后无断言、
                    //      PIEIFR 干净 → 开 PIE 不会伪触发。
                    // 结局：有 trip → ISR 已把 state 改成 FAULT；无 trip → 保持
                    // RUNNING。两种都正确，enable 之后无需再判定。
                    EPWM_clearTripZoneFlag(EPWM1_BASE,
                                           EPWM_TZ_INTERRUPT | EPWM_TZ_FLAG_OST);
                    // 防御性清故障码：正常 IDLE 必为 NONE，但 APP_STOP 竞态可能遗留
                    // 旧 TZ1_EXT（见该处注释）。在开中断前清——若紧接着真 trip 抢入
                    // ISR，会把码重置成本次的值、不被此清零覆盖。
                    g_v2k_fault_code = V2K_FAULT_NONE;
                    g_v2k_sm_state = V2K_STATE_RUNNING;
                    EPWM_enableTripZoneInterrupt(EPWM1_BASE, EPWM_TZ_INTERRUPT_OST);
                    Interrupt_enable(INT_EPWM1_TZ);
                }
                else { result = V2K_CMDR_BAD_STATE; }
                break;

            case V2K_CMD_APP_STOP:
                if (g_v2k_sm_state == V2K_STATE_RUNNING)
                {
                    // 先关两级 TZ 中断（PIE 级 + EPWM 级 TZEINT.OST），再强制 OST
                    // 封输出：force 因此不触发 ISR（无需“预期 trip”标记），且关掉
                    // TZEINT.OST 后这次 latched OST 不会向 PIEIFR 攒陈旧标志去污染
                    // 下次 START。进 IDLE 后两级中断保持关闭。
                    Interrupt_disable(INT_EPWM1_TZ);
                    EPWM_disableTripZoneInterrupt(EPWM1_BASE, EPWM_TZ_INTERRUPT_OST);
                    EPWM_forceTripZoneEvent(EPWM1_BASE, EPWM_TZ_FORCE_EVENT_OST);
                    EPWM_clearTripZoneFlag(EPWM1_BASE, EPWM_TZ_INTERRUPT);
                    // 竞态防护：从上面 g_v2k_sm_state==RUNNING 判定到这里关两级中断
                    // 之间，真 trip 仍可能抢先进 v2k_tz_isr 把 state 置 FAULT。两级
                    // 中断现已关、ISR 不会再改 state，故只在“没被 trip 抢到”时才落
                    // IDLE；抢到了就保留 FAULT——不静默吞掉一次真实保护事件（规则 7）。
                    // 与 APP_START「先置态再开中断」是对称处理。
                    if (g_v2k_sm_state == V2K_STATE_RUNNING)
                    {
                        g_v2k_sm_state = V2K_STATE_IDLE;
                    }
                }
                else { result = V2K_CMDR_BAD_STATE; }
                break;

            case V2K_CMD_CLEAR_FAULT:
                if (g_v2k_sm_state == V2K_STATE_FAULT)
                {
                    // OST 锁存不动（IDLE 同样封锁输出），PIE 级 TZ 中断仍关
                    //（ISR 已关）。只裁决 trip 源是否消失：源仍在（GPIO3 仍低）
                    // → 保持 FAULT（受理但不放行）。
                    if (GPIO_readPin(V2K_FAULT_TZ_GPIO) != 0u)
                    {
                        EPWM_clearTripZoneFlag(EPWM1_BASE, EPWM_TZ_INTERRUPT);
                        g_v2k_fault_code = V2K_FAULT_NONE;
                        g_v2k_sm_state   = V2K_STATE_IDLE;
                    }
                }
                else { result = V2K_CMDR_BAD_STATE; }
                break;

            default:
                result = V2K_CMDR_BAD_CMD;
                break;
        }

        s_cmd_handled  = seq;
        st->ack_seq    = seq;
        st->cmd_result = result;
    }

    st->sys_state  = g_v2k_sm_state;
    st->fault_code = g_v2k_fault_code;
}
