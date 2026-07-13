---
name: ti-ccs
description: Use for Texas Instruments Code Composer Studio (CCS), C2000/MSP/Sitara/mmWave embedded firmware, CCS projects, SysConfig `.syscfg` files, CCS build/debug/serial workflows, TI LaunchPads, or any request that mentions CCS, CCStudio, TI SDKs, C2000Ware, Code Composer Studio MCP, project buildProject, SysConfig MCP, debug MCP, or serial MCP.
---

# TI CCS

## Overview

Use the CCS MCP tools for CCS-owned project state, SysConfig edits, builds, debug sessions, and serial connections. Do not treat the CCS project metadata or `.syscfg` files as ordinary text when CCS MCP tools are available.

## Required Preamble

Before any CCS/Texas Instruments task, read:

`/Applications/ti/ccs2100/ccs/Code Composer Studio.app/Contents/Resources/ai/CCS.md`

Treat `/Applications/ti/ccs2100` as the CCS install directory for this machine.

## Tool Rules

- Use `ccs-project` MCP tools for CCS project inspection and builds.
- Use `ccs-sysconfig` MCP tools for `.syscfg` reads and edits. Never edit `.syscfg` files with ordinary file edit tools.
- Use `ccs-debug` MCP tools for CCS debug sessions when the repo instructions allow MCP debug. If a repo says a project-local `.ccxml` must use GUI/DSS instead of MCP launch, follow the repo rule.
- Use `ccs-serial` MCP tools for TI serial port discovery and UART console work.
- If CCS MCP tools are absent or report that the CCS backend is unavailable, tell the user that CCS must be running with its AI extension loaded; do not restart, close, kill, or reopen CCS unless the user explicitly asks.

## Project Workflow

1. Identify the target device or board if the request depends on connected hardware.
2. Identify the target CCS project and SDK with `ccs-project`.
3. Read the SDK-specific `AGENTS.md` described by CCS.md before changing device-specific code or configuration.
4. Read board-specific `AGENTS.md` for LaunchPad/BoosterPack pin, LED, UART, and jumper details when relevant.
5. Modify ordinary source files with normal edit tools, but keep CCS project metadata and SysConfig under MCP control.
6. Build CCS projects with `ccs-project` `buildProject`, not direct `make`/`gmake`, unless no CCS project exists.
7. During active debug, rebuild with `buildProject`, then load the new `.out`; do not assume `restart` loads a rebuilt binary.

## Safety Boundaries

- Do not terminate CCS GUI debug sessions unless the user explicitly asks or a repo procedure requires it.
- Do not use CCS MCP `launchTargetConfiguration` for repos whose local instructions say it misresolves project-local target configs.
- Do not program or debug an energized power stage without following the repo's protection and flash instructions.
