# Contributing

The server is a C++14 daemon in `native-tv/`; no Node runs on the TV or in the
build. The [layout](https://github.com/ihtisham1211/native-tv/blob/main/CLAUDE.md#code-map)
and [module boundaries](https://github.com/ihtisham1211/native-tv/blob/main/native-tv/ARCHITECTURE.md)
say where a change goes.

## Building and testing

```bash
python3 native-tv/build.py --service-only --test
python3 native-tv/build.py --service-only --test --sanitize --out native-tv/build-host-ninja-asan
```

The ARM build, which `deploy.sh` and the Homebrew package need, takes the webOSbrew SDK:
`python3 native-tv/build.py --sdk /path/to/arm-webos-linux-gnueabi_sdk-buildroot`, which builds the daemon, its worker
and the TV app. To check only that a daemon change cross-compiles, add `--service-only --target nativetv-daemon`.
[Testing and CI](testing.md) lists every check CI runs and how to run
each one locally. Code compiles with `-Wall -Wextra -Werror`.

A new route goes in `native-tv/contract/routes.json` with its owner, auth and CSRF
policy; `check-route-manifest.js` and `check-csrf-policy.py` fail without it. New
logic takes its dependencies as `std::function`s so a test can script the TV; real
TV readings go in `native-tv/tests/fixtures/tv/`.

The pages the TV's own browser shows are strict ES5 and CSS for Chromium 38;
only `native-tv/web/ui/` may use modern JavaScript.

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

## Pull requests

Open with a plain-language summary of what changes for the people using
Native Tv, then a `## Technical details` section for larger changes. Third person,
short, no process commentary.

Every pull request has a **Testing** section: the TV and webOS version the change
was tried on, what was done and seen, and what was not tested. Say so where a change
could only be checked on the host. Names of webOS versions are as the TV reports
them (webOS 4.4, 9, 10); see [tested TVs](../getting-started/tested-tvs.md#webos-version-numbers).
