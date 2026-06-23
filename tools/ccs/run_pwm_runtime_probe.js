// Viewer2000 Phase 5.1 RUNNING-state ePWM/TZ/DC probe (attach-only).
//
// Goal: get the *real* RUNNING snapshot the Phase 5.1 debug note is missing.
// It attaches to the already-running flash firmware WITHOUT loading a program,
// erasing flash, or resetting either core. It:
//   1. attaches CPU1 (register/state reads) and CPU2 (command injection),
//   2. injects APP_START into the CPU2->CPU1 MSGRAM command plane (0x3B000),
//      which only the CPU2 bus master may write, hence the CPU2 session,
//   3. polls CPU1 until V2K_STATE_RUNNING (or reports why it could not),
//   4. dumps the EPWM action-qualifier / dead-band / trip-zone / digital-compare
//      registers plus CMPSS comparator status and live pin levels,
//   5. computes a per-phase A-vs-B verdict, then disconnects and LEAVES BOTH
//      CORES RUNNING so the bench scope sees live PWM.
//
// It never halts CPU1 while judging the waveform (CBC6 would force the pins low
// under a debugger halt). All reads happen in real-time mode.

importPackage(Packages.com.ti.debug.engine.scripting);
importPackage(Packages.com.ti.ccstudio.scripting.environment);
importPackage(Packages.java.lang);

var REPO = "C:/Users/SHOU/Desktop/Code/20260610_Viewer2000/Viewer2000";
var CCXML = REPO + "/cpu1/targetConfigs/TMS320F28P650DK9.ccxml";
var CPU1_OUT = REPO + "/cpu1/FLASH/cpu1.out";

var PAGE = 1;

// --- command plane (CPU2->CPU1 MSGRAM, 0x3B000) ---
var CMD_SEQ  = 0x3B000; // u32
var CMD_CODE = 0x3B002; // u16
var CMD_ARG0 = 0x3B003; // u16
var CMD_ARG1 = 0x3B004; // u32
// --- status plane (CPU1->CPU2 MSGRAM, 0x3A000) ---
var ST_SYSSTATE = 0x3A001; // u16
var ST_ACKSEQ   = 0x3A002; // u32
var ST_CMDRES   = 0x3A004; // u16
var ST_FAULT    = 0x3A005; // u16

var CMD_NOP = 0, CMD_START = 1, CMD_STOP = 2, CMD_CLEAR_FAULT = 3;
var S_INIT = 0, S_IDLE = 1, S_RUNNING = 2, S_FAULT = 3;

var EPWM = [["EPWM1", 0x3000], ["EPWM2", 0x3200], ["EPWM8", 0x3E00]];
var CMPSS = [["CMPSS7_phaseA", 0x5A80], ["CMPSS8_phaseB", 0x5AC0]];

var O = {
    TBCTL:0x00, TBCTR:0x04, SYNCINSEL:0x03, SYNCOUTEN:0x06, CMPCTL:0x08,
    DBCTL:0x0C, GLDCTL:0x34, GLDCFG:0x35, AQCTLA:0x40, AQCTLB:0x42,
    AQSFRC:0x47, AQCSFRC:0x49, DBRED:0x51, DBFED:0x53, TBPRD:0x63,
    CMPA:0x6B, CMPB:0x6D, TZSEL:0x80, TZDCSEL:0x82, TZCTL:0x84, TZCTL2:0x85,
    TZCTLDCA:0x86, TZCTLDCB:0x87, TZEINT:0x8D, TZFLG:0x93, TZCBCFLG:0x94,
    TZOSTFLG:0x95, DCTRIPSEL:0xC0, DCACTL:0xC3, DCBCTL:0xC4
};

function hex(v, w) {
    var s = Long.toHexString(v & 0xffffffff);
    while (s.length < w) s = "0" + s;
    return "0x" + s;
}
function p(t) { System.out.println(t); }

var env = ScriptingEnvironment.instance();
env.setScriptTimeout(60000);
env.traceSetConsoleLevel(TraceLevel.SEVERE); // keep stdout clean for parsing

var server = env.getServer("DebugServer.1");
server.setConfig(CCXML);

function openCpu(pattern) {
    var s = server.openSession(pattern);
    s.target.connect();
    if (s.target.isHalted()) {
        s.target.runAsynch();
        Thread.sleep(80);
    }
    return s;
}

function rd(s, addr, bits) {
    return Number(s.memory.readData(PAGE, addr, bits, false)) & 0xffffffff;
}
function wr(s, addr, val, bits) {
    s.memory.writeData(PAGE, addr, val & 0xffffffff, bits);
}

var cpu1 = null, cpu2 = null, ok = true;
try {
    p("[probe] opening CPU1 ...");
    cpu1 = openCpu(".*C28xx_CPU1.*");
    p("[probe] opening CPU2 ...");
    cpu2 = openCpu(".*C28xx_CPU2.*");

    try { cpu1.symbol.load(CPU1_OUT); } catch (e) { p("[probe] symbol load failed (continuing on fixed addrs): " + e); }

    function expr(name) {
        try { return String(cpu1.expression.evaluateToString(name)); }
        catch (e) { return "<err:" + e + ">"; }
    }

    var state0 = rd(cpu1, ST_SYSSTATE, 16);
    var fault0 = rd(cpu1, ST_FAULT, 16);
    p("[VIEWER_STATE_INITIAL]");
    p("sys_state(MSGRAM)=" + state0 + "  fault_code=" + fault0);
    p("g_v2k_sm_state=" + expr("g_v2k_sm_state"));
    p("g_v2k_fault_code=" + expr("g_v2k_fault_code"));
    p("g_v2k_app_enabled=" + expr("g_v2k_app_enabled"));
    p("g_v2k_tick=" + expr("g_v2k_tick"));
    p("g_v2k_tz_int_cnt=" + expr("g_v2k_tz_int_cnt"));
    p("s_powerstage_mode(0=POWERED,1=DRY_RUN)=" + expr("s_powerstage_mode"));
    p("s_powered_config_approved=" + expr("s_powered_config_approved"));
    p("s_start_phase=" + expr("s_start_phase"));
    p("s_start_block_reason=" + expr("s_start_block_reason"));
    p("current_trip_cfg_err=" + expr("s_current_trip_config_error"));

    function inject(code) {
        var seq = rd(cpu2, CMD_SEQ, 32);
        wr(cpu2, CMD_CODE, code, 16);
        wr(cpu2, CMD_ARG0, 0, 16);
        wr(cpu2, CMD_ARG1, 0, 32);
        wr(cpu2, CMD_SEQ, (seq + 1) & 0xffffffff, 32); // publish last
        // wait for CPU1 to ack
        var target = (seq + 1) & 0xffffffff;
        for (var i = 0; i < 200; i++) {
            Thread.sleep(10);
            if (rd(cpu1, ST_ACKSEQ, 32) == target) break;
        }
        return { ack: rd(cpu1, ST_ACKSEQ, 32), res: rd(cpu1, ST_CMDRES, 16),
                 state: rd(cpu1, ST_SYSSTATE, 16), fault: rd(cpu1, ST_FAULT, 16) };
    }

    var st = state0;
    if (st == S_FAULT) {
        p("[probe] state FAULT -> injecting CLEAR_FAULT");
        var r0 = inject(CMD_CLEAR_FAULT);
        p("[probe] clear_fault ack=" + r0.ack + " res=" + r0.res + " state=" + r0.state + " fault=" + r0.fault);
        st = r0.state;
    }
    if (st == S_IDLE) {
        p("[probe] state IDLE -> injecting APP_START");
        var r1 = inject(CMD_START);
        p("[probe] app_start ack=" + r1.ack + " res=" + r1.res + " state=" + r1.state + " fault=" + r1.fault);
        st = r1.state;
    } else if (st == S_RUNNING) {
        p("[probe] already RUNNING");
    } else {
        p("[probe] unexpected state " + st + " (INIT?) -> not injecting");
    }

    // settle a few background polls
    Thread.sleep(60);
    var stateF = rd(cpu1, ST_SYSSTATE, 16);
    var faultF = rd(cpu1, ST_FAULT, 16);
    p("[VIEWER_STATE_AFTER]");
    p("sys_state=" + stateF + " (2=RUNNING)  fault_code=" + faultF +
      "  cmd_result=" + rd(cpu1, ST_CMDRES, 16));

    // ---- register dump ----
    for (var e = 0; e < EPWM.length; e++) {
        var nm = EPWM[e][0], b = EPWM[e][1];
        p("[" + nm + "]");
        for (var k in O) {
            p("  " + nm + "." + k + "=" + hex(rd(cpu1, b + O[k], 16), 4));
        }
        var tzctl = rd(cpu1, b + O.TZCTL, 16);
        var tzctl2 = rd(cpu1, b + O.TZCTL2, 16);
        var dcaAct = (tzctl2 & 0x8000) ? ("TZCTLDCA=" + hex(rd(cpu1, b + O.TZCTLDCA, 16),4)) : (((tzctl >> 4) & 0x3) + "(0=HiZ,1=hi,2=lo,3=no)");
        var dcbAct = (tzctl2 & 0x8000) ? ("TZCTLDCB=" + hex(rd(cpu1, b + O.TZCTLDCB, 16),4)) : (((tzctl >> 8) & 0x3) + "(0=HiZ,1=hi,2=lo,3=no)");
        p("  " + nm + ".DCAEVT1_action(A-only)=" + dcaAct);
        p("  " + nm + ".DCBEVT1_action(B-only)=" + dcbAct);
    }

    for (var c = 0; c < CMPSS.length; c++) {
        var cn = CMPSS[c][0], cb = CMPSS[c][1];
        p("[" + cn + "]");
        p("  " + cn + ".COMPCTL=" + hex(rd(cpu1, cb + 0x0, 16), 4));
        p("  " + cn + ".COMPSTS=" + hex(rd(cpu1, cb + 0x2, 16), 4));
        p("  " + cn + ".DACHVALA=" + rd(cpu1, cb + 0x7, 16));
        p("  " + cn + ".DACLVALA=" + rd(cpu1, cb + 0x13, 16));
    }

    p("[EPWMXBAR_TRIP7]");
    p("  TRIP7_CFG0TO15=" + hex(rd(cpu1, 0x7A30, 32), 8));
    p("  TRIP7_ENABLE=" + hex(rd(cpu1, 0x7A58, 32), 8));

    p("[INPUTXBAR1_NFAULT]");
    var input1Select = rd(cpu1, 0x7900, 16);
    var inputSelectLock = rd(cpu1, 0x791E, 32);
    p("  INPUT1SELECT=" + input1Select + " (expected GPIO82)");
    p("  INPUTSELECTLOCK=" + hex(inputSelectLock, 8) +
      " INPUT1_locked=" + ((inputSelectLock & 0x1) ? 1 : 0));

    // ---- live pin sampling (GPyDAT_R read regs at 0x7F80) ----
    var aOr=0,aAnd=0xffffffff, cOr=0,cAnd=0xffffffff, dOr=0,dAnd=0xffffffff;
    for (var s2 = 0; s2 < 800; s2++) {
        var a = rd(cpu1, 0x7F80, 32);
        var cc = rd(cpu1, 0x7F84, 32);
        var dd = rd(cpu1, 0x7F86, 32);
        aOr|=a; aAnd&=a; cOr|=cc; cAnd&=cc; dOr|=dd; dAnd&=dd;
    }
    var aTog = (aOr ^ aAnd) & 0xffffffff;
    var cTog = (cOr ^ cAnd) & 0xffffffff;
    var dTog = (dOr ^ dAnd) & 0xffffffff;
    p("[PIN_TOGGLE] (1=toggled during sample window)");
    // A outputs: GPIO0(A.bit0), GPIO2(A.bit2), GPIO99(D.bit3)
    // B outputs: GPIO1(A.bit1), GPIO3(A.bit3), GPIO75(C.bit11)
    var g0 = (aTog & 0x1)?1:0, g2 = (aTog & 0x4)?1:0, g99 = (dTog & 0x8)?1:0;
    var g1 = (aTog & 0x2)?1:0, g3 = (aTog & 0x8)?1:0, g75 = (cTog & 0x800)?1:0;
    p("  A-side EPWMxA: GPIO0=" + g0 + " GPIO2=" + g2 + " GPIO99=" + g99);
    p("  B-side EPWMxB: GPIO1=" + g1 + " GPIO3=" + g3 + " GPIO75=" + g75);

    p("[VERDICT]");
    p("  running=" + (stateF == S_RUNNING) + " fault_code=" + faultF);
    p("  current_trip_cfg_err=" + expr("s_current_trip_config_error") +
      " input1_gpio82=" + ((input1Select == 82) ? 1 : 0) +
      " input1_locked=" + ((inputSelectLock & 0x1) ? 1 : 0));
    for (var e2 = 0; e2 < EPWM.length; e2++) {
        var nm2 = EPWM[e2][0], b2 = EPWM[e2][1];
        var fl = rd(cpu1, b2 + O.TZFLG, 16);
        var ost = rd(cpu1, b2 + O.TZOSTFLG, 16);
        var sel = rd(cpu1, b2 + O.TZSEL, 16);
        p("  " + nm2 + ": TZFLG=" + hex(fl,4) +
          " DCAEVT1_flag=" + ((fl & 0x8)?1:0) +
          " DCBEVT1_flag=" + ((fl & 0x20)?1:0) +
          " DCAEVT1_ostLatch=" + ((ost & 0x40)?1:0) +
          " DCBEVT1_ostLatch=" + ((ost & 0x80)?1:0) +
          " DCAEVT1_armed(TZSEL.4000)=" + ((sel & 0x4000)?1:0) +
          " DCBEVT1_armed(TZSEL.8000)=" + ((sel & 0x8000)?1:0) +
          " OST_flag=" + ((fl & 0x4)?1:0) +
          " CBC_flag=" + ((fl & 0x2)?1:0));
    }
    p("  A_toggle_all=" + ((g0&&g2&&g99)?1:0) + " B_toggle_all=" + ((g1&&g3&&g75)?1:0));
    p("[probe] done; leaving cores RUNNING (disconnect, no halt)");
} catch (ex) {
    p("[probe] ERROR: " + ex);
    ok = false;
} finally {
    try { if (cpu1 != null) cpu1.target.disconnect(); } catch (e1) {}
    try { if (cpu2 != null) cpu2.target.disconnect(); } catch (e2) {}
    try { server.stop(); } catch (e3) {}
}
p("[probe] EXIT ok=" + ok);
