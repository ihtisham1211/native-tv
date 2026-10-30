# Performance baseline

Measurements taken before any of the changes in [the optimizations](performance-optimizations.md), on 30 October 2026. The method is in [the benchmarks](performance-benchmarks.md).

## Environments

| | Host | TV |
|---|---|---|
| Machine | Apple M1 Pro, macOS | LG OLED65B3PUA, webOS 10.3.2 (aarch64 kernel, 32-bit userland) |
| GPU / GL | Metal-backed GL 2.1; swaps returned every 8.3 ms, so the display runs at 120 Hz | PowerVR BXE-4-32, GLES 3.2; 60 Hz |
| Build | CMake Release (`-O3`), SDL 2.32 | CMake Release for ARM (`-O2`), static libstdc++ |
| Load | not controlled | load average about 33 from LG's own processes |

Host figures are for comparing one build with another on the same machine. They are not predictions for the TV.

## TV: the server

The daemon that was running on the TV (not rebuilt for this work), sampled over `/proc` with `scripts/tv-wakeups.sh` for 20 to 30 s.

| Measure | Value |
|---|---|
| Threads | 61 |
| Context switches, whole daemon, a client talking to it | 239 a second (two samples: 239.4 and 238.8) |
| Context switches, whole daemon, nobody connected | 142.0 and 142.2 a second, taken just before the deploy |
| Six threads, each | 22 a second (21.1 to 23.8 across samples) |
| Event-loop thread | 10.8 to 24.8 a second |
| Bursty threads | a civetweb worker at 52 a second in one sample and another thread at 62 in the next, while a client was talking to the server |
| CPU | 4.4 clock ticks a second |
| Memory | 16,044 kB resident, 19,752 kB peak |
| Resident `luna-send` children | 6, together 0.4 switches a second |
| Loopback request, one keep-alive connection | 0 to 1 ms each after the first (`/api/caps`, 212 bytes) |
| Loopback request, new connection each | 1 to 4 ms |

Six threads at 22 a second match six resident children, each of which polls 20 times a second in isolation (see [the optimizations](performance-optimizations.md)). All the daemon's threads are named alike, so the match is by count and rate, not by name.

## TV: the client

Measured by running an ARM build of the client from `/tmp` on the B3 (`scripts/tv-client-run.sh`), against the TV's own server, Home screen. The machine's load varied between batches (the window step alone ranged 85 to 234 ms), so only the interleaved comparisons in [the optimizations](performance-optimizations.md) are fair between builds.

| Start-up, six runs before the vertex-shader change | Median | Range |
|---|---|---|
| Window and GL context | 170 ms | 95 to 234 |
| Renderer (fonts, 8 shader programs) | +190 ms | |
| Atlases | +8 ms | |
| Glyph and icon prewarm | +86 ms | |
| First swap, from the start of `main` | 462 ms | 358 to 560 |

Inside the renderer step (three runs of a temporarily instrumented build): opening the font 1.2 to 1.8 ms; compiling the vertex shader, eight times, 41 to 46 ms; compiling the eight fragment shaders 74 to 81 ms; linking 24 to 27 ms; 142 to 155 ms in all.

Steady state, one 5 s run on Home:

| | Value |
|---|---|
| Full-rate frames after the first four (58) | p50 16.6 ms, p95 18.0 ms, max 26.1 ms |
| Ambient frames | 98, one every 34.7 ms (28.8 fps) |
| CPU per ambient frame, build + submit | 2.37 + 1.26 = 3.66 ms, about 10.6 % of one core |
| Draw calls, vertices | 24, 3,816 |
| GPU time per frame | about 8 ms (see below) |

GPU time: with `NATIVETV_DEBUG_SKIP=finish` (a `glFinish` after the draws, so `submitMs` includes the GPU's work), five paired runs of 6 s gave `submit` 1.25 ms without and 9.18 ms with it for ambient frames (runs 9.16 to 9.22), and 1.28 and 9.25 ms for full-rate frames. A Home frame therefore keeps the GPU busy for about 8 ms, roughly half of a 60 Hz frame while Home animates and a quarter of the 34.7 ms ambient interval. The figure includes driver work.

Which drawing kinds the ambient frame's build time goes on, from `NATIVETV_DEBUG_SKIP` (each left out in turn, median over about 67 ambient frames; two runs with nothing skipped gave 2.37 and 2.33 ms):

| Left out | Build ms | Submit ms | Draws |
|---|---|---|---|
| nothing | 2.37 | 1.26 | 24 |
| gradients | 1.52 | 1.07 | 19 |
| text | 1.84 | 1.05 | 21 |
| images | 1.99 | 0.54 | 7 |
| fills | 2.04 | 1.16 | 22 |
| shadows | 2.28 | 1.19 | 22 |
| icons | 2.29 | 1.20 | 24 |
| wall layers | 2.29 | 1.16 | 22 |

The first run after uploading the binary had a 477 ms gap between the second and third swaps; it did not recur in the next seven runs, and its cause was not established (a cold file cache is one possibility).

## Host: the client

Three runs per scenario, median, the Release client against `tests/home_fixture.py`. CPU, memory and switches are the client's own (`wait4`). Each run includes about 0.3 to 0.4 s of CPU for starting up, so the CPU column of a 10 s run overstates a steady state.

| Scenario | CPU % | RSS MB | Frames | p50 | p95 | p99 | Max ms | Late | Draws | Input→swap p50 / p95 / max ms |
|---|---|---|---|---|---|---|---|---|---|---|
| idle-home (10 s) | 10.0 | 122.8 | 414 | 8.3 | 16.0 | 17.2 | 17.7 | 0 | 27 | none |
| idle-dashboard (10 s) | 4.5 | 95.7 | 101 | 8.3 | 16.3 | 34.1 | 34.1 | 1 | 8 | none |
| nav-rapid (a key every 70 ms) | 15.5 | 115.6 | 469 | 8.3 | 12.4 | 17.2 | 17.8 | 0 | 29 | 7 / 9 / 10 (40 keys) |
| nav-rows | 10.6 | 116.0 | 511 | 8.3 | 9.4 | 17.1 | 21.0 | 0 | 25 | 7 / 8 / 9 (40 keys) |
| quick-drawer | 9.5 | 116.2 | 427 | 8.3 | 9.4 | 17.2 | 17.8 | 0 | 24 | 5 / 8 / 8 (8 keys) |
| apps-scroll | 8.9 | 116.1 | 456 | 8.3 | 9.4 | 17.0 | 17.7 | 0 | 24 | 7 / 9 / 9 (23 keys) |

Frame intervals are for frames drawn at the full rate; ambient frames (30 fps) are counted in Frames but not in the percentiles.

On a screen that never animates (`kit:keyboard`, then `dashboard`), the difference between a 4 s and a 14 s run gives the steady state: 1.6 % and 2.7 % of a core, and 3.3 and 3.6 wakeups a second.

The home screen is not idle: the hero's Ken Burns zoom is an ambient animation, so it keeps drawing at 30 fps. Pausing it after the idle dim would change what is on screen, so it was left as it is.

## Host: client start-up

Milliseconds from the start of `main` (`--report`, `startup`), three runs on the Home screen against an unreachable server:

| Window and GL context | Renderer | Atlases | Prewarm | First swap |
|---|---|---|---|---|
| 184 to 191 | 191 to 198 | 193 to 200 | 203 to 210 | 213 to 221 |

Creating the window and context is 85 % of it; the glyph and icon prewarm is about 10 ms. The B3's figures are in the section above. The daemon logs `startup: HTTP front door listening` and `startup: event loop running` with the milliseconds since `main()` began; these were not read on the TV either.

## What the numbers do not say

- **Input→swap** runs from the operating system's event timestamp to the return of `eglSwapBuffers`. The swap returns before the display scans the frame out, so it is not button-to-photon latency. Scripted keys are stamped when the loop takes them.
- The host client rows are not a prediction for the TV: the Home build step is 0.09 ms on the host and 2.4 ms on the B3. [The TV app page](../apps/tv-app.md) has the B3's frame intervals from 29 October.
- Not measured at all: power-on to launcher, the daemon's start-up milestones on the TV, a long soak, memory pressure, RSS growth over hours.
- The host rows were taken before the pause-between-frames fix. A stream of window or mouse events made them draw more often than the CPU and frame counts of a quiet run would; the same scenarios after the fix are in [the optimizations](performance-optimizations.md).
