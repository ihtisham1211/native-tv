#!/bin/sh
# Declares (install) or removes (remove) the bus service com.webos.glasshouse.boot,
# which the compositor's key filter calls. The compositor's role reaches
# com.webos.* only, so the Homebrew Channel's service is out of its reach and a
# service of ours under that prefix is not. Declared the way the Homebrew
# Channel declares its own, under /var/luna-service2-dev, then read with
# ls-control. Only the compositor may call it: the role's inbound list names
# com.webos.surfacemanager* and the key filters' com.webos.starfish.keyfilters*. The API is in the public group because a
# developer package's client-permissions grant a group to its own clients only
# (the compositor, given a group of its own here, got no reply for 80 s), so
# the role is the restriction, and a call from anything else is to be refused
# with "Not permitted to send". The script it runs is a fixed file and reads
# no input.
ACTION=${1:-install}
NAME=com.webos.glasshouse.boot
SRC=${2:-/var/lib/tvweb/assets/early-service}
DEV=/var/luna-service2-dev
SERVICE=$DEV/services.d/$NAME.service
ROLE=$DEV/roles.d/$NAME.service.json
API=$DEV/api-permissions.d/$NAME.api.public.json
# What the first version of this script wrote, taken away on install and remove.
OLD_API=$DEV/api-permissions.d/$NAME.api.json
OLD_CLIENT=$DEV/client-permissions.d/$NAME.compositor.json
MANIFEST=$DEV/manifests.d/$NAME.json

if [ "$ACTION" = remove ]; then
  rm -f "$SERVICE" "$ROLE" "$API" "$OLD_API" "$OLD_CLIENT" "$MANIFEST"
  /usr/sbin/ls-control scan-volatile-dirs >/dev/null 2>&1
  echo "removed"
  exit 0
fi

[ -d "$DEV" ] || { echo "no $DEV: developer mode is off"; exit 1; }
mkdir -p "$DEV/services.d" "$DEV/roles.d" "$DEV/api-permissions.d" "$DEV/manifests.d"
rm -f "$OLD_API" "$OLD_CLIENT"
cat > "$SERVICE" <<SVC
[D-BUS Service]
Name=$NAME
Exec=/bin/sh $SRC/early-start.sh
Type=dynamic
SVC
cat > "$ROLE" <<ROLE
{"appId":"$NAME","type":"regular","allowedNames":["$NAME"],"trustLevel":"","permissions":[{"service":"$NAME","inbound":["com.webos.surfacemanager*","com.webos.starfish.keyfilters*"],"outbound":[]}]}
ROLE
echo "{\"public\":[\"$NAME/*\"]}" > "$API"
cat > "$MANIFEST" <<MAN
{"version":"1.0.0","serviceFiles":["$SERVICE"],"apiPermissionFiles":["$API"],"id":"$NAME","roleFiles":["$ROLE"],"clientPermissionFiles":[]}
MAN
/usr/sbin/ls-control scan-volatile-dirs >/dev/null 2>&1
echo "installed"
