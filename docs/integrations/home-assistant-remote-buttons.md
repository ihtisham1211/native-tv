# Remote Button Events

Capture presses of the four colored buttons (**Red**, **Green**, **Yellow**, and **Blue**), the streaming buttons (**Netflix**, **Prime Video**, **Disney+**, **LG Channels**, **Rakuten TV**, **Movies** and **Alexa**), the **Google Assistant** button and the **microphone** on your LG Magic Remote or IR remote to trigger automations in Home Assistant.

---

## Overview

Most LG remotes include dedicated colored buttons (traditionally used for teletext or HbbTV interactive services). This feature listens directly to input events from the remote control on webOS and publishes each press to Home Assistant as an [Event entity](https://www.home-assistant.io/integrations/event/).

Because it is exposed as an `event` entity rather than a `sensor`, every press fires an event in Home Assistant—even if you press the same button multiple times in succession.

Presses reach Home Assistant whatever the button is set to under [Remote keys](../dashboard/apps.md#remote-keys) on the Apps tab, and the button still does what it is set to there: a streaming button opens its app, and the microphone starts voice control. To stop the TV acting on a button as well, set it to **Home Assistant** (or **Nothing**, which does the same): the press then does nothing on the TV and still reaches Home Assistant. The choice is offered for the buttons listed here, while MQTT is on, on TVs whose key handling is the JavaScript filter; webOS 11's own key handling cannot do it, so there a streaming button is set to **Nothing** instead. Which buttons a remote has depends on the model and region.

> [!NOTE]
> The button codes and the finding that a press still arrives on the remote's own input device after the TV's key filter has taken it are [Glasshouse](https://github.com/rorygallagher2024/glasshouse)'s measurements: the streaming buttons on a C4, the microphone and Google Assistant on a C2 (webOS 9.2.2) and, for the microphone, a C9 (webOS 4.10.2). They have not yet been tried on a TV with Native Tv, so which buttons a given remote reports is for a TV to confirm. Details are in the [TV research notes](../development/tv-research.md#remote-button-codes-and-the-input-device-c2-webos-922).

<div style="text-align: center; margin: 1.5rem 0;">
  <iframe width="315" height="560" src="https://www.youtube.com/embed/G1o166aweb0" title="Remote button events demonstration" frameborder="0" allow="accelerometer; autoplay; clipboard-write; encrypted-media; gyroscope; picture-in-picture; web-share" allowfullscreen style="max-width: 100%; border-radius: 8px;"></iframe>
</div>

> [!NOTE]
> - **Server Must Be Running**: Remote button event detection requires the Native Tv background server to be running on the TV. It does not run while the TV is powered off or in deep standby.
> - **Cold-Boot / Start-up Delay**: Following a cold-boot or power-on, the server starts up via the webosbrew `init.d` script (`50-tvweb`). Remote button presses sent before the server completes its start-up sequence will not be captured.
> - **Standby with Quick Start+**: When Quick Start+ is enabled in LG settings, the TV enters a low-power suspend state rather than a full system shutdown, allowing the background service to be ready immediately when the TV wakes.
> - **Preventing Power-On Ghost Triggers**: When the TV powers off, the event entity transitions to `unavailable`. In Home Assistant automations, always add `not_from: [unknown, unavailable]` and `not_to: [unknown, unavailable]` to state triggers so the transition back to available does not re-fire the last recorded event.

---

## Home Assistant Entity

| Entity | Type | Event Types | Topic |
|---|---|---|---|
| `event.lg_tv_remote_button` | `event` | `red`, `green`, `yellow`, `blue`, `netflix`, `amazon` (Prime Video), `disneyplus`, `lgchannels`, `rakutentv`, `movies`, `alexa`, `googleassistant`, `voice` (the microphone) | `lgtv/events/button` |

When a button is pressed, the server publishes a JSON payload to the event topic:

```json
{
  "event_type": "red",
  "button": "red"
}
```

---

## Setting Up Automations

### Using the Home Assistant Automation UI

1. Go to **Settings** → **Automations & Scenes** → **Create Automation**.
2. Under **When** (Triggers), choose **Entity** → **State**.
3. Select entity **Remote Button** (`event.lg_tv_remote_button`).
4. In the **Event type** field, enter the button: a color (`red`, `green`, `yellow`, or `blue`), a streaming button (`netflix`, `amazon`, `disneyplus`, `lgchannels`, `rakutentv`, `movies` or `alexa`), `googleassistant` or the microphone, `voice`.
5. Under **Then do** (Actions), add any action you want (e.g. toggle lights, run a script, set a scene).

---

### YAML Examples

#### Example 1: Toggle Living Room Lights with the Red Button

```yaml
alias: "TV Remote: Red button toggles living room lights"
description: "Toggle living room lights when the red button is pressed on the TV remote"
trigger:
  - platform: state
    entity_id: event.lg_tv_remote_button
    not_from:
      - unknown
      - unavailable
    not_to:
      - unknown
      - unavailable
    attribute: event_type
    to: "red"
action:
  - action: light.toggle
    target:
      entity_id: light.living_room
mode: restart
```

#### Example 2: Cinema Mode with the Blue Button

```yaml
alias: "TV Remote: Blue button activates cinema mode"
description: "Dim lights and set cinema scene"
trigger:
  - platform: state
    entity_id: event.lg_tv_remote_button
    not_from:
      - unknown
      - unavailable
    not_to:
      - unknown
      - unavailable
    attribute: event_type
    to: "blue"
action:
  - action: scene.turn_on
    target:
      entity_id: scene.movie_night
mode: restart
```

#### Example 3: The Microphone Toggles the Ambient Light

Set the microphone to **Home Assistant** under Remote keys too, so a press no longer starts LG's voice control as well. The button is checked in a condition rather than in the trigger's `attribute` and `to` fields: a trigger on an attribute fires only when it changes, so pressing the same button twice in a row would run the automation once.

```yaml
alias: "TV Remote: microphone toggles the ambient light"
trigger:
  - platform: state
    entity_id: event.lg_tv_remote_button
    not_from:
      - unknown
      - unavailable
    not_to:
      - unknown
      - unavailable
condition:
  - condition: template
    value_template: "{{ trigger.to_state.attributes.event_type == 'voice' }}"
action:
  - action: light.toggle
    target:
      entity_id: light.tv_ambient
mode: restart
```

#### Example 4: Handling All Four Buttons in a Single Automation

You can handle all four colored buttons within a single automation using a `choose` action:

```yaml
alias: "TV Remote: Colored button actions"
description: "Actions for red, green, yellow, and blue remote buttons"
trigger:
  - platform: state
    entity_id: event.lg_tv_remote_button
    not_from:
      - unknown
      - unavailable
    not_to:
      - unknown
      - unavailable
action:
  - choose:
      # Red Button
      - conditions:
          - condition: template
            value_template: "{{ trigger.to_state.attributes.event_type == 'red' }}"
        sequence:
          - action: light.toggle
            target:
              entity_id: light.living_room

      # Green Button
      - conditions:
          - condition: template
            value_template: "{{ trigger.to_state.attributes.event_type == 'green' }}"
        sequence:
          - action: cover.toggle
            target:
              entity_id: cover.living_room_blinds

      # Yellow Button
      - conditions:
          - condition: template
            value_template: "{{ trigger.to_state.attributes.event_type == 'yellow' }}"
        sequence:
          - action: climate.set_preset_mode
            target:
              entity_id: climate.living_room
            data:
              preset_mode: "comfort"

      # Blue Button
      - conditions:
          - condition: template
            value_template: "{{ trigger.to_state.attributes.event_type == 'blue' }}"
        sequence:
          - action: scene.turn_on
            target:
              entity_id: scene.movie_night
mode: restart
```

---

## Automation Tips & Gotchas

### 1. Preventing Power-On Ghost Triggers
When the TV powers off or enters deep standby, the Native Tv background server stops and Home Assistant marks `event.lg_tv_remote_button` as `unavailable`.

When the TV powers back on, Home Assistant transitions the entity from `unavailable` back to its restored state—which still retains the attributes of whichever button was last pressed (even if hours or days ago).

A bare state trigger (`trigger: state` with no filters) treats this recovery as a state change and fires your automation on TV power-on! To prevent ghost triggers, **always filter out `unknown` and `unavailable` states** in your triggers:

```yaml
trigger:
  - platform: state
    entity_id: event.lg_tv_remote_button
    not_from:
      - unknown
      - unavailable
    not_to:
      - unknown
      - unavailable
```

### 2. Improving Responsiveness (`mode: restart`)
Home Assistant automations default to `mode: single`. If you press Red and then quickly press Green while the network call for Red is still in flight (e.g. communicating with smart bulbs over Wi-Fi), Home Assistant silently drops the Green button press.

Always set **`mode: restart`** at the bottom of remote button automations so that subsequent button presses immediately interrupt any in-flight actions and execute the new command:

```yaml
mode: restart
```

For smart lights (such as LIFX or Philips Hue), also specify `transition: 0` in the `light.turn_on` action data so colors and brightness snap instantly rather than slowly fading.

### 3. Controlling from Multiple LG TVs
If you have multiple LG TVs running Native Tv (e.g. an OLED B8 and an OLED C2), you can list all their remote event entities under a single trigger:

```yaml
trigger:
  - platform: state
    entity_id:
      - event.lg_tv_remote_button
      - event.lg_c2_remote_button
    not_from:
      - unknown
      - unavailable
    not_to:
      - unknown
      - unavailable
```

---

## Technical Details

- **Input Devices**: webOS exposes remote input streams via `/dev/input/event*` devices (`LGE RCU` for infrared, `LGE M-RCU` for Bluetooth Magic Remotes).
- **Non-Invasive**: The listener reads the Linux kernel input stream passively without injecting binaries or intercepting system processes. A key set to **Home Assistant** under Remote keys is taken by the TV's key filter, which does nothing for it; the listener still reads the press from the remote's input device.
- **Button Codes**: Each button is the Linux key code the remote's device sends (for example 1037 for Netflix and 428 for the microphone) or, from LG's input log, the Qt key code of the same key. The codes are listed in the [TV research notes](../development/tv-research.md#remote-button-codes-and-the-input-device-c2-webos-922).
- **Disconnected Devices**: A device that disconnects is closed, and input devices are looked for again every five seconds, so a remote that comes back is picked up and a stale device cannot keep a CPU core busy.
- **Debouncing**: Hardware keypresses across duplicate event devices are automatically debounced within 200 ms to prevent double-triggering.
- **Automatic Discovery**: On supported hardware, the entity is discovered automatically; on unsupported hardware or emulators, the discovery entity is withheld.
