# Home

**Experimental.** Native Tv Home is a home screen of Native Tv's own, shown in place of LG's when the remote's Home or Back button is pressed and when the TV switches on. The **Native Tv Home** tab, `/?tab=home`, under Apps, switches it on and off and arranges it. It has been tried on webOS 9 and 10. On webOS 11 it is offered once `"allowExperimentalHome": true` is set in `config.json`. Elsewhere neither dashboard shows it, unless it is still switched on or LG's Home has not yet been fully restored.

The screen is laid out as Google TV's home, after Google's TV Design Kit: a bar of tabs with the clock and date at the right, a featured carousel across the top (the kit's Immersive layout: its artwork fills the top right and fades into the black, with the text at the left), and rows of cards below it. Until Home has its first content, a boot screen of slowly moving colours shows the Native Tv mark. The tabs are:

* **Home** leads with **Your apps**, a row of round icons (the [themed icon](apps.md) where there is one, else the app's own), with the name under the one in focus. With a TMDB key set (below), focus starts on **Top picks for you** under it and the carousel shows the focused title with its artwork, its facts (film or series, year, rating) and summary, moving on by itself while no key is pressed, with a dot for each title; rows of **Trending films this week** and **Trending series this week** follow, as 16:9 cards whose facts appear on focus. Only what is trending this week is listed, and only titles that can be opened somewhere (a Netflix title, or a YouTube trailer). With Netflix links on (below), **Available on Netflix** lists the trending titles that have a Netflix link, and OK opens the title in Netflix itself. With a YouTube key, **Trending on YouTube** is added. A focused app shows its icon and name in the carousel's place, on a tint of its colour, with the picture dimmed behind it.
* **Apps**: every app on LG's Home as a round icon with its name, six to a row, from just under the bar (the page has no title of its own); with the row's Order set to **By kind**, one row per kind instead (Movies & TV, Music & audio, Games, Smart home, LG & TV, Tools, Other), in the order chosen under **Kinds**, three rows in view and the page following the focus, and holding OK on a row there lifts it so Up and Down move it, OK saves and Back puts it back. This tab has its own hidden apps, apart from the Home tab's **Your apps** row: an app hidden on one is still on the other, and the page by kind drops a row whose apps are all hidden. Holding OK on an app of the grid starts [editing the apps](#editing-the-apps). LG's own promotions (LG Channels, Sports, Always Ready, Alexa, Camera) start hidden on both. An app hidden under [Hide home screen system apps](apps.md) is gone from both, since the TV no longer lists it; the two lists only choose among the apps the TV still shows. A layout saved before this tab had a list of its own starts it as a copy of the Home row's, so what was hidden stays hidden here until it is changed. With [History](advanced.md) on, new apps are placed by how much each is used; otherwise they follow LG's order. **Notifications** and **Search** open LG's notification list and search, which LG's Home reaches from its sidebar. The last tile, **LG Home**, opens LG's Home screen and cannot be hidden per tile; **Show the LG Home tile** leaves it out. The **Native Tv** tile, on this tab and in the apps row of the Home tab, opens the TV app's Settings instead of launching an app, since Native Tv Home is that app and already in front.
* **Inputs** (left out when **Show inputs** is off): every HDMI input. An input with devices on it has a tile for each device the TV knows over HDMI-CEC, saying which device it is behind ("HDMI 2, through STEREO 70s"); an input with nothing connected is greyed out. Choosing a tile switches the TV to that input. Nothing is sent to the devices themselves.
* **Live** (**Show Live** is off by default; with it on, the tab appears once the TV has channels): the channels added here, in the order they are listed, each with a LIVE badge and how far through the programme on now it is, where the TV's guide has it. Choosing one tunes it.

![Native Tv Home on a B3: the featured title, Your apps and Top picks for you](../screenshots/tv-home.jpg)

![The Apps tab on a B3 with the apps sorted by kind](../screenshots/tv-apps.png)

With a TMDB key, **OK** on a film or series opens its details: year, rating,
runtime, genres, overview, cast and **More like this**. **Open on Netflix**
launches the title in Netflix when it is there (see below); **Trailer** opens
YouTube when a trailer is available. These are the only two apps a title opens in, and the feed lists only titles that have one of them.
**Full overview** shows the complete summary, a page at a time with Up and Down.
A title from **More like this** opens on top of the one before, up to eight
deep, and Back returns through them to the original row or search result, with
the focus where it was.
The remote's **Red** key opens a focused title in Netflix immediately, without the details.

![A title's details on a B3: year, rating, genres, overview, cast, the Trailer button and More like this](../screenshots/tv-details.jpg)

**Search**, after the gear in the bar, finds films and series through TMDB.
The on-screen keyboard, the one Setup uses, has Shift, a **123?** key for
symbols, Delete, keys that move the caret and a space bar, with **Clear** under
it; a physical keyboard also works. Search starts after two
characters and a 350 ms pause. The results are cards in two columns; **Results**
(the last key) moves to them, Up and Down scroll through them, OK opens details,
and Back returns to the keyboard and then Home. A failed search keeps the query
and offers **Retry search**. Search and title details are hidden without a TMDB key.
With `"netflixDeepLinks": true` in `config.json`, details can also show
**Open on Netflix**. The server uses TMDB's Wikidata identifier and
[Wikidata's Netflix ID (P1874)](https://www.wikidata.org/wiki/Property:P1874)
to find the title. Netflix must be installed. Its native launch receives
`m=https://www.netflix.com/title/<id>&source_type=4`. A missing or ambiguous
mapping leaves the action out; saved mappings work offline. Availability
still depends on the Netflix account and region. The option defaults off.

The Magic Remote's pointer works the same way: pointing at a card or a tab focuses it, and clicking acts. A tab opens on the click, the carousel opens what it is showing, and its dots move the spotlight on. Hovering does not move the rows down the screen as the keys do, so a card stays under the pointer; where a row slides a card aside to make room for the focused one, the click still opens that focused card.

The **gear**, after the tabs and Search (Up to the bar, then Right), opens **Quick settings**, a drawer that slides in from the right edge over Home. It lists the TV's **Volume** (Left and Right change it, OK mutes), **Picture mode**, **OLED light** (**Backlight** on an LCD; Left and Right change it by 5), **Sound output**, **Input** and **Sleep timer**, where OK opens the choices (on Picture mode, Sound output and Sleep timer, whose value sits between two chevrons, Left and Right step to the previous and the next one without opening them), then **Screensaver**, **Screen off**, **Recent apps**, **Profile** and **Home settings**. The drawer shows what it read last time at once, with a turning circle where a value has not been read yet, and reads the TV again behind it. Its changes go to the TV as the Control tab's do, so they need `allowControl`, which is on by default. Every change shows the moment the key is pressed and is sent behind it: a held key sends the newest value at most every 120 ms, a late answer never moves the value back, and one read after the last answer settles it on what the TV says (a volume the TV capped shows as capped). A change the TV refuses, or does not answer within eight seconds, goes back with a short message that gives the reason. **Home settings** are switches for the clock (24-hour), app names, the Live and Inputs tabs, the LG Home tile and Picture details; each is saved as the dashboards' Home tab would and shows on Home at once. Back goes up a level and then closes the drawer, which slides out and leaves Home as it was, with the gear in focus; the pointer works too, and a click beside the drawer closes it.

**Profiles** give each person a Home layout of their own: the rows and tiles that are hidden or moved, the tabs and the clock. The TV starts with one profile, named after the TV (B3, C4, or LGTV where the model is not known) until it is renamed. A coloured avatar with the profile's first letter shows before the tabs, and the focus reaches it with Left from the first tab, also while there is one profile; it opens **Who's watching?**, a page of round avatars where OK on a profile switches to it and **Add profile** adds one (up to five) and switches to it, with a fresh layout. The chosen avatar is in use at once, with a turning circle until the server answers, and a refusal gives the old one back with the reason. **Profile** in Quick settings opens the same page. Under the avatars, a row of six swatches sets the colour of the profile in use (Down from the avatars, Left and Right between them, OK to pick; the avatar changes at once and goes back with a message if the server refuses). **Rename profile**, below them, uses the same on-screen keyboard, with a 20-character limit and the avatar shown as it will look; Back cancels; Save returns to the avatars with the new name on them at once, and a refusal puts the old name back with the reason. Removing a profile is on the browser dashboard's Home tab, under **Profiles**; removing asks for a second press, and the last profile cannot be removed. The TV app's Home section has a **Profile** row whose OK steps to the next profile, or adds one where there is only the first. Everything else on both dashboards' Home tabs, and arranging on the TV, changes the profile in use. Adding a profile does not copy the layout in use, and what Home shows from TMDB or YouTube is the same for every profile.

The highlight follows the remote immediately; the carousel's title and artwork
follow once navigation settles briefly. Returning from an app keeps the current
tab and selection while Home refreshes its content in the background.

A tap of **Back** only goes back: from the Apps, Inputs or Live tab to the first tab with the focus in its content, then from a lower row to the first row, then to the bar, where it stops. Holding **Back** (650 ms) is the only way to **Recent apps**, from any tab: the apps the TV lists as recent, newest first, each with a snapshot of how it was left (its splash screen, for an app left before the TV last restarted). Holding OK opens nothing. OK opens one. Down from a card reaches its **Close**, and up from the cards reaches **Close all**; closing an app takes it off the list and stops it. Apps the TV keeps running in the background without a card, such as YouTube when it is kept ready, are not listed. Back again closes the panel. On a TV that gives no list, Back moves to the first tile instead.

<span id="editing-the-apps"></span>**Editing the apps** is done from the remote, as in Google TV's edit mode. Holding **OK** on an app, in **Your apps** on Home or in the Apps grid (or in the Apps page's one row when only one kind has apps), starts it: Home's row reads "Moving apps", a **Hide from Home** button (**Hide from Apps** on the Apps page) appears at the top right (the Apps page moves down to make room for it) and Left and Right (Up and Down in the grid) swap the app with its neighbour. With the apps sorted by kind an app swaps only with the apps of its own kind and the apps stay by kind; otherwise saving makes the order **Your order**. **OK** or **Back** saves, **Up** from the first row goes to the button, and OK on it takes the app off that list (it stays installed and on LG's Home, and on the other list: Home's row and the Apps page each have their own hidden apps); **Red** does the same without the button. The apps hidden from the list being edited follow the others, dimmed, while moving: Right at the end moves on to them, the button reads **Show on Home** (**Show on Apps**) on one, and OK puts it back. They can also be shown again in the TV app's Home section and on the browser dashboard. The LG Home tile cannot be hidden this way. Holding OK anywhere else on Home does nothing, and on the Apps page by kind it lifts the row under the focus to move the kind.

![Editing the apps on Home: the Moving apps row and the Hide from Home button](../screenshots/tv-home-edit.jpg)

![Moving a kind on the Apps page: the row is lifted and Up and Down move it](../screenshots/tv-apps-arrange.png)

Keys on Native Tv Home:

| Key | On a tab's content | On the bar |
|---|---|---|
| Arrows | Move the focus | Move between the tabs, avatar, search and the gear |
| OK | Opens the focused app, input, channel or title | Opens the tab or the page |
| Hold OK | An app: starts editing the apps (a kind's row on the Apps page by kind: lifts it); anything else: nothing | Nothing |
| Red | Opens the focused title in its app at once, as OK does for an app, input or channel | Nothing |
| Back | Other tab: first tab. First tab: lower row to the first row, then to the bar | Nothing |
| Hold Back | Opens Recent apps | Opens Recent apps |

While apps are being edited or a kind's row is lifted, OK saves, Back saves (editing) or puts the row back (a lifted kind), and holding Back is ignored.

OK acts once when released. Back also cancels a panel still loading. A close
that cannot be confirmed shows an error and keeps the cards for retry.

With **Picture details** switched on, on the tab or in the TV app's Home section, a line in the top right of the carousel describes the picture of what was last on. It is off by default. For an HDMI input it gives the signal the device is sending (resolution, refresh rate, chroma and bit depth, HDR10, HLG or Dolby Vision, and VRR and low latency when on), read while Native Tv Home is on screen, and the picture mode the TV gives that signal. LG keeps a mode for each input and dynamic range, so HDMI 2 and HDMI 4 can differ, and an input switching from SDR to HDR10 shows its HDR mode. An input with nothing sending shows no line. For an app, the plate gives the last video it played, noted while it played, as Last video (a Dolby Vision film on Netflix shows Dolby Vision and its mode). A channel's signal is not read, so it shows only the mode it gets in SDR, which nearly all broadcasts are. An app that has played nothing since the server started shows no plate.

App video tracking runs while Native Tv Home and **Picture details** are on and the TV is out of standby, including before Home's first visit after a server restart. Switching either off, or the TV going into standby, stops it; videos already noted are kept. Returning to Home applies **Picture details** as soon as its layout arrives; a delayed picture reply cannot bring the plate back or replace a different source's details.

The separate native TV client renders the picture plate and recent-app cards
in OpenGL ES using the existing Home APIs. It preserves the previous video
source when opened. Source tracking still requires the Home service to be
enabled; standalone native launch does not enable Home replacement.

The background is black. The browser dashboard can add an image for each time of day (morning 06:00, afternoon 11:00, evening 17:00, night 21:00): choose a **Time of day**, select a JPEG, PNG or WebP file up to 25 MB, then press **Upload image**. The browser fits it within 1920 × 1080 pixels, the size Native Tv Home is drawn at, and prepares a JPEG before sending it to the TV. Images fill the screen and may be cropped. **Remove uploaded images** puts black back. None are included with Native Tv.

Uploads are stored in `/var/lib/tvweb/home-wallpaper-uploads`, outside the deploy bundle. A failed or interrupted upload leaves the previous image intact. The directory is removed when Native Tv is uninstalled; the layout backup does not include image files.

The TV's screen saver, LG's or one chosen on the [Screensaver](screen-savers.md) tab, is left as it is set.

While Home is hidden, periodic source, guide, picture and catalogue refreshes
pause, along with routine rendering. Returning refreshes
the view immediately. The five-second recovery heartbeat continues while the
browser runs, including behind overlays.

**Show app names**, on both dashboards, shows names beneath the app icons by
default. Switching it off leaves icons only. App names remain available to
assistive technology. The choice
takes effect on Native Tv Home within a few seconds.

**Clock format**, also on both dashboards, chooses **12-hour** time with AM/PM
or **24-hour** time, the default. It applies to Home's clock and programme start
times; midnight is 12 AM and noon is 12 PM. The choice is saved on the TV and
does not change LG's clock settings. Resetting the layout keeps the choice.

**Show inputs**, **Show Live** and **Show the LG Home tile**, also on both
dashboards, choose the Inputs tab, the Live tab and the tile that ends the
Apps row. **Show inputs** and **Show the LG Home tile** are on by default;
**Show Live** is off by default, so the Live tab stays out until it is
switched on. Switching one off leaves only its own part out: the inputs, the
added channels and their individual hide settings are kept. Holding the
remote's Home button still opens LG's Home. **Reset the layout** switches
**Show inputs** and **Show the LG Home tile** back on and **Show Live** back
off.

The titles come from [The Movie Database](https://www.themoviedb.org/), with the
owner's own free API key (32 letters and digits, from themoviedb.org under Settings, API),
pasted into **Films and series** on the Native Tv Home tab of the dashboard and saved there: it is kept on the TV
and never shown again, and **Remove** takes it away. The titles are fetched once a day, in the morning, and kept
only if they have a YouTube trailer or, with `"netflixDeepLinks": true`, a Netflix link; the pictures are kept on the TV, so browsing the rows uses no internet. Without a key, the Home tab shows only what is on the
TV. TMDB asks that its data is credited: this product uses the TMDB API but is
not endorsed or certified by TMDB.

A `"youtubeKey"` (a free YouTube Data API key) adds **Trending on YouTube**, the
videos trending in the US; OK plays the video in YouTube's app. YouTube's own
personal suggestions cannot be read by anything but its app, so the row is the
public list.

## Switching it on and off

**Use Native Tv Home** asks for a second press, then puts Native Tv Home in LG's Home's place: the TV then opens it wherever it would open LG's Home, with the Home button, with Back out of the last app, and when it wakes on Home. LG's Home is not altered: Native Tv Home is laid over it while switched on. With Quick Start+ off, so that every switch-on starts the TV from cold, the one TV setting changed is LG's **Home Auto Launch**, switched off so that the TV does not show LG's Home as it starts; it is left as it is while Quick Start+ is on, which is followed whenever the TV is switched on or off, and **Use LG Home** puts it back as it was. **Use LG Home** takes it away again, and LG's Home is back after about three seconds. The TV app's own **Home** section offers the same, driven by the remote.

While a switch is in progress, further changes are refused with a retry message. A switch-off that is cut short is shown as **Retry restoring LG Home** on both dashboards, and is finished when the server next starts.

**Power-on screen**, on both dashboards while Native Tv Home is on, chooses
**Native Tv Home** or **Follow LG setting**. Native Tv Home holds LG's Home
auto launch off and opens Native Tv once it has taken Home's place, including
when LG was previously set to Recent Input. The TV's last input remains on
screen while a full shutdown starts up; Quick Start+ is not changed. An app or
input chosen during startup cancels the automatic opening. Follow LG setting
uses the earlier behavior described below. Switching Native Tv Home off puts
LG's saved auto-launch setting back.

Two ways lead back to LG's Home without switching anything off:

* the **LG Home** tile;
* holding the remote's **Home** button for a second or more; a short press on LG's Home then brings Native Tv Home back.

LG's Home opens about two seconds after either, and Native Tv Home takes its place again as soon as something else is opened.

Native Tv Home is the Native Tv app on the TV's home screen (the **App on the TV's home screen** switch on the Server tab), started by a screenless app in LG's Home's place, so it is offered only where that app is there and can draw: **Use Native Tv Home** is refused, with the reason, where the app is not on the home screen or its start-up check fails, and a server that starts with Home on and finds either leaves LG's Home in place. If the app is taken off the home screen while Home is on, or keeps stopping (three times within two minutes, or three times LG's Home is still in front 15 seconds after it came), LG's Home takes its place back by itself and Native Tv Home stays off it until the server restarts or **Use Native Tv Home** is pressed again. Home stays switched on in the meantime. The reason is in the server's log and in `nativeHome` of `/api/home`.

After the TV starts from cold, Native Tv Home takes LG's Home's place about 27 seconds in (a G3), 37 seconds on a G5. The same message is shown without a key press as soon as the boot hook starts, which is the earliest Native Tv's own code can run. Home pressed in that time leaves the TV on what it is showing, with that message; once Native Tv Home is in place, Home opens it as usual. Home is held like this for at most 90 seconds after the TV starts, so a TV where Native Tv no longer runs still reaches LG's Home. Anything else that opens LG's Home in that time, such as Back out of an app, shows LG's Home until Native Tv Home replaces it.

With Quick Start+ off, until Native Tv Home is in place the TV shows its last input, as LG's Home Auto Launch is off. Where it was on, Native Tv Home opens once in place, after a power-on with the remote or the TV's own button, as LG's Home would have; it is not opened after a restart from the menu, where LG's would not be either, or where another input or app has been chosen since the TV started. Switched back on within a few seconds, before the TV has powered off, it opens a second or so after the last input. With Quick Start+ on, switching on from standby is not a cold start: Native Tv Home is still in place, and the TV opens it at once.

**Hide LG's Home while the TV starts**, on webOS 9 only and off unless chosen, gives LG's Home an empty layout while Native Tv Home has its place, so that anything opening it in those first seconds shows a black screen instead. It asks for a second press to switch on, and takes effect at once. The layout is kept on the TV, not in Native Tv: if Native Tv stops running without switching Native Tv Home off, for example after a firmware update removes root or `/var/lib/tvweb` is deleted, LG's Home stays black at every start. Over SSH, `tvwebctl restore-nativetv-home` puts LG's Home back while Native Tv is installed. Where it is not:

```sh
rm -rf /mnt/lg/cmn_data/manifest /tmp/checkpoint/*com.webos.app.home* /mnt/lg/cmn_data/checkpoint/*com.webos.app.home* && reboot
```

LG's Home then starts on its default layout, without one set with [LG launcher layout](apps.md#lg-launcher-layout). A TV that already had the empty layout from an earlier build keeps the option on.

The Native Tv server answers about ten seconds later; Native Tv Home opened in between shows the Native Tv mark on black with a slowly turning ring, and fades into the home screen once the server answers. After 30 seconds without an answer it says so under the mark. It does the same whenever the server is not running, as during an update. Should the server stop, or fail three times in ten minutes, LG's Home is given its place back until the server is running again.

Over SSH, `tvwebctl restore-nativetv-home` does what **Use LG Home** does. Uninstalling Native Tv from the Homebrew Channel switches Native Tv Home off before deleting the server; a failed switch-off keeps the server for another attempt.

Earlier builds installed a separate Native Tv Home app and changed the TV's Home and start-up settings to it. The first start of a newer server puts back any of those settings that still name that app, gives the Home button back to LG's filters, and removes the app.

## Arranging

Apps and inputs come from the TV, so they are shown or hidden rather than added. Channels are the other way round: none is on Home until it is added, and one no longer wanted is removed. The Channels row on this tab lists those added, each with **Remove**, and below them **Add channels** finds the rest of the TV's channels by name or number. The TV app's Home section does the same with OK. A change made on either dashboard shows on Native Tv Home within a few seconds, while it is on screen.

Each row has an **Order**: **Your order**, as arranged, or **Last opened**, with the tile opened last first. Last opened counts an app, input or channel however it was opened, from Native Tv Home or not: an input switched to from the remote counts for every device on it, and the channel Live TV was left on counts as opened. Tiles not yet opened follow in the row's own order, and **LG Home** stays last. What was opened is recorded only for a row set to Last opened, only while Native Tv Home is on, and forgotten when the row goes back to Your order. On this tab, a Last opened row's tiles cannot be moved; the TV app's Home section has the same choice at the top of each row's section.

The Apps row has a third order, **By kind**: the Apps screen becomes one row per kind of app, and Your apps on Home follows the same grouping. Each app's kind is fixed by what it is (Netflix and LG Channels are Movies & TV, Spotify is Music & audio, the Web Browser and the Homebrew Channel are Tools, LG's other apps LG & TV, anything unknown Other). The order of the kinds is set under **Kinds**, here with Earlier and Later, in the TV app's Home section, or on the Apps screen itself by holding OK on a row. Tiles keep their own order within a kind.

Tiles are moved here, and apps also on Native Tv Home itself ([editing the apps](#editing-the-apps)); the rows are fixed tabs on Native Tv Home. Each app has two switches: **Shown on Home** for the Home tab's Your apps row and **Shown on the Apps tab** for the Apps tab; the order and the By kind grouping are shared. **Reset the layout** puts the rows, apps and inputs back in their first order, every row in Your order, with LG's promotions hidden again on both, and leaves the channels as they are.

The browser dashboard locks editing until each save and refresh finishes. A failed move keeps the accepted order.

Home can show a recent catalogue from the same browser session while a fresh
one loads. Without a usable cached catalogue, a failed first read asks the
server for LG Home and retries while visible; input waits for usable tiles.
The view sends heartbeats only after rendering. Reset leaves the Apps row
following its default order. A server redeploy reloads the view.

