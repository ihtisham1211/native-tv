# Roadmap

Planned features come first, most useful for the effort first. After them, what the TV app is still missing next to Google TV, whose layout it follows; the web dashboard is not affected by those.

## Planned features

1. **List the package in the Homebrew Channel.** Releases carry the `.ipk` and `io.github.ihtisham1211.nativetv.manifest.json`, which installs directly, and the dashboard's Updates card installs a newer one itself on a server that came from the package, so the Homebrew Channel's list is no longer needed to update. The Homebrew Channel's own app list does not name it yet. Listing is a pull request to [webosbrew/apps-repo](https://github.com/webosbrew/apps-repo) that adds `packages/<name>.yml` (title, icon, category, description, pool `main`, and `manifestUrl` pointing at `releases/latest/download/` for that manifest), as the [publishing guide](https://www.webosbrew.org/develop/guides/publishing/how-to/) sets out; not yet opened.
2. **Remote key actions on a TV.** Screen off and Power off are choices for a remote key where the key handling is the JavaScript filter, tried only against stand-ins: a TV has to show whether the filter's role may call the power service's `powerOff` and `turnOffScreen` (it answered another `tvpower` call on a B3). "Home Assistant" is a choice for the keys whose presses are reported (the color keys, the streaming buttons, Movies, Alexa, Google Assistant and the microphone): the filter does nothing for it and the daemon reads the press from the remote's input device, as Glasshouse found a C2 (webOS 9.2.2) still delivers a press the filter has taken. A TV has to show that this holds for each of those keys, and that the codes in `domain/names.cpp` are the ones a remote sends.
3. **OLED Care and Apple TV on a real TV.** Both themes have been checked in the client's tests, on an emulator and on a webOS OSE virtual machine, not on a TV (webOS 4.4, 9 or 10). OLED Care: whether the near-black surfaces separate on a real panel, the effect on wear, and clicks from the Magic Remote pointer while the layout drifts. Apple TV: how the shadow, glass and text look at viewing distance, pointer clicks on the landscape tiles, and a pointer click in the band at the top of Home, which brings the bar in.
4. **The rest of the Apple TV theme.** `n-apple-tv` lays Home out as tvOS's Home (the current pick full screen, the apps on a glass dock and then a grid, the bar as a pill that Up opens) and restyles the app tiles and the focus; Apps, Inputs, Live, Settings, Details, Quick settings and the profile screen keep the Google TV layouts. Still to do: Home, Apps and Inputs as one scroll, with the tabs left out of the bar; Settings in two halves (title and icon on the left, one list on the right); their own layouts for the other screens; larger poster cards; tilt and layered-icon parallax and a blurred glass, which the renderer does not have; a Top Shelf that plays video; and a typeface other than Google Sans Flex (not SF, which is Apple's).
5. **A Home Assistant select for the TV theme.** The theme is chosen on the TV and in the dashboard's Server tab; no entity offers it yet.

## Found testing the B3 by hand

- **App icon.** The Native Tv tile on LG's launcher does not fill its tile the way the channel tiles do.
- **Privacy.** The Overview should be easier to use (someone who only wants ad tracking off).

## Home, from the second hands-on test

- **Look on a TV.** The five findings from the Home captures (the carousel repeating the first row, small faint row labels, unlabelled app circles, the Netflix and YouTube rows four rows down, 56 px top-bar buttons) are changed in the client and checked in `tests/golden/home-*.png` on the host; they are still owed a look on a TV at viewing distance.
