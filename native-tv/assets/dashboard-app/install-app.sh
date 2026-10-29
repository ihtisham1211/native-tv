#!/bin/sh
# Package a tvweb app as a webOS app and install it, so it appears on the home
# screen (or, for the splash, is there to launch). Runs on the TV, from the
# tools already there, and is safe to re-run: it reinstalls in place.
#
# The dashboard's tile is the native client (nativetv-tv with its libraries,
# fonts and icons, staged beside this script by runtime_payload.py), which
# opens on the dashboard screen and takes its data from the tvweb server on this
# TV. The splash is the same client opened on the splash screen. The old
# Home app (assets/home-app) is still a web page.
#
# Only the developer install service takes an unsigned app; the retail one
# rejects it. That service answers on webOS 9. Where it is absent or the install
# does not complete - as on older webOS - this exits without failing.

ACTION=${1:-install}
SRC=${2:-/var/lib/tvweb/assets/dashboard-app}
# The same script installs nativeTv Home's app (assets/home-app) and the splash
# (assets/splash-app), named by the app's own appinfo.json. Only the dashboard's
# app gives way to the Homebrew Channel's tile: the others have no other.
ID=$(sed -n 's/^ *"id": *"\([^"]*\)".*/\1/p' "$SRC/appinfo.json" 2>/dev/null | head -n 1)
[ -n "$ID" ] || ID=com.tvweb.dashboard
TYPE=$(sed -n 's/^ *"type": *"\([^"]*\)".*/\1/p' "$SRC/appinfo.json" 2>/dev/null | head -n 1)
# What a native app runs: the executable beside appinfo.json.
MAIN=$(sed -n 's/^ *"main": *"\([^"]*\)".*/\1/p' "$SRC/appinfo.json" 2>/dev/null | head -n 1)
VER=1.0.0
ARCH=all
[ "$TYPE" = native ] && ARCH=arm
WHAT="dashboard"
[ "$ID" = com.tvweb.dashboard ] || WHAT="$ID"
[ "$ID" = com.tvweb.splash ] && WHAT="native startup splash"
# Where the developer install service puts an app, and so how to tell whether
# this one is on the home screen without asking the bus. TVWEB_APPS_ROOT is for
# the tests, which run this script against a stand-in tree.
APPS=${TVWEB_APPS_ROOT:-/media/developer/apps/usr/palm}
APPDIR=$APPS/applications/$ID
PKGDIR=$APPS/packages/$ID

installed() { [ -d "$APPDIR" ]; }

# The files an app is made of: everything beside this script except the script
# and the package manifest, which is installed with the packages. Install and
# refresh both walk this list, so what one copies the other compares. Names are
# the staged payload's own (letters, digits and ._+-), so they are not quoted.
app_files() { ( cd "$SRC" && find . -type f ! -name install-app.sh ! -name packageinfo.json ) | sed 's|^\./||' | sort; }
# The text of file $1 as the TV installs it: the home page carries its port.
installed_form() { if [ "$1" = index.html ]; then ported "$SRC/$1"; else cat "$SRC/$1"; fi; }

# The app's page opens the dashboard at the port on its `var PORT = n; // tvweb:port`
# line, which the server keeps in step with config.json at every start. The copy
# installed here carries the same port, or a refresh would see the server's edit
# as a stale page and reinstall it, and a fresh install would open the wrong port.
PORT=$(/var/lib/tvweb/native/nativetv-service port --data-root /var/lib/tvweb 2>/dev/null)
case "$PORT" in ''|*[!0-9]*) PORT=8080 ;; esac
ported() { sed "s/\(var PORT = \)[0-9]*\(; *\/\/ tvweb:port\)/\1$PORT\2/" "$1"; }

# Set when the server was put in place by the app the Homebrew Channel installs.
# That app is then the tile, so this one is retired rather than offered.
HBC_MARK=/var/lib/tvweb/.from-homebrew-channel
from_hbc() { [ "$ID" = com.tvweb.dashboard ] && [ -f "$HBC_MARK" ]; }

# Take this tile away now the Homebrew Channel app replaces it - but never
# while it is running. Closing an open app from outside made a webOS 4 TV
# relaunch its last input in the background, after which that input would not
# open until a reboot. A running tile is left for a later start, and the
# server tries again at every boot, when nothing is open. Only the install
# service removes it: deleting the files behind the app manager's back leaves
# it confused until a reboot.
if [ "$ACTION" = retire ]; then
  installed || { echo '{"ok":true,"retired":false,"reason":"absent"}'; exit 0; }
  RUN=$(luna-send -n 1 -w 5000 luna://com.webos.applicationManager/running '{}' 2>/dev/null)
  # No answer means no way to know it is closed, so leave it alone.
  echo "$RUN" | grep -q '"returnValue":true' || { echo '{"ok":true,"retired":false,"reason":"unknown"}'; exit 0; }
  if echo "$RUN" | grep -q "\"id\":\"$ID\""; then
    echo '{"ok":true,"retired":false,"reason":"running"}'; exit 0
  fi
  RLOG=$(mktemp /tmp/tvweb-rt.XXXXXX) || exit 0
  luna-send -i -w 60000 luna://com.webos.appInstallService/dev/remove \
    "{\"id\":\"$ID\",\"subscribe\":true}" > "$RLOG" 2>&1 &
  rpid=$!
  n=0
  while [ "$n" -lt 30 ]; do
    installed || break
    grep -q 'errorText\|failed' "$RLOG" 2>/dev/null && break
    sleep 1; n=$((n + 1))
  done
  kill "$rpid" 2>/dev/null
  rm -f "$RLOG"
  if installed; then echo '{"ok":false,"retired":false,"reason":"service"}'
  else echo '{"ok":true,"retired":true}'; fi
  exit 0
fi

# Reported as one JSON line so the server can read it back. "supported" is
# whether this TV will take an unsigned app at all.
if [ "$ACTION" = status ]; then
  sup=false; [ -f "$SRC/appinfo.json" ] && command -v luna-send >/dev/null 2>&1 && sup=true
  # A native app with no executable beside it (a tree without the ARM payload) has nothing to run.
  [ "$TYPE" = native ] && [ ! -f "$SRC/$MAIN" ] && sup=false
  from_hbc && sup=false        # the Homebrew Channel app is the tile
  ins=false; installed && ins=true
  printf '{"ok":true,"supported":%s,"installed":%s}\n' "$sup" "$ins"
  exit 0
fi

if [ "$ACTION" = remove ]; then
  command -v luna-send >/dev/null 2>&1 || { echo '{"ok":false,"error":"not supported on this TV"}'; exit 0; }
  installed || { echo '{"ok":true,"installed":false}'; exit 0; }
  RLOG=$(mktemp /tmp/tvweb-rm.XXXXXX) || exit 0
  luna-send -i -w 60000 luna://com.webos.appInstallService/dev/remove \
    "{\"id\":\"$ID\",\"subscribe\":true}" > "$RLOG" 2>&1 &
  rpid=$!
  n=0
  while [ "$n" -lt 20 ]; do
    installed || break
    grep -q 'errorText\|failed' "$RLOG" 2>/dev/null && break
    sleep 1; n=$((n + 1))
  done
  kill "$rpid" 2>/dev/null
  # The service is not always able to finish the job; clearing the directories
  # leaves no tile either way, and sam drops the registration at the next boot.
  rm -rf "$APPDIR" "$PKGDIR" 2>/dev/null
  rm -f "$RLOG"
  if installed; then echo '{"ok":false,"error":"the TV would not remove it"}'
  else echo '{"ok":true,"installed":false}'; fi
  exit 0
fi

# An installed app is a copy packaged when it was added, so later changes to its
# files never reach the home screen by themselves. Reinstall it in place when
# they differ. Only an app that is installed is touched, so a removed one stays
# removed.
if [ "$ACTION" = refresh ]; then
  installed || { echo '{"ok":true,"installed":false,"refreshed":false}'; exit 0; }
  from_hbc && { echo '{"ok":true,"installed":true,"refreshed":false}'; exit 0; }
  # A reinstall would pull the files from under a client that is open; it is
  # done at a later start instead, when nothing is.
  if luna-send -n 1 -w 5000 luna://com.webos.applicationManager/running '{}' 2>/dev/null | grep -q "\"id\":\"$ID\""; then
    echo '{"ok":true,"installed":true,"refreshed":false,"reason":"running"}'; exit 0
  fi
  stale=""
  # packageinfo.json is kept with the packages, the rest with the app.
  [ "$(md5sum < "$SRC/packageinfo.json")" = "$(md5sum < "$PKGDIR/packageinfo.json" 2>/dev/null)" ] || stale=1
  for f in $(app_files); do
    [ -z "$stale" ] || break
    [ "$(installed_form "$f" | md5sum)" = "$(md5sum < "$APPDIR/$f" 2>/dev/null)" ] || stale=1
  done
  [ -n "$stale" ] || { echo '{"ok":true,"installed":true,"refreshed":false}'; exit 0; }
  ACTION=install
fi

if from_hbc; then
  echo "$WHAT app not added: the Homebrew Channel app is on the home screen instead"
  exit 0
fi

# The dashboard and Home apps are windows onto the server, so with the dashboard
# switched off they would open to nothing. The splash has no server dependency.
CONF=/var/lib/tvweb/config.json
if [ "$ID" != com.tvweb.splash ] && [ -f "$CONF" ] && tr -d ' \t\r\n' < "$CONF" | grep -q '"web":{[^}]*"enabled":false'; then
  echo "$WHAT app not added: the dashboard is switched off in config.json"
  exit 0
fi

[ -f "$SRC/appinfo.json" ] || { echo "$WHAT app files not found in $SRC"; exit 0; }
[ "$TYPE" != native ] || [ -f "$SRC/$MAIN" ] || { echo "$WHAT app files not found in $SRC"; exit 0; }
command -v luna-send >/dev/null 2>&1 || { echo "no luna-send; skipping the home-screen app"; exit 0; }

WORK=$(mktemp -d /tmp/tvweb-app.XXXXXX) || exit 0
trap 'rm -rf "$WORK"' EXIT

# The installer wants the app under usr/palm/applications and a matching package
# manifest under usr/palm/packages; without the package manifest it fails.
APP="$WORK/data/usr/palm/applications/$ID"
PKG="$WORK/data/usr/palm/packages/$ID"
mkdir -p "$APP" "$PKG"
cp "$SRC/packageinfo.json" "$PKG/" || exit 0
for f in $(app_files); do
  mkdir -p "$APP/$(dirname "$f")" || exit 0
  installed_form "$f" > "$APP/$f" || exit 0
done
# A native app runs from its own folder: its libraries are found beside it by
# the executable's $ORIGIN runpath. Copies from a Homebrew package come out
# 0666, so the executable's bit is set here.
[ "$TYPE" != native ] || chmod 755 "$APP/$MAIN" || exit 0

printf '2.0\n' > "$WORK/debian-binary"
cat > "$WORK/control" <<CTRL
Package: $ID
Version: $VER
Architecture: $ARCH
Maintainer: tvweb
Description: tvweb $WHAT
CTRL

# tar then gzip separately: the BusyBox tar on webOS 9 is built without gzip, so
# `tar -z` is unavailable there.
( cd "$WORK" && tar -cf control.tar ./control && gzip -nf control.tar )
( cd "$WORK/data" && tar -cf ../data.tar ./usr && gzip -nf ../data.tar )

# Assemble the ar archive by hand: BusyBox `ar` extracts but cannot create, and
# the ipk format is just an ar of three members with 60-byte headers.
IPK="$WORK/${ID}_${VER}_all.ipk"
printf '!<arch>\n' > "$IPK"
ar_add() {
  f="$WORK/$1"; sz=$(wc -c < "$f")
  printf '%-16s%-12d%-6d%-6d%-8s%-10d`\n' "$1" 0 0 0 100644 "$sz" >> "$IPK"
  cat "$f" >> "$IPK"
  [ $((sz % 2)) -eq 1 ] && printf '\n' >> "$IPK"   # members are padded to even
  return 0
}
ar_add debian-binary
ar_add control.tar.gz
ar_add data.tar.gz

LOG="$WORK/install.log"
luna-send -i -w 60000 luna://com.webos.appInstallService/dev/install \
  "{\"id\":\"$ID\",\"ipkUrl\":\"$IPK\",\"subscribe\":true}" > "$LOG" 2>&1 &
lpid=$!
n=0
while [ "$n" -lt 25 ]; do
  grep -q '"state":"installed"' "$LOG" 2>/dev/null && break
  grep -q 'errorText\|install failed' "$LOG" 2>/dev/null && break
  sleep 1; n=$((n + 1))
done
kill "$lpid" 2>/dev/null

if grep -q '"state":"installed"' "$LOG" 2>/dev/null; then
  echo "$WHAT added to the home screen"
else
  echo "$WHAT app not installed (the dashboard is still served to browsers)"
fi
