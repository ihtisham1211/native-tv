#!/bin/bash
# Takes a screenshot of the TV app on a real TV: runs the installed client once for a bounded
# time with the TV's own server, catalogue and feed, optionally playing a key script, and brings
# its last frame back as a PNG (or a JPEG, by the extension of the file name). Screenshots in
# docs/ and README.md are made this way, not with the host's renderer (tests/golden holds that
# renderer's images, which are test fixtures, never documentation).
#
# A running Home client (Home takeover) has to be stopped first: stop it once, take the
# screenshot straight away, and leave 62 s before stopping it again (three stops within 120 s
# trip the Home crash breaker, and LG's Home comes back until the server restarts).
#
#   NATIVETV_SSH_KEY=~/.ssh/lg_tv_root NATIVETV_KNOWN_HOSTS=~/.ssh/lg_tv_known_hosts \
#     ./scripts/tv-screenshot.sh <tv-ip> <out-file> [--screen home] [--ms 9000] [--script keys.json]
#
# --screen is a launch screen of the client (home, live, dashboard, dashboard:privacy, setup:welcome,
# setup:summary). --script is a JSON list of {"at": ms, "key": "up"} steps, as the E2E cases use
# (tests/e2e/scripts/); the first screen is drawn from live data about 6 s in, so keys start there.
# The converter is macOS's sips; elsewhere ImageMagick's convert.

set -u

TV=""; OUT=""; MS=9000; SCREEN=home; SCRIPT=""
while [ $# -gt 0 ]; do
  case "$1" in
    --ms) MS="$2"; shift ;;
    --screen) SCREEN="$2"; shift ;;
    --script) SCRIPT="$2"; shift ;;
    -*) echo "unknown option: $1" >&2; exit 2 ;;
    *) if [ -z "$TV" ]; then TV="$1"; else OUT="$1"; fi ;;
  esac
  shift
done
[ -n "$TV" ] && [ -n "$OUT" ] || { echo "usage: $0 <tv-ip> <out.png|out.jpg> [--screen S] [--ms N] [--script keys.json]" >&2; exit 2; }
case "$MS" in ''|*[!0-9]*) echo "--ms takes a whole number" >&2; exit 2 ;; esac
[ -z "$SCRIPT" ] || [ -f "$SCRIPT" ] || { echo "no such script: $SCRIPT" >&2; exit 2; }

SSH_OPTS=(-q -o ConnectTimeout=6 -o BatchMode=yes)
[ -n "${NATIVETV_SSH_KEY:-}" ] && SSH_OPTS+=(-i "$NATIVETV_SSH_KEY" -o IdentitiesOnly=yes)
[ -n "${NATIVETV_KNOWN_HOSTS:-}" ] && SSH_OPTS+=(-o "UserKnownHostsFile=$NATIVETV_KNOWN_HOSTS")
tv() { ssh "${SSH_OPTS[@]}" "root@$TV" "$@"; }

[ -z "$SCRIPT" ] || tv 'cat > /tmp/nr-script.json' < "$SCRIPT" || exit 1
tv "SCREEN=$SCREEN MS=$MS HAVE_SCRIPT=$([ -n "$SCRIPT" ] && echo 1 || echo 0) sh -s" <<'REMOTE'
A=/media/developer/apps/usr/palm/applications/com.tvweb.dashboard
if ps | grep -q "[n]ativetv-tv"; then echo "a client is running: stop it first" >&2; exit 3; fi
cd $A
S=""; [ "$HAVE_SCRIPT" = 1 ] && S="--script /tmp/nr-script.json"
env XDG_RUNTIME_DIR=/tmp/xdg timeout 60 /usr/bin/jailer -t native_devmode -i com.tvweb.dashboard -p $A \
  $A/nativetv-tv '{"@system_native_app":true,"nid":"com.tvweb.dashboard"}' \
  --server http://127.0.0.1:8080 --screen "$SCREEN" --duration-ms "$MS" $S --capture /tmp/nr-cap.ppm > /tmp/nr-out 2>&1
echo "client exit $?" >&2
REMOTE
code=$?
[ $code -eq 0 ] || exit $code
PPM="$(mktemp -t tvshot).ppm"
tv 'cat /tmp/nr-cap.ppm; rm -f /tmp/nr-cap.ppm /tmp/nr-script.json /tmp/nr-out' > "$PPM" || exit 1
[ -s "$PPM" ] || { echo "no frame came back" >&2; rm -f "$PPM"; exit 1; }
case "$OUT" in
  *.jpg|*.jpeg) FORMAT=jpeg ;;
  *) FORMAT=png ;;
esac
if command -v sips >/dev/null 2>&1; then
  if [ "$FORMAT" = jpeg ]; then sips -s format jpeg -s formatOptions 86 "$PPM" --out "$OUT" >/dev/null
  else sips -s format png "$PPM" --out "$OUT" >/dev/null; fi
else
  convert "$PPM" "$OUT"
fi
rc=$?
rm -f "$PPM"
[ $rc -eq 0 ] && echo "$OUT"
exit $rc
