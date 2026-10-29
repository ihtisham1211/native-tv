# The TV app

The dashboard can also run directly on the TV's home screen, driven by the remote, for when there is no phone or laptop to hand.

It is drawn by the nativeTv client in the style of Google TV: a dark page with one accent colour, cards that lift and glow when they are focused, and a top bar with the profile, the tabs and the clock. The pages below are its own screens, not web pages.

![The nativeTv TV app showing live hardware metrics, temperatures, and storage health on true black](../screenshots/tv-app.png)

*The TV app's Metrics view, showing SoC temperature, CPU load, memory, swap, eMMC storage wear, and TV model.*

### Live

The **Live** tab is the TV's own readings as cards: the processor's load with a line of its recent readings, memory and swap, the SoC temperature, the Wi-Fi link and the rate in and out, how long the TV has been up, the panel's hours and when its pixel refresher is due, and the input in front with its resolution, refresh rate, HDR and VRR. It reads the server every four seconds while it is open: numbers count to their new value and the bars grow, instead of jumping. OK reads at once, and Back leaves for Home. A card that has no reading on a TV, such as the thermal sensor on one that has none, shows a dash.

### Settings

**Settings** lists the dashboard's ten sections: Metrics, Apps, OLED Care, Game, Screen Saver, Home, Privacy, Service Menu, System and Settings. A section that this TV does not have, such as OLED Care on an LCD, is left out. OK opens a section, where each row has a panel beside the list that says what the row is and what OK does to it. Settings are switches, which slide when they change, values that OK steps to the next of, actions, and links that open another page.

* **Up / Down**, or the Magic Remote's wheel: move through the rows, skipping section headings.
* **Channel Up / Channel Down** (or **Page Up / Page Down**): jump 6 rows.
* **Left / Right**: open the previous or next section.
* **OK**: change the row. A change that is hard to undo, such as switching an OLED protection off, uninstalling an app, powering off or resetting a layout, asks for a second OK within five seconds, and Back cancels it.
* **Back**: close a list inside a section (Privacy's consents, Home's channels), then the section, then Settings.
* **Magic Remote pointer**: clicking a row selects it and clicking it again acts on it.

Every change is sent to the same server routes as the browser dashboard, and the section is read again afterwards. Metrics and System refresh every ten seconds while open.

### First-run setup wizard

Opening the TV app on a fresh install starts an on-screen setup wizard to configure the TV in a few quick steps with the remote:

1. **Web dashboard access**: Choose between opening the web dashboard to devices across your home network or keeping it private to this TV only (`127.0.0.1`).
2. **Home Assistant integration**: Connect your TV to Home Assistant via MQTT. The TV displays a QR code to scan with your phone, allowing you to enter MQTT broker details easily without typing credentials on the remote. Beside the code is an on-screen keyboard for typing the broker's address, port, user and password with the remote instead, one field after another; Back returns to the previous field, and the TV shows when the broker has connected.
3. **Active Standby (Always-on)**: On TVs with Always Ready support (such as LG C2 or G2), choose whether nativeTv and Home Assistant remain connected with the screen off so the TV can be controlled and turned back on over the network.
4. **Summary**: Review your setup and open the dashboard with a single click.

A row of dots at the bottom of each page shows how far along the wizard is, and a line of key hints sits under it. Left or Back returns to the previous step, as it always did; Back on the welcome page leaves setup, which then starts again the next time the app opens. When the app is opened from **Settings** to connect Home Assistant, only the phone link and the keyboard appear and Back returns to Settings.

All choices made during setup can be changed at any time from the TV app's **Settings** or the web dashboard.

![Welcome screen of the nativeTv TV app setup wizard](../screenshots/tv-app-setup-welcome.png)

*The setup wizard welcome screen.*

![Network access choice in the TV app setup wizard](../screenshots/tv-app-setup-network.png)

*Choosing whether the web dashboard is open to devices on the home network or TV-local only.*

![Home Assistant phone pairing QR code in the TV app setup wizard](../screenshots/tv-app-setup-handoff.png)

*Scanning the phone handoff code to configure Home Assistant without typing on the remote.*

![Setup wizard summary screen ready to open the dashboard](../screenshots/tv-app-setup-summary.png)

*The final summary screen confirming your settings before opening the dashboard.*

> [!TIP]
> **Testing the setup wizard**: To test or verify the setup flow without modifying TV settings, start the server with the `--emulate-setup` flag, or open the web dashboard in a browser with `?setup=1`.

### Pairing and settings

In Settings, the **Settings** section gives the web dashboard's address, with a QR code to scan on a phone (OK on the address), and the TV's DNS name under it where the home network's DNS has one. Opening the web dashboard to the network or closing it restarts the server, and the TV app waits until it answers again.

Scanning the code automatically pairs the phone with the TV, passing the access token if one is configured, allowing the dashboard to be opened without typing an IP address with the remote.

![Settings tab on the TV app, showing web dashboard address and pairing QR code](../screenshots/tv-app-settings.png)

*The Settings view, presenting the web dashboard's LAN address and phone pairing QR code.*

### Installing apps and system app hiding

The **Apps** section lists the apps installed from outside LG's store, each with an uninstall that takes a second OK, the parts of LG's Home screen and the inputs shown on it, the keys of Quick Access, the system apps that can be hidden, the apps LG keeps ready in the background, and the background services that can be turned off.

**Install homebrew apps** opens the catalog: the Homebrew Channel's apps as a grid of tiles, updates first and then by title, each marked Update, Installed or Homebrew Channel, with the focused app's version, size and description under the grid. OK on a new app or an update has the server fetch and check it, and then shows what it would change: the version, the background services, free space, whether the app asks to run its services as root (a switch, offered only for packages the Homebrew Channel has vetted), and whether it replaces a version from LG's store. Nothing is installed until OK on **Install**; **Cancel**, or Back, throws the download away. While it runs, a progress card replaces the page and Back waits for it to finish; the result then says what was installed or why it failed. OK on an installed app uninstalls it after a second OK.

![Apps tab on the TV app, showing Homebrew catalog installation and home screen app hiding](../screenshots/tv-app-apps.png)

*The Apps view, showing Homebrew Channel catalog browsing and system app hiding.*

### nativeTv Home

The **Home** section sets up [nativeTv Home](../dashboard/home.md) from the TV, as the browser dashboard's Home tab does, and is left out on a TV nativeTv Home is not offered on. It has the on/off switch (a second press confirms), a way to open nativeTv Home while it is on, the order of its rows, then a section for each row in that order: each row's Order (Your order or Last opened), apps and inputs each shown or hidden, and the channels added, each removed with OK, with **Add channels** listing the TV's channels not yet on Home. Then whether wallpapers are uploaded (they are uploaded from the browser dashboard), and **Reset the layout**. Moving tiles within a row is done on nativeTv Home itself, by holding OK.

### Sleep timer and power

The **System** section has the sleep timer under **Timers**: each press of OK moves it to the next of 10, 30, 60, 90 and 120 minutes, then off. Its **Power** section, last in the tab, has **Power off** and **Reboot**, the full restart the browser dashboard's Control tab offers. Each takes a second press of OK within five seconds, and warns first while an app is installing. Both are left out when `allowPower` is false in `config.json`, as on the browser dashboard.

### Adding and removing the app

A first install of nativeTv adds the app to the TV's home screen; updating an existing install leaves the home screen alone. The app is the nativeTv client itself, opened on the dashboard screen, and it is brought up to date with the server at each start of the server.

The app can be added or removed at any time:

* **From the web dashboard**: Go to the **Server** tab and toggle the TV app.
* **From deploy.sh**: Pass `--app` to add it or `--no-app` to omit it during installation.

Removing the app changes nothing else, since the dashboard server continues to run in the background and reaches any browser on the network regardless. Where a TV model will not take the app, the control is hidden and everything else works as before.
