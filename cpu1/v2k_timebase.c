//=============================================================================
// v2k_timebase.c — Phase 2 时基证明（运行时部分）
//
// 信号链（静态配置在 syscfg）：ePWM1（up-down，FREE_SOFT=free-run）→ SOCA
// @ CTR=ZERO → ADCA SOC0 采 ADCINA0 → EOC1 → INT_ADCA1 进 v2k_tb_isr。
// ADCINA0 与 DACA 输出共脚（BP1 排针 30），DACA 置中位（syscfg 配 2048），
// 给数据路径一个零接线的确定性 sanity 值。
//
// FREE_SOFT 决策（AGENTS.md「C2000 特有的坑」第一条）= FREE_RUN，三层语义。
// 下文"相位"指 TBCTR 在载波周期 0→PRD→0 里的位置及其派生定时（SOC 触发点、
// 死区边沿、将来同步组内各 ePWM 的相对对齐），与电机转子电角度无关：
//   ① 载波定时连续：STOP 模式 halt 时计数器冻在任意位置，输出钉在当时电平，
//      resume 第一个周期是"残缺拍"（SOC 时刻/占空比/死区从任意中间状态恢复）；
//      FREE_RUN 计数器照走，硬件链一致推进，resume 第一拍即完整拍、无瞬态。
//      此层在 Phase 2 当下分量不大；
//   ② 硬件可观测：SOC 是 ePWM→ADC 硬件触发不经 CPU，halt 期间转换照常、
//      结果寄存器照更新——但 ISR 不执行、tick 不走，没有软件意义的数据流；
//   ③ 真正分量重的理由：CCS 实时模式（平台主交互方式，Phase 3 后启用）里
//      time-critical ISR 在后台 halt 时照算，前提就是 TBCTR 不能停——
//      FREE_RUN 本质为该模式预设，①是顺带收益。
// 电机不在 FREE_SOFT 的管辖范围：转子在 halt 期间照常转，普通 halt 后控制环
// 恢复必有瞬态——带电调试靠实时模式（环不停、只 halt 后台），不靠让断点
// "对电机无害"。halt 时的输出安全同样与 FREE_SOFT 无关（计数器停走只会把
// 输出钉死在任意电平），由 TZ6（emulation stop）cycle-by-cycle trip 强制
// 拉低承担（syscfg TZ 配置，v2k_tb_check 对账）。
//=============================================================================

#include "driverlib.h"
#include "device.h"
#include "v2k_timebase.h"
#include "v2k_executor.h"

//-----------------------------------------------------------------------------
// 契约自检：安全/正确关键配置从寄存器读回对账。上电即停 ESTOP0，不带病运行。
//   · EPWMCLKDIV / 周期 / TBCLK 分频器 / TZ 源 / TZ 动作 / FREE_SOFT 六项 = 检查
//     syscfg 的工作（syscfg 是配置来源，这里是断言来源；手滑改掉即停机）；
//   · SOCAEN / SOCASEL 一项 = 例外检查 C 侧补写（TI SysConfig codegen bug，
//     见 V2K_TB_SOC_SRC）——SOCASEL 是 v2k_tb_init 显式写的，这条断言守的是
//     “那行补写还在且写对了”，删掉补写或写错源都会被此处拦下。
//   · TODO(Phase 5，上功率级前)：死区 DBCTL/DBRED/DBFED（shoot-through 关键）+
//     ADC 侧 SOC 触发源 ADCSOC0CTL.TRIGSEL（SOC 链收端，漂了同样 tick 卡 0）也应
//     纳入读回（code review #4）；当前实测正确但缺自检兜底——见 BRINGUP。
//-----------------------------------------------------------------------------
static void v2k_tb_check(void)
{
    uint16_t tzsel = HWREGH(EPWM1_BASE + EPWM_O_TZSEL);
    uint16_t tzctl = HWREGH(EPWM1_BASE + EPWM_O_TZCTL);
    uint16_t tbctl = HWREGH(EPWM1_BASE + EPWM_O_TBCTL);
    uint16_t etsel = HWREGH(EPWM1_BASE + EPWM_O_ETSEL);
    uint16_t timer_tcr = HWREGH(CPUTIMER1_BASE + CPUTIMER_O_TCR);
    uint16_t ediv  = HWREGH(CLKCFG_BASE + SYSCTL_O_PERCLKDIVSEL) &
                     SYSCTL_PERCLKDIVSEL_EPWMCLKDIV_M;

    if ((ediv != 0u) ||   // EPWMCLKDIV 必须 /1（errata：/2 丢 TZFRC/TZCLR；
                          // 配置来源 = syscfg 时钟树 → 生成的 Device_init）
        (EPWM_getTimeBasePeriod(EPWM1_BASE) != (uint16_t)V2K_TB_PRD) ||
        ((tzsel & (EPWM_TZSEL_OSHT1 | EPWM_TZSEL_CBC6)) !=
                  (EPWM_TZSEL_OSHT1 | EPWM_TZSEL_CBC6))             ||
        (((tzctl & EPWM_TZCTL_TZA_M) >> EPWM_TZCTL_TZA_S)
            != (uint16_t)EPWM_TZ_ACTION_LOW)                        ||
        (((tzctl & EPWM_TZCTL_TZB_M) >> EPWM_TZCTL_TZB_S)
            != (uint16_t)EPWM_TZ_ACTION_LOW)                        ||
        ((etsel & EPWM_ETSEL_SOCAEN) == 0u)                         ||  // SOC-A 使能（syscfg）
        (((etsel & EPWM_ETSEL_SOCASEL_M) >> EPWM_ETSEL_SOCASEL_S)
            != (uint16_t)V2K_TB_SOC_SRC)                            ||  // SOC 源（C 侧补写，TI bug）
        (((tbctl & EPWM_TBCTL_FREE_SOFT_M) >> EPWM_TBCTL_FREE_SOFT_S) < 2u) ||
                                                   // FREE_SOFT=1x 即 free run
        (HWREG(CPUTIMER1_BASE + CPUTIMER_O_PRD) != 0xFFFFFFFFuL) ||
        ((HWREGH(CPUTIMER1_BASE + CPUTIMER_O_TPR) &
          CPUTIMER_TPR_TDDR_M) != 0u) ||
        ((HWREGH(CPUTIMER1_BASE + CPUTIMER_O_TPRH) &
          CPUTIMER_TPRH_TDDRH_M) != 0u) ||
        ((timer_tcr & CPUTIMER_TCR_TSS) != 0u) ||
        ((timer_tcr & CPUTIMER_TCR_TIE) != 0u) ||
        ((timer_tcr & CPUTIMER_TCR_FREE) == 0u) ||
        // TBCLK 分频器 CLKDIV/HSPCLKDIV 必须都 /1：与 PERIOD 同决定 ISR 频率
        //（TBCLK = EPWMCLK/(HSPCLKDIV*CLKDIV)），但只在 syscfg 有源头、C 侧无镜像。
        // 漂成非 /1 则真实频率偏离而 PERIOD 仍 == V2K_TB_PRD：周期检查照过、频率
        // 静默错（code review #3）。两字段全 /1 ⟺ TBCTL 这两段 bit 全 0。
        ((tbctl & (EPWM_TBCTL_CLKDIV_M | EPWM_TBCTL_HSPCLKDIV_M)) != 0u))
    {
        for (;;) { ESTOP0; }
    }
}

void v2k_tb_init(void)
{
    // SOC 触发源补写（TI C2000Ware 26.01 SysConfig codegen bug 绕过，见
    // V2K_TB_SOC_SRC 注释）：选 TBCTR_ZERO 时 syscfg 不生成
    // EPWM_setADCTriggerSource，SOCASEL 停在复位值 DCxEVT1 → SOC 永不触发。
    // 此字段例外由 C 拥有，须在 v2k_tb_check 读回之前显式写入（其余安全关键项
    // 仍是 syscfg 配、这里只读回对账，两种所有权并存不矛盾）。
    EPWM_setADCTriggerSource(EPWM1_BASE, EPWM_SOC_A, V2K_TB_SOC_SRC);

    // 契约自检：EPWMCLKDIV=/1（syscfg 时钟树→Device_init）、周期、TZ 源/动作、
    // FREE_SOFT 读回检查 syscfg 的工作；SOCAEN/SOCASEL 读回检查上面这行 C 补写。
    v2k_tb_check();

    // 占空比是运行时量（将来 = user_step 输出），不属于 syscfg 静态配置
    EPWM_setCounterCompareValue(EPWM1_BASE, EPWM_COUNTER_COMPARE_A,
                                (uint16_t)V2K_TB_CMPA_INIT);

    // ISR 所有权归 L1（规则：用户/工具不碰 ISR），注册不走 syscfg
    ADC_clearInterruptStatus(ADCA_BASE, ADC_INT_NUMBER1);
    Interrupt_register(INT_ADCA1, &v2k_executor_isr);
    Interrupt_enable(INT_ADCA1);
}

void v2k_tb_start(void)
{
    // 唯一的放行点：保护（v2k_fault_arm/init）与自检全部就位后才到这里
    SysCtl_enablePeripheral(SYSCTL_PERIPH_CLK_TBCLKSYNC);
}
