#!/bin/bash
# Samples the server on the TV once an interval for as long as asked, to see whether it leaks or
# restarts: resident memory and its peak, threads, open files, `luna-send` children and the load
# average, one line a sample. Reads /proc over SSH; nothing on the TV is changed. A summary
# follows the last sample, and `--summary FILE` prints one for a file that is still being
# written (a run of 24 hours can be read at any point).
#
#   NATIVETV_SSH_KEY=~/.ssh/lg_tv_root NATIVETV_KNOWN_HOSTS=~/.ssh/lg_tv_known_hosts \
#     ./scripts/tv-memory-soak.sh <tv-ip> [--hours 24] [--interval 60] [--out FILE]
#   ./scripts/tv-memory-soak.sh --summary FILE
#
# Run it detached for a long soak: nohup setsid ./scripts/tv-memory-soak.sh ... &

set -u

summary() {
  awk '
    { for (i = 1; i <= NF; i++) { split($i, kv, "="); v[kv[1]] = kv[2] } }
    /^#/ { next }
    !/rss_kB=/ { failed++; next }
    { n++; if (!first) { first = v["rss_kB"]; firstAt = $1 }
      last = v["rss_kB"]; lastAt = $1
      if (v["hwm_kB"] + 0 > hwm) hwm = v["hwm_kB"] + 0
      if (!tmin || v["threads"] + 0 < tmin) tmin = v["threads"] + 0
      if (v["threads"] + 0 > tmax) tmax = v["threads"] + 0
      if (!fmin || v["fds"] + 0 < fmin) fmin = v["fds"] + 0
      if (v["fds"] + 0 > fmax) fmax = v["fds"] + 0
      if (v["pid"] != pid && pid != "") restarts++
      pid = v["pid"] }
    END { printf "samples %d (failed %d), %s to %s\n", n, failed, firstAt, lastAt
          printf "resident memory %d kB first, %d kB last, %d kB peak\n", first, last, hwm
          printf "threads %d to %d, open files %d to %d, restarts %d\n", tmin, tmax, fmin, fmax, restarts }' "$1"
}

if [ "${1:-}" = "--summary" ]; then
  [ -f "${2:-}" ] || { echo "usage: $0 --summary FILE" >&2; exit 2; }
  summary "$2"
  exit 0
fi

TV=""; HOURS=24; INTERVAL=60; OUT=""
while [ $# -gt 0 ]; do
  case "$1" in
    --hours) HOURS="$2"; shift ;;
    --interval) INTERVAL="$2"; shift ;;
    --out) OUT="$2"; shift ;;
    -*) echo "unknown option: $1" >&2; exit 2 ;;
    *) TV="$1" ;;
  esac
  shift
done
[ -n "$TV" ] || { echo "usage: $0 <tv-ip> [--hours 24] [--interval 60] [--out FILE]" >&2; exit 2; }
case "$HOURS$INTERVAL" in ''|*[!0-9]*) echo "--hours and --interval take whole numbers" >&2; exit 2 ;; esac
[ -n "$OUT" ] || OUT="tv-memory-soak-$(date +%Y%m%d-%H%M%S).log"

SSH_OPTS=(-o ConnectTimeout=6 -o BatchMode=yes)
[ -n "${NATIVETV_SSH_KEY:-}" ] && SSH_OPTS+=(-i "$NATIVETV_SSH_KEY" -o IdentitiesOnly=yes)
[ -n "${NATIVETV_KNOWN_HOSTS:-}" ] && SSH_OPTS+=(-o "UserKnownHostsFile=$NATIVETV_KNOWN_HOSTS")

# Runs on the TV: busybox ash.
sample() {
  ssh "${SSH_OPTS[@]}" "root@$TV" 'sh -s' <<'EOF' 2>/dev/null
pid=$(cat /var/run/tvweb.pid 2>/dev/null); [ -d /proc/$pid ] || pid=$(ps -eo pid,args | grep 'nativetv-daemon --config' | grep -v grep | awk '{print $1; exit}')
[ -d /proc/$pid ] || { echo "no server"; exit 0; }
kids=0; for st in /proc/[0-9]*/stat; do read -r c _ _ pp _ < $st 2>/dev/null || continue; [ "$pp" = "$pid" ] && kids=$((kids+1)); done
echo "pid=$pid rss_kB=$(awk '/^VmRSS/{print $2}' /proc/$pid/status) hwm_kB=$(awk '/^VmHWM/{print $2}' /proc/$pid/status) threads=$(ls /proc/$pid/task | wc -l) fds=$(ls /proc/$pid/fd | wc -l) children=$kids load=$(cut -d' ' -f1 /proc/loadavg)"
EOF
}

echo "# tv-memory-soak: $TV, every ${INTERVAL}s for ${HOURS}h, started $(date -u +%Y-%m-%dT%H:%M:%SZ)" > "$OUT"
end=$(( $(date +%s) + HOURS * 3600 ))
while [ "$(date +%s)" -lt "$end" ]; do
  echo "$(date -u +%Y-%m-%dT%H:%M:%SZ) $(sample)" >> "$OUT"
  sleep "$INTERVAL"
done
echo "# finished $(date -u +%Y-%m-%dT%H:%M:%SZ)" >> "$OUT"
summary "$OUT"
