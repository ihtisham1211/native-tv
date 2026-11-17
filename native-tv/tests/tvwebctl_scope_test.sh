#!/bin/sh
# lifecycle/tvwebctl's systemd scope: on a TV with systemd-run, start and restart
# run again under `systemd-run --scope`, once, so the daemon and its watchdog are
# not in the cgroup of the LG service that started them; where there is no
# systemd-run, where it refuses, off a TV, inside a scope already and for the other
# actions, they run where they are. systemd-run and start-stop-daemon are stand-ins
# that record their calls, so this runs on any computer.
#
# Usage: tvwebctl_scope_test.sh [path to tvwebctl]

HERE=$(cd "$(dirname "$0")" && pwd)
CTL=$(readlink -f "${1:-$HERE/../lifecycle/tvwebctl}")
DIR=$(mktemp -d)
trap 'rm -rf "$DIR"' EXIT

fail() { echo "FAIL: $1"; echo "--- output ---"; cat "$DIR/out" 2>/dev/null; echo "--- systemd-run ---"; cat "$DIR/scope-calls" 2>/dev/null; echo "--- start-stop-daemon ---"; cat "$DIR/daemon-calls" 2>/dev/null; exit 1; }
ok() { echo "  ok $1"; }

# Only what tvwebctl calls, so that a systemd-run of this computer is not found.
mkdir "$DIR/bin"
for tool in sh grep cat tr ps awk kill sleep date readlink dirname chmod rm wc tail cut head env sed true; do
  path=$(command -v "$tool")
  case "$path" in /*) ln -s "$path" "$DIR/bin/$tool" ;; esac
done
# Records how it was asked, then runs the command as systemd-run --scope does.
# SYSTEMD_RUN_REFUSES makes it fail as it does where no scope can be made.
cat > "$DIR/systemd-run" <<'EOF'
#!/bin/sh
echo "scoped=${TVWEB_SCOPED:-} $*" >> "$SCOPE_CALLS"
[ -n "$SYSTEMD_RUN_REFUSES" ] && exit 1
while [ "$#" -gt 0 ]; do
  case "$1" in --scope|--unit=*) shift ;; *) break ;; esac
done
# /bin/true is not on every computer this runs on.
[ "$1" = /bin/true ] && exit 0
exec "$@"
EOF
chmod 755 "$DIR/systemd-run"
cat > "$DIR/bin/start-stop-daemon" <<'EOF'
#!/bin/sh
echo "$*" >> "$DAEMON_CALLS"
EOF
chmod 755 "$DIR/bin/start-stop-daemon"
printf '#!/bin/sh\nexit 0\n' > "$DIR/nativetv-daemon"
chmod 755 "$DIR/nativetv-daemon"
mkdir "$DIR/palm"

export PATH="$DIR/bin" SCOPE_CALLS="$DIR/scope-calls" DAEMON_CALLS="$DIR/daemon-calls"
export TVWEB_DAEMON="$DIR/nativetv-daemon" TVWEB_SERVICE="$DIR/no-service" TVWEB_LOG="$DIR/tvweb.log"
export TVWEB_PIDFILE="$DIR/tvweb.pid" TVWEB_WATCHPID="$DIR/tvwebwatch.pid" TVWEB_BEAT="$DIR/tvweb.beat"
export TVWEB_RESTARTS="$DIR/tvweb.restarts" TVWEB_CONFIG="$DIR/config.json" TVWEB_NATIVE_ENV="$DIR/native.env"
export TVWEB_PALM_APPS="$DIR/palm"

# Runs tvwebctl with the scope calls and daemon starts it makes counted. RUN_ENV holds
# NAME=value words for this run alone.
RUN_ENV=
run() { # arguments of tvwebctl
  : > "$SCOPE_CALLS"; : > "$DAEMON_CALLS"
  # shellcheck disable=SC2086  # words, none with a space
  env $RUN_ENV sh "$CTL" "$@" >"$DIR/out" 2>&1
}
scope_calls() { grep -c . "$SCOPE_CALLS"; }
daemon_starts() { grep -c -e '--config' "$DAEMON_CALLS"; }

# --- on a TV with systemd-run: tried once with a command that does nothing, then the action, in a scope
cat "$DIR/systemd-run" > "$DIR/bin/systemd-run" && chmod 755 "$DIR/bin/systemd-run"
run start
[ "$(scope_calls)" -eq 2 ] || fail "start made $(scope_calls) systemd-run calls, not the probe and the action"
sed -n 1p "$SCOPE_CALLS" | grep -q '^scoped= --scope --unit=tvweb-probe-[0-9]* /bin/true$' ||
  fail "the first call was not the probe"
sed -n 2p "$SCOPE_CALLS" | grep -q "^scoped=1 --scope --unit=tvweb-[0-9]* /bin/sh $CTL start\$" ||
  fail "the second call was not start in a scope, marked as scoped"
[ "$(daemon_starts)" -eq 1 ] || fail "the daemon was started $(daemon_starts) times, not once"
grep -q -e 'watch' "$DAEMON_CALLS" || fail "the watchdog was not started"
ok "start runs once, in a scope of its own, after a probe"

run restart
sed -n 2p "$SCOPE_CALLS" | grep -q "^scoped=1 --scope --unit=tvweb-[0-9]* /bin/sh $CTL restart\$" ||
  fail "restart was not run in a scope"
[ "$(daemon_starts)" -eq 1 ] || fail "restart started the daemon $(daemon_starts) times, not once"
ok "restart runs once, in a scope of its own"

# --- the other actions and a command already in a scope are left where they are
for action in status stop; do
  run "$action"
  [ "$(scope_calls)" -eq 0 ] || fail "$action asked systemd-run for a scope"
done
RUN_ENV=TVWEB_SCOPED=1 run start
[ "$(scope_calls)" -eq 0 ] && [ "$(daemon_starts)" -eq 1 ] || fail "a start already in a scope made another"
ok "stop, status and a start already in a scope do not ask for one"

# --- off a TV, and where systemd-run is missing or refuses: where they are, once
RUN_ENV=TVWEB_PALM_APPS="$DIR/no-palm" run start
[ "$(scope_calls)" -eq 0 ] && [ "$(daemon_starts)" -eq 1 ] || fail "a computer that is not a TV was given a scope"
ok "off a TV nothing is scoped"

RUN_ENV=SYSTEMD_RUN_REFUSES=1 run start
[ "$(scope_calls)" -eq 1 ] || fail "a refused probe was followed by $(scope_calls) calls"
[ "$(daemon_starts)" -eq 1 ] || fail "after a refused scope the daemon was started $(daemon_starts) times, not once"
grep -q 'did not start it in a scope of its own' "$DIR/out" || fail "a refused scope was not said"
ok "where systemd-run refuses, the daemon starts where it is, once"

rm "$DIR/bin/systemd-run"
RUN_ENV=
run start
[ "$(daemon_starts)" -eq 1 ] || fail "without systemd-run the daemon was started $(daemon_starts) times, not once"
grep -q 'scope' "$DIR/out" && fail "a TV without systemd-run said something about a scope"
ok "where there is no systemd-run, the daemon starts where it is"

echo "ALL tvwebctl_scope_test.sh assertions passed"
