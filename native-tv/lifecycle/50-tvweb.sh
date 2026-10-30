#!/bin/sh
# webosbrew boot hook: start the Native Tv daemon.
# Install to /var/lib/webosbrew/init.d/50-tvweb (chmod +x).
# Note: BusyBox run-parts ignores filenames containing a dot (.),
# so this hook must not have a .sh extension in init.d.
# Remove /var/lib/webosbrew/init.d/50-tvweb to uninstall.
# Nothing on the read-only rootfs is touched.
#
# Deliberately defensive: never block boot, never respawn-loop. If the
# daemon is missing, this exits quietly.

DAEMON_BIN=/var/lib/tvweb/native/nativetv-daemon
[ -x "$DAEMON_BIN" ] || exit 0

export PATH="/bin:/sbin:/usr/bin:/usr/sbin:$PATH"

# Nothing records a boot hook's output, so keep a short log of our own. Kept
# under /var/lib/webosbrew so the boot that went wrong is still there after.
BOOTLOG=/var/lib/webosbrew/tvweb-boot.log
[ "$(wc -c < "$BOOTLOG" 2>/dev/null || echo 0)" -gt 32768 ] && mv -f "$BOOTLOG" "$BOOTLOG.old"
exec >>"$BOOTLOG" 2>&1
# Each line carries the time since power-on, as the server's own log does, so
# the log shows where a slow start spent it. The wall clock can still be wrong
# this early in boot.
bootlog() { echo "$(date): [up $(cut -d' ' -f1 /proc/uptime)s] $*"; }
native_boot_dispatch() {
  operation=$1
  shift
  daemon_path="$DAEMON_BIN"
  [ -x "$daemon_path" ] || return 2
  if LD_LIBRARY_PATH="/var/lib/tvweb/native${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
    "$daemon_path" "$@" >/dev/null 2>&1; then
    return 0
  else
    dispatch_status=$?
  fi
  case "$dispatch_status" in
    3|4) return "$dispatch_status" ;;
  esac
  bootlog "native $operation failed after dispatch; not replaying it through shell"
  return 1
}
# Run from about 22 s by the bus service in assets/early-service, which the key
# filter calls at its first config read, and again by startup.sh at 43-48 s: the
# second run finds the first one's lock. /tmp is empty at every boot.
if ! mkdir /tmp/tvweb-hook-started 2>/dev/null; then
  bootlog "already run this boot"
  exit 0
fi
bootlog "starting"

# After the first app bootd waits up to 15 s for banner-ready, which the input
# app sends when it shows its input banner, before init-boot-done, which
# starts WebAppMgr. The splash covering the input within 0.2 s keeps the
# banner from showing: on a B3 (webOS 10.3.2) init-boot-done then came at the
# timeout, 36 s, against 25-27 s with the input up 1.5 s. The splash is in
# front in its place, so the signal is sent for it.
if grep -q "'com.tvweb.splash' is foreground" /var/log/bootd.log 2>/dev/null &&
   ! grep -q 'SIGNAL_init-boot-done' /var/log/bootd.log 2>/dev/null; then
  luna-send -n 1 luna://com.webos.bootManager/generateSignal '{"name":"banner-ready"}' >/dev/null 2>&1
  bootlog "banner-ready sent for the splash"
fi

# Until Native Tv Home opens, the screen shows the input the TV started on.
# The Home key's localized startup notice is saved in remotekeys.json by
# the daemon. Let the native one-shot parse it and build the Luna JSON
# payload.
native_daemon="$DAEMON_BIN"
if [ -x "$native_daemon" ]; then
  (
    if LD_LIBRARY_PATH="/var/lib/tvweb/native${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
      "$native_daemon" --show-home-starting-notice \
        --config /var/lib/tvweb/config.json --system-root /; then
      :
    else
      bootlog "native Home startup notice failed after dispatch; not replaying it through shell"
    fi
  ) </dev/null &
else
  # The server writes the file one key a line (JSON.stringify with an indent).
  # Retain the old parser only until the native daemon is installed.
  starting=$(sed -n 's/^ *"starting": *"\(.*\)",*$/\1/p' /var/lib/tvweb/remotekeys.json 2>/dev/null | head -n 1)
  if [ -n "$starting" ]; then
    luna-send -a com.webos.app.home -n 1 luna://com.webos.notification/createToast \
      "{\"sourceId\":\"com.webos.app.home\",\"message\":\"$starting\"}" >/dev/null 2>&1 &
  fi
fi

# Hold down the LG daemons switched off in the dashboard. Done in the delayed
# block below, after upstart has had its go at starting them.

# Restore the shared hosts table before network startup. New installs use the
# native daemon's same store as the running front door; the shell copy remains
# only for installations that have not received the native runtime yet.
if [ -x "$DAEMON_BIN" ] &&
   LD_LIBRARY_PATH="/var/lib/tvweb/native${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
     "$DAEMON_BIN" --restore-adblock; then
  bootlog "native ad-block hosts table restored"
else
  # Compatibility for an older Node-only install without the native daemon.
  if [ -f /var/lib/tvweb/adblock_enabled ] && [ -f /var/lib/tvweb/adblock_hosts ]; then
  sed -i '/nextlgsdp\.com/d' /var/lib/tvweb/adblock_hosts 2>/dev/null || true
  # Other tools' lines (AmazOff, whose boot hook runs before this one) are
  # taken from this boot's /etc/hosts, and the last boot's are dropped. The
  # table's own lines are comments, localhost and sinkholes, and the Homebrew
  # Channel's update block points the update servers at 127.0.0.1: privacy.js
  # foreignHostsLines tells them apart the same way.
  OWN='^[[:space:]]*(#|$)|^[[:space:]]*(0\.0\.0\.0|::)[[:space:]]|localhost|ip6-'
  UPDATES='[[:space:]](snu|su|su-dev|su-ssl)\.lge\.com([[:space:]]|$)'
  { grep -E "$OWN" /var/lib/tvweb/adblock_hosts; grep -v -E "$OWN|$UPDATES" /etc/hosts; } \
    > /var/lib/tvweb/adblock_hosts.new 2>/dev/null \
    && cat /var/lib/tvweb/adblock_hosts.new > /var/lib/tvweb/adblock_hosts
  rm -f /var/lib/tvweb/adblock_hosts.new
  mount --bind /var/lib/tvweb/adblock_hosts /etc/hosts 2>/dev/null || true
  fi
fi

# sam reads each appinfo.json only when it scans, and restarting it to make it
# scan is the one thing every black-picture report in #366 has in common.
# Where configd has LG's blocked-app list, adding an id no app has and putting
# the list back as it was makes sam scan again without restarting: on a C2
# on webOS 9.2 a visible:false overlay took effect in seconds and sam kept its
# pid. webOS 4 has no such list, and there sam is restarted as before. Mirrors
# lib/samrescan.js.
SAM_NUDGE_ID=io.github.ihtisham1211.nativetv.rescan
sam_nudge() {
  list=$(luna-send -n 1 luna://com.webos.service.config/getConfigs '{"configNames":["profile.blockedAppList"]}' 2>/dev/null |
    tr -d '\n' | sed -n 's/.*"profile\.blockedAppList": *\(\[[^]]*\]\).*/\1/p')
  [ -n "$list" ] || return 1
  # LG's own list: without the id, should a run cut short have left it in.
  lg=$(echo "$list" | sed "s/\"$SAM_NUDGE_ID\"//g; s/, *,/,/g; s/\[ *,/[/; s/, *\]/]/")
  inner=$(echo "$lg" | sed 's/^\[ *//; s/ *\]$//')
  if [ -z "$inner" ]; then nudged="[\"$SAM_NUDGE_ID\"]"; else nudged="[$inner,\"$SAM_NUDGE_ID\"]"; fi
  luna-send -n 1 luna://com.webos.service.config/setConfigs "{\"configs\":{\"profile.blockedAppList\":$nudged}}" >/dev/null 2>&1 || return 1
  sleep 2
  for _ in 1 2 3; do
    luna-send -n 1 luna://com.webos.service.config/setConfigs "{\"configs\":{\"profile.blockedAppList\":$lg}}" 2>/dev/null |
      grep -q '"returnValue": *true' && return 0
    sleep 1
  done
  return 1
}

# sam ignores that list on webOS 10 (a C2 on 10.3.1), but there it rereads one
# system app on blockSystemApp followed by releaseBlockSystemApp: on a C5 on
# 10.3.1 a Flutter manifest replaced by a QML one was reported within the
# second, sam kept its pid, and LG's blockedSystemAppList file was byte-identical
# afterwards. The release goes out whatever the block answered, so an app is
# never left blocked by a run cut short. Mirrors lib/samrescan.js.
# webOS 11 refuses the block without forceBlockSystemApp, which its schema
# requires (#768); it is sent only where the schema names it.
sam_reread_app() {
  force=
  grep -q '"forceBlockSystemApp"' /etc/palm/schemas/sam/applicationManager.blockSystemApp.schema 2>/dev/null &&
    force=',"forceBlockSystemApp":false'
  luna-send -n 1 luna://com.webos.applicationManager/blockSystemApp "{\"id\":\"$1\"$force}" 2>/dev/null |
    grep -q '"returnValue": *true'
  blocked=$?
  for _ in 1 2 3; do
    luna-send -n 1 luna://com.webos.applicationManager/releaseBlockSystemApp "{\"id\":\"$1\"}" 2>/dev/null |
      grep -q '"returnValue": *true' && return $blocked
    sleep 1
  done
  return 1
}

# Whether sam's getAppInfo for app $1 matches pattern $2 within 20s: at boot sam
# can still be on its own first scan, and takes the nudge after it.
sam_reports() {
  n=0
  for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
    n=$((n + 1))
    luna-send -n 1 luna://com.webos.applicationManager/getAppInfo "{\"id\":\"$1\"}" 2>/dev/null | grep -q "$2" && { bootlog "sam reports $1 after $n polls"; return 0; }
    sleep 1
  done
  return 1
}

webos_major=$(sed -n 's/.*webOS[^0-9]*\([0-9][0-9]*\)\..*/\1/p' /etc/issue 2>/dev/null | head -n 1)
# A G5 (webOS 10.3.1) has no /etc/issue; "Rockhopper release 10.3.1-3006" is
# in starfish-release, as lib/telemetry.js reads it.
[ -n "$webos_major" ] ||
  webos_major=$(sed -n 's/.*release[^0-9]*\([0-9][0-9]*\)\..*/\1/p' /etc/starfish-release 2>/dev/null | head -n 1)

# Native Tv Home in LG's Home's place (lib/home-takeover.js), first of the
# mounts, as anything that opens Home before it opens LG's. LG's Home may have
# started by now, in the background, on the empty layout the server left it on
# webOS 9, so anything opening Home before now showed black; once sam
# reads the mounted manifest it is stopped, and the TV opens Native Tv Home's
# page as Home from then on. Where sam does not take it, LG's files go back
# under it, its own layout too, and it restarts on that. The server does the
# same at its start, for a hook that did not run.
# On webOS 10 and 11 LG's Home is a Flutter app run as
# "flutter-client -i com.webos.app.home"; webOS 11 only with
# allowExperimentalHome in config.json. sam ignores the nudge there (above), so
# the app is reread with a block and release first.
homeapp=/usr/palm/applications/com.webos.app.home
homestage=/var/lib/tvweb/home-takeover
home_os=0
[ "$webos_major" = "9" ] && home_os=1
[ "$webos_major" = "10" ] && home_os=1
if [ "$webos_major" = "11" ]; then
  grep -q '"allowExperimentalHome"[[:space:]]*:[[:space:]]*true' /var/lib/tvweb/config.json 2>/dev/null && home_os=1
fi
# A block and release cut short leaves Home blocked across boots, as for the
# screen saver below, so it is released at every boot where it is ever blocked.
if [ "$webos_major" = "10" ] || [ "$webos_major" = "11" ]; then
  luna-send -n 1 luna://com.webos.applicationManager/releaseBlockSystemApp '{"id":"com.webos.app.home"}' >/dev/null 2>&1 || true
fi
# Only the Flutter runners' command lines are read: a loop over every process in
# /proc, two forks each, took 26 s at 23 s into a cold start (a load average near
# 33 on an OLED65B3PUA, webOS 10.3.2).
lg_home_pids() {
  pidof com.webos.app.home
  for pid in $(pidof flutter-client); do
    { tr '\0' ' ' < "/proc/$pid/cmdline"; } 2>/dev/null | grep -q '^/usr/bin/flutter-client .*-i com\.webos\.app\.home ' && echo "$pid"
  done
}
# The server switches LG's Home auto launch off, so the TV starts on its last
# input rather than on LG's Home, and keeps the owner's value as lgAutoLaunch in
# home.json (lib/home.js). Where it was on, or openOnPowerOn was chosen,
# Native Tv Home opens instead: after a power-on LG opens Home for, and only while the input
# the TV started on is still in front, so an input or app chosen since stays.
# bootd logs the app it put in front as "'<id>' is foreground" (a G5, webOS
# 10.3.1); without that line nothing is opened.
home_open_at_boot_compat() {
  if ! grep -q '"openOnPowerOn"[[:space:]]*:[[:space:]]*true' /var/lib/tvweb/home.json 2>/dev/null; then
    grep -q '"lgAutoLaunch"[[:space:]]*:[[:space:]]*"on"' /var/lib/tvweb/home.json 2>/dev/null || return 0
  fi
  reason=$(luna-send -n 1 luna://com.webos.service.tvpower/power/getPowerOnReason '{}' 2>/dev/null |
    sed -n 's/.*"reason": *"\([^"]*\)".*/\1/p')
  reasons=$(luna-send -n 1 luna://com.webos.service.config/getConfigs \
    '{"configNames":["com.webos.service.tvpower.fullHomeBootingPowerOnReasons"]}' 2>/dev/null | tr -d '\n' |
    sed -n 's/.*fullHomeBootingPowerOnReasons": *\(\[[^]]*\]\).*/\1/p')
  # LG's list on a G5; the same where a TV has none.
  [ -n "$reasons" ] || reasons='["aiKey","lastPowerOn","localKey","remoteKey","seniorHelp"]'
  case "$reasons" in
    *"\"$reason\""*) ;;
    *) bootlog "Native Tv Home not opened: power-on reason ${reason:-unknown}"; return 0 ;;
  esac
  # The first app bootd names, leaving out the splash the key filter opens before it.
  boot_app=$(sed -n "s/.*BootSequencer) '\([^']*\)' is foreground.*/\1/p" /var/log/bootd.log 2>/dev/null |
    grep -v '^com\.tvweb\.splash$' | head -n 1)
  front=$(luna-send -n 1 luna://com.webos.applicationManager/getForegroundAppInfo '{}' 2>/dev/null |
    sed -n 's/.*"appId": *"\([^"]*\)".*/\1/p')
  case "$boot_app" in
    com.webos.app.livetv|com.webos.app.hdmi[0-9]|com.webos.app.av[0-9]|com.webos.app.comp[0-9]) ;;
    *) bootlog "Native Tv Home not opened: the TV started on ${boot_app:-nothing logged}"; return 0 ;;
  esac
  # The splash the key filter puts over the input (lib/remotekeys.js) is in front too.
  if [ "$front" != "$boot_app" ] && [ "$front" != "com.tvweb.splash" ]; then
    bootlog "Native Tv Home not opened: ${front:-nothing} in front since the start on $boot_app"
    return 0
  fi
  # In the background: sam answers once WebAppMgr has the app up, 17 s after
  # the call in an early start on a B3 (28 s to 45 s), and the server's start
  # below would wait for it. stdin is left alone: luna-send prints nothing with
  # it on /dev/null. Its output is the hook's, the boot log, where bootlog writes.
  ( luna-send -n 1 luna://com.webos.applicationManager/launch '{"id":"com.webos.app.home"}' 2>/dev/null |
      grep -q '"returnValue": *true' &&
      bootlog "Native Tv Home opened over $boot_app (in front: $front)" ) &
}
home_open_at_boot() {
  native_daemon="$DAEMON_BIN"
  if [ -x "$native_daemon" ]; then
    (
      if LD_LIBRARY_PATH="/var/lib/tvweb/native${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
        "$native_daemon" --open-home-at-boot \
          --config /var/lib/tvweb/config.json --system-root /; then
        :
      else
        bootlog "native Home boot policy failed after dispatch; not replaying it through compatibility policy"
      fi
    ) </dev/null &
    return 0
  fi
  home_open_at_boot_compat
}
legacy_home_takeover() {
  if ! grep -q '"enabled"[[:space:]]*:[[:space:]]*true' /var/lib/tvweb/home.json 2>/dev/null; then
    bootlog "legacy Home takeover skipped: Native Tv Home is disabled"
    return 0
  fi
  mount --bind "$homestage" "$homeapp" 2>/dev/null || true
  bootlog "Native Tv Home: mounted"
  if [ "$webos_major" = "9" ]; then
    home_reread() { sam_nudge; }
  else
    home_reread() { sam_reread_app com.webos.app.home; }
  fi
  if home_reread && { bootlog "Native Tv Home: sam reread"; sam_reports com.webos.app.home '"type": *"web"'; }; then
    for pid in $(lg_home_pids); do kill "$pid" 2>/dev/null; done
    bootlog "Native Tv Home is in LG's Home's place"
    home_open_at_boot
  else
    umount "$homeapp" 2>/dev/null
    home_reread
    if [ -x "$DAEMON_BIN" ]; then
      LD_LIBRARY_PATH="/var/lib/tvweb/native${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
        "$DAEMON_BIN" --release-home \
        --config /var/lib/tvweb/config.json \
        --assets-root /var/lib/tvweb/assets >/dev/null 2>&1 ||
        bootlog "native Home release failed after dispatch; not replaying it through shell"
    fi
    for pid in $(lg_home_pids); do kill "$pid" 2>/dev/null; done
    bootlog "sam did not take Native Tv Home - LG's Home kept"
  fi
}

native_home_takeover() {
  if native_boot_dispatch "Home lifecycle" --takeover-home \
    --config /var/lib/tvweb/config.json --assets-root /var/lib/tvweb/assets; then
    bootlog "native lifecycle took Native Tv Home's place"
    home_open_at_boot
  else
    dispatch_status=$?
    if [ "$dispatch_status" -eq 2 ]; then
      bootlog "native Home lifecycle unavailable - using compatibility path"
      legacy_home_takeover
    elif [ "$dispatch_status" -eq 3 ]; then
      bootlog "native Home lifecycle skipped by saved Home settings"
    elif [ "$dispatch_status" -eq 4 ]; then
      bootlog "native Home lifecycle skipped: firmware is unsupported or experimental Home is disabled"
    fi
  fi
}

home_takeover_boot() {
  if [ -f "$homestage/appinfo.json" ] || [ -f /var/lib/tvweb/assets/home-app/index.html ]; then
    if [ -x "$DAEMON_BIN" ]; then
      bootlog "Native Tv Home: checking native lifecycle"
      native_home_takeover
    elif [ "$home_os" -eq 1 ] &&
         grep -q '"enabled"[[:space:]]*:[[:space:]]*true' /var/lib/tvweb/home.json 2>/dev/null; then
      bootlog "Native Tv Home: taking LG's Home's place"
      legacy_home_takeover
    fi
  fi
}
home_takeover_boot

# LG's screen saver ships as QML up to webOS 9 and as Flutter from webOS 10.
# On the Flutter TVs, custom screen savers were followed by the picture muted,
# HDMI-CEC and ARC dead and sound on the TV speakers only, until a power cut,
# when switching to one restarted sam (#366). A block and release of the one
# app has replaced the restart, and has held on webOS 10 and 11 (#768), so
# they are applied there; later versions are untried and held back, unless
# config.json has "allowExperimentalScreensavers": true (allowOnWebos10 until
# 0.105, still read). sam is never restarted for one. The
# type is recorded for the server, which cannot
# read the stock manifest while one of ours is mounted over it.
screensaver_compat=0
if native_boot_dispatch "screen saver reconciliation" --reconcile-screensaver \
  --config /var/lib/tvweb/config.json --assets-root /var/lib/tvweb/assets; then
  stock_type=$(cat /var/lib/tvweb/screensaver-stock-type 2>/dev/null)
  if [ -n "$stock_type" ]; then
    bootlog "native screen saver lifecycle reconciled (stock $stock_type)"
  else
    bootlog "native screen saver lifecycle did not record LG's type after dispatch; not replaying it through shell"
  fi
else
  dispatch_status=$?
  if [ "$dispatch_status" -eq 2 ]; then
    screensaver_compat=1
    bootlog "native screen saver lifecycle unavailable - using compatibility path"
  fi
fi
if [ "$screensaver_compat" -eq 1 ]; then
ssapp=/usr/palm/applications/com.webos.app.screensaver
stock_type=$(sed -n 's/.*"type"[^"]*"\([^"]*\)".*/\1/p' "$ssapp/appinfo.json" 2>/dev/null)
[ -n "$stock_type" ] && echo "$stock_type" > /var/lib/tvweb/screensaver-stock-type
# A block and release cut short, by a crash or a power cut between the two,
# leaves LG's screen saver blocked, and the block is kept across boots. Only
# TVs whose stock screen saver is not QML ever block it, so there it is
# released once at every boot; with nothing blocked sam just refuses.
if [ -n "$stock_type" ] && [ "$stock_type" != "qml" ]; then
  luna-send -n 1 luna://com.webos.applicationManager/releaseBlockSystemApp '{"id":"com.webos.app.screensaver"}' >/dev/null 2>&1 || true
fi
held=0
if [ -n "$stock_type" ] && [ "$stock_type" != "qml" ] && [ "$webos_major" != "10" ] && [ "$webos_major" != "11" ]; then
  if grep -Eq '"(allowExperimentalScreensavers|allowOnWebos10)"[[:space:]]*:[[:space:]]*true' /var/lib/tvweb/config.json 2>/dev/null; then
    bootlog "allowExperimentalScreensavers set - custom screen saver not held back"
  else
    held=1
  fi
fi

# Restore the chosen screen saver. The app directory is on the read-only
# overlay, so the replacement is a bind mount and does not survive a reboot.
#
# sam.service is up well before this hook runs and reads each appinfo.json only
# once, so where the replacement changes the app's type it has to read the file
# again or the launch goes to the wrong runner and nothing draws.
if [ -f /var/lib/tvweb/screensaver/.tvweb-screensaver ]; then
  staged_type=$(sed -n 's/.*"type"[^"]*"\([^"]*\)".*/\1/p' /var/lib/tvweb/screensaver/appinfo.json 2>/dev/null)
  if [ "$held" -eq 1 ]; then
    bootlog "custom screen saver held back (stock $stock_type, ours ${staged_type:-unknown})"
  else
    mount --bind /var/lib/tvweb/screensaver "$ssapp" 2>/dev/null || true
    # --no-block: stopping sam waits on every app in its cgroup, which is most
    # of a minute, and no hook may hold up boot for that.
    if [ -n "$stock_type" ] && [ -n "$staged_type" ] && [ "$stock_type" != "$staged_type" ]; then
      if sam_reread_app com.webos.app.screensaver && sam_reports com.webos.app.screensaver "\"type\": *\"$staged_type\""; then
        bootlog "sam read the screen saver again after a block and release, without a restart"
      elif sam_nudge && sam_reports com.webos.app.screensaver "\"type\": *\"$staged_type\""; then
        bootlog "sam read the screen saver again, without a restart"
      else
        # sam still runs LG's: put LG's files back under it rather than restart it.
        umount "$ssapp" 2>/dev/null || true
        bootlog "sam did not read the custom screen saver again - LG's kept, sam not restarted (#366)"
      fi
    fi
  fi
fi
fi

# Restore hidden built-in app overrides if tile hiding is enabled and allowed.
# Not on a Homebrew Channel install, which does not offer it: restarting the app
# manager mid-boot is a risk that store asks its apps not to take.
#
# Tile hiding is experimental on every webOS version and takes
# "allowTileHiding": true in config.json. On a B8 (webOS 4) the kernel log
# showed tvservice faulting in the display engine driver and holding
# kadp-de-mtx for good, so live TV and HDMI stayed black until a reboot, while
# this restart coincided with an Apple TV switching to 4K Dolby Vision (#366).
# Before the server first writes the key, an install where tile hiding was
# running keeps it. On webOS 10 and later (a Flutter stock screen saver) it was
# already held back unless allowOnWebos10 (now allowExperimentalScreensavers)
# was set.
app_tile_compat=0
if native_boot_dispatch "app tile reconciliation" --reconcile-app-tiles \
  --config /var/lib/tvweb/config.json; then
  bootlog "native hidden app tile startup reconciliation completed"
else
  dispatch_status=$?
  if [ "$dispatch_status" -eq 2 ]; then
    app_tile_compat=1
    bootlog "native app tile startup reconciliation unavailable - using compatibility path"
  fi
fi
if [ "$app_tile_compat" -eq 1 ]; then
tile_hiding_allowed=0
if grep -q '"allowTileHiding"[[:space:]]*:[[:space:]]*true' /var/lib/tvweb/config.json 2>/dev/null; then
  tile_hiding_allowed=1
elif { [ -z "$stock_type" ] || [ "$stock_type" = "qml" ] ||
       grep -Eq '"(allowExperimentalScreensavers|allowOnWebos10)"[[:space:]]*:[[:space:]]*true' /var/lib/tvweb/config.json 2>/dev/null; } &&
     ! grep -q '"allowTileHiding"' /var/lib/tvweb/config.json 2>/dev/null &&
     [ "$(cat /var/lib/tvweb/tile_hiding_enabled 2>/dev/null)" = "1" ]; then
  tile_hiding_allowed=1
fi

if [ "$tile_hiding_allowed" -ne 1 ] && [ "$(cat /var/lib/tvweb/tile_hiding_enabled 2>/dev/null)" = "1" ]; then
  bootlog "tile hiding disabled in config (allowTileHiding not true)"
elif [ "$tile_hiding_allowed" -eq 1 ] && [ ! -f /var/lib/tvweb/.from-homebrew-channel ] && [ -f /var/lib/tvweb/tile_hiding_enabled ] && [ "$(cat /var/lib/tvweb/tile_hiding_enabled 2>/dev/null)" = "1" ] && [ -f /var/lib/tvweb/hidden_apps ]; then
  mounted=0
  while read -r app; do
    [ -z "$app" ] && continue
    ovr="/var/lib/tvweb/appinfo-overrides/$app.json"
    if [ -f "$ovr" ]; then
      for base in /media/system/apps/usr/palm/applications /usr/palm/applications /mnt/otncabi/usr/palm/applications /mnt/otycabi/usr/palm/applications; do
        tgt="$base/$app/appinfo.json"
        if [ -f "$tgt" ]; then
          mount --bind "$ovr" "$tgt" 2>/dev/null && mounted=1 && first_hidden=${first_hidden:-$app}
        fi
      done
    fi
  done < /var/lib/tvweb/hidden_apps
  if [ "$mounted" -eq 1 ] && sam_nudge && sam_reports "$first_hidden" '"visible": *false'; then
    bootlog "sam read the hidden tiles, without a restart"
  elif [ "$mounted" -eq 1 ]; then
    # Capture the active foreground app before restarting SAM so we can restore it
    fg_app=$(luna-send -n 1 -f luna://com.webos.applicationManager/getForegroundAppInfo '{}' 2>/dev/null | sed -n 's/.*"appId": *"\([^"]*\)".*/\1/p')

    if command -v systemctl >/dev/null 2>&1; then
      killall -9 LunaExecutable >/dev/null 2>&1 || true
      systemctl kill -s 9 sam.service >/dev/null 2>&1 || systemctl restart --no-block sam >/dev/null 2>&1 || true
    elif command -v initctl >/dev/null 2>&1; then
      initctl restart sam >/dev/null 2>&1 || pkill -9 -x sam >/dev/null 2>&1 || true
    else
      pkill -9 -x sam >/dev/null 2>&1 || true
    fi

    # Wait for SAM to become responsive (support BusyBox usleep with fallback)
    for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do
      usleep 200000 2>/dev/null || sleep 1
      if luna-send -n 1 -f luna://com.webos.applicationManager/getForegroundAppInfo '{}' >/dev/null 2>&1; then
        break
      fi
    done

    # If the user was on an HDMI port or any non-home app, immediately restore it
    # so they are never stranded on the Home screen
    if [ -n "$fg_app" ] && [ "$fg_app" != "com.webos.app.home" ]; then
      luna-send -n 1 -f luna://com.webos.applicationManager/launch "{\"id\":\"$fg_app\"}" >/dev/null 2>&1 || true
    fi
  fi
fi
fi

# Detach fully so upstart/webosbrew startup is never held up by this.
# tvwebctl starts the watchdog alongside the daemon.
(
  # Started as soon as WebAppMgr is up (below): the sooner it is up, the
  # sooner Home Assistant has the TV's state after a power-on, and the sooner
  # Native Tv Home's page, which shows the Native Tv mark until it answers,
  # can load. It used to wait 5 s for the
  # busiest part of boot to pass; the services hook's systemd reload, the
  # largest part of that, now waits for the server instead (lib/services.js).
  # The server retries MQTT, so it is fine to start before the network settles.
  # The Node server of the release before this one, on an install not yet
  # restarted since the update. The wait is for it to die, so none when there
  # was none, as at boot.
  if /usr/bin/pkill -9 -f /var/lib/tvweb/tvweb.js 2>/dev/null; then sleep 1; fi
  if [ -f /var/lib/tvweb/services_stopped ]; then
    while read -r job; do
      [ -n "$job" ] && /sbin/initctl stop "$job" >/dev/null 2>&1
    done < /var/lib/tvweb/services_stopped
  fi

  # Started alongside WebAppMgr, the server's start held WebAppMgr back: on a B3
  # (webOS 10.3.2), three early-start boots each way, it was up at 35-39 s
  # with the server waiting for it and at 47-54 s without, the server
  # listening 6 s later. Native Tv Home draws through WebAppMgr, which drew it
  # 10 s after its start with the server held and 21 s after with it running.
  # Home in front ends the wait too: systemd has called WebAppMgr active both
  # 9.6 s after it drew Home and 12 s before. Held until Home was in front,
  # Home came 4 s sooner but showed its own mark 5 s waiting for the server;
  # the full view came at 49-50 s either way. Capped, as a TV whose WebAppMgr never comes up still wants the server;
  # timed by the clock, as each systemctl call took about 2 s.
  if [ -f /etc/systemd/system/webapp-mgr.service ]; then
    hold_from=$(cut -d. -f1 /proc/uptime)
    while [ $(( $(cut -d. -f1 /proc/uptime) - hold_from )) -lt 30 ] &&
          ! grep -q "'com.webos.app.home' is foreground" /var/log/bootd.log 2>/dev/null &&
          ! systemctl is-active -q webapp-mgr.service 2>/dev/null; do
      sleep 1
    done
    bootlog "server held $(( $(cut -d. -f1 /proc/uptime) - hold_from )) s for WebAppMgr"
  fi
  bootlog "starting the server"
  /var/lib/tvweb/tvwebctl start
) &

# The early start (assets/early-service) is done with: it left this flag until here.
rm -f /var/lib/tvweb/early-pending

exit 0
