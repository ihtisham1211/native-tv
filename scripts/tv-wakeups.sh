#!/bin/bash
# Which threads of the server wake the CPU while the TV sits idle: context switches a second
# per thread of nativetv-daemon, and the total. A thread near 20 a second is a 50 ms poll loop.
# Reads /proc over SSH; nothing on the TV is changed.
#
#   NATIVETV_SSH_KEY=~/.ssh/lg_tv_root NATIVETV_KNOWN_HOSTS=~/.ssh/lg_tv_known_hosts \
#     ./scripts/tv-wakeups.sh <tv-ip> [--seconds 20]

set -u

TV=""; SECS=20
while [ $# -gt 0 ]; do
  case "$1" in
    --seconds) SECS="$2"; shift ;;
    -*) echo "unknown option: $1" >&2; exit 2 ;;
    *) TV="$1" ;;
  esac
  shift
done
[ -n "$TV" ] || { echo "usage: $0 <tv-ip> [--seconds 20]" >&2; exit 2; }
case "$SECS" in ''|*[!0-9]*) echo "--seconds takes a whole number" >&2; exit 2 ;; esac

SSH_OPTS=(-o ConnectTimeout=6 -o BatchMode=yes)
[ -n "${NATIVETV_SSH_KEY:-}" ] && SSH_OPTS+=(-i "$NATIVETV_SSH_KEY" -o IdentitiesOnly=yes)
[ -n "${NATIVETV_KNOWN_HOSTS:-}" ] && SSH_OPTS+=(-o "UserKnownHostsFile=$NATIVETV_KNOWN_HOSTS")

# Runs on the TV: busybox ash, so arithmetic goes through awk.
ssh "${SSH_OPTS[@]}" "root@$TV" "SECS=$SECS sh -s" <<'EOF'
pid=$(cat /var/run/tvweb.pid 2>/dev/null)
[ -n "$pid" ] && [ -d "/proc/$pid" ] ||
  pid=$(ps -eo pid,args | grep 'nativetv-daemon --config' | grep -v grep | awk '{print $1; exit}')
[ -n "$pid" ] || { echo "the server is not running"; exit 1; }
snap() {
  for t in /proc/$pid/task/*; do
    echo "$(basename "$t") $(cat "$t/comm" 2>/dev/null) $(awk '/ctxt_switches/{s+=$2} END{print s+0}' "$t/status" 2>/dev/null)"
  done
}
snap > /tmp/wakeups.0
sleep "$SECS"
snap > /tmp/wakeups.1
awk -v s="$SECS" 'NR==FNR{a[$1]=$3; next} $1 in a {d=($3-a[$1])/s; total+=d; if (d>=1) printf "%s %s %.1f/s\n", $1, $2, d}
  END{printf "total %.1f/s over %d threads\n", total, NR-FNR}' /tmp/wakeups.0 /tmp/wakeups.1 | sort -k3 -rn
rm -f /tmp/wakeups.0 /tmp/wakeups.1
EOF
