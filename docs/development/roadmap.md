# Roadmap

Planned features come first, most useful for the effort first. After them, what the TV app is still missing next to Google TV, whose layout it follows; the web dashboard is not affected by those.

## Planned features

1. **List the package in the Homebrew Channel.** Releases carry the `.ipk` and `io.github.ihtisham1211.nativetv.manifest.json`, which installs directly; the Homebrew Channel's own app list does not name it yet. Listing is a pull request to [webosbrew/apps-repo](https://github.com/webosbrew/apps-repo) that adds `packages/<name>.yml` (title, icon, category, description, pool `main`, and `manifestUrl` pointing at `releases/latest/download/` for that manifest), as the [publishing guide](https://www.webosbrew.org/develop/guides/publishing/how-to/) sets out; not yet opened.
2. **Remote key actions on a TV.** Screen off and Power off are choices for a remote key where the key handling is the JavaScript filter, tried only against stand-ins: a TV has to show whether the filter's role may call the power service's `powerOff` and `turnOffScreen` (it answered another `tvpower` call on a B3). "Send to Home Assistant" is not done, as a key the filter takes does not reach the server; it needs a way for the filter to tell the daemon, which a TV has to show.

## Found testing the B3 by hand

- **App icon.** The Native Tv tile on LG's launcher does not fill its tile the way the channel tiles do.
- **Privacy.** The Overview should be easier to use (someone who only wants ad tracking off).

## Home, from the second hands-on test

- **Review.** From the Home captures (`tests/golden/home-*.png`), to be tried on the TV:
  - The carousel and the first row, Top picks for you, show the same six titles one above the other.
  - Row labels (Top picks for you, Your apps) are small and low in contrast at TV distance.
  - Only the focused app circle shows its name; the others are unlabelled.
  - Available on Netflix and Trending on YouTube are the last two rows, four rows down from the top.
  - The avatar and the search and settings buttons are small targets at 56 px.
