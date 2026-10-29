#!/bin/sh
# Run by the bus when the key filter calls com.webos.nativetv.boot. The bus is
# not ready for the call at the filter's first config read (about 17.5 s into a
# cold start; the first call that got through, at about 21.6 s, was the third),
# so the filter calls every second until about 33 s and this runs once per boot:
# the first run takes the lock, the rest find it. Takes no input from the call.
# Starts the services hook, and the boot hook while nativeTv Home is on
# (early-home), which startup.sh would only run at 43-48 s; each hook runs once
# per boot whoever starts it first. Detached: the bus gives this script no
# reply to wait for.
mkdir /tmp/nativetv-early-lock 2>/dev/null || exit 0
# Set here and taken away once the hooks are under way (the boot hook removes
# it once it has started the server). A boot that began here and never got that
# far leaves it, and the next boot leaves the start to startup.sh, once, so a
# start that breaks boot cannot repeat.
PENDING=/var/lib/tvweb/early-pending
if [ -e "$PENDING" ]; then
  rm -f "$PENDING"
  echo "$(cut -d' ' -f1 /proc/uptime) skipped: the last early start did not finish" >> /tmp/nativetv-early-ran
  exit 0
fi
touch "$PENDING"
echo "$(cut -d' ' -f1 /proc/uptime) epoch=$(date +%s) uid=$(id -u) pid=$$" >> /tmp/nativetv-early-ran
SERVICES=/var/lib/webosbrew/init.d/20-tvweb-services
HOOK=/var/lib/webosbrew/init.d/50-tvweb
HOME_ON=/var/lib/tvweb/early-home
# The services hook beside the boot hook, as its stops take seconds under boot
# load. Not niced: at nice 10 its stubs took 15 s to bind (CPU ~90 % busy), and
# lg.thinqai.adapter, launched at about 32 s, came first.
setsid sh -c "[ -x $SERVICES ] && sh $SERVICES; [ -e $HOME_ON ] || rm -f $PENDING" >/dev/null 2>&1 </dev/null &
[ -e "$HOME_ON" ] && [ -x "$HOOK" ] && setsid sh "$HOOK" >/dev/null 2>&1 </dev/null &
exit 0
