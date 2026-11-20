---
description: "Update, back up and uninstall Native Tv on a rooted LG webOS TV."
---

# Updating and uninstalling

```bash
ssh root@<tv-ip> /var/lib/tvweb/tvwebctl status    # start | stop | restart | status
```

## Updating

The **Updates** card, on the dashboard's [Server tab](../dashboard/server.md),
tells when a newer release is out, when asked or once a day if switched on. A
server that came from the app package installs it from the card; one deployed
with `deploy.sh` is updated by deploying it again.

**Deployed from a git checkout.** Pull the desired release or commit, rebuild,
then run the deploy script again. The deploy script packages whatever is in the
build folder and does not rebuild it, so skipping the build installs the old
binaries beside the new files:

```bash
cd native-tv
git pull
python3 native-tv/build.py --sdk /path/to/arm-webos-linux-gnueabi_sdk-buildroot
native-tv/lifecycle/deploy.sh <tv-ip>
```

The deploy replaces server files and keeps the TV's configuration and data.
To return to an earlier release, check it out, rebuild and deploy it again.

**Installed from the Homebrew Channel or as an `.ipk`.** On the Server tab,
**Check for updates**, then **Install version x.y.z** and confirm (the
[admin PIN](../administration/security.md) is asked for when one is set). The
server then:

1. reads the release's own manifest from GitHub over HTTPS and downloads the
   `.ipk` it names to `/media/developer/temp/nativetv-update`;
2. checks the file's SHA-256 against the manifest's, and stops here if they
   differ;
3. has the TV's app installer replace the Native Tv app with it. The Native Tv
   app on the TV closes while its package is replaced;
4. runs the new app's `payload/install.sh`, the script the app's launch page runs
   too. Only one run works at a time: a second, such as the launch page's while
   an update is under way, is answered "an install is already running". It
   keeps the files it is about to replace in `/var/lib/tvweb/.previous` (as hard
   links, so they cost no room; copies where a link cannot be made), moves the
   new ones in by rename and restarts the server through `tvwebctl`. The server
   has up to a minute to write its first heartbeat, and then 12 seconds to still
   be the same process with its heartbeat still there. If it is not, the script
   puts the kept files back and starts the old server again, and the card says
   the update did not hold. Settings and data are not touched either way.

An update that did not hold is not tried again by opening the app, which would
cost that time on every opening; **Install** on the Server tab tries again, and a
newer package is installed on opening the app as before.

A failure before step 3 leaves the TV as it was. From step 3 the app is newer
than the server; a run of `install.sh` that works settles that, and one that
put the old server back leaves it so until **Install** is used again. The package is checked against a checksum from the same
release, so it detects a damaged download, not a release published by someone
else; see [Security](../administration/security.md#the-release-check).

The newer `.ipk` from [GitHub releases](https://github.com/ihtisham1211/native-tv/releases)
can still be installed by hand over the existing one with webOS Dev Manager,
Homebrew Channel sideload, or `ares-install`; the server follows the updated
app the next time it is opened, preserving settings. The package registers the
native TV app if needed and opens it from the Homebrew tile. Installing an earlier
`.ipk` returns to that release. A release from before the card could install
only reports; a TV on one of those takes the newer `.ipk` by hand once.

A server deployed from a git clone shows the commit it was built from after
its version, as `0.80.1+55e51e5`, with `.dirty` added when files under
the daemon's sources, assets or lifecycle scripts had uncommitted changes. A release installed later shows its plain
version.

The Homebrew Channel app catalog in the Apps tab still installs and updates
third-party apps. The **Block TV software updates** switch controls LG firmware
updates; it does not control Native Tv release deployment.

## Uninstalling

```bash
ssh root@<tv-ip>
/var/lib/tvweb/tvwebctl restore-home
/var/lib/tvweb/tvwebctl restore-keys
/var/lib/tvweb/tvwebctl restore-nativetv-home
/var/lib/tvweb/tvwebctl stop
sh /var/lib/tvweb/assets/early-service/install.sh remove
sh /var/lib/tvweb/assets/dashboard-app/install-app.sh remove
sh /var/lib/tvweb/assets/dashboard-app/install-app.sh remove /var/lib/tvweb/assets/splash-app
[ /usr/palm/applications/com.webos.app.home -ef /var/lib/tvweb/home-takeover ] && umount -l /usr/palm/applications/com.webos.app.home
grep -q 'LG Ad & Telemetry Blackhole (lg-webos-' /etc/hosts && umount /etc/hosts
rm -rf /var/lib/tvweb /media/developer/temp/nativetv-install
rm -f /var/lib/webosbrew/init.d/50-tvweb* /var/lib/webosbrew/init.d/20-tvweb-services /var/lib/webosbrew/init.d/20-services.sh
rm -f /var/lib/webosbrew/tvweb-boot.log*
rm -f /var/luna-service2-dev/manifests.d/io.github.ihtisham1211.nativetv.hbchannel.json /var/luna-service2-dev/client-permissions.d/io.github.ihtisham1211.nativetv.hbchannel.json
```

`restore-home` puts LG's Home screen layout back where [LG launcher layout](../dashboard/apps.md#lg-launcher-layout) changed it, `restore-keys` the remote's keys where [Remote keys](../dashboard/apps.md#remote-keys) did, and `restore-nativetv-home` LG's Home where [Native Tv Home](../dashboard/home.md) replaced it. `stop` also gives LG's Home its place back while the server is stopped. The three `sh` lines remove the boot service the remote's key filter calls, the TV dashboard app and the startup splash, and the last line the app's grant to Homebrew Channel's service. The first `umount` line takes Native Tv Home's files off LG's Home only where they are still there because `restore-nativetv-home` and `stop` both failed; the TV then needs a restart before the Home key opens LG's Home again. The ad-block and update-block table stays mounted over `/etc/hosts` after `stop`, and the second `umount` line takes it off; it recognises the table by its header, so another tool's mount over `/etc/hosts` is left in place, as is the table when that mount lies over it. A table deleted while mounted keeps blocking until a reboot, and a reinstall before then takes it for its own. Uninstalling Native Tv through the Homebrew Channel restores LG Home automatically. Nothing on the TV's read-only rootfs is ever modified. [What the dashboard changes on the TV](../administration/tv-changes.md) lists everything else, including the settings that uninstalling leaves as they are.
