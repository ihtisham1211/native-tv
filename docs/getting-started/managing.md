---
description: "Update, back up and uninstall Native Tv on a rooted LG webOS TV."
---

# Updating and uninstalling

```bash
ssh root@<tv-ip> /var/lib/tvweb/tvwebctl status    # start | stop | restart | status
```

## Updating

Native Tv no longer checks for or installs its own server releases. Update a
TV by deploying the server or installing the Homebrew Channel package.

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

**Installed from the Homebrew Channel or as an `.ipk`.** Update the package
through the Homebrew Channel once the package is listed there. Until then,
install the newer `.ipk` from [GitHub releases](https://github.com/ihtisham1211/native-tv/releases)
over the existing one with webOS Dev Manager, Homebrew Channel sideload, or
`ares-install`. The server follows the updated app on launch or reboot,
preserving settings. Installing an earlier `.ipk` returns to that release.

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
grep -q /var/lib/tvweb/adblock_hosts /proc/mounts && umount /etc/hosts
rm -rf /var/lib/tvweb /media/developer/temp/nativetv-install
rm -f /var/lib/webosbrew/init.d/50-tvweb* /var/lib/webosbrew/init.d/20-tvweb-services /var/lib/webosbrew/init.d/20-services.sh
rm -f /var/lib/webosbrew/tvweb-boot.log*
```

`restore-home` puts LG's Home screen layout back where [Home screen layout](../dashboard/apps.md#home-screen-layout) changed it, `restore-keys` the remote's keys where [Remote keys](../dashboard/apps.md#remote-keys) did, and `restore-nativetv-home` LG's Home where [Native Tv Home](../dashboard/home.md) replaced it. `stop` also gives LG's Home its place back while the server is stopped. The three `sh` lines remove the boot service the remote's key filter calls, the TV dashboard app and the startup splash. The ad-block and update-block table stays mounted over `/etc/hosts` after `stop`; deleted while mounted, it keeps blocking until a reboot, and a reinstall before then takes it for its own. Uninstalling Native Tv through the Homebrew Channel restores LG Home automatically. Nothing on the TV's read-only rootfs is ever modified. [What the dashboard changes on the TV](../administration/tv-changes.md) lists everything else, including the settings that uninstalling leaves as they are.
