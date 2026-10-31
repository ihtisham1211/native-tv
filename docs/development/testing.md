# Testing and CI

Every pull request is checked on [GitHub Actions](https://github.com/ihtisham1211/native-tv/actions) before it is merged. Most of
the checks exist because a mistake of that kind once reached a release, or
could have without anyone noticing: the TV gives little feedback when
something is wrong, and Home Assistant and the TV's own browser fail quietly.

## On every pull request

[`checks.yml`](https://github.com/ihtisham1211/native-tv/blob/main/.github/workflows/checks.yml) runs these on each pull request and each merge to main. [`native-tv.yml`](https://github.com/ihtisham1211/native-tv/blob/main/.github/workflows/native-tv.yml) builds the daemon and its worker for the host with address and undefined-behaviour sanitizers and runs CTest when native sources change.

| Check | What it catches |
| :--- | :--- |
| `check-es5.py` | ES6 syntax, or a call or CSS the TV browser lacks, in the pages the TV shows (`native-tv/assets/google-tv/`, the Home app). The phone dashboard is not restricted. |
| `check-ui-ids.py` | An element the dashboard's script reaches for that the page no longer has. |
| `check-strings.py` | Dashboard text without a key, and one key used for two different texts. [Strings](strings.md) has the rules. |
| `check-screensavers.py` | Screen saver QML that the TV's QtQuick version cannot load. |
| `check-drift.py` | The [entity reference](../integrations/home-assistant-entities.md) out of step with the entities the daemon publishes, a file missing from `native-tv/lifecycle/deploy.sh`'s list, which would never be installed, and an asset the TV client names (`local:...`, the Google Sans Flex and Roboto fonts) that `tools/runtime_payload.py` does not stage for it. |
| `check-entity-stability.py` | A Home Assistant entity from the last release that has gone, been renamed or changed domain, which leaves it unavailable and stops every automation that used it. A deliberate removal is listed in `scripts/retired-entities.txt` with its reason. |
| `shellcheck` | Mistakes in `deploy.sh`, `tvwebctl` and the boot script in `native-tv/lifecycle/`. |
| `check-entities.py` | A Home Assistant entity reading a field the TV's telemetry does not have, checked against a B8's real telemetry. |
| `check-retired-files.py` | A file the last Node release installed that `deploy.sh` no longer ships and `retired_files.cpp` does not delete at start, which an upgrade would leave on the TV for good. The release's file list is `scripts/last-node-release-files.txt`. |
| `check-route-manifest.js`, `check-test-matrix.js` | A route in `native-tv/contract/routes.json` the daemon does not answer or answers without its guards, and a legacy suite that `native-tv/contract/test-migration.json` does not account for. Run by `native-tv.yml`. |
| CTest: `native-csrf-policy`, the UI suites | A mutating route without its CSRF defences; the dashboard's scripts and the boot hook's launch decision, run on the host (`native-tv/tests/ui/`). |
| Package build | The Homebrew Channel `.ipk` still builds, and passes the Homebrew Channel's own compatibility check for webOS 4 and later. |
| Native tests | Each part of the daemon, and the daemon as a whole against a stand-in `luna-send` and MQTT broker (`native-tv/tests/daemon_runtime.py`), with a TV that answers, one that answers late, wrongly or not at all, MQTT disabled and a Native Tv Home file missing, checking that the dashboard keeps answering and telemetry keeps flowing. |

## Running the suites

From the repository root:

```bash
python3 native-tv/build.py --service-only --test
python3 native-tv/build.py --service-only --test --sanitize --out native-tv/build-host-ninja-asan
```

`--test-filter REGEX` limits CTest to matching names and `--target NAME` builds
only that target, so a change to one module builds and runs only what it
touches. A separate `--out` directory is needed for `--sanitize`, since the
compiler flags differ.

The tests need no TV. `native-tv/tests/daemon_runtime.py` starts the real
daemon on a fixture root with `native-fake-luna-send` as its `luna-send`
(`NATIVETV_FAKE_LUNA_MODE` picks a scenario, `NATIVETV_FAKE_LUNA_DETAILS`
logs each call) and drives it over HTTP. Real captured TV data lives in
`native-tv/tests/fixtures/tv/`: stats from a B8 (webOS 4), a CX (webOS 5), a G4
and a C4 (webOS 9), and receiver and settings dumps. Expected values recorded
from the former Node server are in `native-tv/tests/fixtures/*.json`.

`node native-tv/tests/ui/run.js [filter]` runs the UI suites.

## Native TV checks

[`native-tv.yml`](https://github.com/ihtisham1211/native-tv/blob/main/.github/workflows/native-tv.yml) builds the C++ daemon
with address/undefined-behavior sanitizers and runs CTest when native sources
change. `python3 native-tv/build.py --test` reuses the host build and runs
the unit suites, the daemon runtime suite and the contract checks. Native
tests live under `native-tv/tests/`; they require no running server or TV.
Compilation is measured separately from test runtime.

The legacy `test/` suites that tested the Node release are gone.
`native-tv/contract/test-migration.json` records, for each of them, the native
test that took it over or the reason it was retired, and `check-test-matrix.js`
keeps that record whole. Where this page says a native test "ports"
`test-xxx.js`, that is the suite it replaced.

`native-tv/tests/contract/node-free-probe.py BUILD/nativetv-daemon` starts the daemon with no upstream
and requests every route in `native-tv/contract/routes.json`. It lists the method/path pairs that still
answer 502, which must stay empty. Routes that outlast the probe's timeout on the fixture bus are listed
separately. `--flags=STATS,INSTALL,...` adds `TVWEB_NATIVE_*` switches; `--fail-on-node` makes a non-empty
list a failure, which `native-probe-node-free` uses. The icon listener (main port plus two, `iconServer`
on) is asked on its own port.

### The TV client

`nativetv-tv` has three options for tests and for the TV.

- `--probe` opens the same window and GLES 2 context as the client, draws one
  frame, reads a pixel back and prints one JSON line. The exit code says why it
  failed: 10 no display, 11 no window or context, 12 shader or GL error or a
  texture limit under 2048, 13 the pixel read back is wrong (codes in
  `src/platform/probe.hpp`). The install gates the native Home on 0.
- `--time=<ms>` runs on a fixed clock (frame N at N x 1000/60 ms) and stops at
  that time, capturing a PPM with `--capture`. Two runs of the same script give
  identical files. Between frames the loop waits for the replies and pictures the
  last frame asked for, so a screen that reads the daemon (Home, against
  `tests/home_fixture.py`) is drawn the same on every run.
- `--perf=<file>` writes one JSON line per frame (interval, build and submit
  ms, draws, vertices, texture MB, and `inputMs` when the frame shows an input
  event: the time from the event's timestamp to the swap call returning, not
  to the display) and a final summary record, and prints
  `perf: frames=... p50=... p95=... p99=... late=...` at exit. `late` counts
  intervals over 33 ms (over 50 ms in the 30 fps ambient mode). No overlay is
  drawn. The `swap` field is the loop's pace, 1 or 2 (ambient); the swap
  interval itself stays 1, because changing it cost the first frame after
  the change two display periods.
- `--report=<file>` writes a JSON summary at exit: renderer, GL version, frame
  count, texture bytes, the screens shown, checkpoints, and `startup`, the
  milliseconds from the start of `main` to the window, the renderer, the atlases,
  the prewarm and the first swap.
- `NATIVETV_DEBUG_SKIP=a,b,...` (environment, read once) leaves one kind of
  drawing out, to find what a frame's time goes on: `shadows`, `rings`,
  `images`, `text`, `icons`, `gradients`, `fills`, and `wallimg`, `wallgrad`,
  `wallfill` (or `wall`) for the same kinds when they cover most of the screen.
  Other switches change how it draws: `nocore` draws every shape whole on the
  distance field, `flat` drops the field from the shaders, `noblend`, `half` and
  `quarter` (a smaller viewport of the same swap), `finish` (a `glFinish` after
  the draws, so the perf log's `submitMs` is GPU time) and `trace` (prints the
  phases of any submit over 8 ms).
  `NATIVETV_DEBUG_BENCH=<ms>` draws the frame on screen at that time again and
  again under each of those switches, then synthetic full-screen layers and a chain
  of draws, and prints the milliseconds per frame to stderr; the screen script puts
  the client in the state to measure.

`tests/perf_bench.py` is not a CI test: it runs the client through fixed
scenarios (idle, rapid navigation, the Quick drawer, the Apps row) against the
Home fixture and prints frame times, input-to-swap time, CPU, memory and
context switches as medians over several runs, for comparing two builds on one
machine ([the benchmarks](performance-benchmarks.md)).

`native-client-platform` (`tests/client_platform.py`) runs the binary for the
probe exit codes, capture determinism, the perf log, the 30 fps ambient pacing
and SIGTERM. `native-frame-loop` covers the statistics and the clock.
`tests/golden/golden.py check` renders the screens in `tests/golden/cases.json`
with `--time` and compares each with its PNG, per pixel with a tolerance,
writing `<name>.actual.png` and `<name>.diff.png` when one fails;
`golden.py update` rewrites the PNGs after an intended change. A case may name
`"fixture": "home"`, which serves the stand-in daemon in `tests/home_fixture.py`
(an invented catalogue, feed and generated artwork), and a `"script"` of timed
keys; the Home and Apps cases catch the page at rest, mid-entrance, with the
pill stretched, after a row snap and with the recent apps open. The Quick drawer
(mid-slide, at rest, a list of choices, the switches), Profiles (`"profiles": 3`
gives the fixture three profiles), the profile editor, title details with and
without the overview sheet and the search with results each have a case; the
fixture also answers stats, details and search. It can also run alone
(`home_fixture.py [--art DIR]`) to point a hand-run client at it.
`"fixture": "dashboard"` serves `tests/dashboard_fixture.py` instead: a real C4's
stats (`tests/fixtures/tv/stats-c4-webos9.json`), the ten dashboard sections' replies
and the Homebrew catalogue with a small install state machine, all from
`tests/fixtures/tv/dashboard/`. The Live, Settings and Catalog cases use it
(`live-*`, `settings-*`, `catalog-*`; `settings-subsections` and `settings-follows` show the
subsection pills and the page following the list), and it also runs alone for a hand-run client;
`NATIVETV_FIXTURE_DELAY_MS` makes its sections answer late, to see the loading indicator with
`--duration-ms` (a `--time` run waits for each reply before it draws).
The Setup cases use
`"fixture": "setup"` (the first-run routes of `tests/setup_fixture.py` over a wiped data root, so the client opens setup) or `"setup-settled"` (a TV that has done it, so `--screen setup:handoff` stays put); the fixture can run alone
(`setup_fixture.py ROOT [--no-standby] [--demo]`). `setup-pin` and `setup-pin-own` are the admin PIN
step's two choices and its digit pad; `pin-prompt` is the PIN screen (`--screen pin`, two digits typed) over the
settled fixture. The tolerance
absorbs the anti-aliasing difference between GPUs (a Mac and Mesa's llvmpipe),
so a case that must match exactly on one machine sets `tolerance` and `maxBad`
itself. The tests that need a GL context skip where there is none (a CI runner
without Mesa); `NATIVETV_REQUIRE_GL=1` makes that a failure. On Linux without a
display they use SDL's `offscreen` driver, which needs Mesa's EGL
(`libegl1`, `libgl1-mesa-dri`).

`native-dashboard` (`tests/dashboard_screens.cpp`) covers Live, Settings and
Catalog. Its model checks snapshot what the screens would show of the real stats of
four TVs (`tests/fixtures/tv/stats-*.json`) and of every dashboard section's
rows (text files in `tests/fixtures/tv/dashboard/snapshots/`; change one on
purpose and rewrite them with `NATIVETV_UPDATE_SNAPSHOTS=1`). Its screen checks
drive the application over a fake `Services`: focus paths, the writes OK issues and
the routes they use, the second OK a risky change waits for, an install from the
catalog to its result (an install the server refuses for want of the admin PIN opens the PIN screen over the catalog, and an unlock returns to it), and what each screen does when a read fails. Settings' two
panes are checked key by key (Right and OK into the page, Left and Back out, Left
and Right never changing the section, the subsection pills and the one a section
reopens on). Every write kind (a switch, a stepper, a confirmed action, the network
switch, the screen saver's delayed start, the catalog's uninstall and install) is
checked for the visible state changing in the next frame with no reply, and for a
refusal putting it back with a message. A refresh while the focus is on the last
row, also among rows with the same title, must keep the focus, the subsection and
the scroll. `native-kit` also checks that the loading indicator stays under forty
nodes and turns.

`native-overlay-screens` (`tests/overlay_screens.cpp`) drives the Quick drawer,
Profiles, title details and the search and profile editors the same way: focus
paths, the requests each makes and the replies it takes (a refused write is taken
back and says why, a reply to an older request is dropped), the profile avatars and
their names, a rename shown at once and put back when refused, the eight-deep stack
of related titles, and Back returning to Home as it was. It shares its fake `Services`
and clock with `native-quick-drawer` in `tests/overlay_harness.hpp`.

`native-quick-drawer` (`tests/quick_drawer.cpp`) drives the Quick drawer through the
application: the first opening shows turning circles where nothing has been read, the
next opening shows last time's values before any reply and reads behind them, a held
volume or light key (5 to 95 and 95 to 5, replies late and out of order, stale reads
between) shows every value at once and never moves it against the key, the TV's cap wins
after the one read-back, a refusal or silence puts the value back with a message (also
after the drawer has gone), the choices (picture, sound, sleep) and the Home switches
show at once with the newest winning, and Back, a click beside the drawer and a route to
Home all leave it. `tests/home_fixture.py` keeps what `/api/control` is told, so a read
after a write says what was written.

`native-navigation-walk` (`tests/navigation_walk.cpp`) walks the remote through every screen with
seeded random keys, OK held, Back held, pointer clicks on the scene's hits, catalogue changes under the
page and replies that come late, out of order, fail or never come (300 seeds of 80 steps; `NAVWALK_SEED=n`
replays one seed with its whole trail, `NAVWALK_SEEDS` and `NAVWALK_STEPS` widen a search). After every
step the screen is a known one, every focus index is inside what exists, and at rest the focused thing has
a hit on the canvas. From any state a few taps of Back reach the first Home tab without passing through
the recent apps, and a tap of Back on the top bar changes nothing; Back from a drawer, Search, Profiles
or Details gives Home back with the focus where it was; a key on a dimmed screen only wakes it. A failure
prints the seed, the step and the trail; `kKnown` in the file lists failures that wait for a fix.

`native-optimistic` (`tests/optimistic.cpp`) holds the optimistic-update layer every page
writes through (`ui/optimistic.hpp`) to its rules, over a fake `Services` whose replies the test
holds back, reorders and refuses and a clock the test moves: a changed value shows at once with
no reply at all, a held key (5 to 95 and 95 to 5, many writes, replies out of order, stale
reads in between) never moves the shown value backwards, writes leave at most every 120 ms
and the last always goes, a refusal and eight seconds of silence put the value back with a
message, a read during a pending write does not overwrite it and the one read-back afterwards
decides, volume steps are sent as a net sum one at a time, batches and dependent rows follow
what is shown. `native-optimistic-guard` (`tests/optimistic_guard.py`) fails when a page in
`src/ui` calls the raw `services().write` instead of `host.optimistic()`; the pages that
still do are listed in `tests/optimistic_guard.json`, and the counts only go down.

The client's parts have their own tests. `native-motion` checks the springs
against a reference integrator, velocity kept on retarget, the bezier curves and
the stagger on a fake clock. `native-kit` holds every component's geometry to the
numbers in `native-tv/design/metrics.json`, and `native-kit-tokens` fails when
`src/ui/kit/tokens.hpp` is stale against `native-tv/design/tokens.json`
(`tools/gen_tokens.py` regenerates it). `native-raster` renders the SDF fill, ring,
shadow, gradient, clip and a Home-shaped frame on the CPU and compares them with
the PPMs in `tests/fixtures/raster/` (`NATIVETV_UPDATE_GOLDEN=1` rewrites them
after an intended change), and checks that a shape drawn as a flat core inside a
distance-field rim paints what one field quad over all of it does, and that the
field stays on rims; `native-renderer-gl` draws the same scenes on the GPU
and compares them with that raster, and is skipped (exit 77) with no GL context.
`native-tv-name` (`tests/tv_name.cpp`) names the TV from its model (`OLED65B3PUA` is `B3`,
`OLED42C24LA` `C2`, `55UQ8000PUA` `UQ8000`, nothing known `LGTV`), over the model of every
`stats-*.json` fixture. `native-setup` (`tests/setup_screen.cpp`) drives first-run setup the way a person does,
with scripted keys and the server's answers handed back by the test: the whole path on a TV with
and without Always-on, the name step with its optimistic write and refusal (the answers to the layer's
writes come on `w:<key>` with the sequence it was given), the admin PIN step (the default in one press,
four digits typed on the digit pad, Done refused with fewer, the server's refusal, the session handed to
the services, the steps and dots counted without the step on a TV that has a PIN, and not recounted
when the restarted server answers `adminPin: true`), the PIN prompt that any 403 with `pin: true`
opens (an unrelated 403 does not; once at a time; a wrong PIN, a lock-out, a server that does not
answer, a PIN removed meanwhile; Back cancels; the unlock returns to where the prompt came from and
the refused action is not repeated; never over the first-run pages, but over Settings' phone-only
entry), the restart after the network choice and the trouble page after 60 failed
tries, the phone link refused, expired, never connecting and connecting, Back and Left, Settings'
phone-only entry, the dry run, typing the broker on the TV (and its refusals), the pointer, the
keyboard pad (layers, capitals, caret, limits, spring press) and the QR layout (whole-pixel
modules inside the tile). `native-setup-flow` (`tests/setup_flow.py` with `setup_flow.cpp`) runs the
same path with the real HTTP adapter against `tests/setup_fixture.py` and checks what the server
wrote to its wiped data root and in what order. The stand-in guards `POST /api/settings` with the
session the PIN step answers, as the daemon does, so the broker reaching it shows that the adapter sent
`X-Admin-Session`. It also launches the client as the tile, Home and
Settings do on that wiped root (they must open the welcome page, and Settings' own `setup:handoff`
entry must stay put) and again once setup is done (they must open what they were asked for).
`native-setup-daemon` (`tests/setup_daemon.py`) runs the same first run against the real
`nativetv-daemon` without Node, over a wiped data root and with its stand-in luna-send, as far as
the broker typed on the TV (written under the session of the default PIN the run chose), and reads back
what the daemon wrote (`config.json`, the main profile in `home.json`, `admin-pin.json`, the restarts it
asked for), then marks setup done and launches again. `native-qr-capture` (`tests/qr_capture.py`)
captures the handoff page and compares every module of the code on it with qrcodegen's matrix
(no QR decoder is installed, so the pixels are read instead); it skips with no GL context.
`native-atlas` covers glyph packing, text measuring, ellipsis and wrapping,
`native-image-loader` the off-thread decode, the cache limit and the upload
budget, and `native-woff-to-ttf` that a WOFF packed from each bundled Google Sans Flex TTF unpacks
to the same bytes.

The daemon's answers were held to the Node release's by `parity.js`, which ran both on one fixture TV and
compared 45 read-only requests and refusals, the backup restores and `/api/stats` on the B8, C2, C3, G4, C4
and CX profiles; all matched, and the check went with the Node release. What it compared lives on as
recorded goldens (`ha-golden.json`, `prometheus-golden.json`, `mqtt-publish.json`, `stats-fields.json`).

`native-mqtt-publish` holds the daemon's telemetry payload (`daemon/mqtt_publish_payload`: the whole stats read
with LG's settings, the switched-off flag, the watch summary and PicCap's power, the last app standing in while
the TV is off) to what the Node release's `mqtt-state.js` `publishPayload` made of all seven captured `/api/stats` responses in
three states (21 cases, held in `tests/fixtures/mqtt-publish.json`).
`test_native_mqtt_production_uses_live_identity_topics_and_status` also checks the discovery device record carries the
model and firmware the TV reports, that the daemon's media snapshot is no state topic and `<prefix>/update` is cleared,
and that the TV's sensors are offered; `native-stats-collector` holds the device, picture modes and sound outputs
discovery takes from the stats read. It also checks the published telemetry carries what
Home Assistant's entities read and not the apps, temperature history or CPU times, that the device record carries
the network address, that the PicCap switch and its state topic follow PicCap answering, and that
`/_internal/mqtt/forget` (uninstall) clears every retained topic and disconnects cleanly. `native-history` covers
the watch summary.

`--service-only --out native-tv/build-service --test` builds the worker and its
four suites using json-c, threads and Python.
`build.py` auto-detects `ccache` and uses it as the compiler launcher when it
is installed; `--no-ccache` disables it. That reduces clean or header-touching
rebuilds, not CTest runtime. It also generates new build directories with Ninja
when installed, which makes no-op and incremental rebuilds faster; CI installs
`ninja-build` for this.

For a native code change, build only the affected target and run only its
related CTest cases. For example, the managed app install front door can be
checked with:

```sh
python3 native-tv/build.py --service-only \
  --out native-tv/build-host-ninja \
  --target nativetv-daemon \
  --target native-app-catalog-frontdoor-tests \
  --test-filter '^(native-app-catalog-frontdoor|native-daemon-runtime-test_native_apps_uninstall_sideload_and_repo_routes)$' \
  --build-jobs 8 --test-jobs 2
```

The existing Ninja tree rebuilds only stale objects, while the filter prevents
CTest from starting unrelated suites. Use a separate `--out` tree with
`--sanitize` for sanitizer coverage; compiler flags differ, so those objects
need their own build directory.

Metrics fixtures cover page size, names with parentheses, malformed/disappearing
processes, CPU deltas, counter resets and recycled PIDs. `test-native-metrics.js`
checks the transitional adapter's coalescing, fixed arguments, output protocol,
execution limits, explicit errors and failure backoff without launching a TV process.
`system_metrics.py` exercises native memory/swap, temperature sources/units,
clock formats, eMMC regions, CPU modes and boot time, online/present core ranges,
LG load slots, uptime/load-average parsing, interface ranking, MAC validation,
Wi-Fi units, picture-engine window/HDR parsing, Magic Remote pairing facts,
missing/malformed files, bounded reads and real `statvfs` app-storage counters. The shared snapshot fixture also supplies the daemon
runtime tests. CPU window arithmetic and Home Assistant's
publish baseline are covered by the stats collector tests against synthetic native counters. Cold
stats tests hold the native reply, then verify partial data, timeout ownership
and late-reply rejection. `native-tv/tests/stats_collector.cpp` runs the native
`/api/stats` collector against a scripted TV: the always-on, input-key, live-read,
play-state, power, hostname, sound/volume, HDMI, apps and device cases of
`test-telemetry.js` and its sibling suites, the 0.4 s reuse, the early (`fast=1`)
read, and how expire, clear, forget and the volume refusal are taken. `native-tv/tests/retired_files.cpp` runs the start-up removal of files earlier releases
installed (`test-retired.js`'s two cases on temporary folders).

`native-services-keeper` (`tests/services_keeper.cpp`) runs what `services.js` does besides the toggle, on a
fake root with scripted programs: at start, dropping the retired ids, removing the old init script, lifting the
masks, binding the stubs and rewriting the boot hook, and the five-minute stop of a switched-off service found
running (unit, job and the delayed signal). The hook text is held to `services.js`'s own for five lists of
services with every program present (`tests/fixtures/boot-scripts.json`, from `tests/tools/gen-boot-scripts.js`;
run it again after changing the hook). `test_native_services_keeper_runs_at_start_only_when_switched_on` starts
the daemon with and without `TVWEB_NATIVE_SERVICES=1`. `test-services.js` also checks that the native owner leaves
the list, the hook and the enforcement to the daemon. The Alexa key on JavaScript-filter firmware is part of
`test_native_apps_list_projection_and_context_fallback` (saved and written to configd like Netflix, no app block).

`native-mqtt-client` (`tests/mqtt_client.cpp`) also runs `test-mqtt.js`'s cases against a scripted broker, one thread
per connection: CONNACK 4 and 5 are reported as "bad username or password" and "not authorized", a PINGRESP replaces the
connect time as the latency, a broker that never answers a ping is dropped and the client connects again, and a broker
that leaves its end open after DISCONNECT does not hold up the reconnect. `test-mqtt-ca.js` is held by `native-mqtt-tls`
(a custom CA is trusted when given, an untrusted one is refused) and `test-mqtt-remote.js` by
`native-mqtt-command-router` (a remote key asks for no telemetry read, other commands do). `test-power-state.js` is
`native-names` (the full state table: which states have the screen on, and that an unknown or missing state is off) and
`native-mqtt-state` (a screen reported on while the system is off stays OFF). `native-hdmi-inputs` adds an input the map
marks `none` and a TV with no map to `test-hdmi-map.js`'s cases; `native-history` the screen-off and screen-saver
splits of `test-history.js`; `native-mqtt-telemetry` the 1.5 s gap and the coalescing of `test-state-push.js`;
`native-syslog-forwarder` a destination given as IPv6 (skipped where the host has no IPv6 loopback);
`native-devtools-firewall` (`tests/devtools_firewall.cpp`) the twelve firewall cases of `test-devtools.js` with fake
iptables and ip6tables in a fake root; `test_native_game_without_the_service_is_not_asked_again` a TV that has no Game
Optimizer pipeline being asked once. `native-piccap` checks the start-up migration of an old `piccap.enabled`
(`test-piccap-gate.js`: false becomes PicCap unticked in `mqtt.entities.disabled`, the key and an empty section go) and
`test_native_start_migrates_the_old_piccap_enabled_key` that the daemon saves it at start, as Node's migration is gone
with Node.

`native-lg-settings` (`tests/lg_settings.cpp`) ports `test-lgsettings.js`: only the rows the TV
reports, the stored value of a switch, choice or number and which values a row takes, choices and ranges
from the TV's description (numbers kept as text written as text), a value outside the list shown by its
own name, a per-port row giving way to the one setting, the greyable rows, rows outside the table refused,
the sound output names, the value a row shows being the one the TV's dimension names when it differs from the stored one
(the sound output on a speaker, stored as HDMI ARC), and the backup restore of each kind of row. `native-sam-rescan-manager`
(`tests/sam_rescan_manager.cpp`, with `tests/sam_fake_tv.py` as `luna-send` and a stand-in `/bin/sh` in a fake
root) ports `test-samrescan.js`: the blocked-list nudge put back exactly and serialized, a failed restore
retried, one app reread with a block and a release (refused, lost and repeated releases), the waits for sam,
and the restart that follows when sam does not catch up. `native-home-screen-manager`
(`tests/home_screen_manager.cpp`) ports `test-homescreen.js` and `test-homescreen-webos10.js` for what the
manager owns: the webOS 9 parts written as a marked layout, the last part kept, the Native Tv Home
layout left alone, restore removing only what was written here, the empty starting layout, the recorded input
tiles, the webOS 10 overlay behind the experimental switch with its row heights, a reapply after a TV
restart, and the home app found by its arguments on a restart (children of the test). The input tiles'
launch points are the front door's and are covered by the Home screen runtime tests. The lost and dropped
screen saver cases of `test-screensaver-lost.js` and `test-screensaver-dropped.js` are in
`native-screensaver-control`. `launcherNeedsRefresh` (`daemon/apps_state`, in `native-apps-state`) ports
`test-launcher-refresh.js`: sam is read again after an install or uninstall only when tiles are hidden (not on
a Homebrew Channel install) or sam was restarted since start, and never where tile hiding is not allowed; the
front door runs it after both.

`native-launcher-sync` (`tests/launcher_sync.cpp`) ports the port and appinfo halves of `test-dashboard-port.js`:
the `var PORT = n; // tvweb:port` line of each installed launcher page follows the dashboard port, idempotently,
skipping absent or unmarked pages and ports outside 1-65535, `appinfo.json` gets `resolution` 1920x1080, and the
pages that ship (the Home app's and the Homebrew Channel app's; the dashboard tile is a native app with no page)
carry the marker at port 8080; the daemon runs it at every start. `native-tv-app-manager`
(`tests/tv_app_manager.cpp`) runs `TvAppManager` against a stand-in `install-app.sh`: the last line is the answer,
and the start-up refresh covers the tile and then the splash. `native-install-app` (`tests/install_app.py`) runs the
real `install-app.sh` against a stand-in `luna-send` that unpacks the package it builds: a native app is installed
whole (executable bit, fonts, icons, libraries, licences) and not offered without its executable, a refresh leaves a
current app alone, replaces a stale one and waits while the app is open, and a web app keeps its port line. The port refusals are in
`test_native_server_tab_controls`. `native-devtools-firewall` (`tests/devtools_firewall.cpp`, scripted `iptables`/`ip6tables`)
ports `test-devtools.js`: the rule per family, `closed`, `open`, `unsupported` and `off`, the tools found in
`/sbin`, and `allowNetworkDebugger` taking a rule out; the Node bridge cases die with Node.
`native-game-frame-rate` (`tests/game_frame_rate.cpp`, a scripted `luna-send` following feed files) ports
`test-game.js`: the stats' one-shot read cached on the bound pipeline, the Game tab's stream sharing the bind
and serving the stats, a dead pipeline rebound afresh, webOS 5's sublist strings, a bind that never answers,
and a TV without the service not asked again (the 15 s and 300 s timers are not waited out). `native-hdmi-inputs`
reads a C4's and a C9's real receiver status files for `test-hdmi-map.js`: the signal follows the receiver the
map gives each input, the base table, a C9 without a map, an input marked none, and the pixel clocks;
`test_native_hdmi_receiver_map_is_asked_until_configd_answers` that no answer is asked again and a refusal is final.
`native-history` gained the opt-in, hour split, restart and time zone cases of `test-history.js` (titles by
`appTitle` are Node's). `native-hostname-cache` (`tests/hostname_cache.cpp`) ports `test-hostname.js`: a name
or none kept an hour per address, a stale name kept while it is redone, a timeout is none and its late answer
is dropped, a new address asked at once, and the caller never waits. `native-static-assets`
(`tests/http_static_assets.cpp`) ports `test-lazy-ui.js`: the TV pages never load the phone page, which is
assembled once on its first request, compressed, with missing parts reported and not retried.

`native-ha-golden` (`tests/ha_golden.py`) and `native-prometheus-golden` (`tests/prometheus_golden.py`) hold
the entity catalogue, the capability filter, the discovery payloads and the Prometheus text to what `ha.js` and
`prometheus.js` answered when recorded (`tests/fixtures/ha-golden.json`, `prometheus-golden.json`; the recording
went with the Node server), so they need no Node. The Prometheus golden carries the `nativetv_` names. `test-timers.js` is
`test_native_power_timer_writes` (the days bitmask, a timer that is off only stored, one that is on switched off, changed and on
again, the Live TV no-channel switch to Home and its absence on webOS 3-5, bad times and days refused); the stored
settings come from the file in `NATIVETV_FAKE_TIME_SETTINGS_FILE` and a Home app missing from the marker named by
`NATIVETV_FAKE_NO_HOME_APP`. `test-routes.js` is in `test_native_json_body_guards` (content type, origin and JSON
shape before any action), `native-config-settings` (the defaults, an empty form's two errors, a broker address with a scheme, path or space and a device name with `<`, `>` or a control character refused, the icon switch) and `native-config`
(a writer behind a live config lock is waited for, its update kept). `test-apps.js` adds the protected ids
(`native-app-list`; an empty id is protected) and `test_native_tile_hiding_flag_and_hidden_list` (the flag file, a hidden list
with no flag, an explicit 0, protected ids dropped from the list). `test-privacy.js` adds the built-in tables to
`native-adblock` (both address families for every host, the common ad networks, unique hosts under 24 KiB, the
playback and platform hosts left reachable) and the SDP grace period to `native-adblock-manager`
(`tests/adblock_manager.cpp`: a valid clock at restart, a cold boot waiting for SDP, webOS 4 without `timeValid`; the
120 s wait for a time from another source is not run). `test-oled-protections.js` is in `native-oled-care` through
`oledStatsObject`. Nine targets that check with `assert()` are built with `-UNDEBUG`, as a Release build otherwise
compiles every check away.

`native-mqtt-state` also ports `test-window-state.js` (an empty focus that lasts under 200 ms is not
published, whatever other window event arrives meanwhile; standby releases the remote at once; a reconnect
republishes) and the window-less TV of `test-state-push.js` (no `control_app_id`). `native-state-subscriptions`
checks that a window group refused as an unknown method is not asked again and a busy one is, and `native-luna`
that an indented reply split across writes, with a brace inside a string, is read whole. `native-apps-state`
holds the tile-hiding carry-over of `test-tile-hiding-gate.js` (the daemon saves `allowTileHiding` at start
where an install has it running, except on webOS 10 and later without the experimental switch). The runtime
suite adds the version shown with matching build metadata only (`test-version.js`) and a restart that waits for
an install started after the settings save (`test-restart-wait.js`). The debounce and publish-gap timing of
`test-state-push.js` belongs to the Home Assistant publication, which is the daemon's now. The suites
`test-dashboard-nav.js`, `test-dashboard-search.js`, `test-dashboard-view.js`, `test-mqtt-save-ui.js`,
`test-token-url.js`, `test-admin-session.js` and `test-home-boot.js` load the shipped sources (`native-tv/web/ui/`,
`native-tv/assets/google-tv/` and `native-tv/lifecycle/50-tvweb.sh`) and run on the host with
`node native-tv/tests/ui/run.js`. `test-admin-session.js` runs the fetch wrapper of `ui/core.js` against a stand-in network: the admin session goes out with API requests only, a 403 that says `pin:true` raises the unlock prompt and retries once, a cancelled prompt leaves the refusal, and a 401 raises the passcode screen. `test-dashboard-nav.js` holds the phone dashboard's sections and tabs
against `ui.html` (the 13 panels, five sections, the tab buttons the scripts reveal), the Android wrapper's
`navState` call, and the `?theme=` and `data-theme` behaviour. `test-hdmi-grid.js` runs `hdmiTable()` from `ui/lgsettings.js` and holds that no column of the HDMI inputs
table keeps a minimum width: three of them once did (150, 130 and 130 px), which pushed the Audio format column off a 390 px phone.

`native-luna` also covers the urgent call's one extra slot (one key press at a time, ahead of the
normal calls waiting), a child killed before answering run once more and then given up on, a
timeout never retried, and calls logged at the debug level only, against a scripted `luna-send`.
`native-control-command` holds the whole RCU navigation and media key tables. `native-backup` checks
which daily backup is due (named by the UTC date; none while off or before 2026) and that a pruned
or temporary one cannot be read. The runtime suite checks that a sound output change the audio
service never reports is answered after 4 s, and that a refused volume change stops `/api/stats`
offering volume control on that output.

`native-install-jobs` runs the install job coordinator (`TVWEB_NATIVE_INSTALL=1`) against scripted
downloads, inspections, previews and installs: 37 scenarios held in
`tests/fixtures/install-jobs.json` (states, final snapshot and wording), then a held download cancelled, the busy refusal, an expiring preview,
recovery of an interrupted job and failing job-record writes. The runtime suite uploads a package,
previews, confirms with root, installs and cancels a second upload with no request reaching Node's
installer.

`native-home-orchestrator` runs what the Node release's `home.js` still did around the daemon's Home routes
(`TVWEB_NATIVE_HOME_ORCH=1`): the start-up sequence, the wake from standby that opens Native Tv Home
over the input LG put in front, and LG's auto launch following the screen. 43 scenarios held in
`tests/fixtures/home-orchestrator.json`, run on a virtual clock (the calls made to the
TV, in order, and the config left; a switch-off is recorded
as its first call, `release`), then a failing switch-off, a lost inferred save, coalesced syncs, a
stopped orchestrator and a timer firing after its wake was cancelled. The runtime suite checks that the
daemon reads what earlier builds left only with the flag.

`native-home-crash-breaker` checks the count that gives LG's Home back when the native client keeps
failing: three exits within two minutes trip it, two do not, an exit a window old is forgotten, and a
tripped breaker stays tripped (more exits, and time, do not clear it) until it is reset. The same
executable as `native-home-orchestrator` goes on to run the watch itself on the virtual clock: three crashes (the client
gone from the running list as LG's Home comes to the front) and three grace periods with the stub left
in front each give LG's Home back once, clear the Home key's starting notice and put auto launch back; a client sam refuses
(it does not know the app, or would not open it) is given up as the server opens it for the stub, 0.4 s after the stub came, with
no exit counted; the Home key (client still
listed), another app opened from Home, a running list that cannot be read, a client that comes up inside the grace,
standby, a screen that is off, LG's Home opened on purpose, no takeover in place and Home switched off count nothing; a
tripped orchestrator takes nothing again (no wake open, no later crash counts) until `rearm()`; a failed release is
logged; `stop()` leaves no timer; and a probe that refuses at start gives LG's Home the place without switching Home
off, where a webOS version that is not offered still switches it off.

`native-home-probe-gate` runs stand-in clients (shell scripts) through the real child runner: exits 0, 10, 11, 12 and
13 and any other exit are classified, a missing or non-executable file, a hang and a crash are refused, 10 and 11 are
tried again (a display that comes up on the third try passes, one that never does ends on the last answer) and 12 is
not, a verdict that cannot change (ok, 12, 13) is kept in memory and on disk for the same file and firmware and asked
again for another file, firmware or `force`, one that may be timing (10, 11, a timeout) is never kept, what the status
route reports follows the file (a replaced client is not reported as refused), and a refusal gives `supported` the
reason with the exit in it. An app sam does not know is refused ahead of any kept verdict, each time it is asked, and without
a probe run; a missing file is refused before sam is asked. The runtime suite adds `test_native_home_probe_gate` (the dashboard's
switch-on refused with the exit, the status route's `supported` and `nativeClient` (a refusal that may be timing leaves the switch
offered), the switch-on probing again, the boot
hook's `--takeover-home` exiting 4 on a refusal and on no client file, and the kept verdict sparing the boot's probe) and
`test_native_home_crash_breaker` (the daemon against the stand-in `luna-send`: foreground and running lists
scripted from files; the Home key and an app opened from Home counting nothing, three crashes and then three stubs left
in front each giving LG's Home back with Home still on and the notice cleared, nothing taking the place again, switching
Home on again doing so, a restart doing so, and a probe that now refuses giving it back at start; the stub staged
over LG's Home names `com.tvweb.dashboard`, and the retired `com.tvweb.native` counts as nothing) and
`test_native_home_client_registration` (the tile off the home screen: the switch-on refused with the Server tab
named, nothing mounted and no probe run, `--takeover-home` exiting 4, the switch working again once the tile is back;
with Home on, the Home key from an input finding the app refused and LG's Home given back within seconds with no exit
counted, and a restart doing the same; the developer client `com.tvweb.native` deleted at start with its package folder, the
tile's app kept).

`native-retired-files` also checks `removeRetiredApps`: the developer client's two folders go whole and count once,
the tile's app and a missing apps folder are left alone.

The end-to-end cases (`native-tv/tests/e2e`, run against a real TV with `NATIVETV_TV_HOST`) run the client as the tile's
app. Four flow cases (`E2E-080` to `E2E-083`, `requires: ["takeover"]`, so only with `--allow takeover`) switch Home on,
send the Home key from an HDMI input and check the Native Tv app is in front within 5 s and stays, send Back and Home on the
native Home and check it holds, and switch Home off and close the client Home started (one left running refuses every case
that starts one). They run `scripts/home-flow.js` under the TV's Node, which calls the
server on loopback (no token) and reads the app in front from the app manager; the steps are a closed vocabulary that
`run.py --check` enforces, and the flow script itself runs on the host against a stand-in server and `luna-send` in
`test_runner.py`.

`native-tv/tests/tv-metrics.js` runs with the TV's own Node (LG ships one) against its loopback
API and invokes the native worker directly for snapshot-only fields. It checks
system metrics, process responses and the HDR/HDMI capability flags against the
TV filesystem; it also observes the CPU worker and requires concurrent CPU
requests to share one sample. It makes no writes and has a fifteen-second
deadline. A custom port is passed as `node - PORT` when the script is piped over
SSH. The check passed on B3/webOS 10.3.2.
The native metrics screen also rendered the live snapshot at 1920×1080 on that
TV; its capture was reviewed. Seven standalone system reads, including worker
startup, measured 10.3 ms median and 19.0 ms maximum.

`runtime_package.py` covers invalid runtime ABI/dependencies, file hashes,
executable permissions and reproducible archives, and that the client is staged
with the fonts, icons and licences it opens (`CLIENT_ASSETS` in
`tools/runtime_payload.py`), each listed with its hash in the client's
`runtime.json`, in both the dashboard tile's and the splash's folders. Isolated Homebrew installer
fixtures exercise fresh install, upgrade with config/layout preserved,
same-version component repair, newer-runtime preservation and rejection of an
incomplete or absent payload before installed files change. Package fixtures
also require an ARM worker and reject missing builds before deployment.
`scripts/build-ipk.py
--native-build native-tv/build-webos` includes the shared runtime payload;
the packager defaults to `native-tv/build-webos` and fails when it is missing. The TV check
observes the canonical `/var/lib/tvweb/native/nativetv-service` process.

`--sanitize --out native-tv/build-sanitize --test` enables the same sanitizer
checks locally. `tests/smoke/content.json` and `dashboard.json` are bounded,
read-only hardware scripts for `deploy.py --script`. The content script (search, a
result's details, the Quick drawer and its picture list, Profiles, the profile
editor) needs TMDB discovery enabled and one profile; run with `--time` against
`tests/home_fixture.py` and a title from its feed in place of "Stranger Things", it
runs on the host. Hardware captures must also be reviewed:
scene assertions alone cannot detect font clipping or GPU capture faults.
A separately installed ARM IPK also passed normal app-manager launch,
background/resume, close and cold relaunch on OLED65B3PUA/webOS 10.3.2.
Up/Right/OK/Back injected through the TV input service also opened and closed
the native Quick settings drawer; compositor captures verified the result.
The [native README](https://github.com/ihtisham1211/native-tv/blob/main/native-tv/README.md) records tested hardware and
migration limits.

The daemon's remote-key reconcile (`/_internal/remote-keys/apply`, secret-guarded) does the start-up, settings-change, power and half-hourly reapply of the keys, the Home key and the starting message. `test_native_webos11_remote_keys_read_and_write` calls the route (refused without the secret). It also waits past the log
watcher's 200 ms poll after the AI key is restored and checks the LG record has not come back: `JsonFile::load()` restores a
missing file from its `.last-good` copy, so a record deleted on purpose now takes the copy with it.

## End-to-end tests on a TV

`native-tv/tests/e2e/` holds the cases that only a real TV can answer: the client's probe, the
screens on the B3's GPU, frame intervals under scripted keys, and the idle dim. `cases.json` lists
them (id `E2E-nnn`, title, purpose, screen, key script in `scripts/`, duration, the steps and the
pass criteria), and `run.py` (Python's standard library only) runs them over ssh.

```
export NATIVETV_TV_HOST=ADDRESS          # or USER@ADDRESS; the user defaults to root
export NATIVETV_SSH_KEY=PATH             # optional: the private key (default: ssh's own)
export NATIVETV_SSH_KNOWN_HOSTS=PATH     # optional: the known-hosts file (default: ssh's own)
python3 native-tv/tests/e2e/run.py --list
python3 native-tv/tests/e2e/run.py --show E2E-030   # the case and the commands it runs
python3 native-tv/tests/e2e/run.py --case E2E-030 --case E2E-041
python3 native-tv/tests/e2e/run.py --all --report report.md   # .json for the full metrics
```

Without `NATIVETV_TV_HOST` the run options exit 77, so the CTest entry `native-e2e-tv` shows
as skipped and nothing runs in CI; `native-e2e-runner` checks the runner and the registry without
a TV (`run.py --check` does the same by hand). The client on the TV is the one already staged;
`--stage --allow stage` runs `deploy.py` first, which also stages the managed runtime under
`/var/lib/tvweb/native`.

A case runs one client through the app's jailer for at most 60 s (5 minutes for a settings sweep), as `deploy.py --run` does, one
at a time under a lock folder in `/tmp`. The runner reads the server's port from the TV and never
reads or prints the access token. It does not reboot, deploy or restart the daemon or write a TV
setting, and a navigate-only script is refused when it holds OK, Back, text, a click or a capture.
A case that needs the takeover or a reboot lists it under `requires` and runs only with
`--allow takeover`, `--allow reboot` or `--allow control`; the four flow cases (`E2E-080` to `E2E-083`)
require the takeover, since they switch Home on and off, the two Quick cases that hold the volume
or the brightness (`E2E-091`, `E2E-092`) require `control`, since they change the TV's setting and
put it back, and `E2E-123` requires a reboot.

The api cases (kind `api`, from `E2E-100`) are where a bug found by testing a TV goes, so it is checked on a real TV
with every change: GET requests to the server's own routes, made from the TV under its Node like the flow cases (loopback,
no token), with the reply's fields compared (`expect` a value, `same` two paths of one reply; `rows[id=x].value` picks the
array item whose `id` is `x`; `status` is the HTTP status a reply must have, for one that is not JSON). A read-only case
needs no `--allow`. A case that has to change a setting POSTs, which `--check` accepts only with `requires: ["control"]`,
and it puts the setting back (`E2E-101`, `E2E-102`, `E2E-104` to `E2E-119`). The round-trip cases begin by checking the values they expect to start from and
change nothing on a TV that has others (`E2E-119` expects the test B3's own levels for volume and brightness). The dashboard port change, a reboot and a power-off have no case: the first moves the server under the flow, the other two take the TV away. `keepSession` holds the admin session a reply carries and
`withSession` sends it back, which `E2E-104` uses to unlock; `save {name: "path"}` keeps a field of a reply and a body value `"$name"` is that field in
a later step, which `E2E-113` uses for a saved page's id. A path name picks an array item by any value, dots included (`preload[id=com.webos.app.browser].off`). `E2E-103` is a client run, not an api case, with the same permission:
it presses OK on a number row of the Game settings and goes round its range.
`E2E-120` is a client run that changes the server: with the apps sorted by kind, a long press on a row of the Apps tab lifts it, Down
moves it and OK saves the kinds' order, then it is moved back and Back cancels. A run case may carry a `before` flow of api steps
(POSTs need `control`), which the runner sends before the client starts, and a `cleanup` flow, which it sends even when the run
failed, so `E2E-120` starts with the apps by kind and ends with them in Your order and the kinds in their default order. The script's
`longPress` step is the client's hold of OK and `longBack` its hold of Back (650 ms on a real remote), and a Home checkpoint
reports the client's snapshot: `tab`, `row`, `rowGroup` (the kind of the focused row), `arranging`, `editing`, `editHide`,
`focusCard` (the id of the focused card) and `rowHead` (the first three card ids of the focused row). `E2E-178` is a client run
that changes the server too: on the Apps tab a long press starts editing the apps, the Hide from Apps button hides one, and the
grid and the server's `hidden.appsTab` list both lose it while `hidden.app` (Home's row) is left alone; `E2E-182` changes nothing and checks the Back keys (a tap goes back and stops at the
bar, a hold opens the recent apps). `E2E-123` is the reboot case: the one that restarts the TV, run only with `--allow reboot`, which then reads
back what was set (Native Tv Home, the TMDB key, the privacy and screen saver values) once the server has been up for 40 s.

The sweep cases (`E2E-130` to `E2E-149`, and `E2E-146`, `E2E-148`, `E2E-160` to `E2E-166`) walk each group of the TV's own Settings screen with the remote's keys, row by row and page by page, in one client run: on every row the screen calls a switch or a choice the client presses OK, waits for the write to come back, checks the row shows another value, and presses OK until it shows the first one again (a switch in two presses, a choice after one lap of its values; a row that asks to press OK again to confirm, such as switching off a burn-in protection, is confirmed only where the script names it). The script step is `{"sweep": {"skip": [...], "only": [...], "confirm": [...], "act": [...], "maxRows": n, "startAfter": n, "withinMs": n, "press": false}}` (`withinMs` starts no new row after that long, so a run is not cut off with a row in the other state; `act` names action rows that are pressed once, which `E2E-143` and `E2E-144` use for Reset the layout and Cancel a scheduled refresher, the criterion `actedMin` counting them: what they prove is that the press is handled and the app stays up), implemented in `src/platform/row_sweep.hpp`, and the report holds one entry per row for the criterion `{"sweep": {"pressedMin": n}}`, which fails on a pressed row that did not change, did not come back, or whose write did not return. A sweep waits seconds for each write, so its run may last up to 5 minutes, and a long group is cut in slices with `maxRows` and `startAfter`; the client ends the run 1.5 s after the sweep has gone through every row, so `durationMs` is only the most a case may take. It presses the timers and IP control too, each back within seconds. The rows it leaves alone are Power off, Reboot, the installs, Open EZ Adjust and Open In Start (they can change panel calibration), the web dashboard's own switch (it stops the server the TV talks to), and the pixel refresher's rows, which the B3 refuses (`E2E-150`); Native Tv Home's takeover is pressed from the TV by `E2E-147` (two OKs, then read back through the server), each screen saver by its own case (`E2E-160` to `E2E-166`, read back from the server), the service menu lock by `E2E-146`, and Reset the advertising ID and Reset the layout by `E2E-148` and `E2E-143`. The two stabilizers are covered by `E2E-103` and `E2E-131`. A run case may also carry an `after` flow of GET steps, read back through the server once the client has run and judged by `flowOk`. `press: false` only lists the rows, which is how a TV's rows are found before a case is written. The sweeps found three faults that no earlier case reached: the OLED protections read as off for a moment when a client started and the server held that reading for its 30 s cache (see tv-research.md; a sweep then turned the real setting off, which showed as E2E-112 failing at its first step in a full run), and two in the TV app: OK on Privacy's Screen content recognition ended the app (the section heading and the switch shared an id, so the press found the heading and its empty command), and the LG Promotions & Tips Popups row went back to Active because that service takes 12.3 s to switch and a write was given 8 s; a write may now carry its own time limit.

`scripts/dashboard-controls.js` is the same round trip through the browser dashboard, for the controls a person clicks (it is not run by `run.py`, which has no browser): open the TV's dashboard, paste the file into the console, and read `window.__rt` once `__rt.done` is true. It changes each switch, drop-down, slider and colour chip of the Apps, Home screen, Native Tv Home, OLED Care, Screen savers, Game, Privacy, Advanced and Service menu tabs, shows another tab and comes back so the state is read again from the TV, checks the change held, puts the original back the same way and checks that. A run on the B3 changed 90 controls and restored them all with no error message from the page; a switch that hides a tile moves it in the list, so a control is found again by its label, and a group of chips is restored by selecting the original chip. Its recipes cover the Server tab's switches (watch time, daily backup, Prometheus), the MQTT tab's (the icons, a category and an entity, each saved with Save & restart, which waits for the server to restart) and the Tools tab's log viewer switches, whose state belongs to the view and is checked without leaving it; the time server is entered, saved and switched off, and its saved server cleared through the page's own command, because the form cannot save an empty server (Save on an empty field only focuses it), so a server that was switched off stays saved; the experimental tile hiding and Home layout switches are answered yes and switched back, and with the Home layout on, LG's Home screen tiles (HDMI 1 to 4, side icons, quick cards, recommended) are switched one by one. It skips what powers, installs, deletes or restores, the timers, and the experimental Home layout and screen saver switches, and it answers yes only to the OLED protection question; `window.__tabs` and `window.__only` narrow a run.

A client case needs the TV to itself: while someone has the Native Tv app open (launched from LG's Home) every client case ends with "a nativetv-tv is already running on the TV", and a case that ends that way still runs its `cleanup`. A full run on the B3 takes about an hour, three quarters of it the Settings sweeps (each press waits for a real write to the TV, and LG's promotions service alone takes 12 s). `run.py --all --quick` leaves out the sweeps and the client runs over 45 s (65 of the 90 cases, about 15 minutes), which is what a change needs between releases; the full run is for a release.

A result is **pass** when every criterion holds, **floor** when only the interval limits are missed
and the case's `floor` limits (what the platform has been seen to keep) hold, **fail** otherwise,
and **error** when the case could not run. The exit code is 1 for fail or error. The report has
the intervals after the first 1.5 s (the screen loading), the longest draw and vertex counts,
the texture memory, the checkpoints the script took and the size of the capture.

| Id | Case | Screen |
|---|---|---|
| E2E-001 to 003 | Client probe: exit 0, and the forced faults, exit 12 (GL error) and 13 (wrong pixel) | |
| E2E-010 to 014 | Kit gallery: home, apps, live, settings, setup | `kit:` |
| E2E-020 | Splash at 60 fps, the control for the others | `splash` |
| E2E-030, 031 | Home navigation, 17 s and 60 s | `home` |
| E2E-032, 041, 051, 061, 075 | Idle dim after 30 s: the first key only wakes, the next acts | Home, Live, Settings, Catalog, Setup |
| E2E-040 | Live navigation | `live` |
| E2E-050, 052, 053 | Settings: the list, the page, the subsection pills and back (checkpoints on the zone and section); the list of sections; the last row of a page through a refresh (checkpoints on `focusLast`), no OK | `dashboard:apps`, `dashboard`, `dashboard:system` |
| E2E-060 | Catalog navigation, no OK | `catalog` |
| E2E-070 to 074 | Setup welcome, Home Assistant, summary, standby and network pages, Up and Down only (the phone hand-off page is left out: opening it asks the server for a one-time link) | `setup:` |
| E2E-076 | Setup name page on the test B3: the keyboard starts with `B3` and the arrows move over the keys, nothing typed | `setup:name` |
| E2E-077, 078 | First launch from the dashboard tile and from Home opens the setup welcome page (needs a TV whose first-run marker, the empty file `/var/lib/tvweb/.setup-pending`, is present: `--allow fresh`; on a TV that has done setup, create the file, run the two cases with Native Tv Home off, and delete it again) | `dashboard`, `home` |
| E2E-079 | Launch after setup opens the dashboard's own screen | `dashboard` |
| E2E-090 | Quick drawer navigation, no OK | `quick` |
| E2E-091, 092 | Hold Left then Right on the Quick drawer's volume or light: it loads, never moves against the key, ends settled (`control`) | `quick` |
| E2E-093 | Quick drawer closes with Back onto Home | `quick` |
| E2E-100 | Advanced tab's Sound output row is the output the TV is using, not the stored HDMI ARC pick (`api`) | |
| E2E-101 | The Prometheus address opens and closes with its switch, without a restart (`api`, `control`) | |
| E2E-102 | Tile hiding refuses while its experimental switch is off and not once it is on, without a restart (`api`, `control`) | |
| E2E-103 | OK on a number row steps it as a whole number (11, not 11.0) and 21 presses come back to the start (`control`) | `dashboard:game` |
| E2E-104 | The admin PIN gates a risky action until unlocked, refuses a wrong PIN and closes on lock; sets PIN 2468 and removes it (`api`, `control`) | |
| E2E-105 | The Home Assistant settings refuse a URL or a space as the broker and markup as the device name (`api`, `control`) | |
| E2E-106, 107 | LG rows round-trip: the ad switches, sound mode, balance, automatic volume and Bluetooth speaker mode (106); the Game Optimizer rows (107). Each is set, read back from the TV's settings service and put back (`api`, `control`) | |
| E2E-110 to 119 | The rest of the settings, each set through the server, read back and put back: privacy switches and the ad blocker (110); screen saver and brightness (111); OLED protections, screen shift, logo dimming and the service menu lock (112); app preload and saved web pages, with the id the add gives (113); watch time and screen-time limits (114); NTP server (115); a damaged backup refused (116); an SSH key added and removed, the TV's own keys untouched (117); a 22 KB catalogue app installed and uninstalled (118); volume, mute, brightness, picture mode, energy saving and the sleep timer, from the test B3's own levels (119) (`api`, `control`) | |
| E2E-120 | Arranging the kinds on the Apps tab: with the apps by kind, a long press lifts a row, Down moves it and OK saves, a second long press and Up move it back, Back cancels; the order is reset after (`control`, `before`, `cleanup`) | `home` |
| E2E-178 | Editing the apps on the Apps tab: a long press starts it, the Hide from Apps button hides an app, and the grid and the server's `hidden.appsTab` list lose it while Home's `hidden.app` stays empty; the order and both hidden lists are reset after (`control`, `before`, `cleanup`) | `home` |
| E2E-121, 122 | A TMDB key that is not 32 hex digits (a word, 20 digits, a number) is refused; `kind: title` is no longer a way to open a feed title (`api`, `control`) | |
| E2E-125 | OK on Dark room mode in the TV's Game settings steps it Off, Level 1, Level 2, Off through the server and leaves it off (`control`, `cleanup`) | `dashboard:game` |
| E2E-130 to 144, 146 to 149, 160 to 166 | Every switch and choice of the TV's Settings groups, pressed through the TV's own screen and put back: Game (130, 131), OLED Care (132), Screen Saver brightness (133), Privacy (134), System with the timers and IP control (135, 136, 149), Home (137 to 139), Apps (141, 142); the actions Reset the layout (143), Cancel a scheduled refresher (144) and Reset the advertising ID (148) are pressed once; the service menu lock (146), the Native Tv Home takeover (147) and each of the seven screen savers (160 to 166) are pressed and read back through the server (`control`) | `dashboard:<group>` |
| E2E-182 | Back goes back (Apps to the Home tab, then up to the bar, where it stops) and only a held Back opens the recent apps | `home` |
| E2E-150 | The pixel refresher is refused by the B3 on schedule and on cancel, and stays Idle (`api`, `control`) | |
| E2E-123 | Native Tv Home, the TMDB key, the privacy and screen saver values are as they were after a reboot (`reboot`) | |
| E2E-108, 109 | The Advanced power switches (Quick Boot, Stay connected, Wake-on-LAN, LG logo, standby light, device detection) and the On and Off timers round-trip through `/api/stats` (`api`, `control`) | |

The timing criteria are the target of the client's spec: p50 and p95 of the frame intervals at 60 fps
(17.0 and 16.8 ms), none over 33 ms, at most 40 draws and 8192 vertices a frame and 64 MB of textures.
On the B3 the splash itself has a p95 near 17 ms, so the floor is 20 ms with a few intervals over 33 ms
(`glClear` blocks for about 21 ms roughly every seven seconds).

## Trying a build on a TV

`native-tv/lifecycle/deploy.sh <tv-ip>` installs the ARM build over ssh (the key and
known-hosts options are set at the top of the script) and restarts the server.
`tvwebctl` loads `/var/lib/tvweb/native.env` before it starts the daemon, one
`NAME=value` per line, so a variable set for a trial survives a restart and a reboot;
without the file nothing changes. `TVWEB_LUNA_SEND` can name a wrapper script that logs
each `luna-send` call and its reply, to see which calls a TV refuses. `tvwebctl stop`
also stops the resident daemon and waits for it, since a new one cannot bind the ports
while the old one holds them.

A comparison on a TV: save the read-only routes of `routes.json` (token as a Bearer
header) before the change and after it, then diff them with the clock-driven fields
dropped. On a B3 (OLED65B3PUA, webOS 10.3.2, firmware 33.31.75) this found the panel
controller's missing `subscribe`, consent flags under `eulaStatus`, `unit: null` on
services without a unit, the settings page's telemetry default and device name,
agreement-described consent flags listed as unknown, `stoppable` on on-demand daemons
and the restart race above. Home Assistant publication is compared by running a bare
MQTT broker on the TV (a test script in Node, which LG ships) and capturing every
PUBLISH, with the default topics, device record, retained clears and select options
in view; a command is carried out by publishing to its `command_topic`.

The B3 run covered, with the daemon as the only server: every read route, a reboot, a
restore of the TV's own backup (nothing changed, no restart) and of one that changed
a setting (the daemon restarted and came back), an MQTT command, an uploaded dummy
package through preview, confirm, install and uninstall, and Home Assistant discovery
against a test broker. The rollback copied the earlier install's `/var/lib/tvweb` and
boot hook back from a tarball taken beforehand and started it; the upgrade ran
`deploy.sh` over that. `scripts/check-drift.py` also checks that `deploy.sh` ships every
file.

## Home Assistant

[`home-assistant.yml`](https://github.com/ihtisham1211/native-tv/blob/main/.github/workflows/home-assistant.yml) builds every discovery config the server can publish and
runs each through Home Assistant's own validation, then checks that the volume
entities are available exactly when the TV lets their volume be changed. Home
Assistant creates no entity for a config it rejects, and says so only in its
own log, which is how Volume and Mute went missing in 0.78.0. It also renders
every template that reads telemetry against the stats each fixture TV reported
and fails on any warning Home Assistant logs: a field missing from the payload
is logged on every message, which was tens of thousands of lines an hour with
a few TVs (#761).

It runs when the server or the check changes, not for documentation, and
weekly, since Home Assistant is not pinned: a release that starts rejecting a
config shows there first.

## The dashboards in a browser

`visitHomeContent` exercises details, direct Open, trailers, Netflix actions,
related-title Back navigation, full overviews, failed and delayed replies,
search minimum length/debounce/stale responses and result paging. It also
checks profile rename cancellation, failed-save retry and hiding Search when
the TMDB key is removed.

`visitKitStages` opens the host-only design reference and checks that shared carousel and boot frames fit at
1920 and 1280 pixels wide and respect reduced motion. With
`NATIVETV_REVIEW_DIR` set, it captures both examples in both gallery themes.
`visitHomeColours` also checks that carousel text and the boot mark retain
Home's colour overrides after extraction into shared sections.

[`dashboards.yml`](https://github.com/ihtisham1211/native-tv/blob/main/.github/workflows/dashboards.yml) starts the daemon against a stand-in `luna-send`, opens every tab of
the web dashboard and the TV app in a headless browser, and fails on any
script error or failed request, naming the tab. A 404 for an app's icon is
not a failure: it is how the server says an app has none, and the stand-in TV's
installed apps have no icon files. A function a page calls that has gone, or an
element it reaches for that has moved, breaks only when that tab opens, which
no unit test does. It runs when the daemon, the pages or the stand-in TV change.

For the Home view, picture checks cover a layout with picture details left at their default (off), the plate's signal and mode, an input with no signal showing none, a delayed reply after changing source, and a delayed reply after switching picture details off. Its service replies are simulated; the page and its timers run in Chromium.

Home colour checks cover custom accents matching the stylesheet's original colours, changes and reset arriving through the heartbeat, and malformed colour values being discarded without stopping the server.

For the TV app dashboard, checks verify the two-column layout geometry (the row list on the left and the hint pane on the right with non-zero dimensions and horizontal adjacency), remote D-pad navigation (`ArrowDown`, `ArrowUp`, `PageDown`), list translateY scrolling with the scroll arrow indicator, tab switching (`ArrowRight`, `ArrowLeft`), and that the OLED Care tab is dropped when running on a non-OLED (LCD) TV profile.

Home preference checks operate both dashboards' controls and verify icons-only
tiles, their accessible names, remote app selection, label restoration and
unchanged input and channel labels. `NATIVETV_REVIEW_DIR` saves Home images
with app names shown and hidden for visual review.

Clock checks operate the choice on both dashboards and cover midnight, noon,
afternoon, late-night and leading-zero formatting, the smaller AM/PM marker
fitting its column, programme times, and a format change arriving through the
heartbeat without waiting for the clock's next periodic refresh.

Home visibility checks switch the Inputs row and the LG Home tile off from both
dashboards, then check that the view leaves out only the one switched off and
that Down skips a row that is off.

Home view checks drive `google-tv/pages/home/index.html` against mocked server data: its tabs,
the hero starting on what was last on, keys moving focus, OK opening the focused
app, Back's levels (a lower row, the bar, recent apps), the top picks row and
OK on a title from the feed, the carousel's page dots, the boot screen leaving with the first render and not showing over a kept catalogue, and the quick settings: the gear opening the drawer, volume steps sent together, a choice from a list, an input opened from it, a Home setting saved, Back's levels and the pointer. Like the other TV pages it is held to Chromium 38 by `check-es5.py`.

Moved pages checks open the TV pages' old addresses and check that each lands
on its page in `native-tv/assets/google-tv/pages/`.

TV asset assembly checks visit the four production page folders, including
setup and its one-time phone form. The component kit runs on a separate static
reference server under `native-tv/tools/`. They assert that CSS and JavaScript arrive
inline without resource requests, Home retains its editable theme stylesheet,
and legacy flat URLs return the pages without another redirect. Route tests
also reject inline CSS/JS in source HTML, validate shared section names and
script ordering, and diagnose missing page files. The phone form keeps its
modern browser styles.

TV polling checks use a virtual clock and delayed responses to verify ten-second
refreshes, no overlaps, one fresh post-control read, cancellation while hidden and
immediate resume. Home background checks verify that hidden nonessential reads
and wallpaper animation pause while the recovery heartbeat continues, without
reloading a hidden page on a version change. Visibility events are
simulated here; actual webOS visibility behavior needs a real-TV check.

## Documentation

[`docs.yml`](https://github.com/ihtisham1211/native-tv/blob/main/.github/workflows/docs.yml) builds this site on each pull request that changes it, with every
link and section link checked, and [`docs-publish.yml`](https://github.com/ihtisham1211/native-tv/blob/main/.github/workflows/docs-publish.yml) publishes it when the
change is merged.

## Releases

Publishing a release runs [`homebrew-channel.yml`](https://github.com/ihtisham1211/native-tv/blob/main/.github/workflows/homebrew-channel.yml), which builds the `.ipk`,
attaches it and the Homebrew Channel manifest to the release, and records the
package's sha256 in the manifest.

A release's notes open with a short summary for people using Native Tv, one
to three plain lines, then a `## Details` heading with the full notes and their
pull requests. The Server tab and TV app
show only the summary, so it leaves out changes to the docs and the code
that people using it would not notice. Releases before 0.91 have no summary,
and show none.

## Running them locally

From the repository root:

```bash
python3 native-tv/build.py --service-only --test
python3 native-tv/build.py --service-only --test --sanitize --out native-tv/build-host-ninja-asan
node native-tv/tests/contract/check-route-manifest.js
python3 native-tv/tests/contract/check-csrf-policy.py
node native-tv/tests/contract/check-test-matrix.js
node native-tv/tests/ui/run.js
for s in check-es5 check-ui-ids check-strings check-screensavers check-drift check-entity-stability check-retired-files; do ./scripts/$s.py || break; done
./scripts/check-entities.py --stats native-tv/tests/fixtures/tv/stats-b8-webos4.json
```

The ARM daemon, which `deploy.sh` and the Homebrew package need, is built with
`python3 native-tv/build.py --sdk /path/to/arm-webos-linux-gnueabi_sdk-buildroot --service-only --target nativetv-daemon`.

The dashboard check opens every tab in a headless browser against the daemon, so it needs the host build (`python3 native-tv/build.py --service-only`; `NATIVETV_DAEMON` names another) and Playwright and its browser:

```bash
npm install --no-save playwright && npx playwright install chromium
node scripts/check-dashboards.js
```

Scripts that talk to a TV or to GitHub rather than to the tree, and need no change when a release changes: `soak.js` (a soak test against a real TV, with an optional MQTT broker; it carries its own small MQTT client), `footprint.sh` (the daemon's CPU and memory on the TV), `tv-wakeups.sh` (context switches a second for each thread of the daemon, over SSH), `tv-client-run.sh` (a client build that is not installed, run once on the TV with its report and perf summary printed), `tv-memory-soak.sh` (the server's memory, threads and open files over hours, over SSH), `tvmon.py` with `probe_defs.py` and `tvstats.sh` (live SoC and load readings over telnet), `run-prometheus.sh` and `profile-home.js` (pasted into the Home page's inspector, and loaded by `check-dashboards.js`). Retired with Node: `check-es5.py` for the server, `check-module-calls.py`, `check-upgrade.js`, `discovery-configs.js` and the two `check-native-ha-*.py` comparisons, which the `native-ha-golden` test replaced.

The Home Assistant check needs Python 3.13 and the `homeassistant` package:

```bash
pip install homeassistant
./scripts/check-ha-discovery.py
```

## Local TV emulation

`scripts/emulate-tv.js` runs the host build of the daemon as on a TV with no Node (`TVWEB_NODE_FREE=1`), on the build's stand-in `luna-send`, a fixed system snapshot and a seeded Native Tv Home. It serves the web dashboard, the TV pages and the API on a computer, for development without a TV; build first with `python3 native-tv/build.py --service-only`.

```bash
node scripts/emulate-tv.js [--port 8080] [--model c2|b8|c3] [--open]
```

## Home content on a TV

On 2026-10-25, the deployed content views were checked on an OLED65B3PUA,
webOS 10.3.2, firmware 33.31.75, through the developer-installed Native Tv
app navigated to the Home page. Its on-device Chromium 120 rendered at
1920 × 1080. CDP dispatched remote key codes and keyboard input; captures
covered Home, Lupin details, search results, Stranger Things details and
profile naming. No runtime exception was recorded.

The live feed returned both derived rows. Details showed runtime, genres and
cast; Back restored the original row and tile. Search for Stranger Things
returned fifteen results. Wikidata resolved TMDB TV ID 66732 to Netflix ID
80057281, and the native Netflix app became foreground after Open on Netflix.
Profile renaming saved successfully. Original profile and config files were
restored after the checks; the TV's Home takeover and Netflix option retain
their original off settings. The updated server remains installed.

Native Netflix account selection and playback, cold boot/Home takeover,
C2 hardware and Chromium 38 rendering remain unverified. Screenshots and the local TV report
are in `/private/tmp/nativetv-phase4-tv/` for this session.

Production route tests require the retired gallery URLs to return 404, and retirement
tests exercise installed-file cleanup.

Native controller/HTTP tests cover source-specific picture queries, fractional
refresh rates and HDR labels, disabled/background/stale reply suppression,
recent snapshot revisions, card/Close/Close-all navigation and failed-close
retry. `tests/smoke/home-extras.json` uses `tests/tv-fixture.js`, a bounded
loopback fixture on the TV. Fixture close actions change only fake app records;
the fixture never calls TV services or writes preferences.
