# Working conventions

## Docs move with the change

A change updates the docs that describe it, in the same PR, before it is
pushed:

- CI, tests or `scripts/`: docs/development/testing.md. Its links into
  `.github/workflows/*.yml` name line ranges, so check them whenever a
  workflow changes.
- Something the dashboards show or do: docs/dashboard/ for that tab, and its
  screenshot once released.
- A new feature, an integration, or support for a new TV or webOS version:
  README.md (a bullet under Features that links its docs page, the
  Compatibility table, and the FAQ if people would search for it) and
  docs/index.md. The README is what GitHub and Google show first, so keep its
  opening paragraph and the GitHub description and topics current
  (docs/development/contributing.md#readme-and-search).
- Anything the server changes on the TV: docs/administration/tv-changes.md.
- What contributors must do differently: docs/development/contributing.md.
- A finding about how LG's software works: docs/development/tv-research.md.
- Build, install, update or deploy steps (`build.py` flags, `deploy.sh`,
  `tvwebctl`, the Homebrew package): the install steps in README.md, and
  docs/getting-started/install.md and managing.md. Readers copy these
  commands, so they must work as written.
- A dashboard section, tab, setting label or default, the webOS versions a
  feature needs, or an experimental switch: docs/dashboard/index.md (the
  sections and tabs), the tab's page, and docs/administration/experimental.md.
- A Home Assistant entity, MQTT topic, config key or Prometheus metric:
  docs/integrations/ (entity names as discovery publishes them, when each is
  published, and the metrics table).
- Dashboard text, a daemon message the dashboard shows (`say::msg`), or a
  language: docs/development/strings.md, and the Language setting in
  docs/dashboard/server.md.
- A TV app screen or a setup wizard step: docs/apps/tv-app.md and the wizard
  summary in docs/getting-started/install.md.
- The look of a TV screen (type, colour, shape, spacing, focus, motion, icons):
  docs/development/material-3-expressive.md. Its tables hold the token values,
  so a change to `native-tv/design/` updates them. Its screenshots in
  `docs/screenshots/` and README.md come from a real TV
  (`scripts/tv-screenshot.sh`, docs/development/contributing.md#screenshots), not
  from the host's renderer: `tests/golden/` holds test fixtures, not documentation.

Write what the code does, not what a feature was meant to do: check a flag, a
default, a label or a version against the source before it goes in a page. A
count or a list that several pages repeat (TVs tested, entities, tabs) changes
in all of them: README.md, docs/index.md and the page that holds the data.

The published site is `docs/` and `mkdocs.yml`, built by `mkdocs build
--strict` (docs.yml) and deployed to GitHub Pages when merged. Its look follows
Google's Material 3 (`docs/stylesheets/extra.css`: the palette, rounded shapes
and state layers); keep new content inside Material's components so it
inherits that.

# Code map

The server is one C++14 daemon, `nativetv-daemon`, that runs on the TV
beside a small metrics worker, `nativetv-service`. No Node runs on the TV.
The pages the TV's own browser shows are strict ES5 (webOS 4's browser);
`native-tv/web/ui.html` and `native-tv/web/ui/` run in a phone or computer's
browser and may use modern JavaScript.

- `native-tv/src/daemon/main.cpp` - entry point: config, wiring the modules
  together, the MQTT bridge, the state subscriptions and the heartbeat.
- `native-tv/src/daemon/http_frontdoor.cpp` - every HTTP route, auth, first-run
  setup and serving the dashboard; `http_static_assets.cpp` puts
  `native-tv/web/ui/` and the TV pages' parts back inline.
- `native-tv/src/daemon/` otherwise, one concern each: `luna` and
  `child_process` (luna-send calls, subscriptions and the spacing of child
  starts), `state_subscriptions` (live state), `stats_collector`
  (`/api/stats`), `mqtt_*` and `ha_*` (the MQTT client, topics, Home Assistant
  discovery and telemetry), `install_jobs` (installing .ipk packages),
  `backup_restore`, `home_orchestrator`, `home_crash_breaker`, `home_probe_gate` and `home_*_manager` (Native Tv Home
  and LG's Home screen), `screensaver_*`, `sam_rescan_manager`,
  `remote_keys_reconciler` and `remote_button_log_watcher` (the remote's
  keys), `services_keeper`, `devtools_firewall`, `retired_files` (files earlier
  releases installed, deleted at start).
- `native-tv/src/service/` - the logic the daemon calls, testable without a
  TV: `lg_settings`, `oled_care`, `privacy_*`, `adblock*`, `app_*`,
  `background_services`, `backup`, `history*`, `screen_time`, `piccap`,
  `prometheus`, `system_metrics`, and the process/system metrics worker.
- `native-tv/src/domain/` - `names` (raw TV values as display strings and
  Prometheus labels), `say` (server-side strings), the dashboard and Home
  models; `native-tv/src/core/` - JSON and small shared helpers.
- `native-tv/src/{ui,graphics,platform}` - `nativetv-tv`, the client that draws the
  TV's screens in OpenGL ES 2 on SDL2: `ui/motion` (springs, tweens, one clock),
  `ui/scene` (a frame's nodes), `ui/kit` (the components after Google TV's design;
  `tokens.hpp` is generated by `tools/gen_tokens.py` from `native-tv/design/`),
  `graphics` (batched renderer, glyph and icon atlases, image loader) and
  `platform` (window, `--probe`, `--time`, `--perf`, the frame loop).
  `tests/golden/` compares rendered screens.
- `native-tv/web/ui.html` - the phone/browser dashboard's markup; its CSS and
  scripts are in `native-tv/web/ui/`, one script per tab or concern. `ui/i18n.js`
  is the string loader (`t()`, the language choice); the dictionaries it reads are
  `native-tv/assets/i18n/<lang>.json`, which the daemon also reads to translate
  its messages (`domain/say.cpp`, applied in `HttpExchange::respond`).
- `native-tv/assets/google-tv/` - the design system of the few web pages that
  remain, after Google's TV Design Kit: `shared/common.css` (named sections
  for literal tokens, fonts, themes and the components those pages use) and
  `shared/common.js` (text helpers). The daemon inlines the sections selected
  by each page's `data-sections`, so a page arrives in one response.
- `native-tv/assets/google-tv/pages/` - `dashboard/` and `setup/`, which the
  Homebrew Channel app opens in the TV's browser, and `setup-phone/` (served to
  the phone by the one-time link). The TV's own screens, Home included, are the
  native client's (`src/ui`); there is no web Home page. English only
  (`shared/common.js`). `native-tv/assets/dashboard-app/` is the TV app
  itself (a native app: the C++ client opened on the dashboard screen, with
  `install-app.sh` for it and the other apps), `native-tv/assets/home-app/` the
  black stub that opens the client's Home screen, `native-tv/assets/splash-app/`
  the splash the boot key filter opens.
- `native-tv/assets/screensavers/` - QML.
- `native-tv/lifecycle/` - `tvwebctl` (start, stop and watchdog on the TV),
  `50-tvweb.sh` (the boot hook) and `deploy.sh` (installs over ssh; its
  `FILES` list must name every file the TV needs).
- `hbc/` and `scripts/build-ipk.py` - the Homebrew Channel package.
- `native-tv/` otherwise - `ARCHITECTURE.md` defines module boundaries and
  `tools/runtime_payload.py` stages the managed
  runtime and its ABI/hash manifest for SSH and Homebrew packages;
  `contract/routes.json` lists every route and its owner.
- `native-tv/tests/` - C++ unit tests, `daemon_runtime.py` (the daemon against
  a stand-in `luna-send`), `contract/` (route manifest, CSRF policy, test
  matrix, goldens), `ui/` (the dashboard scripts run on the host), `e2e/` (the
  cases that need a real TV, skipped unless `NATIVETV_TV_HOST` is set) and
  `fixtures/tv/`, real stats from a B8 (webOS 4), a CX (webOS 5), a G4 and a C4
  (webOS 9).

Before pushing, run what CI runs: `python3 native-tv/build.py --service-only
--test` and the same with `--sanitize --out native-tv/build-host-ninja-asan`;
the ARM build (`build.py --sdk ... --target nativetv-daemon`) when daemon
code changed; the contract checks in `native-tv/tests/contract/`
(`check-route-manifest.js`, `check-csrf-policy.py`, `check-test-matrix.js`);
the UI suites in `native-tv/tests/ui/`; `mkdocs build --strict` when `docs/`
or `mkdocs.yml` changed; and `scripts/check-ui-ids.py`,
`check-strings.py`, `check-screensavers.py`, `check-drift.py`,
`check-entity-stability.py`, `check-es5.py` (the TV pages), `check-retired-files.py` and
`check-entities.py --stats native-tv/tests/fixtures/tv/stats-b8-webos4.json`.
Hardware rendering changes also need a TV build and capture review.

For background, open the doc for the area rather than README.md:
docs/development/implementation.md (platform quirks and how each subsystem works),
docs/integrations/home-assistant.md (setup) and docs/integrations/home-assistant-entities.md (entities), docs/administration/tv-changes.md (what the server changes
on the TV), docs/development/tv-specs/index.md (hardware of the tested TVs),
docs/development/tv-research.md (how LG's software was found to work, to
read before investigating it again and to add findings to), docs/administration/acr.md,
docs/administration/security.md and docs/development/strings.md.

# TV design language

The TV client follows Google's TV Design Kit (Material 3), and Material 3
Expressive is the reference for new TV screens and restyles. Read
docs/development/material-3-expressive.md before adding or restyling one: it
holds what Expressive specifies for type, colour, shape, spacing, focus,
motion and icons, what the client uses today, and the differences.

Take values from `native-tv/design/tokens.json` (`tools/gen_tokens.py`
generates `tokens.hpp`), never literals in a screen's code. A value that
departs from that page is a decision: record it there with its reason. A
rendered change gets a check on a real TV, and the screens it touches are
looked at in `tests/golden/` before the goldens are updated.

# Writing conventions

Applies to commit messages, PR descriptions, release notes, README and docs.

## Keep it short

State what changed and why. Stop. A PR description is usually a paragraph or
two, not a report.

Length should track the size of the change, not the effort behind it. A
one-line fix gets one line.

## PR descriptions: summary first

A PR description must be readable at a glance by someone who has not seen
the code. Open with a plain-language summary of what changes for the people
using Native Tv: short sentences or a few bullets, no file names, function
names or PR numbers. Put the technical detail - the code changes, related PRs,
config keys, what was left out - under a `## Technical details` heading after
it.

A small change can skip the heading: one or two plain sentences are enough.

## Don't narrate the work

No process commentary: what was tried, what was ruled out, what turned out to
be a false alarm. If a check found nothing, that is not a finding worth
writing down.

The exception is a PR's **Testing** section, which docs/development/contributing.md asks
for: the TV and webOS version the change was tried on, what was done and seen,
and what wasn't tested. State them as facts, briefly, not as a story.

Record decisions and their reasons, not the route taken to reach them.

## Third person, always

These are project artefacts, not messages to a reader. Never "you", "your TV",
"as you asked". Write "the TV", "the panel switch".

## No filler structure

Don't add headings, tables or bullet lists to a short change (the
`## Technical details` split above is for larger PRs). Tables are for
genuine matrices - a compatibility list, a unit reference - not for restating
three sentences.

Don't list things that did not change.

## Comments in code

Explain why, where a reader would otherwise wonder or get it wrong: a platform
quirk, a non-obvious ordering constraint, a value that looks wrong but isn't.
Skip comments that restate the code.

Keep the measured facts that justify a decision (an observed value, a version,
a threshold). Drop the story around them.

## webOS versions

Name a webOS version as the TV reports it (`/etc/issue`, the dashboard): webOS
4.4, webOS 9, webOS 10. LG's year names (webOS 22 to 26) are the reported major
version plus 15; give one only in brackets, as "LG's webOS 25", where an owner
would look for it. A TV's model year is not its webOS: the C2 runs 9.2.
docs/getting-started/tested-tvs.md#webos-version-numbers has the table.

## Names in code

Name a function, or anything used beyond a few lines, for what it does:
`readTrimmed`, `sendCommand`, not `rd` or `c`. A name read away from its
definition has to carry the meaning, and an abbreviation is hard to search
for. Short names are fine for locals used within a few lines, and for the
dashboards' `q()` and `t()`, which every script uses.

# Strings on the dashboards

Text shown on the phone/browser dashboard is keyed: `data-t` in markup,
`t('key', 'English', {vars})` in script, and the daemon's messages it shows
`say::msg("srv.key", "English", vars)`. The dashboard is in English and Spanish:
a translation is an entry in `native-tv/assets/i18n/es.json` that records the English
it was made from, and English that has changed since is shown as English until the
entry is redone, so editing English never waits on a translation. Removing or
renaming a key takes its entries with it, or the check fails. The Spanish is partly
machine-translated; say so wherever it is described. The TV pages and the native TV
client are English only. docs/development/strings.md has the rules, and
`scripts/check-strings.py` checks them. The parts already converted are listed in
`CONVERTED` in that script, and new text in one of them needs a key.
