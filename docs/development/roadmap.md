# Roadmap

What the TV app is still missing next to Google TV, whose layout it follows. The web dashboard is not affected.

## Settings

- The Settings page has no tabs and no quick-settings icon, both of which Google TV's Settings has.

## Found testing the B3 by hand

- **App icon.** The Native Tv tile on LG's launcher does not fill its tile the way the channel tiles do.
- **Settings screen.** It shows the Home bar (Home, Apps, Inputs, Quick settings), which belongs to Home, not to Settings.
- **Metrics, Live.** It should refresh by itself every 0.5 s (it reads every 4 s now), and Back from Live should return to where it was opened from, not to Home.
- **Names.** "Home screen" in the Apps settings means LG's own launcher and should say so; "Services" in the Apps settings are LG's bloatware background services and should be named for that.
- **Choice rows** (Game genre and the others that step through values) should show chevrons left and right, and Left and Right should move through the values once the row is chosen, not only OK.
- **Screen saver.** Start screen saver belongs on the Screen Saver tab; Options needs the time before the saver starts; a plain black screen saver is missing.
- **Colours.** The TV client has no theming engine: the Colours setting (and the text and accent colours the dashboard offers) is stored and served but nothing in the C++ client reads it. Remove it from the TV and the dashboard, unless a theming engine is built. The same goes for the Wallpaper and More rows of the Home settings.
- **Privacy.** The Overview should be easier to use (someone who only wants ad tracking off); Blocking needs the owner's own block list; LG ads needs one switch for all of its switches.
