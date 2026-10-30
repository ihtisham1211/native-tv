#!/bin/bash
# Runs a client binary that is not installed on the TV, for a bounded time, under the same
# jailer command as the E2E cases, and prints its report and perf summary. Nothing installed
# is replaced: the binary goes to /tmp/nr beside symlinks to the installed app's libraries and
# assets, and everything is removed afterwards. The client is on screen for the run, and a
# running Home client (Home takeover) must be stopped first.
#
#   NATIVETV_SSH_KEY=~/.ssh/lg_tv_root NATIVETV_KNOWN_HOSTS=~/.ssh/lg_tv_known_hosts \
#     ./scripts/tv-client-run.sh <tv-ip> <arm-nativetv-tv> [--ms 5000] [--screen home] [--env NAME=VALUE]...
#
# Compare two builds by running this for each, alternating, several times each.

set -u

TV=""; BIN=""; MS=5000; SCREEN=home; ENVS=()
while [ $# -gt 0 ]; do
  case "$1" in
    --ms) MS="$2"; shift ;;
    --screen) SCREEN="$2"; shift ;;
    --env) ENVS+=("$2"); shift ;;
    -*) echo "unknown option: $1" >&2; exit 2 ;;
    *) if [ -z "$TV" ]; then TV="$1"; else BIN="$1"; fi ;;
  esac
  shift
done
[ -n "$TV" ] && [ -f "$BIN" ] || { echo "usage: $0 <tv-ip> <arm-nativetv-tv> [--ms N] [--screen S] [--env N=V]..." >&2; exit 2; }
case "$MS" in ''|*[!0-9]*) echo "--ms takes a whole number" >&2; exit 2 ;; esac

SSH_OPTS=(-o ConnectTimeout=6 -o BatchMode=yes)
[ -n "${NATIVETV_SSH_KEY:-}" ] && SSH_OPTS+=(-i "$NATIVETV_SSH_KEY" -o IdentitiesOnly=yes)
[ -n "${NATIVETV_KNOWN_HOSTS:-}" ] && SSH_OPTS+=(-o "UserKnownHostsFile=$NATIVETV_KNOWN_HOSTS")
tv() { ssh "${SSH_OPTS[@]}" "root@$TV" "$@"; }

tv 'rm -rf /tmp/nr && mkdir -p /tmp/nr && cat > /tmp/nr/nativetv-tv && chmod +x /tmp/nr/nativetv-tv' < "$BIN" || exit 1
# The jail lets the client write directly in /tmp, not in a subdirectory.
tv "MS=$MS SCREEN=$SCREEN EXTRAENV='${ENVS[*]:-}' sh -s" <<'EOF'
A=/media/developer/apps/usr/palm/applications/com.tvweb.dashboard
R=/tmp/nr
for f in $A/*; do b=$(basename $f); [ "$b" = nativetv-tv ] || ln -sf $f $R/$b; done
cd $A
env XDG_RUNTIME_DIR=/tmp/xdg $EXTRAENV timeout 60 /usr/bin/jailer -t native_devmode -i com.tvweb.dashboard -p $A $R/nativetv-tv '{"@system_native_app":true,"nid":"com.tvweb.dashboard"}' --server http://127.0.0.1:8080 --screen $SCREEN --duration-ms $MS --report /tmp/nr-rep.json --perf /tmp/nr-perf.jsonl > $R/out 2>&1
echo "exit=$?"
grep '^perf:' $R/out
cat /tmp/nr-rep.json
rm -rf /tmp/nr /tmp/nr-rep.json /tmp/nr-perf.jsonl
EOF
