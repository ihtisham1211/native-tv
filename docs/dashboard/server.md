# Server

The **Server and backup** tab of the **Settings** section, `/?tab=server`, holds Native Tv's server status and settings, and an Advanced section for developers and home-lab setups. The theme is the button at the top right of every screen.

## Native Tv

**Server health** shows the background process uptime and memory footprint on the TV.

**Watch time** counts how long the screen is on, per app and input, for [the Metrics tab](metrics.md#watch-time) and Home Assistant. It is off by default, as it records what the household watches. **Clear history** deletes what has been kept, whether counting is on or off.

**Settings backup** saves LG's sound, device, HDMI and ad settings, and Native Tv's own, to a file, and restores one, on the same TV or another. A restore sets only the settings the TV has and offers, skips those LG greys out at the moment, such as an HDMI input's settings while another input is on screen, and says how many it changed and skipped. Native Tv restarts if its own settings changed. A backup also lists the apps installed from the Homebrew Channel or by hand. A restore installs none of them itself: those the TV lacks and the Homebrew Channel catalog has are offered on the [Apps tab](apps.md) with **Install all**, each through its usual preview, which stops for a choice such as root, and those not in the catalog are named to install by hand. A backup leaves out what decides who can reach the dashboard (port, host, token, controls), MQTT, whose topic prefix and device id must differ between TVs, the experimental switches, and LG's game settings, which LG keeps per input and genre.

**Daily backup** saves the same backup on the TV once a day, in `/var/lib/tvweb/backups`, and keeps the last 7. It is off by default. Switched on, it saves that day's at once, then one each day the TV is on, as the server checks hourly. Each one kept is listed by its date with **Download** and **Restore**. Switching it off keeps those saved.

**Updates** asks GitHub for the newest published release and says whether it is newer than the one on the TV. **Check for updates** asks once. When there is a newer one, the card shows its release notes and `.ipk` with the package's SHA-256, and **Open Homebrew Channel** opens that app on the TV, where Native Tv is updated; see [Updating](../getting-started/managing.md#updating). Native Tv does not install its own releases. A server built from a git checkout is told to pull and deploy again. The check fetches the release's manifest over HTTPS, so GitHub sees the TV's address and the version in use, and nothing else is sent.

**Check daily** is off by default, so the TV contacts GitHub only when asked. Switched on, it checks five minutes after the server starts and again once a day has passed, retrying after an hour when GitHub could not be reached. It is `"update": {"check": true}` in `config.json`, which [Settings backup](#native-tv) carries.

**App on the TV's home screen** adds or removes [the dashboard's own app](../apps/tv-app.md), which opens it on the TV and is driven by the remote.

## Advanced

Folded away, since nothing in it needs changing for everyday use. Once opened, it stays open in that browser.

* **Admin PIN** shows whether the four-digit [admin PIN](../administration/security.md) is set, and sets, changes or removes it. A PIN is set in the TV's first-run setup, where 0000 is offered as the default, and this card says while that default is still in use. It is asked before the risky actions in this dashboard and in the TV app: installing and removing apps, SSH keys, restoring a backup, these settings, and reboot and power off. Changing or removing it asks for the current one first. A browser the TV does not know the token of shows an **Enter passcode** screen in place of an error; the PIN, once it is not the default, signs it in.
* **Dashboard port** is the port the dashboard answers on, 8080 unless changed. Change it when another app on the TV needs 8080 (Stremio's background service binds it, and fails to start while the dashboard holds it). Saving checks the port is free, writes `port` to `config.json` and restarts the server; the page then opens on the new address. The TV apps that open the dashboard, Native Tv Home included, follow the new port at the server's next start.
* **Set the clock from a time server** switches the TV over to [a time server on the local network](../administration/ntp.md), for networks that run one. Switched on, it asks for the server's address. Switched off, the address is kept for next time.
* **Prometheus metrics** turns on the [Prometheus endpoint](../integrations/prometheus.md).
* **Web app debugger** opens or closes Developer Mode's debugger to the network. It is closed unless it is needed, since webOS leaves it open with no password. On a TV whose kernel has no firewall there is no switch, since the port cannot be closed, and the row says the debugger stays open to the network.
* **SSH keys** lists the public keys that can log in as root over SSH, in `/home/root/.ssh/authorized_keys`, by comment, type and SHA256 fingerprint, as `ssh-keygen -l` prints it. A public key pasted in is added, and **Remove** takes one out. While the file is there, the Homebrew Channel leaves root without a password, so only those keys get in. **Delete the key file** stops every key at once, and from the next reboot the Homebrew Channel gives root the password `alpine`, which everyone knows; until that reboot nothing can log in over SSH. The row says which applies now and after the next reboot, and offers **Reboot now** when they differ. None of it applies while SSH is off in the Homebrew Channel. Changing the keys needs a [token](../administration/security.md); without one the row only lists them, by type and fingerprint.
* **Experimental features**: hiding built-in apps from the Home screen, custom screen savers on webOS 12 and later, and the LG launcher layout on webOS 10. See [Experimental features](../administration/experimental.md) for why they are experimental.
* **Block TV software updates**, in the System updates card of the [Advanced tab](advanced.md), stops LG from installing firmware updates that may remove root access. This is separate from deploying Native Tv releases; see [Updating](../getting-started/managing.md#updating).
