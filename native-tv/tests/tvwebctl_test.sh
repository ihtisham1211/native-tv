#!/bin/sh
# lifecycle/tvwebctl against a stand-in daemon: the watchdog restarts a wedged
# daemon, frees a stuck child and clears a dead daemon's leftovers, gives LG's
# Home its place back after repeated restarts and leaves a healthy daemon alone;
# the restores report the daemon's status and are never replayed through anything
# else. Needs Linux's /proc and start-stop-daemon; skipped (77) elsewhere.
#
# Usage: tvwebctl_test.sh [path to tvwebctl]

[ "$(uname -s)" = Linux ] && command -v start-stop-daemon >/dev/null 2>&1 || {
  echo "skipped: needs Linux with start-stop-daemon"
  exit 77
}

HERE=$(cd "$(dirname "$0")" && pwd)
CTL=$(readlink -f "${1:-$HERE/../lifecycle/tvwebctl}")
DIR=$(mktemp -d)
LEGACY=
cleanup() {
  sh "$CTL" stop >/dev/null 2>&1
  [ -n "$LEGACY" ] && kill -9 "$LEGACY" 2>/dev/null
  rm -rf "$DIR"
}
trap cleanup EXIT

fail() { echo "FAIL: $1"; echo "--- restarts ---"; cat "$RESTARTS" 2>/dev/null; echo "--- log ---"; tail -n 20 "$TVWEB_LOG" 2>/dev/null; exit 1; }
ok() { echo "  ok $1"; }
wait_for() { # description, test command, seconds
  n=0
  while [ "$n" -lt "$3" ]; do eval "$2" && return 0; sleep 1; n=$((n + 1)); done
  fail "timed out after $3 s waiting for $1"
}
alive() { kill -0 "$1" 2>/dev/null && ! grep -q '^State:[[:space:]]*Z' "/proc/$1/status" 2>/dev/null; }
pid() { cat "$PIDFILE" 2>/dev/null; }
mode() { printf '%s\n' "$1" > "$MODEFILE"; }

# The stand-in daemon. Started with --listen it records how it was started,
# what native.env gave it, and keeps the heartbeat the way MODEFILE says; any
# other call is a one-shot (a restore, release-home) that is recorded and exits
# with TVWEB_TEST_EXIT.
DAEMON=$DIR/nativetv-daemon
cat > "$DAEMON" <<'EOF'
#!/bin/sh
case "$*" in
  *--listen*) ;;
  *) echo "$@" >> "$CALLS"; exit "${TVWEB_TEST_EXIT:-0}" ;;
esac
echo "$@" > "$MODEFILE.args"
echo "$TVWEB_PROBE" > "$MODEFILE.probe"
while [ "$#" -gt 0 ]; do [ "$1" = --heartbeat ] && beat=$2; shift; done
spawned=
while true; do
  case "$(cat "$MODEFILE" 2>/dev/null)" in
    freeze) ;;
    stuck)
      if [ -z "$spawned" ]; then
        sh -c 'while :; do sleep 1; done' &
        echo $! > "$MODEFILE.child"
        spawned=1
      fi
      date +%s > "$beat" ;;
    *) date +%s > "$beat" ;;
  esac
  sleep 1
done
EOF
chmod 755 "$DAEMON"

export TVWEB_DAEMON=$DAEMON TVWEB_SERVICE=$DIR/no-service TVWEB_LOG=$DIR/tvweb.log
export TVWEB_LEGACY_DAEMON=$DIR/glasshouse-daemon
export TVWEB_PIDFILE=$DIR/tvweb.pid TVWEB_WATCHPID=$DIR/tvwebwatch.pid TVWEB_BEAT=$DIR/tvweb.beat
export TVWEB_RESTARTS=$DIR/tvweb.restarts TVWEB_CONFIG=$DIR/config.json TVWEB_ASSETS=$DIR/assets
export TVWEB_NATIVE_ENV=$DIR/native.env
export TVWEB_CHECK=2 TVWEB_STALE=4 TVWEB_STARTUP=0 TVWEB_STARTUP_STALE=4
export MODEFILE=$DIR/mode CALLS=$DIR/calls
PIDFILE=$TVWEB_PIDFILE
RESTARTS=$TVWEB_RESTARTS
echo '{}' > "$TVWEB_CONFIG"
printf 'TVWEB_PROBE=yes\n' > "$TVWEB_NATIVE_ENV"

ctl() { sh "$CTL" "$@"; }

# --- the restores: the daemon's status, and no replay
env TVWEB_TEST_EXIT=1 sh "$CTL" restore-keys; [ $? -eq 1 ] || fail "a failed key restore did not report the daemon's status"
grep -q -- '--restore-remote-keys' "$CALLS" || fail "restore-keys did not call the daemon"
rm -f "$CALLS"
ctl restore-nativetv-home || fail "restore-nativetv-home failed with a daemon that succeeded"
grep -q -- "--restore-nativetv-home --config $TVWEB_CONFIG" "$CALLS" || fail "restore-nativetv-home called the daemon with: $(cat "$CALLS")"
env TVWEB_TEST_EXIT=5 sh "$CTL" restore-nativetv-home; [ $? -eq 5 ] || fail "the daemon's status 5 was not passed on"
env TVWEB_DAEMON="$DIR/missing" sh "$CTL" restore-keys; [ $? -eq 1 ] || fail "restore-keys without a daemon did not fail"
env TVWEB_DAEMON="$DIR/missing" sh "$CTL" restore-nativetv-home; [ $? -eq 1 ] || fail "restore-nativetv-home without a daemon did not fail"
ok "restores pass the daemon's status on and are never replayed"

# --- the native stuck-child scan, and the shell scan without the service
SERVICE_STUB=$DIR/service
cat > "$SERVICE_STUB" <<'EOF'
#!/bin/sh
echo "$@" >> "$SERVICE_CALLS"
[ "$SERVICE_FAIL" = 1 ] && { echo "native scan error"; exit 1; }
echo "child 4322 stuck before exec (syscall 202 at 0x1000 in libc.so+0x10) - killed it"
EOF
chmod 755 "$SERVICE_STUB"
FUNC=$(sed -n '/^free_stuck_children() {/,/^}/p' "$CTL")
run_free() { # service path, fail flag
  rm -f "$DIR/service.calls" "$DIR/free.restarts" "$DIR/fallback"
  SERVICE_CALLS=$DIR/service.calls SERVICE_FAIL=$2 SERVICE=$1 RESTARTS=$DIR/free.restarts \
    PROC_ROOT=/fixture/proc STUCK_SETTLE_MS=17 sh -c \
    "free_stuck_children_compat() { echo fallback >> '$DIR/fallback'; }
$FUNC
free_stuck_children 4321" >/dev/null 2>&1
}
run_free "$SERVICE_STUB" 0
grep -q -- 'reap-stuck-children --pid 4321 --proc-root /fixture/proc --settle-ms 17' "$DIR/service.calls" &&
  grep -q 'child 4322 stuck before exec' "$DIR/free.restarts" && [ ! -e "$DIR/fallback" ] ||
  fail "the native stuck-child result was not logged"
run_free "$SERVICE_STUB" 1
grep -q 'native stuck-child scan failed' "$DIR/free.restarts" && [ ! -e "$DIR/fallback" ] ||
  fail "a failed native scan was replayed through the shell detector"
run_free "$DIR/no-service" 0
[ -e "$DIR/fallback" ] || fail "the shell detector was lost when the service is absent"
ok "the native stuck-child reaper is used, with the shell scan when the service is absent"
rm -f "$CALLS"

# --- start: the daemon, once, from /, with native.env
mode beat
ctl start >/dev/null
wait_for "the daemon and its heartbeat" 'alive "$(pid)" && [ -n "$(cat "$TVWEB_BEAT" 2>/dev/null)" ] && [ -s "$MODEFILE.args" ]' 10
first=$(pid)
ARGS=$(cat "$MODEFILE.args")
tr '\0' ' ' < "/proc/$first/cmdline" | grep -qF "$DAEMON" || fail "the process started is not the daemon"
case "$ARGS" in *--listen*--assets-root*|*--assets-root*--listen*) ;; *) fail "started with: $ARGS" ;; esac
case "$ARGS" in *"--config $TVWEB_CONFIG"*"--heartbeat $TVWEB_BEAT"*) ;; *) fail "config and heartbeat missing from: $ARGS" ;; esac
case "$ARGS" in *--node-port*) fail "a Node port was passed: $ARGS" ;; esac
[ "$(cat "$MODEFILE.probe")" = yes ] || fail "native.env did not reach the daemon"
ok "start runs only the daemon, with native.env's overrides"

ctl start >/dev/null
[ "$(pid)" = "$first" ] || fail "a second start replaced the pid file"
copies=$(ps -eo args | grep -F "$DAEMON" | grep -v grep | grep -c -- '--listen')
[ "$copies" -eq 1 ] || fail "a second start left $copies daemons running"
ok "a second start leaves the running daemon as the only one"

[ "$(readlink "/proc/$first/cwd")" = / ] || fail "the daemon does not run from /"
watch=$(cat "$TVWEB_WATCHPID" 2>/dev/null)
[ -z "$watch" ] || [ "$(readlink "/proc/$watch/cwd")" = / ] || fail "the watchdog does not run from /"

# --- a daemon that keeps beating is left alone
sleep 9
[ ! -s "$RESTARTS" ] || fail "a healthy daemon was restarted"
[ "$(pid)" = "$first" ] || fail "the daemon pid changed"
ok "a daemon that keeps beating is left alone"

# --- a frozen daemon is restarted after two stale checks
mode freeze
wait_for "the wedge restart" 'grep -q "event loop wedged" "$RESTARTS" 2>/dev/null' 40
mode beat
wait_for "a new daemon" '[ -n "$(pid)" ] && [ "$(pid)" != "$first" ] && alive "$(pid)"' 15
alive "$first" && fail "the frozen daemon was left running"
second=$(pid)
ok "a frozen daemon is restarted after two stale checks"

# --- a child stuck as the daemon's own binary is killed and the daemon carries on
rm -f "$MODEFILE.child"
mode stuck
wait_for "the stuck child to be killed" 'grep -q "stuck before exec" "$RESTARTS" 2>/dev/null' 30
mode beat
sleep 1
stuck=$(cat "$MODEFILE.child" 2>/dev/null)
[ -n "$stuck" ] && alive "$stuck" && fail "the stuck child is still running"
[ "$(pid)" = "$second" ] && alive "$second" || fail "the daemon was restarted for a stuck child"
ok "a child stuck before exec is killed, and the daemon carries on"

# --- a dead daemon's leftover copy is cleared before a new one starts
"$DAEMON" --config "$TVWEB_CONFIG" --heartbeat "$DIR/other.beat" --listen >/dev/null 2>&1 &
leftover=$!
kill -9 "$second"
wait_for "the restart after the daemon died" 'grep -q "process gone" "$RESTARTS" 2>/dev/null && [ "$(pid)" != "$second" ] && alive "$(pid)"' 30
sleep 1
alive "$leftover" && fail "the leftover copy is still running"
ok "a dead daemon's leftover copy is cleared before a new one starts"

# --- a third restart inside ten minutes gives LG's Home its place back
third=$(pid)
rm -f "$CALLS"
kill -9 "$third"
wait_for "LG's Home given back" 'grep -q "3 restarts in 600s - LG.s Home given back" "$RESTARTS" 2>/dev/null && [ "$(pid)" != "$third" ] && alive "$(pid)"' 30
grep -q -- '--release-home' "$CALLS" || fail "LG's Home was not released through the daemon"
ok "a third restart in ten minutes gives LG's Home its place back, and the daemon still starts"

# --- one watchdog: a restart clears an extra one, a start adds none
watchers() { ps -eo args | grep -F "$CTL watch" | grep -v grep | wc -l; }
sh "$CTL" watch >/dev/null 2>&1 &
extra=$!
wait_for "the extra watchdog" '[ "$(watchers)" -eq 2 ]' 5
ctl restart >/dev/null
sleep 1
alive "$extra" && fail "a restart left the extra watchdog running"
[ "$(watchers)" -eq 1 ] || fail "a restart left $(watchers) watchdogs"
ctl start >/dev/null
[ "$(watchers)" -eq 1 ] || fail "a start beside a watchdog left $(watchers)"
ok "a restart clears an extra watchdog, and a start adds none"

# --- the Node server of the release before this one is stopped too
# Its sleep outlives the shell that tvwebctl kills: no inherited stdio, or CTest waits on the pipe.
sh -c 'sleep 60; : /var/lib/tvweb/tvweb.js' >/dev/null 2>&1 </dev/null &
LEGACY=$!
wait_for "the stand-in for the old server" 'ps -eo args | grep -F /var/lib/tvweb/tvweb.js | grep -v grep | grep -q .' 5
ctl restart >/dev/null
sleep 1
alive "$LEGACY" && fail "a restart left the old Node server running"
LEGACY=
ok "a restart stops the Node server an earlier release left running"

# --- a daemon still running under the name from before the rename is stopped too
cp "$DAEMON" "$DIR/glasshouse-daemon"
"$DIR/glasshouse-daemon" --config "$TVWEB_CONFIG" --heartbeat "$DIR/old.beat" --listen >/dev/null 2>&1 &
earlier=$!
wait_for "the stand-in for the renamed daemon" 'ps -eo args | grep -F "$DIR/glasshouse-daemon --config" | grep -v grep | grep -q .' 5
ctl restart >/dev/null
sleep 1
alive "$earlier" && fail "a restart left the daemon under its old name running"
ok "a restart stops a daemon still running under its name from before the rename"

# --- stop leaves nothing
daemon=$(pid)
ctl stop >/dev/null
sleep 1
alive "$daemon" && fail "stop left the daemon running"
[ "$(watchers)" -eq 0 ] || fail "stop left a watchdog"
ok "stop ends the daemon and the watchdog"

echo "ALL tvwebctl_test.sh assertions passed"
