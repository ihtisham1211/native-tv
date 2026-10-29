# History

How the server moved from Node to C++, and what that run found. The detailed
ledger and the research behind it were removed once the move was finished; git
holds them.

## What moved

Until `v0.118.0` (`LAST-NODE-RELEASE`) the server was a Node program
(`server/tvweb.js` and 61 modules) that the TV ran under its own Node. In
October 2026 it was replaced by one C++14 daemon, `glasshouse-daemon`, with a
metrics worker, `glasshouse-service`, beside it. The Node server and its test
suites were deleted on 29 October 2026.

- The daemon owns every route, authentication, first-run setup, the dashboards,
  stats, Home Assistant MQTT discovery and telemetry, install jobs, backups,
  Glasshouse Home, remote keys, services and the screen savers.
- `tvwebctl` (the watchdog) and the boot hook start the daemon directly. No Node
  runs on the TV, and an install of the Node release is cleaned at the first
  start (`retired_files`).
- Layout: `native-tv/src` (daemon, service, domain, core), `native-tv/web` (the
  phone and computer dashboard), `native-tv/assets` (the TV pages, TV apps, fonts
  and screen savers), `native-tv/lifecycle` (watchdog, boot hook, installer).

## The rename

The project was called Glasshouse until October 2026, when it became nativeTv:
the binaries are `nativetv-daemon`, `nativetv-service` and `nativetv-tv`, the
namespace `nativetv`, the environment variables `NATIVETV_*` and the Prometheus
metrics `nativetv_*`. What an install made under the old name keeps, and what it
loses, is in `docs/TV-CHANGES.md`. The name `glasshouse` is still read in a
backup file, a saved daily backup, `syslog.sources`, the configd key
`glasshouse.remoteKeys`, the bus service `com.webos.glasshouse.boot` and the
`glasshouse-home-starting` marker, and in the Android app's own names.

## Decisions

- **D1.** webOS 2.2 and 3.x are dropped after the cutover; webOS 4 is the floor.
- **D2.** CivetWeb for HTTP, libcurl with the SDK's OpenSSL for MQTT TLS; no
  Mongoose (licence).
- **D3.** The shell watchdog stays; the daemon writes the heartbeat it watches.
- **D4.** No native IPC adapter; the daemon's HTTP API serves its consumers.
- **D5.** The phone dashboard source stays in `native-tv/web/`.

## Verified, and where

One TV, an LG OLED65B3PUA on webOS 10.3.2 (firmware 33.31.75), driven over ssh:

- Every read route compared before and after the move, 31 routes.
- Upgrade over the Node install, a rollback to it and an upgrade again; reboots,
  including one with no Node at all.
- A soak of 1656 mixed requests over 12 minutes with no restart.
- Install, preview, confirm and uninstall of a package through the daemon.
- Backup restore (unchanged restore restarts nothing; a changed one restarts once).
- Home Assistant against a real Mosquitto 2.1: discovery, retained state, a
  command, the last will after a hard kill, reconnection, and TLS with a private
  CA and a password.
- Glasshouse Home enabled and disabled, with LG's Home restored.

The build and the whole test suite also pass on Ubuntu 24.04 (GCC 13) and on
macOS, plain and under ASan/UBSan, and `scripts/check-dashboards.js` passes.

## Bugs only a TV or a second platform found

- The OLED hours came from a file, not the panel: `panelcontroller` needs
  `{"subscribe": false}`.
- LG's consent flags sit under `eulaStatus` in `/var/luna/preferences/eula`.
- A restart left the old daemon on port 8080, so the new one failed to bind.
- `deploy.sh` did not ship one module the server needed to start.
- A Luna subscription holds an event-loop worker for as long as it runs; fewer
  workers than subscriptions starved one-shot calls.
- json-c's reference counts are plain integers, so request threads sharing a
  cached answer corrupted the heap ("double free" under load, watchdog restarts).
- The private MQTT CA was read from `<data>/server/mqtt-ca.pem`, not
  `<data>/mqtt-ca.pem`.
- GCC rejects ignored `write` and `system` results under `-Werror`.

## Not tried

- Any TV model or webOS version other than the B3 on webOS 10.3.2.
- Physical presses of the remote's keys for the remote-key mappings.
