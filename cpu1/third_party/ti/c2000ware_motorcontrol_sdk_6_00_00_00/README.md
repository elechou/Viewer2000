# C2000Ware MotorControl SDK vendored files

This directory contains small, project-local copies of files taken from:

`/Applications/ti/c2000/C2000Ware_MotorControl_SDK_6_00_00_00`

The repository vendors only the files required by the firmware build instead of
depending on an absolute SDK installation path. Keep upstream copyright and
license headers intact in copied source files.

## Current contents

| SDK-relative path | Purpose |
|---|---|
| `libraries/drvic/drv8323/include/drv8323s.h` | TI DRV8323S/RS register definitions and public driver API |
| `libraries/drvic/drv8323/source/drv8323s.c` | TI DRV8323S/RS SPI driver implementation |

## Local patch policy

Local changes to vendored files must be minimal and documented here.

- `libraries/drvic/drv8323/source/drv8323s.c`: `DRV8323_readSPI()` now exits
  when the existing RX FIFO timeout flag is raised and releases chip select
  before returning. The upstream function set `rxTimeOut` but continued
  waiting for RX FIFO data, which is unsafe for Viewer2000's bounded foreground
  start/diagnostic paths.
- `libraries/drvic/drv8323/source/drv8323s.c`: `DRV8323_writeSPI()` now waits
  for one RX FIFO word before releasing chip select, drains that full-duplex
  response word, and raises the same bounded timeout flag on failure. The
  upstream fixed NOP delay can release chip select before a 16-bit frame ends
  at the SDK lab's 400 kHz SPI rate; on hardware this left writable control
  registers unchanged while reads remained valid.
