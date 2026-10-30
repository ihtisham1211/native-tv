# Performance optimizations

What was changed on 30 October 2026, why, and what was and was not verified. The findings are in [the audit](performance-audit.md) and the starting numbers in [the baseline](performance-baseline.md).

Each result is labelled: **measured on the TV** (a B3, webOS 10.3.2), **measured on the host** (Apple M1 Pro), **implemented, not hardware-validated** or **estimated**.

## Resident child processes wake 4.5 times a second instead of 20

`ChildProcessRunner::run` polls a child's pipes with a 50 ms timeout, and checks a cancellation flag and `waitpid` on every wake-up. Output and exit wake `poll` by themselves, so only a cancel from another thread needs the timeout. The daemon keeps six to eight children without a deadline (the `luna-send -i` subscriptions), each on its own thread.

- A child with no deadline now polls every 250 ms. A child with a deadline still polls every 50 ms.
- Once both pipes are closed there is nothing left to wake `poll`, and the only wait is for the exit status, so the wait is 5 ms instead of a full poll interval.
- Cost: a cancelled resident child is killed up to 250 ms later than before. Cancellation is asynchronous (nothing joins on it), so this only delays a restart that waits for the old child to exit.

Files: `src/daemon/child_process.cpp`, `tests/child_process.cpp`.

| | Before | After |
|---|---|---|
| One resident child, 5 s, B3 | 20.2, 20.6, 20.6 switches a second | 4.4, 4.4, 4.6 (**measured on the TV**, the changed file cross-compiled on its own and run from `/tmp`, three paired runs) |
| One resident child, 3 s, host | 65 and 67 switches | 13 and 13 (**measured on the host**) |
| The whole daemon on the TV, nobody connected, 20 s samples | 142.0 and 142.2 a second; six threads at 21 to 24 | 35.3, 35.1 and 35.2 a second; the same six at 4.2 (**measured on the TV**, after deploying this build) |

Tests: `testResidentChildWakesRarely` fails on the old code (49 switches in 2 s against a limit of 25) and passes on the new one; `testCancelWithoutDeadlineIsNoticed` and `testExitAfterPipesClose` cover a late cancel and a child that closes its pipes before it exits. The six threads account for the whole drop: 6 × 17.8 = 106.8 a second against a measured 106.9. **Measured on the TV end to end.** The build on the TV before was another session's (`f77f44db9644`), not `HEAD`, so thread counts differ (61 and 59); the per-thread rates are the comparison.

## Two config pollers stop reading files that have not changed

- The remote-key watcher (`remote_button_log_watcher.cpp`) polled every 200 ms and each time loaded `remotekeys.json` and `remotekeys-lg.json`, with a second try on `.last-good` when one was missing: up to four opens and two parses, 300 times a minute.
- The NTP client (`ntp_client.cpp`) read and parsed the whole `config.json` every second.

Both now compare `fileStamp()` (modification time to the nanosecond, size and inode, or `-` when missing) and read only when it changes. The daemon writes these files by atomic rename, so a rewrite always changes the inode. The NTP client records the stamp only after a successful parse, so a failed read is tried again. The poll intervals are unchanged, so a key press and a config edit are noticed as fast as before.

Files: `src/daemon/json_file.{hpp,cpp}` (the shared helper), `remote_button_log_watcher.{hpp,cpp}`, `ntp_client.{hpp,cpp}`, `tests/json_file.cpp` (`testFileStampFollowsChanges`).

Result: the file work per poll falls from up to four opens and two parses to two `stat` calls, and from a full read and parse to one `stat`. **Verified by reading and by the runtime cases for remote keys; the saving was not counted in system calls.** Implemented, not hardware-validated.

## The picture cache keeps what is on screen while the loop idles

The LRU clock in `ImageLoader` advanced on every `pump()`, and the loop pumps on every wake-up, including the idle ones that draw nothing. Two idle wake-ups (about 100 ms) made every picture on screen evictable, so a new picture arriving at a full 40 MB budget could evict a visible poster, which was then fetched, decoded and faded in again. The clock now advances only on a pump that follows a `get()`.

Files: `src/graphics/image_loader.{hpp,cpp}`, `tests/image_loader.cpp` (`testIdlePumpsKeepVisiblePictures`, which fails on the old code and passes on the new). Needs a full cache and an idle spell to show, so it is rare. **Measured on the host by test; not observed on the TV.**

## The 30 fps ambient pause survives pointer and window events

Between frames the loop waits on events instead of sleeping, so that a key press ends the wait. Any event ended it: on the host the window manager and the mouse sent `SDL_WINDOWEVENT` and `SDL_MOUSEMOTION` events at the display rate, and the ambient pause (33 ms) collapsed to the swap interval. `native-client-platform`'s `test_ambient_animation_runs_at_30_fps` failed for that reason in 3 to 10 of every 20 runs, on a clean `HEAD` build as well (6 of 20 in one batch). The pause now ends only for the events the loop acts on (quit, key, text, click, wheel, and the window events that pause or resume it); the rest are dropped during the pause, which the loop would have ignored anyway. A Magic Remote pointer is expected to send motion events the same way; that was not tested.

Files: `src/main.cpp` (`endsPause`).

| 20 runs of 1.2 s with `NATIVETV_AMBIENT=1`, interleaved | Runs paced faster than 30 fps | Median frames per run |
|---|---|---|
| Before | 9 of 20 and 10 of 20 | 53 and 64 |
| After | 0 of 20 and 0 of 20 | 36 and 37 |

**Measured on the host.** Input latency is unchanged (nav-rapid input→swap 7 / 9 / 9 ms after, 7 / 9 / 10 before), and `native-client-platform` passed three times in a row after the change. The rate of window and mouse events depends on the machine and what the user is doing, so the before figures vary from run to run; the TV's compositor was not measured, and a test for a key arriving mid-pause needs an event-injection facility the client does not have.

## The renderer compiles its vertex shader once

All eight programs use the same vertex shader, and each `link()` compiled it again: 41 to 46 ms of the B3's start-up. It is now compiled the first time a program is built and attached to all of them. Files: `src/graphics/renderer.{hpp,cpp}`.

| Eight interleaved pairs on the B3, median | Before | After |
|---|---|---|
| Renderer step | 158 ms | 118 ms |
| First swap, from the start of `main` | 359 ms (320 to 387) | 306 ms (292 to 335) |

**Measured on the TV.** The output is unchanged: the renderer-on-GPU comparison and the golden captures pass on the host. Not changed: the fragment shaders (74 to 81 ms) differ per program.

## Start-up phase times

`--report` gains `startup`: milliseconds from the start of `main` to the window and GL context, the renderer, the atlases, the prewarm and the first swap. The daemon logs `startup: HTTP front door listening` and `startup: event loop running`, with the milliseconds since `main()` began (before `main()` the dynamic loader and static initialisers are not counted). Files: `src/main.cpp`, `src/daemon/main.cpp`, `tests/client_platform.py` (`test_report_lists_startup_phases_in_order`). Host figures are in [the baseline](performance-baseline.md); the client's are read on the B3 (see the baseline). The daemon's two lines are written at info level, which is block-buffered into `tvweb.log` (see the audit), so they appear there only when the buffer fills or the daemon stops; they were not read on the TV. `test_help_and_runtime_lifecycle` runs the real daemon, stops it and asserts the `event loop running` line with its milliseconds, so the line is known to be emitted; the HTTP-listening line (which needs `--listen`) is not asserted.

## Input latency and a benchmark runner

`--perf` now also logs, for each frame that shows an input event, `inputMs`: from the event's operating-system timestamp (or, for a scripted key, from when the loop took it) to the return of the swap call. The summary record gains `inputFrames`, `inputP50Ms`, `inputP95Ms` and `inputMaxMs`. The one-line `perf:` summary is unchanged. An event the pacing sleep pushed back keeps the timestamp it woke with, because `SDL_PushEvent` restamps it. An input that draws nothing is not carried into a later frame.

`tests/perf_bench.py` runs the scenarios in [the benchmarks](performance-benchmarks.md). `scripts/tv-wakeups.sh` samples the server's wakeups per thread on a TV.

Files: `src/main.cpp`, `src/platform/frame_stats.{hpp,cpp}`, `tests/frame_loop.cpp` (`summarisesInputLatency`), `tests/perf_bench.py`, `scripts/tv-wakeups.sh`. The instrumentation adds one comparison per input event; its overhead was not measured separately.

## Regressions checked

- Host test suite (`ctest`, Release), rerun after the last code change: 241 passed, 2 skipped because they need a real TV (`native-tvwebctl`, `native-e2e-tv`). It includes the renderer-on-GPU comparison, the golden captures, the client platform tests and the daemon runtime cases.
- ARM build of `nativetv-daemon`, `nativetv-tv` and `nativetv-service` with the SDK, rebuilt after the last code change: builds.
- Deployed to the B3 on 30 October with `deploy.sh --no-app`, at the user's request. It replaced the running daemon (`f77f44db9644`, built by another session from a different working tree) with this build (`e117d5b1a3b8`), and the client in the dashboard and splash apps (`329fde4356ae`) with this build's (`24b47d3f5c04`). The home screen tile was left alone. Whoever relies on the other build has to deploy it again.
- AddressSanitizer build of the changed test targets: the child, image loader, store and frame-loop tests pass, and so do the client platform tests with the client built under it. `native-ntp-client` aborts in that build on the Mac with and without the NTP change, and passes in the Release build; it is not caused by this work and was not investigated.
- Route manifest, CSRF policy, test matrix, drift, retired files, strings and UI id checks pass, and `mkdocs build --strict` builds the docs.
- Not run: the E2E cases on the TV (they need the shared TV's Home takeover off), the dashboard UI suites in `tests/ui/` (no dashboard script changed), and the Linux sanitizer job.

## On the TV after the deploy

- **E2E cases** (`run.py --all --quick`, no `--allow`, so nothing that changes a setting, takes over Home, reboots or deploys): 18 passed, 11 at the frame-time floor, 7 failed and 38 were skipped because they need an `--allow`. The seven failures:
  - E2E-030 (Home navigation) needs 45 draw calls against a limit of 40. A client built from the commit before this work (`d3b96fc`) needs 45 too, in all 17 runs, so it predates these changes.
  - E2E-124, 170, 172, 173, 174 and 176 check UI behaviour (a title's details opening on Trailer, no Home bar in TV Settings, Back from Live, the Live reading rate, Search before Quick settings, the avatar opening Who's watching) that is in the other session's uncommitted working tree and not in this one. That tree's build was on the TV until this deploy replaced it. These were not run against that build, so that they pass there is not confirmed.
- **Late frames, E2E-030's navigation script, 17 runs of each client interleaved:** the client from before this work and this one are not distinguishable: p95 medians 17.9 to 19.3 ms and 17.7 to 18.2 ms; frames over 33 ms per 17.5 s run, 3.9 on average and 4.5; one run of each had a gap over 100 ms.
- **15-minute soak of the deployed daemon**, one sample a minute while the E2E cases were running: the same process throughout, 59 threads, 32 to 33 open files, 6 `luna-send` children, no restart. Resident memory rose from 13.6 MB to 25.2 MB over the first 10 minutes, with the E2E cases exercising the API, then stayed at 24.8 to 25.2 MB for the last five samples. That fits caches filling, but 15 minutes cannot exclude a slow leak; a 24-hour run of `scripts/tv-memory-soak.sh` (one sample a minute, read-only) was started detached at 22:45 UTC on 30 October and writes to `~/.cache/nativetv/soak-b3-20261030.log`; `scripts/tv-memory-soak.sh --summary` on that file prints memory, thread, open-file and restart figures at any point. Its result is not in these pages. The 35 minutes after the soak above held resident memory at 24,760 kB.

## Not changed, and why

| Idea | Why it was left |
|---|---|
| Lower the priority of decode and HTTP threads | No contention shows: during Home's first load on the B3, with the threads at `nice -10`, the 58 full-rate frames after start-up had p95 18.0 ms and a maximum of 26.1 ms |
| Skip a Home reply identical to the last | On the B3, 774 ambient frames over 36 s showed one refresh-time frame (a 10 s refresh: build 11.8 ms, interval 44 ms against 34.7), and the refreshes at 20 s and 30 s showed none; a catalogue reply also starts the "now playing" read, so skipping it is not free of risk |
| Cache compiled shader programs (`glProgramBinary`) | Would save most of the 142 ms, but needs ES 3 or extension detection, a cache location the jail can write, and invalidation; the other TVs cannot be tested |
| Draw the glyph prewarm after the first swap | Moves 86 ms from before the first frame to after it; the first frames would then pay for glyphs on demand |
| Reuse the static layers' geometry between ambient frames | The largest remaining client idle cost (see the audit), but a larger change to the batcher |
| Wake the loop when a reply arrives, then raise the 50 ms idle wait | Small on a static screen (1.6 % of a core, 3 wakeups a second on the host); the longer wait needs every screen's timers to report a next deadline |
| `tcp_nodelay` on the server | No stall measured (see the audit) |
| Pause the hero's Ken Burns after the idle dim | Changes what is on screen |
| `--gc-sections`, strip, LTO | Size only; not measured on the TV |
| MQTT loop, `signalPoll`, TV page cache, history totals, Luna queue cap | Not exercised on the test TV (MQTT is off); each needs its own measurement |
