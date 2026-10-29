# Contributing

The server is a C++14 daemon in `native-tv/`; no Node runs on the TV or in the
build. The [layout](https://github.com/ihtisham1211/nativeTv/blob/main/CLAUDE.md#code-map)
and [module boundaries](https://github.com/ihtisham1211/nativeTv/blob/main/native-tv/ARCHITECTURE.md)
say where a change goes.

## Building and testing

```bash
python3 native-tv/build.py --service-only --test
python3 native-tv/build.py --service-only --test --sanitize --out native-tv/build-host-ninja-asan
```

The ARM build, which `deploy.sh` and the Homebrew package need, takes the webOSbrew SDK:
`python3 native-tv/build.py --sdk /path/to/arm-webos-linux-gnueabi_sdk-buildroot --service-only --target nativetv-daemon`.
[Testing and CI](development/testing.md) lists every check CI runs and how to run
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
TV](TV-CHANGES.md), [testing](development/testing.md) when tests, scripts or CI
change, and [TV research](development/TV-RESEARCH.md) for anything learned about
how LG's software works.

## Pull requests

Open with a plain-language summary of what changes for the people using
Native Tv, then a `## Technical details` section for larger changes. Third person,
short, no process commentary.

Every pull request has a **Testing** section: the TV and webOS version the change
was tried on, what was done and seen, and what was not tested. Say so where a change
could only be checked on the host. Names of webOS versions are as the TV reports
them (webOS 4.4, 9, 10); see [tested TVs](tested-tvs.md#webos-version-numbers).
