# Performance benchmarks

How to repeat the measurements in [the baseline](performance-baseline.md), what each scenario covers, and what is still missing. Results are in the baseline and [the optimizations](performance-optimizations.md); this page holds the method.

## On the host

```
python3 native-tv/build.py --out build-host --target nativetv-tv
python3 native-tv/tests/perf_bench.py build-host/nativetv-tv --runs 3 --json bench.json
```

`perf_bench.py` serves `tests/home_fixture.py`, runs the client in real time (not the fixed `--time` clock) for each scenario `--runs` times, and prints the median of each column as a Markdown table; `--scenario NAME` repeats a single scenario and `--json` keeps every run. Frame statistics come from `--perf`; CPU, peak memory and context switches come from the kernel's accounting of the client process alone (`wait4`). Compare two builds by running the script on each binary in turn on the same machine. Debug builds are not representative.

The CPU column of a 10 s run includes about 0.3 to 0.4 s of start-up. For a steady state, subtract a short run from a long one (the baseline did this with 4 s and 14 s runs).

## On the TV

- **A client build that is not installed.** `NATIVETV_SSH_KEY=… NATIVETV_KNOWN_HOSTS=… scripts/tv-client-run.sh <tv-ip> build-arm/nativetv-tv --ms 5000` copies the binary to `/tmp/nr` beside symlinks to the installed app's libraries and assets, runs it under the same `jailer` command as the E2E cases, prints the `perf:` line and the `--report` JSON (including `startup`), and deletes everything. Nothing installed is replaced and no setting changes, but the client is on screen for the run and a running Home client must be stopped first. `--env NATIVETV_DEBUG_SKIP=gradients` passes an environment variable. To compare two builds, alternate them at least eight times each: the TV's load moves the figures more than most changes do.
- **Client frames and input from the E2E cases.** The commands in `tests/e2e/cases.json` run the installed client under `jailer` with `--screen`, `--script`, `--duration-ms` and `--perf`; the log and its summary record come back as on the host. The Home takeover must be off for the run, because a running Home client blocks it.
- **Server wakeups.** `NATIVETV_SSH_KEY=… NATIVETV_KNOWN_HOSTS=… scripts/tv-wakeups.sh <tv-ip> --seconds 20` prints context switches a second for each thread of the daemon and the total. Run it with every dashboard closed; an open dashboard shows up as busy HTTP workers.
- **Server CPU, memory and child processes.** `scripts/footprint.sh <tv-ip> --seconds 60`.
- **Availability over hours.** `scripts/soak.js`.
- **Before and after a daemon change.** Take `tv-wakeups.sh` before deploying, deploy with `native-tv/lifecycle/deploy.sh`, wait a minute for the start-up spacing (150 ms between child starts in the first 30 s) to end, and take it again. The daemon on the shared test TV is replaced by a deploy, so check first who else is using it.

## Scenarios

| Scenario | Covered by | Notes |
|---|---|---|
| A. Launcher idle | `idle-home`, `idle-dashboard` | Home keeps drawing at 30 fps (Ken Burns); the dashboard screen does not |
| B. Rapid remote navigation | `nav-rapid` | A key every 70 ms, left and right |
| C. Opening and closing menus | `quick-drawer` | The Quick drawer opened, moved in and closed |
| D. Scrolling through apps | `apps-scroll` | The Apps row entered, then 14 right and 6 left |
| E. Complex screens | `NATIVETV_DEBUG_BENCH`, `tests/raster.cpp` | Milliseconds per frame under each draw switch; see [testing](testing.md) |
| F. Switching focus | `nav-rows` | Up and down between rows |
| G, H. Launching an app, returning | not scripted | Needs a real TV; the E2E cases launch apps |
| I. Dashboard interactions | not scripted | The E2E cases on a TV exercise the dashboard |
| J. Repeated launcher activation | not scripted | |
| K. Long idle | `scripts/soak.js`, `scripts/footprint.sh` | Not run for 24 hours |
| L. Memory pressure | not run | |

## Limits of the method

- The host figures say how one build compares with another on one machine; the host display runs at 120 Hz and its GPU is not the TV's.
- Input→swap ends when the swap call returns, before scan-out. Button-to-photon latency needs an external camera or photodiode and was not measured.
- GPU time is not in the log by default. `NATIVETV_DEBUG_SKIP=finish` adds a `glFinish` after the draws so that `submitMs` becomes GPU time, which slows the frame; the baseline's GPU figure is from a pair of runs with and without it.
- `perf_bench.py` has no pass or fail limit. The E2E cases on a TV check frame intervals against limits.
- Thread names in the daemon are all `nativetv-daemon`, so `tv-wakeups.sh` identifies a thread by its id and rate only.

## Remaining performance issues

The open items in [the audit](performance-audit.md), in this order of value for the effort:

1. Cut the Home ambient frame's CPU: 3.7 ms on the B3, about 10.6 % of a core while Home sits still, of which gradients are a third of the build time. Reusing the static layers' geometry between ambient frames is the way, and the `NATIVETV_DEBUG_SKIP` table in [the baseline](performance-baseline.md) is the measure.
2. Shader start-up: 142 to 155 ms went on eight programs before the vertex-shader change, and about 100 ms of fragment compile and link remain. A program-binary cache would remove most of it; see [the optimizations](performance-optimizations.md) for why it was not done.
3. Read the daemon's `startup:` log lines on the TV, and time the first reply from the server, which the client does not log yet.
4. Warm the Home catalogue snapshots when the server starts instead of on the first client request.
5. Skip the Home layers under a fully open Recents veil, and extend the start-up prewarm to the settings, live and setup galleries.
6. Compare each Home reply with the last before parsing and rebuilding (the catalogue reply also triggers the "now playing" read, so it cannot simply be skipped).
7. Daemon idle timers that remain: the MQTT loop (13 wakeups a second with MQTT on), `signalPoll` (4 a second), and the Luna requests the telemetry timer starts.
