# Roadmap

Planned features come first, most useful for the effort first. After them, what the TV app is still missing next to Google TV, whose layout it follows; the web dashboard is not affected by those.

## Planned features

1. **List the package in the Homebrew Channel.** Releases carry the `.ipk` and `io.github.ihtisham1211.nativetv.manifest.json`, which installs directly; the Homebrew Channel's own app list does not name it yet. Listing is a pull request to [webosbrew/apps-repo](https://github.com/webosbrew/apps-repo) that adds `packages/<name>.yml` (title, icon, category, description, pool `main`, and `manifestUrl` pointing at `releases/latest/download/` for that manifest), as the [publishing guide](https://www.webosbrew.org/develop/guides/publishing/how-to/) sets out; not yet opened.
2. **Remote key actions on a TV.** Screen off and Power off are choices for a remote key where the key handling is the JavaScript filter, tried only against stand-ins: a TV has to show whether the filter's role may call the power service's `powerOff` and `turnOffScreen` (it answered another `tvpower` call on a B3). "Send to Home Assistant" is not done, as a key the filter takes does not reach the server; it needs a way for the filter to tell the daemon, which a TV has to show.
3. **OLED Care on a real TV.** The theme has been checked in the client's tests, on an emulator and on a webOS OSE virtual machine, not on a TV (webOS 4.4, 9 or 10): whether the near-black surfaces separate on a real panel, the effect on wear, and clicks from the Magic Remote pointer while the layout drifts.
4. **Apple TV theme.** A theme to be called `n-apple-tv` would need layout values in the theme record (`design/themes/`) and a typeface other than SF.
5. **A Home Assistant select for the TV theme.** The theme is chosen on the TV and in the dashboard's Server tab; no entity offers it yet.

## Found testing the B3 by hand

- **App icon.** The Native Tv tile on LG's launcher does not fill its tile the way the channel tiles do.
- **Privacy.** The Overview should be easier to use (someone who only wants ad tracking off).

## Home, from the second hands-on test

- **Look on a TV.** The five findings from the Home captures (the carousel repeating the first row, small faint row labels, unlabelled app circles, the Netflix and YouTube rows four rows down, 56 px top-bar buttons) are changed in the client and checked in `tests/golden/home-*.png` on the host; they are still owed a look on a TV at viewing distance.
