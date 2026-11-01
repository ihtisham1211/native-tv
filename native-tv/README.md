# Native TV application

Native Tv's native TV client uses SDL2 for the window and input, and OpenGL ES 2
for drawing. Home, content views, the keyboard, profiles, quick settings, setup
and TV dashboard tabs live here. The phone/browser dashboard remains a web app
with its canonical sources in `web/`.

The native client is under development. Its data adapter calls the Native Tv
daemon, the server that runs on the TV, over loopback. The Node server it
replaced is gone; [ARCHITECTURE.md](ARCHITECTURE.md) defines the module boundaries. The developer
app is the Native Tv tile's, `com.tvweb.dashboard`, which Native Tv Home also opens;
deployment does not replace LG Home or enable Home takeover.

## Layout

```
assets/             Fonts, Figma SVGs, icons and licences, and the TV pages, apps and screen savers the daemon serves
lifecycle/          tvwebctl (start, stop, watchdog), the boot hook and deploy.sh
web/                Phone/browser dashboard source, kept for web clients
src/core/           JSON ownership and backend operation interfaces
src/daemon/         The server: HTTP front door, MQTT, state, install jobs and the other daemon modules
src/domain/         Pure Home, dashboard and setup models
src/ui/             Screen state, controllers, layout and reusable components
src/graphics/       GLES shaders, texture cache and text rasterization
src/platform/       Operating-system integration
src/adapters/       HTTP backend that maps screen operations to the daemon's routes
src/service/        Separate native worker for process and system metrics
tests/              Native model, controller and integration tests
third_party/        Pinned QR encoder and NanoSVG, with licences
```

Domain and presentation targets have no SDL, GL or HTTP dependency.
Their tests use a fake `Services` implementation. Endpoints and URL encoding
belong to the adapter; screens send named `Operation` values.

## Build and test

What to install is in [Contributing](../docs/development/contributing.md#building-and-testing);
`python3 native-tv/build.py --check` lists what is missing. The full host build, with the
TV client, and a run of the client in a window:

```sh
python3 native-tv/build.py --out native-tv/build-host-ninja --test
native-tv/build-host-ninja/nativetv-tv --windowed
```

`--sanitize` enables the address and undefined behavior sanitizers; an optimized
sanitized service build is
`python3 native-tv/build.py --service-only --sanitize --out native-tv/build-host-sanitize-ninja --test`.
Without `--out`, `build.py` uses `build-host-ninja` when Ninja is installed and
`build-host` otherwise; a build directory keeps the generator it was made with, so
delete an older `build-*` directory to switch it. `ccache` is the compiler launcher
when it is installed (`--no-ccache` turns that off). `--build-jobs N` sets compile
parallelism, up to eight CPUs by default. Native tests run through CTest. Host test
builds shorten SAM's wait intervals to 5 ms; SDK builds set `BUILD_TESTING=OFF` and
keep production timings.

The service can also build without any SDL/OpenGL dependencies:

```sh
python3 native-tv/build.py --service-only --out native-tv/build-service-ninja --test
```

This target builds both `nativetv-daemon` and `nativetv-service`. The
resident daemon loads the config, starts the event loop and 20-second
heartbeat, and exits cleanly on SIGTERM/SIGINT. `lifecycle/tvwebctl` and the boot
hook start it with `--listen` on the TV.

With `--listen`, the daemon serves the phone dashboard and the static files under
`/var/lib/tvweb/assets`, answers every route in `contract/routes.json` itself and
owns the phone setup listener at the configured port plus one. The caps response
keeps the API key TV-local and reports setup and Homebrew state from native files.
Host integration can override the asset root with `--assets-root PATH`. The
compressed dashboard uses the TV system's `libz.so.1`; the runtime ABI check
allows this system library. `--node-port` names an upstream for the daemon's
tests; with none, a route the daemon does not own answers 404.

CTest uses twice the available CPU count, capped at sixteen workers by default, since
most of the daemon tests wait on timers rather than computing (CI keeps one per CPU,
capped at eight); use
`--test-jobs 1` for a serial run or choose another value from 1 to 32. During
development, build only the target you changed and filter CTest to the matching
suite. For example:

```sh
python3 native-tv/build.py --out native-tv/build-host-ninja \
  --target native-app-package-inspector-tests \
  --test-filter '^native-app-package-inspector$'
```

`--target` can be repeated. `--test-filter` is a CTest regular expression and
also runs tests when `--test` is omitted. If both `--target` and `--test` are
provided, a filter is required so CTest does not try to run targets that were
not built. Use `--test` without a filter for the full native suite.

CTest registers each `native-daemon-runtime-*` Python integration case
separately, so `--test-jobs` can run independent cases in parallel. Filter to
the runtime group or one case while iterating:

```sh
python3 native-tv/build.py --service-only --out native-tv/build-host-ninja \
  --target nativetv-daemon --test-filter '^native-daemon-runtime-' --test-jobs 8
python3 native-tv/tests/daemon_runtime.py \
  native-tv/build-host-ninja/nativetv-daemon \
  DaemonRuntimeTests.test_native_last_app_internal_bridge
```

`nativetv-service processes [--all]`, `nativetv-service cpu` and
`nativetv-service system` return a versioned JSON envelope. The
`nativetv-service port [--data-root PATH]` command prints the validated
dashboard port from `config.json`, defaulting to 8080 for missing or invalid
configuration. Deployment scripts use it to read the config file. The watchdog can call
`nativetv-service reap-stuck-children --pid PID [--proc-root PATH] [--settle-ms MS]`
to kill direct children that remain on the server executable after the settle
interval; its default interval is three seconds. The worker reads
procfs/sysfs and filesystem counters, then exits. The reaper mode only signals
matching server children; it owns no settings or TV controls. CPU sampling
retains the five-second API window and checks
process start times to exclude recycled PIDs. The daemon coalesces matching
reads, uses the child-start queue and bounds runtime and output. A missing,
incompatible or failed worker returns an explicit API error. System snapshots
include memory, swap, temperature, eMMC health, clocks, app storage, CPU times,
uptime/load, network/Wi-Fi, picture-engine and Magic Remote pairing readings.
The five-second CPU window, Home Assistant's publish baseline and the network rate
are kept by the daemon over native counters.
The worker and resident daemon live at `/var/lib/tvweb/native/`, independently
of the client app. `deploy.py` installs that runtime and a development build of the client.
Server deployment and the Homebrew packager require an ARM build, using
`build-webos` by default; `--native-build` selects an explicit build directory.
The client IPK carries the native executable, its non-system SDK libraries, and
the fonts, icons and licences it opens (`CLIENT_ASSETS` in
`tools/runtime_payload.py`, hashed in its `runtime.json`). SDL2, FreeType and
GLES stay system libraries on the TV. Server and Homebrew deployments stage that
client twice: as the dashboard's tile, which opens on the dashboard screen, and
as the hidden OpenGL startup splash app.

TV builds embed json-c and the pinned CivetWeb HTTP listener. `tools/runtime_payload.py`
checks the daemon and worker; `--client` checks the TV app. Each mode resolves
its non-system ARM libraries recursively from the configured webOS SDK
sysroot, verifies ARM ELF and glibc requirements, and records a SHA-256
manifest. Daemon and worker use the managed runtime directory in
`LD_LIBRARY_PATH`; the client uses `$ORIGIN` to find its colocated curl
libraries. Verified SDK notices are preferred. When the SDK omits them, the
packager uses version and SHA-256 pinned upstream notices recorded in
`third_party/sdk-runtime-notices/SOURCE.md`.

```sh
python3 native-tv/tools/runtime_payload.py --out native-tv/out/runtime/native \
  --archive native-tv/out/nativetv-native-runtime-arm.tar.gz
python3 native-tv/tools/runtime_payload.py --client \
  --out native-tv/out/runtime/client
python3 scripts/build-ipk.py --native-build native-tv/build-webos
```

The json-c notice has one source copy in `third_party/json-c/SOURCE.md` and is
extracted into generated client/runtime packages. CivetWeb's upstream
`LICENSE.md` and notices for staged SDK libraries are also included in the
server runtime package. Cross-building both the client and service with the
webOSbrew SDK confirms the current SONAMEs; runtime staging verifies their
dependency closure and notices. Native Tv
self-update and rollback were removed; releases are
installed through deployment or the Homebrew Channel package. Homebrew app
catalog updates remain available. Manual installation includes the runtime.

The process contract suite checks the native executable against fixed expected
API responses. Native suites use native fixtures and fake services.

The read-only hardware API check runs on the TV:

```sh
ssh root@TV_ADDRESS node < native-tv/tests/tv-metrics.js
```

For a custom server port, the remote command is `node - PORT` (the TV's own Node,
which LG ships). The check requires an installed worker and daemon.

The TV build uses a relocated [webOSbrew native SDK](https://github.com/webosbrew/native-toolchain):

```sh
python3 native-tv/build.py --sdk /path/to/arm-webos-linux-gnueabi_sdk-buildroot
python3 native-tv/deploy.py --tv root@TV_ADDRESS \
  --identity ~/.ssh/id_ed25519 --known-hosts ~/.ssh/known_hosts --run
```

`deploy.py` stages the client in the Native Tv tile's app folder (`com.tvweb.dashboard`) and the managed service runtime.
It removes the obsolete worker copy from that directory.
`--run` launches a bounded 12-second smoke check, saves a PPM capture and JSON report in ignored `out/`,
then exits. `--screen dashboard:system` opens a dashboard tab; `--script` accepts
a JSON array of timed key/input/capture/assertion steps. The launcher supports
`--help`. The TV build links its system SDL and GLES libraries; it does not
ship an SDL replacement. The compiler's C++ runtime is linked statically.

The HTTP adapter uses separate API/artwork queues, response limits, timeouts
and request generations. The renderer keeps at most 80 MiB of textures, skips
off-screen nodes and stops submitting frames when the scene is idle. Exact
memory and frame measurements require hardware tests.

## Validation status

The ARM build rendered live Home data on OLED65B3PUA/webOS 10.3.2 at
1920×1080 with the system PowerVR GLES driver. Host tests cover models,
navigation, debounced search, delayed Netflix replies, Unicode editing, busy
commands, idle rendering, SVG/text rasterization and HTTP integration.
Read-only hardware scripts cover all eleven dashboard tabs, search, details,
drawers and profile rename.

The ARM IPK installs through LG's developer installer. Normal app-manager
launch, background/resume with the same process, close and cold relaunch passed
on the same TV. Up/Right/OK opened Quick settings and Back returned to Home
through `networkinput/test/sendKeyCode`, exercising SDL's TV input mapping;
compositor captures verified the result. The existing dashboard was restored
to the foreground after testing.

Full native feature parity, Home takeover and older-TV compatibility remain
under validation. The phone/browser dashboard and the TV pages are served by the daemon alongside it.

The native component gallery opens with `--screen kit:keyboard` (also `field`, `button`
and `drawer`). `tests/smoke/` contains read-only TV scripts; the content script
expects a TMDB-enabled catalogue, a single profile and a Stranger Things result.

`python3 native-tv/package.py` creates an ARM IPK of the tile's app in `native-tv/out/`
using LG's packager. It rejects host executables. Packaging does not bundle the
Node server or enable Home takeover.


Home picture details and screenshot recents draw in GLES. Picture details follow
the active video source and the saved opt-in; late replies are suppressed.
The recents panel preserves the remote's card, Close and Close-all navigation.
`tests/smoke/home-extras.json` exercises both against `tests/tv-fixture.js`;
`deploy.py --server http://127.0.0.1:18081` selects that temporary fixture.
The `home-wallpaper.json` and `home-wallpaper-missing.json` scripts verify the
uploaded slot cross-fade, readable focused carousel text and black fallback.
Start the fixture with `--wallpaper=ready` or `--wallpaper=missing` for those
scripts; its default `none` mode verifies a TV with no uploaded slots.
