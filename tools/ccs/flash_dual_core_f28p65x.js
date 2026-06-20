/*
 * Viewer2000 dual-core Flash programmer for LAUNCHXL-F28P65X / TMS320F28P650DK9.
 *
 * Run through DSS from the repository root, preferably via:
 *
 *   tools/ccs/flash_dual_core_f28p65x.sh
 *
 * The programming transaction uses CPU1 only. It temporarily maps Bank0-4 to
 * CPU1, loads both CPU1 and CPU2 Flash ELFs through the CPU1 Flash Plugin, then
 * restores the deployment map (Bank0-2 CPU1, Bank3-4 CPU2). Both loads use
 * "Necessary Sectors Only" and never run either application image.
 */

importPackage(Packages.com.ti.debug.engine.scripting);
importPackage(Packages.com.ti.ccstudio.scripting.environment);
importPackage(Packages.java.io);
importPackage(Packages.java.lang);

function usage() {
    print([
        "Usage:",
        "  dss.sh tools/ccs/flash_dual_core_f28p65x.js [options]",
        "",
        "Options:",
        "  --repo DIR       Repository root. Default: current directory.",
        "  --ccxml FILE     Target configuration. Default: cpu1/targetConfigs/TMS320F28P650DK9.ccxml.",
        "  --cpu1 FILE      CPU1 .out. Default: cpu1/FLASH/cpu1.out.",
        "  --cpu2 FILE      CPU2 .out. Default: cpu2/FLASH/cpu2.out.",
        "  --log FILE       DSS XML trace log. Default: /tmp/viewer2000_flash_dual_core.xml.",
        "  --help           Print this help and exit.",
        "",
        "Safety notes:",
        "  - An active CCS GUI debug session will make probe acquisition fail before Flash operations.",
        "  - Only CPU1 is connected; CPU2 remains in the Boot-ROM wait state.",
        "  - Bank0-4 are temporarily mapped to CPU1 only for programming.",
        "  - For formal acceptance, power-cycle after programming and use Flash boot."
    ].join("\n"));
}

function fail(message) {
    throw new Error(message);
}

function canonical(path) {
    return String(new File(path).getCanonicalPath());
}

function join(base, path) {
    return canonical(new File(base, path).getPath());
}

function requireFile(path, label) {
    var file = new File(path);
    if (!file.isFile()) {
        fail(label + " does not exist: " + path);
    }
}

function parseArgs() {
    var args = this.arguments;
    var repo = canonical(".");
    var cfg = null;
    var cpu1 = null;
    var cpu2 = null;
    var log = "/tmp/viewer2000_flash_dual_core.xml";

    for (var i = 0; i < args.length; ++i) {
        var arg = String(args[i]);
        if (arg == "--help" || arg == "-h") {
            usage();
            java.lang.System.exit(0);
        } else if (arg == "--repo") {
            if (++i >= args.length) {
                fail("--repo requires a directory");
            }
            repo = canonical(String(args[i]));
        } else if (arg == "--ccxml") {
            if (++i >= args.length) {
                fail("--ccxml requires a file");
            }
            cfg = canonical(String(args[i]));
        } else if (arg == "--cpu1") {
            if (++i >= args.length) {
                fail("--cpu1 requires a file");
            }
            cpu1 = canonical(String(args[i]));
        } else if (arg == "--cpu2") {
            if (++i >= args.length) {
                fail("--cpu2 requires a file");
            }
            cpu2 = canonical(String(args[i]));
        } else if (arg == "--log") {
            if (++i >= args.length) {
                fail("--log requires a file");
            }
            log = canonical(String(args[i]));
        } else {
            fail("Unknown argument: " + arg);
        }
    }

    if (cfg == null) {
        cfg = join(repo, "cpu1/targetConfigs/TMS320F28P650DK9.ccxml");
    }
    if (cpu1 == null) {
        cpu1 = join(repo, "cpu1/FLASH/cpu1.out");
    }
    if (cpu2 == null) {
        cpu2 = join(repo, "cpu2/FLASH/cpu2.out");
    }

    return {
        repo: repo,
        cfg: cfg,
        cpu1: cpu1,
        cpu2: cpu2,
        log: log
    };
}

function log(message) {
    if (env != null) {
        env.traceWrite(message);
    } else {
        print(message);
    }
}

function haltRequired(session, name) {
    try {
        log("HALT " + name);
        session.target.halt();
    } catch (ex) {
        fail("Cannot halt " + name + ": " + ex);
    }
    if (!session.target.isHalted()) {
        fail(name + " is not halted; refusing to start Flash operations");
    }
}

function connectAndHalt(session, name) {
    try {
        if (!session.target.isConnected()) {
            log("CONNECT " + name);
            session.target.connect();
        }
    } catch (ex) {
        fail("Cannot acquire " + name +
             ". Terminate any active CCS GUI debug session and verify the XDS110 connection. " +
             "Details: " + ex);
    }
    if (!session.target.isConnected()) {
        fail(name + " is not connected; refusing to start Flash operations");
    }
    haltRequired(session, name);
}

function setProgramOnlyDebuggerOptions(session) {
    if (session.options.optionExist("FlashVerboseMode")) {
        session.options.setBoolean("FlashVerboseMode", true);
    }
    if (session.options.optionExist("AddCIOBreakpointAfterLoad")) {
        session.options.setBoolean("AddCIOBreakpointAfterLoad", false);
    }
    if (session.options.optionExist("AddCEXITbreakpointAfterLoad")) {
        session.options.setBoolean("AddCEXITbreakpointAfterLoad", false);
    }
    if (session.options.optionExist("AutoRunToLabelOnRestart")) {
        session.options.setBoolean("AutoRunToLabelOnRestart", false);
    }
    if (session.options.optionExist("AutoRunToLabelOnReset")) {
        session.options.setBoolean("AutoRunToLabelOnReset", false);
    }
}

function setCommonFlashLoadOptions(session) {
    session.flash.options.setString("FlashEraseSelection", "Necessary Sectors Only (for Program Load)");
    session.flash.options.setString("FlashDownloadSetting", "Erase and Program");
    session.flash.options.setBoolean("FlashVerifySetting", true);
    session.flash.options.setBoolean("FlashResetOnOperation", true);
    session.flash.options.setBoolean("FlashAutoECCSetting", true);
}

function setAllBanksCpu1(session) {
    session.flash.options.setString("FlashCoreSelection", "CPU1");
    session.flash.options.setString("FlashMapC28Bank0", "0");
    session.flash.options.setString("FlashMapC28Bank1", "0");
    session.flash.options.setString("FlashMapC28Bank2", "0");
    session.flash.options.setString("FlashMapC28Bank3", "0");
    session.flash.options.setString("FlashMapC28Bank4", "0");
}

function setDeploymentBankMap(session) {
    session.flash.options.setString("FlashCoreSelection", "CPU1");
    session.flash.options.setString("FlashMapC28Bank0", "0");
    session.flash.options.setString("FlashMapC28Bank1", "0");
    session.flash.options.setString("FlashMapC28Bank2", "0");
    session.flash.options.setString("FlashMapC28Bank3", "1");
    session.flash.options.setString("FlashMapC28Bank4", "1");
}

function performBankMap(session) {
    log("CONFIGURE FLASH BANK MAP via ConfigureBanks");
    session.flash.performOperation("ConfigureBanks");
}

function evaluateSafe(session, expr) {
    try {
        var value = session.expression.evaluate(expr);
        log("EVAL " + session.getCPUName() + " " + expr + " = " + value);
        return value;
    } catch (ex) {
        log("EVAL " + session.getCPUName() + " " + expr + " failed: " + ex);
        return null;
    }
}

function assertBankMap(session, expected, description) {
    var expr = "((*(unsigned long *)0x0005D060) & 0x3FF) == " + expected;
    var value = session.expression.evaluate(expr);
    log("ASSERT " + expr + " = " + value);
    if (String(value) != "1" && String(value).toLowerCase() != "true") {
        fail("BANKMUXSEL does not match " + description);
    }
}

var env = null;
var server = null;
var cpu1Session = null;
var exitCode = 0;

try {
    var opts = parseArgs();
    requireFile(opts.cfg, "Target configuration");
    requireFile(opts.cpu1, "CPU1 output");
    requireFile(opts.cpu2, "CPU2 output");

    env = ScriptingEnvironment.instance();
    env.setScriptTimeout(600000);
    env.traceSetConsoleLevel(TraceLevel.INFO);
    env.traceBegin(opts.log, "/Applications/ti/ccs2100/ccs/ccs_base/scripting/examples/DebugServerExamples/DefaultStylesheet.xsl");
    env.traceSetFileLevel(TraceLevel.ALL);

    log("Viewer2000 CPU1-only dual-image Flash programming");
    log("REPO  " + opts.repo);
    log("CCXML " + opts.cfg);
    log("CPU1  " + opts.cpu1);
    log("CPU2  " + opts.cpu2);
    log("LOG   " + opts.log);

    server = env.getServer("DebugServer.1");
    server.setConfig(opts.cfg);

    var cpus = server.getListOfCPUs();
    for (var i = 0; i < cpus.length; ++i) {
        log("CPU[" + i + "] " + cpus[i]);
    }

    cpu1Session = server.openSession(".*C28xx_CPU1.*");
    log("OPEN CPU1 " + cpu1Session.getCPUName());

    setProgramOnlyDebuggerOptions(cpu1Session);

    connectAndHalt(cpu1Session, "CPU1");

    log("SET CPU1-ONLY PROGRAMMING MAP");
    setCommonFlashLoadOptions(cpu1Session);
    setAllBanksCpu1(cpu1Session);

    log("CONFIGURE CLOCK");
    cpu1Session.flash.performOperation("ConfigureClock");
    performBankMap(cpu1Session);
    evaluateSafe(cpu1Session, "*(unsigned long *)0x0005D060");
    assertBankMap(cpu1Session, "0x000", "the temporary all-banks-to-CPU1 programming map");

    log("LOAD CPU1 IMAGE THROUGH CPU1");
    cpu1Session.memory.loadProgram(opts.cpu1);
    haltRequired(cpu1Session, "CPU1 after load");

    // A program load can reset the device. Reassert temporary CPU1 ownership
    // before using the same CPU1 Flash Plugin to write the Bank3/Bank4 image.
    log("REASSERT CPU1-ONLY PROGRAMMING MAP");
    setAllBanksCpu1(cpu1Session);
    performBankMap(cpu1Session);
    evaluateSafe(cpu1Session, "*(unsigned long *)0x0005D060");
    assertBankMap(cpu1Session, "0x000", "the temporary all-banks-to-CPU1 programming map");

    log("LOAD CPU2 IMAGE THROUGH CPU1");
    setCommonFlashLoadOptions(cpu1Session);
    cpu1Session.memory.loadProgram(opts.cpu2);
    haltRequired(cpu1Session, "CPU1 after CPU2 image load");

    log("RESTORE DEPLOYMENT BANK MAP");
    setDeploymentBankMap(cpu1Session);
    performBankMap(cpu1Session);
    evaluateSafe(cpu1Session, "*(unsigned long *)0x0005D060");
    assertBankMap(cpu1Session, "0x3C0", "Bank0-2 on CPU1 and Bank3-4 on CPU2");

    log("PROGRAMMING COMPLETE");
    log("For acceptance: terminate debug, set S3 to Flash boot, power-cycle, then verify cold boot.");
} catch (ex) {
    print("FLASH SCRIPT FAILED: " + ex);
    if (env != null) {
        env.traceWrite("FLASH SCRIPT FAILED: " + ex);
    }
    exitCode = 1;
} finally {
    try {
        if (cpu1Session != null) {
            cpu1Session.terminate();
        }
    } catch (ex2) {
        print("CPU1 terminate failed: " + ex2);
    }
    try {
        if (server != null) {
            server.stop();
        }
    } catch (ex3) {
        print("Debug server stop failed: " + ex3);
    }
    try {
        if (env != null) {
            env.traceEnd();
        }
    } catch (ex4) {
        print("Trace end failed: " + ex4);
    }
}

if (exitCode != 0) {
    java.lang.System.exit(exitCode);
}
