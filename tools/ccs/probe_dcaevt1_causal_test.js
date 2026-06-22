// Viewer2000 Phase 5.1 live causal test (attach-only, no halt-judge, no reload).
//
// Isolates the root cause of the high-side (EPWMxA) distortion:
//   TZCTL.DCAEVT1 action == 0 (High-Impedance) forces EPWMxA whenever the
//   DCAEVT1 digital-compare event is asserted (TRIPIN7 high from the idle CMPSS
//   low-comparator). DCAEVT1 affects only EPWMxA; EPWMxB has no DCBEVT1 and keeps
//   switching. The per-event TZCTL force is NOT gated by TZSEL arming, so it is
//   active even in DRY_RUN.
//
// Mode (first argument):
//   "fix"  (default) : TZCTL.DCAEVT1 -> 3 (no action). Expect A AND B running.
//   "bug"            : TZCTL.DCAEVT1 -> 0 (Hi-Z). Expect A floating, B running.
//
// Each run also clears OST|CBC|INT first, because the DSS connect() momentarily
// halts CPU1 and CBC6 latches on a debugger halt (forcing BOTH outputs low via
// TZA/TZB). Clearing it undoes that self-inflicted artifact so the scope shows
// only the DCAEVT1->A effect. DCAEVT1 re-asserts continuously (TRIPIN7 high) but
// in "fix" mode its action is "no action", so A is no longer forced.

importPackage(Packages.com.ti.debug.engine.scripting);
importPackage(Packages.com.ti.ccstudio.scripting.environment);
importPackage(Packages.java.lang);

var CCXML = "C:/Users/SHOU/Desktop/Code/20260610_Viewer2000/Viewer2000/cpu1/targetConfigs/TMS320F28P650DK9.ccxml";
var PAGE = 1;
var O_TZCTL = 0x84, O_TZFLG = 0x93, O_TZCLR = 0x97;
var EPWM = [["EPWM1", 0x3000], ["EPWM2", 0x3200], ["EPWM8", 0x3E00]];

var mode = "fix";
if (this.arguments.length > 0) mode = String(this.arguments[0]);
var bug = (mode == "bug");

function hex(v, w) { var s = Long.toHexString(v & 0xffffffff); while (s.length < w) s = "0" + s; return "0x" + s; }
function p(t) { System.out.println(t); }

var env = ScriptingEnvironment.instance();
env.setScriptTimeout(40000);
env.traceSetConsoleLevel(TraceLevel.SEVERE);
var server = env.getServer("DebugServer.1");
server.setConfig(CCXML);

function r16(s,a){return Number(s.memory.readData(PAGE,a,16,false))&0xffff;}
function w16(s,a,v){s.memory.writeData(PAGE,a,v&0xffff,16);}

var cpu1 = null;
try {
    cpu1 = server.openSession(".*C28xx_CPU1.*");
    cpu1.target.connect();
    if (cpu1.target.isHalted()) { cpu1.target.runAsynch(); Thread.sleep(80); }

    p("[causal] mode=" + mode + " -> TZCTL.DCAEVT1 action = " + (bug ? "0 (Hi-Z, bug)" : "3 (no action, fix)"));
    for (var e = 0; e < EPWM.length; e++) {
        var nm = EPWM[e][0], b = EPWM[e][1];
        var before = r16(cpu1, b + O_TZCTL);
        var next = bug ? (before & ~0x30) : ((before & ~0x30) | 0x30);
        w16(cpu1, b + O_TZCTL, next);
        // Undo the debugger-halt artifact (OST|CBC|INT). Clear twice across a
        // short delay so a CBC re-latch from the connect settle is also cleared.
        w16(cpu1, b + O_TZCLR, 0x7);
    }
    Thread.sleep(40);
    for (var e2 = 0; e2 < EPWM.length; e2++) {
        var nm2 = EPWM[e2][0], b2 = EPWM[e2][1];
        w16(cpu1, b2 + O_TZCLR, 0x7);
    }
    Thread.sleep(40);
    for (var e3 = 0; e3 < EPWM.length; e3++) {
        var nm3 = EPWM[e3][0], b3 = EPWM[e3][1];
        var fl = r16(cpu1, b3 + O_TZFLG);
        var tz = r16(cpu1, b3 + O_TZCTL);
        p("  " + nm3 + ": TZCTL=" + hex(tz,4) + " DCAEVT1_act=" + ((tz>>4)&0x3) +
          "  TZFLG=" + hex(fl,4) + " (OST=" + ((fl&0x4)?1:0) + " CBC=" + ((fl&0x2)?1:0) +
          " DCAEVT1=" + ((fl&0x8)?1:0) + ")");
    }
    p("[causal] done; cores left RUNNING");
} catch (ex) {
    p("[causal] ERROR: " + ex);
} finally {
    try { if (cpu1 != null) cpu1.target.disconnect(); } catch (e1) {}
    try { server.stop(); } catch (e2) {}
}
