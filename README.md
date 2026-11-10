# wafel_screen_log

Stroopwafel plugin that shows the IOSU log on the **TV**, starting in plugin `kern_main`, so problems can be seen on consoles without a serial port. On data aborts, prefetch aborts, undefined instructions and IOS panics it draws a crash screen (registers, faulting instruction, exception stack, last log lines).

## Variants (install one)

| File | Behaviour |
|---|---|
| `01scrlog.ipx` | Shows the log until Cafe OS takes over the TV, then stops (the Wii U Menu looks normal). |
| `01scrovl.ipx` | Keeps drawing the last 20 lines in a box at the bottom over Cafe OS's picture (flickers). |
| `01scrlgd.ipx` / `01scrovd.ipx` | Same, plus one line with the display registers found before init (for debugging the display takeover). |

Copy the file to `sd:/wiiu/ios_plugins/` (or `slc:/sys/hax/ios_plugins/`). The `01` prefix loads it right after `00core.ipx`.

## What you see

- **During plugin loading:** core/plugin `debug_printf` output, drawn immediately and wrapping at the bottom. A `-----` row marks the newest line.
- **From MCP start:** a scrolling view with the whole IOSU syslog (all processes) plus `debug_printf` output.
- **PPC start:** drawing stops (the framebuffer is Cafe OS memory); the last picture stays until Cafe OS takes the TV.
- **Crash:** the crash screen. It stays up because the system halts.
- **ECO / standby wake:** the TV is left off (unless something crashes).

Works with `5debug.ipx` installed (no duplicated lines).

## Build

```
export STROOPWAFEL_ROOT=/path/to/stroopwafel 
export DEVKITARM=/opt/devkitpro/devkitARM 
make            # 01scrlog.ipx
make OVERLAY=1  # 01scrovl.ipx
# add DIAG=1 for the register dump variants
```

## Files

- `source/main.c`: entry points, log hooks, MCP render thread, stop/overlay logic.
- `source/hw.c`, `source/gpu_tables.c`: display init ported from minute (GPLv2).
- `source/render.c`: font rendering, scrolling/incremental/overlay views.
- `source/log.c`: lock-free text ring.
- `source/crash.c`: crash screen (MMU-off drawing).
- `source/cpu.S`: cp15/cpsr helpers.
