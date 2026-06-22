// Attach-only PWM/GPIO register snapshot for Viewer2000 bring-up.
// This script does not load a program, erase/program Flash, or reset the target.

importPackage(Packages.com.ti.debug.engine.scripting);
importPackage(Packages.com.ti.ccstudio.scripting.environment);
importPackage(Packages.java.lang);

var script = ScriptingEnvironment.instance();
script.setScriptTimeout(20000);
script.traceSetConsoleLevel(TraceLevel.INFO);

var debugServer = script.getServer("DebugServer.1");
var ccxml = "C:/Users/SHOU/Desktop/Code/20260610_Viewer2000/Viewer2000/cpu1/targetConfigs/TMS320F28P650DK9.ccxml";
debugServer.setConfig(ccxml);

function hex(value, width)
{
    var s = Long.toHexString(value);
    while (s.length < width)
    {
        s = "0" + s;
    }
    return "0x" + s;
}

function println(text)
{
    System.out.println(text);
}

var cpus = debugServer.getListOfCPUs();
println("cpus=[");
for (var i = 0; i < cpus.length; i++)
{
    println("  " + i + ": " + cpus[i]);
}
println("]");

var cpuPattern = null;
for (var c = 0; c < cpus.length; c++)
{
    if (String(cpus[c]).indexOf("CPU1") >= 0)
    {
        cpuPattern = String(cpus[c]);
        break;
    }
}
if (cpuPattern == null)
{
    for (var d = 0; d < cpus.length; d++)
    {
        if (String(cpus[d]).indexOf("C28") >= 0)
        {
            cpuPattern = String(cpus[d]);
            break;
        }
    }
}
if (cpuPattern == null)
{
    cpuPattern = ".*";
}

println("openSession=" + cpuPattern);
var session = debugServer.openSession(cpuPattern);
session.target.connect();

var wasHalted = session.target.isHalted();
println("target_halted_after_connect=" + wasHalted);
if (wasHalted)
{
    session.target.runAsynch();
    java.lang.Thread.sleep(100);
    println("target_released_for_realtime_read=true");
}

var symbols = "C:/Users/SHOU/Desktop/Code/20260610_Viewer2000/Viewer2000/cpu1/FLASH/cpu1.out";
session.symbol.load(symbols);
println("[VIEWER_STATE]");
function expr(name)
{
    try
    {
        println(name + "=" + session.expression.evaluateToString(name));
    }
    catch (e)
    {
        println(name + "=<expr_error:" + e + ">");
    }
}
expr("g_v2k_sm_state");
expr("g_v2k_fault_code");
expr("g_v2k_app_enabled");
expr("g_v2k_tick");
expr("g_v2k_tz_int_cnt");

var DATA_PAGE = 1;

function read16(addr)
{
    return session.memory.readData(DATA_PAGE, addr, 16, false);
}

function read32(addr)
{
    return session.memory.readData(DATA_PAGE, addr, 32, false);
}

function reg16(name, addr)
{
    println(name + "=" + hex(read16(addr), 4));
}

function reg32(name, addr)
{
    println(name + "=" + hex(read32(addr), 8));
}

function dumpEpwm(label, base)
{
    println("[" + label + "]");
    reg16(label + ".TBCTL",     base + 0x0000);
    reg16(label + ".SYNCINSEL", base + 0x0003);
    reg16(label + ".TBCTR",     base + 0x0004);
    reg16(label + ".SYNCOUTEN", base + 0x0006);
    reg16(label + ".CMPCTL",    base + 0x0008);
    reg16(label + ".DBCTL",     base + 0x000C);
    reg16(label + ".DBCTL2",    base + 0x000D);
    reg16(label + ".AQCTLA",    base + 0x0040);
    reg16(label + ".AQCTLB",    base + 0x0042);
    reg16(label + ".DBRED",     base + 0x0051);
    reg16(label + ".DBFED",     base + 0x0053);
    reg16(label + ".TBPRD",     base + 0x0063);
    reg16(label + ".CMPA",      base + 0x006B);
    reg16(label + ".TZSEL",     base + 0x0080);
    reg16(label + ".TZDCSEL",   base + 0x0082);
    reg16(label + ".TZCTL",     base + 0x0084);
    reg16(label + ".TZFLG",     base + 0x0093);
    reg16(label + ".TZCBCFLG",  base + 0x0094);
    reg16(label + ".TZOSTFLG",  base + 0x0095);
    reg16(label + ".DCTRIPSEL", base + 0x00C0);
    reg16(label + ".DCACTL",    base + 0x00C3);
}

dumpEpwm("EPWM1", 0x3000);
dumpEpwm("EPWM2", 0x3200);
dumpEpwm("EPWM8", 0x3E00);

println("[GPIOCTRL]");
reg32("GPIO.GPAMUX1",  0x7C06);
reg32("GPIO.GPAGMUX1", 0x7C20);
reg32("GPIO.GPAQSEL1", 0x7C02);
reg32("GPIO.GPAPUD",   0x7C0C);
reg32("GPIO.GPACSEL1", 0x7C28);
reg32("GPIO.GPCMUX1",  0x7C86);
reg32("GPIO.GPCGMUX1", 0x7CA0);
reg32("GPIO.GPCQSEL1", 0x7C82);
reg32("GPIO.GPCPUD",   0x7C8C);
reg32("GPIO.GPCCSEL2", 0x7CAA);
reg32("GPIO.GPDMUX1",  0x7CC6);
reg32("GPIO.GPDGMUX1", 0x7CE0);
reg32("GPIO.GPDQSEL1", 0x7CC2);
reg32("GPIO.GPDPUD",   0x7CCC);
reg32("GPIO.GPDCSEL1", 0x7CE8);

println("[EPWMXBAR_TRIP7]");
reg32("EPWMXBAR.TRIP7_CFG0TO15", 0x7A30);
reg32("EPWMXBAR.TRIP7_ENABLE",   0x7A58);

println("[PIN_LEVEL_SAMPLE]");
var gpaOr = 0;
var gpaAnd = 0xffffffff;
var gpcOr = 0;
var gpcAnd = 0xffffffff;
var gpdOr = 0;
var gpdAnd = 0xffffffff;
for (var s = 0; s < 512; s++)
{
    var a = Number(read32(0x7F80));
    var c = Number(read32(0x7F84));
    var dval = Number(read32(0x7F86));
    gpaOr = gpaOr | a;
    gpaAnd = gpaAnd & a;
    gpcOr = gpcOr | c;
    gpcAnd = gpcAnd & c;
    gpdOr = gpdOr | dval;
    gpdAnd = gpdAnd & dval;
}
println("GPADAT_R_OR=" + hex(gpaOr & 0xffffffff, 8));
println("GPADAT_R_AND=" + hex(gpaAnd & 0xffffffff, 8));
println("GPCDAT_R_OR=" + hex(gpcOr & 0xffffffff, 8));
println("GPCDAT_R_AND=" + hex(gpcAnd & 0xffffffff, 8));
println("GPDDAT_R_OR=" + hex(gpdOr & 0xffffffff, 8));
println("GPDDAT_R_AND=" + hex(gpdAnd & 0xffffffff, 8));
println("toggle_mask_A_GPIO0_1_2_3=" + hex((gpaOr ^ gpaAnd) & 0x0000000f, 8));
println("toggle_mask_C_GPIO75=" + hex((gpcOr ^ gpcAnd) & 0x00000800, 8));
println("toggle_mask_D_GPIO99=" + hex((gpdOr ^ gpdAnd) & 0x00000008, 8));

session.target.disconnect();
debugServer.stop();
