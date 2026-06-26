# Viewer2000

**A rapid-prototyping platform for motors on the dual-core C2000 (F28P65x).**

![MCU](https://img.shields.io/badge/MCU-C2000%20F28P65x-CC0000)
![Host](https://img.shields.io/badge/host-Scope2000%20·%20Rust%20%2B%20egui-DEA584)
![License](https://img.shields.io/badge/license-MIT%20OR%20Apache--2.0-blue)
![Status](https://img.shields.io/badge/status-active%20development-yellow)

**English** · [中文](README.zh.md) · [日本語](README.ja.md)

<table>
<tr>
<td width="50%"><img src="assets/Motor.jpg" alt="LAUNCHXL-F28P65X + BOOSTXL-DRV8323RX driving a PMSM with an AS5600 encoder"></td>
<td width="50%"><img src="assets/Screenshot.png" alt="Live three-phase capture in the Scope2000 host viewer"></td>
</tr>
</table>

*Left — the supported kit: a **LAUNCHXL-F28P65X + BOOSTXL-DRV8323RX** power stage driving
a PMSM with an AS5600 encoder. Right — that same board's open-loop V/f run streamed live
into **Scope2000**: electrical angle, three-phase currents, and three-phase PWM duty —
every trace a plain C variable straight out of `control()`.*

If you've dealt with motor control before, you probably know the feeling: getting FOC
running on a C2000 means configuring boot, clocks, pinmux, the ePWM→ADC→EOC chain, fault
protection, and dual-core comms before you've even started writing control code. And
observability is its own hassle — CCS Graph works but it's not flexible, and getting
multi-channel waveforms going takes real effort.

Viewer2000 does one thing: **take over all that low-level work**. You write a `setup()` and
a `control()` over a flat I/O struct, and that's your entire application. Meanwhile, the
companion [Scope2000](https://github.com/elechou/Scope2000) host viewer lets you observe
and modify every variable inside it live, at control-loop rates — no reflashing, no CCS
window configuration, variable names are baked into the firmware and travel with the device
automatically. You can even plug the board into a machine with no project source code at all
and use [Scope2000](https://github.com/elechou/Scope2000) to control the motor directly.

## Highlights

- **Arduino-style control loop** — your whole application is `setup()` + `control()`
  over a global `v2k_io`. No boilerplate, no register wrangling.
- **Live observability, no reflashing** — the build automatically bakes your plain C
  variable names into the firmware; Scope2000 shows them by name. Pin, plot, and even
  write to them live — change a value and keep running.
- **Deterministic by construction** — the platform owns the ePWM→ADC→EOC chain and calls
  `control()` once per period. One sample always equals one control cycle — there's never
  a question of "which cycle does this sample belong to."
- **Protection you can't disable** — hardware trip (TZ/CMPSS) and a fault state machine
  sit *beneath* your code, not *inside* it. If your `control()` blows up, PWM still shuts
  off.
- **Dual-core isolation** — CPU1 runs control, CPU2 runs comms. If the comms side stalls,
  drops, or crashes, the control loop keeps running.
- **Use the official TI ecosystem directly** — pure C means C2000Ware, the
  MOTORCONTROL-SDK, and DCL drop in with a plain include, exactly the way the TI examples
  use them.
- **Triggered snapshots & CSV** — pre-trigger history capture and dead-simple CSV export.

---

## Write motor control like Arduino

Your entire application is two functions and one global I/O struct:

```c
#include "v2k.h"

// Plain C globals. Baked into the firmware at build time — Scope2000
// shows them by name. Pin, watch, scope, and write to them live, no reflashing.
float    duty   = V2K_DUTY_NEUTRAL;   // 0.50 = neutral
uint16_t ia_raw;

void setup(void) {
    // Runs once after every reset, before control() is ever called.
    duty = V2K_DUTY_NEUTRAL;
}

void control(void) {
    // Runs every control period (e.g. 20 kHz), the instant the ADC frame closes.
    ia_raw = v2k_io.adc.ia_raw;          // a coherent, time-aligned sample
    v2k_pwm_apply(duty, duty, duty);     // submit the three-phase duty command
}
```

That's it. There are only three things to explain:

- **`v2k_io.adc`** — this control period's completed ADC raw frame: phase currents, phase
  voltages, bus voltage — all in one place.
- **`v2k_io.sys`** — the platform tick, schedule flags (`V2K_DUE_1KHZ` / `V2K_DUE_100HZ`
  — to know "should I run slow-rate work this tick?"), state, and fault code.
- **`v2k_pwm_apply(a, b, c)`** — submit your three-phase duty. No implicit output — the
  platform won't secretly apply anything after `control()` returns. Output is always your
  own explicit call.

Note that `control()` is not quite the same as Arduino's `loop()`. Arduino's `loop()` is a
free-running super-loop that hogs the CPU whenever no interrupt is pending. Our `control()`
is the body of a 20 kHz ISR, because for motor control, "this function *is* the interrupt
service routine" is the one thing that actually matters to get right.

A worked example — low-energy open-loop V/f with current-offset calibration and frequency
slew — ships in [`cpu1/app/user.c`](cpu1/app/user.c), ready to copy and modify.

---

## The hardware

The directly supported, out-of-the-box target is TI's public evaluation kit (pictured at the
top):

| Part | What it is |
|---|---|
| **LAUNCHXL-F28P65X** | Dual-core C2000 LaunchPad (control MCU) |
| **BOOSTXL-DRV8323RX** | Three-phase gate-driver + FET booster pack (DRV8323RS) |
| **AS5600** | Magnetic absolute-angle position sensor |

If your bench differs, that's a board-layer (L0) change — see the layer map below.

## ⚠️ Safety

This platform drives real power stages and spins real motors. It can source large currents
and switch hazardous voltages.

- New code **must** be brought up at low bus voltage, low modulation depth, with a
  current-limited supply. Only increase energy after confirming correct behavior.
- Keep clear of rotating parts. Mechanically secure the motor.
- The built-in protection (overcurrent trip, fault latch) is a **safety net**, not an excuse
  to skip a current limit.
- Your hardware, your wiring, your motor — safety is your responsibility.

---

## Quick start

You'll need [Code Composer Studio](https://www.ti.com/tool/CCSTUDIO), TI's C2000Ware
MOTORCONTROL-SDK, the evaluation kit above, and the
[Scope2000](https://github.com/elechou/Scope2000) host viewer.

1. **Build.** Import the `cpu1/` and `cpu2/` projects into CCS and build the `FLASH`
   configuration for both. The legacy `RAM` configuration is deprecated from Phase 5.0
   onward and is no longer a supported acceptance target. The build automatically runs
   SysConfig and bakes user-variable descriptors — nothing extra to set up.
2. **Program and run.** Terminate any CCS GUI debug session, flash both cores with
   `tools/ccs/flash_dual_core_f28p65x.sh` (or `.cmd` on Windows), set S3 to Flash boot,
   and power-cycle the board.
3. **Watch waveforms, tune parameters.** Launch Scope2000 and connect to the board's XDS110
   virtual COM port. Your variables pop up automatically — pin them, scope them, change them
   live.
4. **Make it yours.** Edit `control()` in [`cpu1/app/user.c`](cpu1/app/user.c), rebuild,
   reload — your new variables appear in Scope2000 on the next enumeration.

The full, hardware-verified bring-up procedure is in [`BRINGUP.md`](BRINGUP.md), with
per-phase design documents under [`docs/`](docs/).

---

## The four-layer architecture

Viewer2000 is cut into four layers. The key boundary runs between **L1 and L2**: the
platform owns L0–L1, you own L2–L3.

| Layer | Where | What it does |
|:--:|---|---|
| **L3** | `cpu1/app/` | `setup()` / `control()`, your application state |
| **L2** | Your own control modules | Control math & motor semantics: C2000Ware MOTORCONTROL-SDK, DCL, hand-written, or Simulink-generated C |
| **L1** | `cpu1/runtime/` | ISR scheduling, control state machine, protection policy, parameter/descriptor registry, RAM scope |
| **L0** | `cpu1/board/` | Boot, memory map, pinmux, ePWM/ADC/CMPSS/TZ/X-BAR, DRV8323RS & AS5600 drivers — on C2000Ware driverlib |

A second CPU runs alongside: **CPU1 is the control domain, CPU2 is the comms domain**
(SCI today, higher-bandwidth EtherCAT later). The split isolates fault *and* time domains —
if the comms side goes down, the control loop's determinism is unaffected.

### Which layer do I edit?

In the common case, you only touch the one that matches your goal:

| What you want to do | Edit | Layer |
|---|---|:--:|
| Run a control law (FOC / V/f / your own loop) | `cpu1/app/user.c` — `setup()` / `control()` | **L3** |
| Bring in a specific control library | Your own control module: pull in MOTORCONTROL-SDK / DCL, or write directly in `app/` | **L2** |
| Adjust protection thresholds, advanced timing/scheduling | `cpu1/runtime/` | **L1** |
| Adapt to a different board, wiring, or sensor | `cpu1/board/` (board profile) | **L0** |

Most users only work in L2–L3. L0 is a one-time adaptation when the board or wiring
changes. L1 rarely needs touching.

---

## Who this platform is for

1. **Researchers** — focused on the control law itself, not on chip bring-up and peripheral
   configuration.
2. **Students** — learning FOC or motor control, wanting to see their math drive real
   current waveforms on a screen.
3. **Embedded beginners** — wanting to drive a motor on C2000 without drowning in init code,
   register setup, and protection plumbing first.
4. **People who need fast evaluation** — swap a control law for A/B comparison, characterize
   a new motor — and want answers the same day.
5. **Educators and TAs** — need a reproducible teaching rig where the core code is short,
   readable, and the same on every bench.
6. **Algorithm engineers** — validating Simulink-generated or hand-written control C on a
   real board before it ever goes near a product BSP.

## Who this platform is *not* for

1. **People who want to learn C2000 itself / the C2000Ware SDK in depth.** This is honestly
   still a work in progress. I try to keep the platform compatible with C2000Ware and the
   MOTORCONTROL-SDK, so you can call the official APIs much like the official examples do.
   But the runtime **enforces some safety protections** that, in certain cases, make an
   official C2000Ware API ineffective — because the platform retains ownership of timing,
   output release, interrupts, and protection. The gap is shrinking and compatibility keeps
   improving.
2. **STM32 (or other non-C2000) users, and non-dual-core users.** The platform assumes a
   dual-core C2000 fault/time-domain split from the ground up. Porting to a different DSP
   family would likely mean a major restructuring, so future DSP targets will probably get
   their own repo.
3. **People building a motor *product*.** The platform can help you get a control algorithm
   running and validated — but the control algorithm is often the *smallest* part of a
   product. The IO, initialization, and BSP that the platform manages for you are exactly
   the parts you'd have to redo by hand, and that migration will be super painful.

---

## FAQ

**Why does CPU1 handle sampling — can't I just read the ADC myself?**
> Because of **timing unity**. The platform owns the ePWM→ADC SOC→EOC chain and only calls
> your `control()` after the ADC frame has finished. Every sample is taken at a fixed point
> in the period: the very **beginning** of the 20 kHz control cycle. And `control()` is
> entered **immediately after sampling completes**, so you can always trust the `v2k_io.adc`
> interface — it is a value from a fixed-period ADC sample that has definitely finished
> converting for this tick. Your control law always sees a coherent, time-aligned snapshot —
> not "an ADC read from some random point in a loop."

**CPU1 is tight on compute — why does scope sampling also run on CPU1, instead of offloading
it to CPU2?**

> Because sampling must be **deterministic in time**. If CPU2 reached across to sample CPU1's
> variables, nothing would guarantee that each read lands exactly one interrupt after the
> last control tick. CPU2 could miss a tick, sample another one twice, or — worse — blend
> two adjacent ticks' values and hand them to the host as if they were a single tick. For
> anyone who needs to observe the control result *precisely*, that's unacceptable.
>
> Sampling inside the CPU1 control ISR makes every channel naturally same-tick: one sample,
> one control cycle, clean and simple.
>
> That said, CPU1 cycles are indeed tight. For very-high-rate operation (100 kHz and up), we
> plan to move cooperatively splittable scope work to the CLA or CPU2. If you don't need
> tick-level precision but need maximum CPU1 headroom, there will likely also be a path to
> lift the scope function off CPU1 entirely.

**Arduino is C++ too — why is this pure C?**

> Because in this context, C++'s benefits don't yet outweigh the hassle:
>
> - **Observability.** The primary debug interface is CCS Expressions/Graph and the Scope2000
>   watch tree, which read flat C structs directly. C++ name mangling, templates, and private
>   members are all obstacles in the map file and watch window.
> - **Use the official code directly.** C2000Ware, the MOTORCONTROL-SDK, and DCL are all
>   plain C. Staying in C means you just include and use them, exactly like the TI examples
>   — no `extern "C"` wrapping, no fighting mixed-compilation linker issues.
> - **CLA option.** The CLA accepts only a C subset. Keeping the fast loop in C preserves
>   the option of later moving it onto the CLA.
> - **Audience.** The target users — beginners and motor-control researchers — write C day to
>   day, and their future work will most likely involve TI's C2000Ware ecosystem.
>
> If the platform's complexity ever outgrows C, this decision gets revisited. But today, C
> is the shortest path to having the official TI ecosystem one include away.

---

## Current status

Viewer2000 is under **active development** and currently runs on a single hardware target.
Working end to end today: dual-core boot and IPC, hardware-enforced protection, multi-rate
scheduling, RAM scope, atomic parameter transactions, SCI streaming to Scope2000, and an
open-loop V/f first-rotation application.

Up next:

- closed-loop FOC user examples on top of the same `setup()`/`control()` interface;
- **EtherCAT** transport layer (the protocol lives inside the pipe — swapping the physical
  layer doesn't change the service model);
- hardening the L0 board layer so a new target is a single-layer change.

---

## Documentation

- [`docs/wire-spec.md`](docs/wire-spec.md) — the host↔firmware wire protocol (authority)
- [`docs/board-portability.md`](docs/board-portability.md) — the board layer and porting model
- [`docs/protection-architecture.md`](docs/protection-architecture.md) — the protection architecture
- [`docs/`](docs/) — per-phase design documents
- [`BRINGUP.md`](BRINGUP.md) — the hardware-verified bring-up log

## Companion host: Scope2000

The screenshots above are [**Scope2000**](https://github.com/elechou/Scope2000), a Rust +
egui host viewer (independent sibling repository). It enumerates your baked-in variables at
runtime, streams `ScopeBlock` data for live monitoring and triggered snapshots, renders
waveforms, and exports CSV — no `.out` parsing, no reflashing, just plug in and start
watching and tuning.

## Repository layout

This repository ([`elechou/Viewer2000`](https://github.com/elechou/Viewer2000)) is
**firmware only** — everything flashed into the F28P65x, both cores. The host viewer lives
in the sibling repository [`elechou/Scope2000`](https://github.com/elechou/Scope2000).

The wire protocol is defined in [`docs/wire-spec.md`](docs/wire-spec.md), the headers under
[`contracts/`](contracts/), and the golden conformance vectors under
[`contracts/vectors/`](contracts/vectors/). Compatibility with older devices belongs in a
separate out-of-process bridge and must not alter the native Viewer2000 protocol or data
path.

## License

[Apache License 2.0](LICENSE-APACHE) / [MIT](LICENSE-MIT)

## Acknowledgements

Built on TI's C2000Ware, MOTORCONTROL-SDK, and DCL; example board is the official F28P65x
LaunchPad + DRV8323 BoosterPack.

The host viewer [`Scope2000`](https://github.com/elechou/Scope2000) is built with
[egui](https://github.com/emilk/egui), with style inspiration from
[rerun](https://github.com/rerun-io/rerun).
