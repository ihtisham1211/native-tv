---
description: "Replace LG’s screen saver on a rooted webOS TV with a clock, starfield, fireworks or the TV’s own readings."
---

# Screen savers

The **Screen savers** tab of the **Display** section, `/?tab=screensaver`, provides six alternatives to LG's default:

* Clock
* Black screen, nothing on screen (on an OLED the pixels are off)
* Starfield
* Fireworks
* Bokeh, soft circles of light drifting in and out
* Harmonograph, pendulum ribbons of light weaving and fading on black
* Panel vitals, showing the TV's own panel hours and refresher countdown

Each mode but the black screen offers dim and bright variants, and visual elements continuously drift across the screen to prevent OLED burn-in or image retention.

**Start after** sets how long the TV sits idle before its screen saver starts: 3, 10, 20 or 30 minutes, the four times LG's own menu offers (any other value stops the screen saver coming at all, so no other is offered). It is LG's `option.screenSaverTimer` setting, shown only on a TV that reports it, and the same row is on the TV app's Screen Saver Options page.

A firmware update restores the LG default.

On webOS 12 and later they are experimental: the tab shows a switch above the list that turns them on, after a warning, and until then only LG's default can be chosen. See [Experimental features](../administration/experimental.md) for why.

**Start screen saver** asks the TV for its screen saver. Some TVs accept the request and then refuse it; a G3 on webOS 9.2.2 refused every one, its own idle screen saver included. There the dashboard opens the screen saver itself, unless something is playing full screen or playback cannot be checked. Any key on the remote, including Home and the coloured buttons, or a second press of the button closes it. A failed close leaves it tracked so another press can retry. The TV does not know about a screen saver opened this way: its power state stays On, in the dashboards and in Home Assistant, and watch time counts the app beneath it. The Screen Saver sensor does show it.

