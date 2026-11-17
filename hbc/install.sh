#!/bin/sh
# Put the server this app carries in place, or bring it up to date, and make
# sure it is running. Run as root by the app's own launch page, through the
# Homebrew Channel's exec service, whenever the server does not answer with the
# version the app was built with. Safe to run any number of times.
#
# Does what deploy.sh does on the TV, from files that arrived inside the app
# rather than over SSH. Installs made either way share /var/lib/tvweb, so one
# made with deploy.sh is upgraded in place, config and all.
#
# Prints one JSON line last, which the launch page reads.

HERE=$(cd "$(dirname "$0")" && pwd)
SRC="$HERE/tvweb"
D=/var/lib/tvweb
HOOKDIR=/var/lib/webosbrew/init.d
# What an update replaces, kept for a server that does not start. The names
# after TVWEB_ are only moved by native-tv/tests/runtime_package.py.
PREV="$D/.previous"
BEAT=${TVWEB_BEAT:-/var/run/tvweb.beat}
PIDFILE=${TVWEB_PIDFILE:-/var/run/tvweb.pid}
UP_WAIT=${TVWEB_UP_WAIT:-60}
SETTLE=${TVWEB_UPDATE_SETTLE_SECONDS:-12}

version_of() { tr -d '[:space:]' < "$1" 2>/dev/null; }
# An install from before the daemon was the whole server kept its version in tvweb.js.
old_version_of() { sed -n "s/^var TVWEB_VERSION = '\([^']*\)';.*/\1/p" "$1/tvweb.js" 2>/dev/null; }
# Whether dotted version $1 is newer than $2. BusyBox sort has no -V.
newer() {
  awk -v a="$1" -v b="$2" 'BEGIN {
    n = split(a, x, "."); m = split(b, y, "."); if (m > n) n = m
    for (i = 1; i <= n; i++) { if (x[i] + 0 > y[i] + 0) exit 0; if (x[i] + 0 < y[i] + 0) exit 1 }
    exit 1 }'
}
running() { [ -x "$D/tvwebctl" ] && "$D/tvwebctl" status 2>/dev/null | grep -q '^running'; }
web_off() {
  [ -f "$D/config.json" ] && tr -d ' \t\r\n' < "$D/config.json" | grep -q '"web":{[^}]*"enabled":false'
}
result() {   # action; also whether the dashboard is off, and the old tile's fate
  off=false; web_off && off=true
  printf '{"ok":true,"action":"%s","version":"%s","webOff":%s,"oldTile":%s}\n' \
    "$1" "$(version_of "$D/VERSION")" "$off" "${OLD_TILE:-null}"
}

# This app is the tile from now on: say so, which also stops the server's own
# installer offering or refreshing the one deploy.sh added, then retire that
# one if it is not open. Its answer is passed back for the launch page.
adopt() {
  # Records where the app lives, so the server can tell when it is uninstalled.
  dirname "$HERE" > "$D/.from-homebrew-channel"
  OLD_TILE=$(sh "$D/assets/dashboard-app/install-app.sh" retire 2>/dev/null | tail -1)
  case "$OLD_TILE" in "{"*) ;; *) OLD_TILE=null ;; esac
}
# A link into this app rather than a copy, as the Homebrew Channel asks: once
# the app is removed the link leads nowhere and nothing runs at boot. Replaces
# the copy an earlier build or deploy.sh left there.
link_hook() {
  # The same for the app itself, whose page has the Homebrew Channel run this
  # as root, and whose hook runs at boot.
  chmod -R go-w "$(dirname "$HERE")" 2>/dev/null
  chmod +x "$SRC/50-tvweb.sh" 2>/dev/null
  mkdir -p "$HOOKDIR" && ln -sf "$SRC/50-tvweb.sh" "$HOOKDIR/50-tvweb"
}
fail() { printf '{"ok":false,"error":"%s"}\n' "$1"; exit 0; }

# The files this update is about to replace, copied aside, so that a server which
# does not come up can be put back. An update goes ahead only when all of them
# are kept.
keep_previous() {
  rm -rf "$PREV" "$D/.update-failed"
  ( cd "$S" && find . -type f ) | while read -r f; do
    case "$f" in ./50-tvweb.sh) continue ;; esac
    [ -f "$D/$f" ] || continue
    mkdir -p "$PREV/$(dirname "$f")" && cp -p "$D/$f" "$PREV/$f" || exit 1
  done
}
# The server is up once it has written its first heartbeat, which tvwebctl
# removes when it stops one and the server writes as it finishes starting, and
# has gone on running for a while: the same process, with its heartbeat still
# there. A daemon that gets as far as its event loop and then dies, which the
# watchdog would start again under another pid, is not up.
came_up() {
  waited=0
  while [ ! -s "$BEAT" ]; do
    [ "$waited" -lt "$UP_WAIT" ] || return 1
    sleep 1
    waited=$((waited + 1))
  done
  daemon=$(cat "$PIDFILE" 2>/dev/null)
  case "$daemon" in ''|*[!0-9]*) return 1 ;; esac
  settled=0
  while [ "$settled" -lt "$SETTLE" ]; do
    kill -0 "$daemon" 2>/dev/null || return 1
    [ "$(cat "$PIDFILE" 2>/dev/null)" = "$daemon" ] || return 1
    [ -s "$BEAT" ] || return 1
    sleep 1
    settled=$((settled + 1))
  done
  return 0
}
# An update that did not take is undone: the files it replaced go back by rename,
# as the new ones went in, the old server is started, and what happened is left
# for the server to show. A first install has nothing to go back to.
abandon() {
  if [ -n "$have" ] && [ -d "$PREV" ]; then
    ( cd "$PREV" && find . -type f ) | while read -r f; do
      cp -p "$PREV/$f" "$D/$f.new" && mv -f "$D/$f.new" "$D/$f"
    done
    printf '{"version":"%s","error":"%s, so version %s was put back"}\n' "$want" "$1" "$have" > "$D/.update-failed"
    "$D/tvwebctl" restart >/dev/null 2>&1
  fi
  fail "$1"
}

# Same-version app launches also repair a missing or outdated native component.
# Newer server installs keep their own runtime when an older app is opened.
runtime_has_file() {
  for listed in $files; do
    [ "$listed" = "$1" ] && return 0
  done
  return 1
}
runtime_complete() {
  for f in nativetv-daemon nativetv-service LICENSE-civetweb.txt LICENSE-json-c.txt remotekeys-filter.js runtime.json; do
    [ -f "$SRC/native/$f" ] || return 1
  done
  files=$(sed -n 's/^[[:space:]]*"path":[[:space:]]*"\([^"]*\)".*/\1/p' "$SRC/native/runtime.json")
  [ -n "$files" ] || return 1
  for f in $files; do
    case "$f" in *[!A-Za-z0-9._+-]*|'') return 1 ;; esac
    [ -f "$SRC/native/$f" ] || return 1
  done
  for f in nativetv-daemon nativetv-service LICENSE-civetweb.txt LICENSE-json-c.txt remotekeys-filter.js; do
    runtime_has_file "$f" || return 1
  done
}
sync_native() {
  runtime_complete || return 1
  mkdir -p "$D/native" || return 1
  for f in $files runtime.json; do
    if ! cmp -s "$SRC/native/$f" "$D/native/$f"; then
      mkdir -p "$D/native/$(dirname "$f")" || return 1
      cp "$SRC/native/$f" "$D/native/$f.new" || return 1
      chmod 644 "$D/native/$f.new" || return 1
      case "$f" in nativetv-daemon|nativetv-service) chmod 755 "$D/native/$f.new" || return 1 ;; esac
      mv -f "$D/native/$f.new" "$D/native/$f" || return 1
    fi
  done
  chmod 755 "$D/native/nativetv-daemon" "$D/native/nativetv-service" || return 1
}

# One run at a time. The launch page can start this while the dashboard's update
# is still waiting for its server or putting the old one back, and two runs would
# clear each other's stage and copy of the old files. The lock is a symlink, whose
# creation is atomic and whose target says who holds it: the pid and the boot it
# belongs to, so a lock left by a power cut, or by a pid something else has since
# been given, is taken over.
LOCK="$D/.install.lock"
boot_id() { cat /proc/sys/kernel/random/boot_id 2>/dev/null; }
holder_alive() {   # the lock's target, pid:boot
  holder_pid=${1%%:*}
  case "$holder_pid" in ''|*[!0-9]*) return 1 ;; esac
  [ "${1#*:}" = "$(boot_id)" ] && kill -0 "$holder_pid" 2>/dev/null
}
release_lock() { [ "$(readlink "$LOCK" 2>/dev/null)" = "$ME" ] && rm -f "$LOCK"; }
acquire_lock() {
  mkdir -p "$D" || fail "could not create $D"
  ME="$$:$(boot_id)"
  if ! ln -s "$ME" "$LOCK" 2>/dev/null; then
    held=$(readlink "$LOCK" 2>/dev/null)
    holder_alive "$held" && return 1
    # Only the lock that was seen to be stale is removed, not one taken meanwhile.
    [ "$(readlink "$LOCK" 2>/dev/null)" = "$held" ] && rm -f "$LOCK"
    ln -s "$ME" "$LOCK" 2>/dev/null || return 1
  fi
  [ "$(readlink "$LOCK" 2>/dev/null)" = "$ME" ]
}

[ -f "$SRC/VERSION" ] || fail "the app has no server files in it"
runtime_complete || fail "the app has an incomplete native runtime"
acquire_lock || fail "an install is already running"
# Released on every way out, including a signal, which only a trap sees to.
trap release_lock EXIT
trap 'exit 1' HUP INT TERM
want=$(version_of "$SRC/VERSION")
have=$(version_of "$D/VERSION")
[ -n "$have" ] || have=$(old_version_of "$D")

# Already this version, or newer - never downgrade: only make sure it is up. At
# a cold boot the launch page can get here before the boot hook has started it.
if [ -n "$have" ] && { [ "$have" = "$want" ] || newer "$have" "$want"; }; then
  if [ "$have" = "$want" ]; then sync_native || fail "could not install the native runtime"; fi
  running || "$D/tvwebctl" start >/dev/null 2>&1
  link_hook
  adopt
  result started
  exit 0
fi

# Copied beside the install and renamed into place, never written over: BusyBox's
# shell reads a script as it runs it, so overwriting tvwebctl would corrupt the
# watchdog running out of it.
mkdir -p "$D" || fail "could not create $D"
S="$D/.hbc-stage"
rm -rf "$S"
cp -r "$SRC" "$S" || fail "could not copy the server files"
if [ -n "$have" ] && ! keep_previous; then
  rm -rf "$S" "$PREV"
  fail "there is not enough room to keep the version being replaced"
fi
( cd "$S" && find . -type f ) | while read -r f; do
  case "$f" in ./50-tvweb.sh) continue ;; esac
  mkdir -p "$D/$(dirname "$f")"
  mv -f "$S/$f" "$D/$f"
done
chmod +x "$D/tvwebctl" 2>/dev/null
# The Node server of earlier releases: nothing runs it now.
rm -rf "${D:?}/tvweb.js" "${D:?}/lib"
if [ -f "$D/native/nativetv-service" ] && [ -f "$D/native/nativetv-daemon" ]; then
  chmod 755 "$D/native/nativetv-daemon" "$D/native/nativetv-service" || abandon "could not make native runtimes executable"
fi
# ares-package stores plain files as 0666. The server runs as root, so nothing
# else on the TV may be able to change its code.
chmod -R go-w "$D" 2>/dev/null

# A first install starts closed - answering only on the TV itself - and asks
# the owner through the setup screens whether to open it to the network. One
# that already has a config, from deploy.sh or before, keeps it as it is.
if [ -z "$have" ] && [ ! -f "$D/config.json" ]; then
  printf '{\n  "host": "127.0.0.1"\n}\n' > "$D/config.json"
  : > "$D/.setup-pending"
fi
chmod 600 "$D/config.json" 2>/dev/null

rm -rf "$S"

[ "$(version_of "$D/VERSION")" = "$want" ] || abandon "the new files did not take"
link_hook
# Before the restart, so the server starting up finds nothing left to retire.
adopt
"$D/tvwebctl" restart >/dev/null 2>&1
# An upgrade is reported once the new server is running: the launch page and the
# dashboard's update both wait on this answer.
if [ -n "$have" ]; then
  came_up || abandon "the new server did not start"
  rm -rf "$PREV"
  result upgraded
else
  result installed
fi
