  # Roadmap

  What the TV app is still missing next to Google TV, whose layout it follows. The web dashboard is not affected.

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
