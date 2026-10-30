---
description: "Every change Native Tv makes to a rooted LG webOS TV, and how each one is undone."
---

# What the dashboard changes on the TV

Nothing on the read-only root filesystem is written. Everything below other
than the server's own files happens only when the owner switches it on, and the
riskier controls ask for confirmation in the dashboard first.

## Files and startup

| Change | Where | Undone by |
| :----- | :---- | :-------- |
| The server, its config and state | `/var/lib/tvweb` | Uninstalling |
| Files of the earlier Node release (`tvweb.js` and `lib/`) | Deleted from `/var/lib/tvweb` by `deploy.sh` and by the daemon at its first start; nothing on the TV runs Node for Native Tv | Not restorable except by installing the Node release again |
| Overrides for one TV | `/var/lib/tvweb/native.env`, `NAME=value` lines (`TVWEB_*`) that `tvwebctl` loads before it starts the daemon; absent unless created | Deleting the file |
| Home's shared carousel and boot styles | Named `carousel` and `boot` sections in `/var/lib/tvweb/assets/google-tv/shared/common.css`, inlined into Home | Uninstalling |
| Boot hook that starts the server | `/var/lib/webosbrew/init.d/50-tvweb`, a link into the app when installed from the Homebrew Channel | Uninstalling |
| Boot log | `/var/lib/webosbrew/tvweb-boot.log`, kept under 32 KB, each line with the seconds since power-on | Uninstalling |
| Screen saver launched after a refused request | `/var/run/tvweb-screensaver-launched` records that Native Tv opened it directly, so a server restart can resume remote dismissal without adopting a cached native screen saver. Remote keys are watched while it shows | Closing the screen saver, or restarting the TV |
| Boot hook that holds switched-off services down | `/var/lib/webosbrew/init.d/20-tvweb-services`, only once a service is switched off | Switching the services back on, or uninstalling |
| Home-screen tile | The Homebrew Channel app, or with `deploy.sh` a developer app | Removing the app |
| Packages being installed | `/media/developer/temp/nativetv-install`, deleted when the install ends or at the next start | Itself |
| Watch time, while switched on in the Server tab | `/var/lib/tvweb/history`, a file for each month, kept for 13 months | **Clear history** in the Server tab, or uninstalling |
| Daily backups, while switched on in the Server tab | `/var/lib/tvweb/backups`, one file a day, the last 7 kept | Uninstalling |
| Install job and services run as root | `install-job.json` and `elevated.json` in `/var/lib/tvweb` | Uninstalling |

Installed from the Homebrew Channel, uninstalling the app stops the server
starting at boot straight away, and within a few minutes the server removes the
rest, including its Home Assistant entities. Installed with `deploy.sh`, see
[Uninstalling](../getting-started/managing.md#uninstalling).

The developer-only web component gallery no longer ships under
`assets/google-tv/pages/kit/`. The server removes its old HTML/CSS/JS and empty
directory at startup, including after an in-place upgrade. Its old addresses
return 404.

## Files of earlier test installs

A TV that ran an earlier test build keeps what that build wrote under other
names, and the server neither reads nor removes it: executables in
`/var/lib/tvweb/native`, a bus service manifest in
`/var/luna-service2-dev/manifests.d`, an earlier configd key for the remote's
keys, and a marker comment in LG's Home layout. Delete the files by hand and
restore LG's Home layout from the dashboard before installing a build that uses
the current names.

## Native client and archived benchmark

`deploy.py` puts a development build of the native TV client over the files of the Native Tv tile's app
(below), under `/media/developer/apps/usr/palm/applications/com.tvweb.dashboard/`, and runs it through the
app's native-app jail. Bounded smoke runs write `/tmp/nativetv-native.ppm`,
`/tmp/nativetv-native.json` and named captures under `/tmp/`; the server brings the folder back to the
release's copy at its next start. The client calls the existing server over loopback. The native app
is protected from dashboard uninstall and excluded from Close all recent apps.
Opening it preserves the previous video source and does not record the client
as a recently opened video app. Existing Home watchdog ownership is unchanged.
The developer client `com.tvweb.native`, which `deploy.py` staged before the tile's app was the one native
app, is deleted at the server's start, with its package folder.
The dashboard's tile on the TV's home screen, `com.tvweb.dashboard`, is the same
client installed as a native app of its own: it opens on the dashboard screen
(the app's id names the screen; other screens are named in the launch
parameters), and replaces the web page earlier releases opened in the TV's
browser, which the server deletes at start. The tile and the start-up splash
each hold a whole copy of the client in
`/media/developer/apps/usr/palm/applications/<app id>`: the executable, the
libraries it bundles (libcurl, OpenSSL and libatomic; SDL2, SDL2_image,
SDL2_ttf, FreeType and OpenGL ES are the TV's own), the Figtree fonts, the
icons and the older renderer's Roboto fonts, their licences, and a
`runtime.json` that lists every file with its SHA-256. At each server start the
installed copies are compared file by file with the ones the server carries,
and a tile or splash that differs is installed again, except while it is open.
With the web dashboard switched off in `config.json` the tile is not added.
The standalone ARM IPK (`package.py`) is the same app, `com.tvweb.dashboard`, for LG's
developer installer. Its service migration is described in
[`native-tv/ARCHITECTURE.md`](https://github.com/ihtisham1211/native-tv/blob/main/native-tv/ARCHITECTURE.md).

The managed native runtime installs `nativetv-service`, `runtime.json` and a
generated dependency notice under `/var/lib/tvweb/native/`. SSH and Homebrew
packaging include it from a local ARM build or an explicit `--native-build`
directory. The client IPK carries only its UI, and deployment removes its
obsolete private worker copy. The
existing CPU/process API routes use it for read-only procfs inspection. Stats
also request a native snapshot for memory/swap, temperature, eMMC health,
CPU/GPU clocks, app storage, CPU times, uptime/load, network/Wi-Fi,
picture-engine HDR/colourimetry and Magic Remote pairing readings. The storage
probe uses `statvfs` rather than launching `df`; the five-second CPU window and
network rate are kept by the daemon over native counters.
It exits after each coalesced read, writes no settings and opens no
listener. Absent, incompatible or failed workers return an API error;
valid native read failures retain their API error. Development deployment
renames new binary inodes so an active worker can finish during an update.
Homebrew launches at the same server version repair the runtime if needed;
opening an older app preserves the newer install's runtime. SSH and Homebrew
builds require the ARM runtime; the source-only metric readers are retired.

The retired renderer benchmark previously staged `com.tvweb.home.nativebench` under the developer-app directory
and created its native-app jail. It used `/tmp/nativetv-native-result.json`,
`/tmp/nativetv-native-preview.ppm` and a temporary loopback fixture server on
port 18080. Its executable and TV runners have been removed from the source
tree; results and offline analysis remain. Repository cleanup does not remove
an app directory or jail previously staged on a TV. The active client and its
smoke tests use `native-tv/`.

## Features that change how the TV runs

These last until a reboot and are put back by the boot hook, so they end with
the first reboot after uninstalling.

| Feature | What it does | Undone by |
| :------ | :----------- | :-------- |
| Ad blocker | Bind-mounts a replacement `/etc/hosts`, carrying over the lines other tools such as AmazOff add to it. An uploaded hosts list is kept in `/var/lib/tvweb/adblock_custom.json` | Switching it and update blocking off. While another tool's lines are in it and the `/etc/hosts` underneath cannot hold them, it stays up, blocking nothing, until the next reboot |
| Hidden home-screen tiles | Bind-mounts edited `appinfo.json` files, then has the app manager reread them: where the TV has LG's blocked-app list in configd (webOS 9 does, webOS 4 doesn't), by briefly adding an id no app has to LG's blocked-app list in configd and putting the list back, and otherwise by restarting it. Experimental: turned on under Server → Advanced → Experimental features. Not offered when installed from the Homebrew Channel, since the restart happens during boot | Switching tile hiding off |
| Apps no longer kept ready in the background | Sets `isEnabled` false for each in configd's `com.webos.service.preloadmanager.supportAppList`, which the TV keeps as a changed setting, and tells the preload manager at once, and again at each start (after a cold start, 3 s after boot-done, when the preload manager starts) and each time the TV is switched on. An app already loaded stays until it is next closed or the TV restarts. Put back at start if the list loses them | Switching each back on, which gives it LG's own setting again. The list stays a changed setting, which the TV has no way to remove, and uninstalling leaves the apps switched off |
| Switched-off background services | Binds a stub that exits at once over each one's program (`/var/lib/tvweb/service-off`, or `service-off.js` for a JavaScript service), so neither systemd nor the bus hub runs it, then stops any running; ThinQ AI's copy in its LG jail (`/var/palm/jail/lg.thinqai.adapter`, set up early with `jailer ... /bin/true`) is stubbed too. Stops any found running again every five minutes. Masks under `/run/systemd/transient` from earlier releases are removed | Switching them back on (unbinds the stubs), or a reboot after uninstalling |
| Debugger closed to the network | Adds a firewall rule dropping port 9998 (WebAppMgr's DevTools, opened by Developer Mode) on every interface but loopback. Put back each time the server starts, not by the boot hook. Nothing on a TV whose kernel has no firewall | `allowNetworkDebugger` in `config.json` or toggled from the Server tab, uninstalling from the Homebrew Channel, a reboot after any other uninstall, or `iptables -D INPUT -p tcp --dport 9998 ! -i lo -j DROP` |
| Clock set from an NTP server | With a time server set on the Server tab, or `ntp.server` in `config.json`, sets the TV's clock through its time service, hourly and when the TV switches on. The service records the time as coming from LG's time servers | Switching it off on the Server tab, or emptying `ntp.server`. The clock stays as set until LG's next sync |
| Home screen layout | On webOS 9, writes a copy of LG's own Home screen layout without the parts switched off to `/mnt/lg/cmn_data/manifest/`, where the home app looks before its built-in one, and restarts the home app after deleting its saved copies (`/tmp/checkpoint` and `/mnt/lg/cmn_data/checkpoint`), which the TV makes again. On webOS 10 (experimental), mounts an overlay over the home app's assets folder with a changed `home.xml` on top, from `/tmp/nativetv-home`, put back each time the server starts; the choices and a copy of LG's `home.xml` are kept in `/var/lib/tvweb`. Input tiles are LG's own launch points, recorded in `home-inputs.json` in `/var/lib/tvweb`. On webOS 9, their titles are made visible and refreshed from CEC names when the Home screen opens, shortly after the TV switches on and at server start, never in standby; only recorded tiles are updated | **Put back LG's Home screen** on the Apps tab, or `tvwebctl restore-home` before uninstalling |
| Screen-time limits | While on in the Advanced tab, shows toasts and, at a limit, switches the TV or its screen off or goes to Home | Switching them off |
| Replacement screen saver | Bind-mounts over the built-in screen saver app. Experimental on webOS 12 and later, where the replacement is untried: turned on from the Screensaver tab, or under Server → Advanced → Experimental features. On webOS 10 and 11, where the built-in one is a Flutter app, a switch the app manager does not take without a restart is undone, rather than the app manager restarted | Choosing the stock screen saver |

## Changes that stay

These go through LG's own services and persist like any change made in the TV's
menus. Uninstalling the dashboard leaves them as they are.

| Change | Note |
| :----- | :--- |
| Uninstalling an app | Permanent; reinstall it from the LG Content Store |
| SSH keys added or removed on the Server tab | In `/home/root/.ssh/authorized_keys`. Deleting the file there gives root the password `alpine` from the next boot. Uninstalling leaves the file as it is |
| Installing an app | Installed as a developer app; remove it from the Apps tab or the Homebrew Channel. Its services stay elevated to root if that was chosen |
| Picture mode, energy saving, sound output, Quick Start+, sleep timer, Quick Access keys | Ordinary TV settings |
| LG settings restored from a backup | Sound, device, HDMI and ad settings, set as the dashboard sets them one by one. Undone by changing them back, or by restoring a backup made before |
| Screen Shift, Logo Luminance Adjustment | OLED panel settings in the TV's own menus |
| Global Stress Reduction and Temporal Peak Control | OLED burn-in protections. Switching one off asks for confirmation, and neither is exposed to Home Assistant. Not switched back on by uninstalling |
| Native Tv Home | While switched on, bind-mounts `/var/lib/tvweb/home-takeover`, the Native Tv Home app under LG's Home's id `com.webos.app.home`, over `/usr/palm/applications/com.webos.app.home`, from the boot hook and at server start, and stops LG's Home process so the TV opens the mounted app. The mount does not survive a reboot. With **Hide LG's Home while the TV starts** chosen, so that LG's Home is not seen before the boot hook mounts it again, LG's Home layout files in `/mnt/lg/cmn_data/manifest` hold an empty layout while it has the place, the files there before kept in `/var/lib/tvweb/home-takeover-lg-layout`; they are put back, and LG's Home's checkpoints in `/tmp/checkpoint` and `/mnt/lg/cmn_data/checkpoint` deleted, whenever LG's Home gets its place back or the option is switched off. While it is on, `nativetv.remoteKeys` also carries `starting`, the message the remote key filter shows when Home is pressed before Native Tv Home is in place after a cold start, for at most 90 s after the filter loads; every release to LG's Home removes it, including stopping the server and SSH recovery. With the empty layout chosen, a manual uninstall that deletes `/var/lib/tvweb` without `tvwebctl restore-nativetv-home`, or a TV where Native Tv no longer runs, leaves LG's Home blank until `/mnt/lg/cmn_data/manifest` and LG's Home's checkpoints are deleted and the TV restarted ([Home](../dashboard/home.md#switching-it-on-and-off) has the command). Making sam read it again uses LG's blocked-app list on webOS 9, and a block and release of `com.webos.app.home` on webOS 10 and 11 (below). On upgrade or restart, sam must also report the home category; otherwise LG's files are put back and the switch reports failure. While it is on and Quick Start+ (`option.quickStartMode`) is off, LG's `general.homeAutoLaunch` is set to `off`; it is put back when Quick Start+ is found on, at server start and at each switch on or off, with its value and `checkHomeAutoLaunchValue` (which LG turns from `default` to `user` on any write) kept only when changed, as `lgAutoLaunch` and `lgAutoLaunchCheck` in `home.json` and put back on switching off; where it was `on`, the boot hook launches `com.webos.app.home` once mounted, after a power-on reason in LG's `fullHomeBootingPowerOnReasons`, while the input bootd started on is still in front, and the server does the same on a wake from Active Standby unless a source choice, standby, switch-off or server stop cancels it. A refused takeover restores auto launch, and switching off waits for pending settings work before restoring it. **Use LG Home** on the Home tab, or `tvwebctl restore-nativetv-home`, unmounts it; `tvwebctl stop` and a server failing three times in ten minutes do too, leaving it switched on for the server's next start. The layout, and with profiles each profile's own, is in `/var/lib/tvweb/home.json`; when each tile was last opened, for rows set to Last opened, is in `/var/lib/tvweb/home-recent.json`, deleted once no row uses it. The TV's app and input lists, as last read, are kept in `/var/lib/tvweb/home-lists.json` (rewritten only when they change), so the view's first screen after a cold start does not wait on reading them again. The app mounted over LG's Home has no screen of its own: when it loads, and each time it is shown again (the Home key opens it), it asks sam to open the Native Tv tile's app, `com.tvweb.dashboard`, at its Home screen, asks again every 3 s if sam refuses, and is black until that app is in front. If it is still in front 2.5 s later the server opens the app itself, and if sam refuses (the owner took the tile off the home screen, so sam no longer knows the app) LG's Home is given back at once. So that the TV is never left with a black screen, the app has to be on the home screen and able to draw: switching Home on, and the server's start with Home on, ask sam for the app (`getAppInfo`) and run `nativetv-tv --probe`, and refuse the takeover when either fails, with the reason (no app file, sam does not know the app, no display 10, no window 11, a GL fault 12, a wrong pixel 13, a crash or a timeout); `nativeClient` in `config.json` sets the probe's `path`, `probeCommand` and `probeTimeoutMs`. A verdict a rerun cannot change (ok, 12, 13) is kept for that app file and firmware in `/var/lib/tvweb/native-probe.json`, so the probe's window is not opened at every boot, and the boot hook's takeover (`--takeover-home`) skips on a refusal with exit 4. The server also watches the app: three exits within two minutes, each either the app gone from sam's running list as LG's Home or nothing comes to the front, or LG's Home still in front 15 s later with the app not (`nativeClient.stubGraceMs`), give LG's Home its place back as `tvwebctl stop` does, clear the Home key's starting notice and put LG's auto launch back, leaving Home switched on. Nothing takes the place again, including the return after a way back to LG's Home, until the server restarts or Home is switched on again. `GET /api/home` reports `nativeHome` (whether it is halted, why, the exits counted) and `nativeClient` (the last probe's verdict). Earlier builds installed the developer app `com.tvweb.home` and pointed `defaultApps.home`, `lastInputApp`, `physicalLastInputApp` and the remote key filter's Home at it; a newer server puts back whichever of those still name it and removes the app. Uninstalling Native Tv from the Homebrew Channel switches it off before deleting the server; a failed switch-off keeps the server for retry |
| Start-up splash | While Native Tv Home is on and opens at power-on (the power-on setting, or LG's Home auto launch having been on), `nativetv.remoteKeys` carries `splash`, and the remote key filter opens `com.tvweb.splash` as soon as it loads, after a power-on reason in `aiKey`, `lastPowerOn`, `localKey`, `remoteKey` or `seniorHelp`, and again 0.5 s after the input bootd starts on comes to the front. The splash is a hidden native OpenGL app with no tile, installed under `/media/developer/apps/usr/palm/applications/com.tvweb.splash` by `deploy.sh` and by the server when it sets `splash`, and brought up to date with the dashboard's tile (the same client) at each server start. It shows the Native Tv mark on black until the boot hook opens Native Tv Home over it, and closes itself after 100 s | Switching Native Tv Home off, the power-on setting off, or removing the app |
| Early boot start | While Native Tv Home is on or a background service is switched off, the server declares a bus service of its own, `com.webos.nativetv.boot`, in `/var/luna-service2-dev` (a service, role, public API permission and manifest, then `ls-control scan-volatile-dirs`), from `assets/early-service/install.sh`; it is removed when neither is wanted and on uninstall. The remote key filter calls it once a second from its first config read until about 33 s, and the bus runs `early-start.sh` as root on the first call that gets through, about 22 s into a cold start. That starts the services hook (`20-tvweb-services`), and while Native Tv Home is on (the flag file `/var/lib/tvweb/early-home`) the boot hook (`50-tvweb`), which `startup.sh` would otherwise run at 43-48 s; each hook takes a lock in `/tmp` so it runs once per boot. The service's role lets only `com.webos.surfacemanager*` and `com.webos.starfish.keyfilters*` call it (a call from a shell is refused with "Not permitted to send"), it takes no input, and its script is a fixed file. `/var/lib/tvweb/early-pending` is set when the script starts the hook and removed when the hook has started the server; a boot that finds it left over skips the early start once, so a start that breaks boot cannot repeat | Switching Native Tv Home off with no service switched off, `tvwebctl restore-nativetv-home`, or removing the service with `sh /var/lib/tvweb/assets/early-service/install.sh remove` |
| Remote keys | Once a key is changed on the Apps tab, `/var/lib/tvweb/remotekeys-filter.js` is named first in the configd key `com.webos.surfacemanager.keyFilters`, ahead of LG's own filter files, which are not touched, and the mappings are kept in a configd value of its own, `nativetv.remoteKeys`. configd keeps both across reboots. A key set to a media control sends that key through the network input service when pressed. **As LG has it** on every key empties the mappings and leaves the filter passing every key on; `tvwebctl restore-keys`, which uninstalling from the Homebrew Channel runs, takes the entry out and deletes the file. configd cannot delete a value, so `nativetv.remoteKeys` stays behind as an empty map. An uninstall by hand without it leaves an entry naming a file that is gone, which the compositor skips, and LG's filters handle every key. On webOS 11, where the key filters are native, LG's own values are changed instead: a streaming button's entry in the settings service's `other.mapping_info`, the ? button's `other.customShortPress` and `other.customLongPress`, and the configd values `com.webos.keyaction.tvGuide` and `com.webos.keyaction.inputHub`, with what each held kept in `/var/lib/tvweb/remotekeys-lg.json` (and the `.last-good` copy beside it, which goes with it). LG writes `mapping_info` back at each switch off and on, and the server sets it again. **As LG has it** writes LG's value back, and `tvwebctl restore-keys` writes them all back and deletes the file. While the AI button is set, LG's voice assistant (`voiceconductor.service`) is switched off as a background service is, and switched back on for **As LG has it** and by `tvwebctl restore-keys`. While the Alexa button is set, `amazon.alexa.view` is blocked with `blockSystemApp` and released the same way, and toasts are switched off for 4 s after each press, with `/var/lib/tvweb/remotekeys-toasts-off` there meanwhile; the server's start switches them back on if it finds it |
| LG's blocked-app list | Where hidden tiles or a screen saver switch use it, it is put back with LG's value but saved as a changed setting, which the TV has no way to remove. It keeps that value if LG's own list later changes with a firmware update or a new country. Not undone by uninstalling |
| LG's blocked system app list | A screen saver switch on webOS 10 and later blocks the screen saver app and releases it at once, so that sam reads its manifest again, and so does Native Tv Home with LG's Home app as it takes or gives back the place. The block is written to `/var/preferences/com.webos.applicationManager/blockedSystemAppList/<country>.json` and the release takes it out; the file was byte-identical afterwards on a C5. The release is sent even when the block's answer did not come back. Only a run cut short between the two calls leaves the app blocked, and on these TVs the boot hook releases the screen saver and LG's Home at every boot |

Native Tv Home saves its enabled state before changing the TV's Home. Its settings writes run in order, so switching off restores the snapshot after pending guard writes finish. A late guard read cannot change the restored value.

The Home tab's **Power-on screen** choice is saved as `openOnPowerOn` in
`home.json`. When chosen, LG Home auto launch is held off regardless of Quick
Start+, and the boot hook and guarded wake handler open Native Tv Home even
when the earlier LG setting was Recent Input. Quick Start+ is not changed.
**Show app names** is saved as `appNames` in the same file and affects only the
app tiles' visible labels; their accessible names are kept. **Show inputs** and
**Show the LG Home tile** are saved as `showInputs` and `showLgHome` and only
leave the Inputs tab and the LG Home tile out of the view.

**Clock format** is saved as `clockFormat` (`12h` or `24h`, default `24h`) in
`home.json`. It controls Home's clock and programme times only; no LG setting
is written. Layout reset leaves the clock preference alone.

A refused device tile leaves the next input switch from the remote recorded for every device on that input, in rows set to Last opened.

Switching Native Tv Home off reports a Home-key restore failure, including a saved mapping that could not be deleted. App removal is refused until the key and Home and start-up settings no longer target it, and is confirmed only after the TV reports the app absent. A missing app found by the guard switches the feature off and restores its saved settings and Home key.

Home wallpapers uploaded through the browser go into `/var/lib/tvweb/home-wallpaper-uploads`, one for each time of day. The directory is excluded from deploy bundles and removed with the server. Failed LG Home restoration remains visible and retryable on both dashboards.

Native Tv Home's text and accent colours are saved in `/var/lib/tvweb/home.json` and applied to its view within a few seconds. **Use the default colours** clears the choice.

While Native Tv Home and its picture details (off by default) are on and the TV is out of standby, a read-only `videooutput/getStatus` subscription records the last video each app plays for the plate. It starts before the first Home visit and stops when either is switched off or the TV goes into standby. Recorded videos stay in memory until the server restarts. HDMI signal and picture-mode reads change no TV settings.

Back on Native Tv Home reads the TV's recent-app list and its snapshots.

With `tmdbKey` set in `config.json`, the TV view of Native Tv Home asks `api.themoviedb.org` over `curl` or `wget` for trending, popular and anime titles, at most five requests, once a day: the first visit to Home after 05:00 on a day the feed was not fetched. It keeps the reply in `home-discover.json` beside the server, and the posters and backdrops in `home-images/` there (about 10 to 20 MB; the server fetches the card pictures with the feed, a larger backdrop the first time it is shown, and deletes those not used for a week). The view is served every picture by the server, which accepts only `image.tmdb.org` and `i.ytimg.com` addresses. With `youtubeKey` set it also asks `www.googleapis.com` for the popular videos, and fetches their thumbnails from `i.ytimg.com`. Nothing is sent but the key and the language.
Close and Close all stop only the selected listed apps, through
`applicationManager/closeByAppId` for native apps and WebAppMgr's `killApp`
for web apps, and confirm they have stopped before removing their cards with
`com.webos.service.recent/removeRecent`. A stop that cannot be confirmed keeps
the cards for retry. Home and apps running without a card are left alone.
Snapshot responses are private and must not be cached.

## Home-screen app port

The Dashboard, Native Tv Home and Homebrew Channel apps are static entry points and carry the dashboard port in a `var PORT = n; // tvweb:port` line. At every start the server rewrites that line in each installed copy to match `port` in `config.json`. The managed Home takeover stager writes the configured port into the page mounted over LG Home; the same line is written by every stager. Only the port number changes in each page, and reinstalling a launcher brings its default back until the next server start.

## Network

The phone/computer dashboard is assembled and compressed only on its first
request, then cached until restart. TV pages and the local JSON API do not load
it. Metrics and System in the TV app poll every ten seconds without overlapping
reads, pause while hidden and refresh on return. Hidden Home pauses nonessential
refreshes and wallpaper animation but keeps its five-second recovery heartbeat.

The dashboard listens dual-stack on port 8080 (IPv4-only on a kernel without
IPv6), on the whole network unless it is limited
to the TV itself in `config.json` or from Settings on the TV. Installed from the
Homebrew Channel it starts limited to the TV, until the owner opens it during
setup. While a phone is being used to enter Home Assistant details,
port 8081 answers a one-time code for up to ten minutes. Integrations connect to
their configured MQTT, NTP, metadata and package services. Native Tv no longer
polls GitHub for its own releases; releases are installed through deployment or
the Homebrew Channel package.

With **Share app icons** on in the MQTT tab (`iconServer` in `config.json`), port 8082 (the dashboard's
port plus two) answers on the whole network, dual-stack like the dashboard,
with installed apps' icons at `/icons/<appId>.png` and nothing else: no other
paths, no listing, read-only and without a token, whatever the dashboard is
limited to. An icon is only read from inside its app's own folder,
whatever the app's `appinfo.json` names. Anyone on the network can tell which
apps are installed by asking for their icons. Off by default. A port already in use is logged and left, and the
dashboard and Home Assistant bridge carry on without it.

With a TMDB key, title details request `/movie/{id}` or `/tv/{id}`, appending
credits, videos, similar titles and external IDs. Search requests
`/search/multi` with the entered query and adult results disabled. Both go
through the server; only title IDs, queries, the API key and language leave
for these reads. `home-content.json` beside the server keeps up to 40 replies,
mode 0600: details for a day, search for 15 minutes. Search cache keys are
hashes rather than raw queries. An unavailable service can return a saved
reply. The added rows reuse the daily feed.

`netflixDeepLinks`, off by default, permits a server request to
`www.wikidata.org/wiki/Special:EntityData/<QID>.json` for a title's `P1874`
claim. The QID comes from TMDB's external IDs; the TMDB key is not sent to
Wikidata. Mappings, including empty results, share the bounded metadata cache
for seven days. A Netflix launch passes the resolved numeric ID to the
installed app in `contentTarget` as
`m=https://www.netflix.com/title/<id>&source_type=4`.
