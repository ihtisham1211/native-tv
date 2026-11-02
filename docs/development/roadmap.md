  # Roadmap

  Planned features come first, most useful for the effort first. After them, what the TV app is still missing next to Google TV, whose layout it follows; the web dashboard is not affected by those.

  ## Planned features

  1. **Install without building.** No release has been published yet, so installing needs the SDK and a build. Publish releases, so the `.ipk` the release workflow attaches can be installed directly, and list the package in the Homebrew Channel.
  2. **Block-list catch-up.** `nsu.lge.com`, `snu-dev.lge.com`, `tv.wiselg.com` and `elastic.lgwebostv.com` are not in the platform hosts, so blocking TV software updates can still leak.
  3. **Volume ceiling.** A maximum volume, a night limit and a start-up volume, kept by the daemon.
  4. **Keyboard box.** Type into the TV's text fields from the phone, through the TV's text-input service. It is also the base for typing into apps.
  5. **Home Assistant example fixes and a config generator.** The media-player example reports the volume as 0 to 100 where Home Assistant expects 0 to 1. A "copy my config" button would fill in the TV's address, MAC, topic prefix and device id.
  6. **Block-list export.** Download the block list in Pi-hole, AdGuard or hosts format, so the router can block too and the protection survives a factory reset.
  7. **TV report.** A redacted compatibility report (model, webOS version, what worked) to paste into an issue.
  8. **More remote keys.** Screen off, Power off and "send to Home Assistant" as choices for a remote key.

  ## Settings

  - The Settings page has no tabs and no quick-settings icon, both of which Google TV's Settings has.

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
