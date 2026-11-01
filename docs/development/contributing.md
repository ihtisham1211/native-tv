# Contributing

The server is a C++14 daemon in `native-tv/`; no Node runs on the TV, and the
build needs none. The [layout](https://github.com/ihtisham1211/native-tv/blob/main/CLAUDE.md#code-map)
and [module boundaries](https://github.com/ihtisham1211/native-tv/blob/main/native-tv/ARCHITECTURE.md)
say where a change goes.

## Building and testing

The host build and its tests need a C++14 compiler, CMake, pkg-config, Python 3
(standard library only), json-c and libcurl, and nothing else:

```bash
# macOS, after xcode-select --install
brew install cmake pkgconf json-c
# Debian/Ubuntu
sudo apt install g++ cmake ninja-build pkg-config python3 libjson-c-dev libcurl4-openssl-dev
```

`python3 native-tv/build.py --check` lists what a computer lacks and the command that
installs it; `build.py` runs the same check before it starts CMake. Then:

```bash
python3 native-tv/build.py --service-only --test
python3 native-tv/build.py --service-only --test --sanitize --out native-tv/build-host-ninja-asan
```

`ninja` and `ccache` are used when installed and make rebuilds faster. `node` runs three
more tests (the dashboard scripts and the metrics worker's output), the route and
test-matrix checks and the emulator; without it those tests are listed as skipped (CI fails
without it). `openssl` runs one more test and
is skipped the same way. Building without `--service-only` adds the TV client, which needs SDL2,
SDL2_image, SDL2_ttf and FreeType:

```bash
brew install sdl2 sdl2_image sdl2_ttf freetype
sudo apt install libsdl2-dev libsdl2-image-dev libsdl2-ttf-dev libfreetype6-dev libgles2-mesa-dev
```

The ARM build, which `deploy.sh` and the Homebrew package need, takes the webOSbrew SDK
([getting it](../getting-started/install.md#get-the-sdk)) and, on the computer, only
CMake, pkg-config and Ninja or make: `python3 native-tv/build.py --sdk /path/to/arm-webos-linux-gnueabi_sdk-buildroot`
builds the daemon, its worker and the TV app. To check only that a daemon change
cross-compiles, add `--service-only --target nativetv-daemon`. The Homebrew package
(`scripts/build-ipk.py`) also needs LG's `ares-package`, which it runs with `npx` (so Node)
unless it is on `PATH`.

[Testing and CI](testing.md) lists every check CI runs and how to run
each one locally. Code compiles with `-Wall -Wextra -Werror`.

A new route goes in `native-tv/contract/routes.json` with its owner, auth and CSRF
policy; `check-route-manifest.js` and `check-csrf-policy.py` fail without it. New
logic takes its dependencies as `std::function`s so a test can script the TV; real
TV readings go in `native-tv/tests/fixtures/tv/`.

The pages the TV's own browser shows are strict ES5 and CSS for Chromium 38;
only `native-tv/web/ui/` may use modern JavaScript.

Text on the phone and computer dashboard, and the daemon's messages it shows, is keyed
and has a Spanish translation in `native-tv/assets/i18n/es.json`. A change to such text
changes its English in one place and leaves the Spanish entry outdated: the English shows
until the entry is redone (`scripts/check-strings.py` lists them), so a pull request need
not translate, but may. A string removed or renamed takes its entry with it, or the check
fails. [Strings](strings.md) has the rules and how to add a language.

## Docs move with the change

A change updates the docs that describe it, in the same pull request: the page for
the tab it touches under `docs/dashboard/`, [what the server changes on the
TV](../administration/tv-changes.md), [testing](testing.md) when tests, scripts or CI
change, and [TV research](tv-research.md) for anything learned about
how LG's software works.

## README and search

`README.md` is what GitHub and search engines show first. A pull request that adds a
feature or an integration adds one bullet to its **Features** list, linking the docs
page, and the same line to `docs/index.md`. One that changes which TVs are supported
updates the **Compatibility** table in the README and [tested TVs](../getting-started/tested-tvs.md).
Claims in the README match what has been verified: the native server is verified on the
TVs listed there, not on every TV an earlier release was reported from.

The README's opening paragraph names what people search for (rooted LG webOS TV, ACR,
Home Assistant, MQTT, OLED burn-in, debloat). Each docs page has a `description` in its
front matter, and the pages people land on from a search have a `title` with the
subject in it. Quote a description that contains a colon, or the front matter is dropped
without a warning.

Maintainers keep the GitHub description and topics in step with the README:

```bash
gh repo edit ihtisham1211/native-tv \
  --description "Open-source dashboard, privacy controls and Home Assistant MQTT bridge for rooted LG webOS TVs: turn off ACR and ads, debloat apps, protect OLED panels, remap the remote." \
  --homepage https://ihtisham1211.github.io/native-tv/
```

The topics are `webos`, `webos-tv`, `lg-tv`, `lg-webos`, `lgtv`, `lg-smart-tv`, `webosbrew`,
`webos-homebrew`, `homebrew-channel`, `home-assistant`, `mqtt`, `privacy`, `acr`, `smart-tv`,
`oled`, `burn-in`, `debloat`, `adblock`, `smart-home` and `prometheus`, the most GitHub
allows. Swap one with `--remove-topic` and `--add-topic` rather than adding a twenty-first.
The social preview image, `docs/assets/social-preview.png`, is uploaded under the
repository's **Settings → General → Social preview**.

## Screenshots

Screenshots of the TV app in `docs/screenshots/` and `README.md` are taken on a real TV,
never with the host's renderer: the images in `native-tv/tests/golden/` are test fixtures
drawn by the host from made-up data, and are not documentation. `scripts/tv-screenshot.sh`
runs the installed client once on the TV, with the TV's own server and feed, plays a key
script from `scripts/screenshots/` and brings the last frame back. Stop the Home client first
(once, then wait 62 s before the next stop: the crash breaker counts three within 120 s).

| File | Command arguments |
|---|---|
| `tv-home.jpg` | `--screen home --ms 9000` |
| `tv-apps.png` | `--screen home --ms 10500 --script scripts/screenshots/apps.json` |
| `tv-apps-arrange.png` | `--screen home --ms 12000 --script scripts/screenshots/apps-arrange.json` |
| `tv-home-edit.jpg` | `--screen home --ms 11500 --script scripts/screenshots/home-edit.json` |
| `tv-details.jpg` | `--screen home --ms 10500 --script scripts/screenshots/details.json` |
| `tv-quick-settings.jpg` | `--screen home --ms 10500 --script scripts/screenshots/quick.json` |
| `tv-settings.png` | `--screen dashboard --ms 8000` |
| `tv-privacy.png` | `--screen dashboard:privacy --ms 8000` |
| `tv-app-setup-welcome.png` | `--screen setup:welcome --ms 8000` |
| `tv-app-setup-summary.png` | `--screen setup:summary --ms 8000` |

The key scripts press Right a fixed number of times along the Home bar, so they assume the
TV shows the Home and Apps tabs only; with Inputs or Live on, add a Right to `quick.json`.
Look at a screenshot before it goes in: it shows the TV's own titles, apps and names. The Live screen's Network card shows the Wi-Fi network's name, so no Live screenshot is published.
The wizard's PIN, network and phone pairing screens (`tv-app-setup-pin.png`, `-network`,
`-handoff` and `tv-app-pin-prompt.png`) are reached by pressing through the wizard, which
writes the TV's settings, so they come from a TV before it is set up.

## Pull requests

Open with a plain-language summary of what changes for the people using
Native Tv, then a `## Technical details` section for larger changes. Third person,
short, no process commentary.

Every pull request has a **Testing** section: the TV and webOS version the change
was tried on, what was done and seen, and what was not tested. Say so where a change
could only be checked on the host. Names of webOS versions are as the TV reports
them (webOS 4.4, 9, 10); see [tested TVs](../getting-started/tested-tvs.md#webos-version-numbers).
