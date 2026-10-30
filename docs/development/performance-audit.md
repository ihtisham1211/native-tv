# Performance audit

Where the TV client (`nativetv-tv`) and the server (`nativetv-daemon`) spend time and wakeups, as of 30 October 2026. The numbers behind it are in [the baseline](performance-baseline.md), what was changed in [the optimizations](performance-optimizations.md), and how to repeat the measurements in [the benchmarks](performance-benchmarks.md).

Each finding carries how it is known: **measured** (a number from a run), **read** (checked in the source by the author) or **audit** (reported by a read-only pass over the code and not run). Audit findings are leads, not results.

## How the parts interact

- The client is one thread that handles input, builds the scene and submits GL (`src/main.cpp`). It waits in `SDL_WaitEventTimeout` when nothing is animating, for at most 50 ms (`kIdleWaitMs`), and draws ambient animation at 30 fps. The swap interval is fixed at 1, because changing it cost the first frame after the change two display periods.
- Replies from the server arrive on three libcurl threads (one for API calls, two for pictures, `src/adapters/http_services.cpp`) and wait in a queue. The loop drains the queue once per iteration; the worker does not wake it, so a reply that lands while nothing animates waits up to 50 ms for the next iteration (read).
- Pictures are decoded on worker threads and uploaded on the render thread, at most `Renderer::uploadBudget()` (about 1 MB) per frame, into an LRU cache of 40 MB (`src/graphics/image_loader.cpp`).
- The server is one event-loop thread, a pool of civetweb workers for HTTP, and a runner thread for every resident `luna-send -i` subscription child. Luna calls are `luna-send` children started with `posix_spawn`, at least 50 ms apart (150 ms during the first 30 s after start, `src/daemon/child_process.hpp`).
- The client reads the server over HTTP on loopback. Home re-reads its data every 10 s and posts a beat every 5 s.

## Already in place

These were checked and are not repeated as findings: idle waiting on events, 30 fps ambient pacing, an upload budget per frame, off-thread decode, an orphaned streaming vertex buffer, `glUseProgram` guarded against repeats, no `glGet*`, `glFinish` or `glReadPixels` in the normal frame, culling of off-screen rows and cards, a deterministic fixed clock (`--time`), and per-frame statistics with percentiles (`--perf`). Frame intervals on the B3 are in [the TV app page](../apps/tv-app.md).

## Findings

Ranked by expected benefit for the effort and risk.

| # | Finding | Where | Basis | State |
|---|---|---|---|---|
| 1 | Every resident `luna-send -i` child polls its pipes every 50 ms: about 20 wakeups a second each, six to eight children | `src/daemon/child_process.cpp` | measured on the B3 | fixed; whole daemon 142 to 35 wakeups a second at idle |
| 2 | The remote-key watcher loads two JSON files every 200 ms; the NTP client reads and parses `config.json` every second; neither checks whether the file changed | `remote_button_log_watcher.cpp`, `ntp_client.cpp` | read | fixed |
| 3 | The picture cache's LRU clock ticks once per loop iteration, idle wakeups included, so pictures on screen become evictable after about 100 ms of idling | `src/graphics/image_loader.cpp` | read | fixed |
| 4 | The vertex shader was compiled once per program, eight times, at start-up: 41 to 46 ms on the B3 | `src/graphics/renderer.cpp` | measured on the B3 | fixed (first swap 359 to 306 ms) |
| 5 | A Home ambient frame costs 3.7 ms of CPU on the B3, about 10.6 % of a core at 29 fps; gradients are 36 % of the build time, text 22 % | `batch.cpp`, `home_view.cpp` | measured on the B3 | open |
| 6 | The pause between ambient frames ended at any event, so a stream of mouse-motion or window events (the window manager on the host; a Magic Remote pointer is expected to do the same) made 30 fps animation draw at the event rate: in 19 of 40 host runs the frames came faster than 30 fps | `src/main.cpp` | measured on the host | fixed |
| 7 | There was no input-latency figure: `--perf` logged frame intervals only | `src/main.cpp`, `frame_stats.cpp` | read | added |
| 8 | The decode and HTTP threads inherit the main thread's `nice -10`, so they compete with the render thread at its priority | `src/main.cpp` (`setpriority`) | audit; measured on the B3: no effect on frame cadence | dropped |
| 9 | A server reply waits up to 50 ms for the loop to notice it when nothing animates | `src/main.cpp`, `http_services.cpp` | read | open; small (see below) |
| 10 | Opening Recents draws the whole Home under a 0.97 veil | `application.cpp`, `recents_view.cpp` | audit | open |
| 11 | Icons and glyphs not covered by the startup prewarm are rasterised on the render thread on first use; the prewarm covers the home and apps galleries only | `main.cpp`, `icon_atlas.cpp` | audit | open |
| 12 | Home re-parses and rebuilds its models every 10 s even when the reply is unchanged | `data_controller.cpp` | audit | open |
| 13 | Per-frame string, JSON and vector copies in the Home view; `Scene` builders copy nodes instead of moving them | `home_view.cpp`, `scene.cpp` | audit, unmeasured | open |
| 14 | `Batcher::place` is quadratic in batches per primitive | `batch.cpp` | audit, suspected | open |
| 15 | Source pictures up to 64 Mpx are accepted; decoded pixels in flight are not counted against the 40 MB budget | `image_loader.cpp` | audit | open |
| 16 | An EGL context loss is not handled; the renderer holds raw GL names | `main.cpp`, `renderer.cpp` | audit | open |
| 17 | With MQTT on, one telemetry collection starts roughly 15 to 20 `luna-send` children, about 30 to 45 a minute with the other timers | `stats_collector.cpp`, `mqtt_telemetry_publisher.cpp` | audit, estimated from cache lifetimes | open |
| 18 | The MQTT client loop wakes about 13 times a second; `signalPoll` wakes every 250 ms | `mqtt_client.cpp`, `daemon/main.cpp` | audit | open |
| 19 | The TV pages are rebuilt on every request (file reads, `std::regex`, a hash of the body); only the phone dashboard is cached | `http_static_assets.cpp` | audit | open |
| 20 | `collectLgSettingsData` and `oledStats` hold their mutexes across Luna calls of up to 5 s, so setting writes wait behind them | `http_frontdoor.cpp` | audit | open |
| 21 | No phase timestamps existed for client or server start, so start-up cost could not be attributed | `main.cpp`, `daemon/main.cpp` | audit | added; the client's are read on the B3 |
| 22 | The first Home catalogue request starts serial `luna-send` children 150 ms apart; 8 shader programs and about 1,235 single-glyph uploads run before the first frame; on the B3 the glyph prewarm takes 86 ms and the 8 programs 142 to 155 ms (10 ms and not timed on the host) | `http_frontdoor.cpp`, `renderer.cpp`, `main.cpp` | audit | open |
| 23 | Info log lines go through block-buffered `std::cout` and only warnings (unbuffered `cerr`) and debug lines are flushed, so on the TV they reach `tvweb.log` when the buffer fills or the daemon stops, and are lost if the watchdog kills it: the log held the stopped daemon's info lines, flushed at its exit, and none from the running one | `src/daemon/logger.cpp` (`defaultSink`) | read, and the TV's `tvweb.log` | open; flushing every line costs a write each, so a flush after the start-up milestones is the cheap fix |
| 24 | ARM builds use `-O2`, a static libstdc++ and `--as-needed`; there is no section GC, LTO or strip step | `CMakeLists.txt`, SDK toolchain file | read | open; size only |

Finding 9 is small. The client costs about 1.6 % of a core and 3 wakeups a second on a screen that never animates (Apple M1 Pro; the TV was not measured), and optimistic updates hide most reply waits. Waking the loop from the worker is a few lines, but it only pays once the 50 ms idle wait can also be raised, and that needs a next-deadline answer from every screen's timers.

## Checked and ruled out

- **Nagle on the server's HTTP responses.** civetweb is built without `tcp_nodelay` and writes headers and body separately, which can stall a keep-alive response by about 40 ms. On the B3, 12 requests for `/api/caps` over one connection took 0 to 1 ms each after the first, so no stall shows and the option is left alone.
- **GL state churn.** The audit found no `glGet*`, `glFinish` or `glReadPixels` in the normal frame, and a guarded program switch.

## Not measurable with the access available

button-to-photon latency (needs an external camera), power-on to launcher time, a 24-hour soak, the client's CPU on the TV, and behaviour under memory pressure.
