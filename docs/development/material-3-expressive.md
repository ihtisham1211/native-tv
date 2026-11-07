---
description: "What Material 3 Expressive specifies for type, colour, shape, spacing, focus, motion and icons, what the Native Tv TV client uses today, and where the two differ."
---

# Material 3 Expressive

The TV client draws every screen from tokens in `native-tv/design/` (`tokens.json` and `metrics.json`; `tools/gen_tokens.py` writes `native-tv/src/ui/kit/tokens.hpp`). The tokens come from Google's TV Design Kit, which is Material 3 at 2 px per dp: the 1920×1080 canvas is 960×540 dp.

Material 3 Expressive is Google's May 2025 update to Material 3. Google's [Android TV design guide](https://developer.android.com/design/ui/tv/guides/foundations/design-for-tv) and `androidx.tv:tv-material` 1.1.0 do not document any adoption of it for TV: the library's theme has colour, shapes and typography, and no motion scheme. This page records what Expressive specifies, what the client does, and the differences. It was checked on 31 October 2026.

A TV screen takes its type, colour, shape, spacing and motion from the tokens, not from literals in a screen's code. A value that departs from the tables below is a decision, and is written here with its reason.

## Themes

`tokens.json` is the theme `n-google-tv`. Each file in `native-tv/design/themes/` is another theme, and names only what differs from it: colours (`color`, `colorFloat`, `colorAlpha`, `focusRingColor`) and `look`, the values that are not colours. `gen_tokens.py` writes one `Palette` per theme into `tokens.hpp` and `palette.cpp`. `kit::useTheme(id)` switches the palette in use, and `color::name`, `alpha::name` and `look::name` read it, so a screen's code does not change with the theme. `nativetv-tv --theme ID` starts the client in a theme; the `oled-*` golden cases use it. Type, shape, spacing, layout and motion are the same in every theme.

### n-oled-care

A theme for OLED panels: the screens should never be what wears the panel. Its departures from `n-google-tv`:

| Token | n-oled-care | n-google-tv | Reason |
|---|---|---|---|
| `ink` | #000000 | #0C0B0E | A pixel that is off does not wear. |
| `surface1` to `surface3` | #0E0E0E, #181818, #232323 | #16151A, #201F26, #2C2B34 | Steps wide enough that a panel's near-black handling does not merge them. |
| `onSurface` to `onSurface3` | #D9D9D9, #9A9A9A, #777777 | #F1EEF4, #B9B4C2, #8A8495 | No static text above 85% luma (Google TV's is 94%). #777777 is 4.7:1 on black, so it is kept for text of 28 px and up. |
| `accent` | #27B45E | #A8C7FA | The brand green at its dark end. Its blue channel is 37%, against 98% for the blue accent, and blue is the panel's weakest emitter. |
| `focusFill` / `onFocus` | #2E2E2E / #D9D9D9 | #F1EEF4 / #131118 | A filled white pill is the largest bright block that sits still. |
| `focusRingColor` | #CFCFCF | #F1EEF4 | |
| `alpha::outline` | 0 | 0.12 | A static hairline is the classic burn-in shape. |
| `wash` and `look.wall` | black, 0 | teal, 1 | The accent glow and the tint wash behind a screen light every pixel. |
| `look.pictures` | false | true | The Details backdrop and any artwork are not drawn or downloaded: artwork is the largest lit region on a screen. App icons are icons and stay. |
| `look.discover` | false | true | No feed: Home has no hero text, no pager, no picks and no YouTube row, and the client does not ask for the TMDB or YouTube feed, so there is no title search either. Home is the apps, the inputs and the panel's cards, which come from the stats the Live screen already reads. |
| `look.homeTop` | 196 | 556 | Where Home's first row starts: under the hero for a theme with a feed, near the top for one without. |
| `look.ringFocus` | true | false | Focus is a ring over the fill and the selected tab an underline. A dark fill alone is 1.3:1 on black, too faint to find from the sofa. |
| `look.restFill` | 0 | 1 | Settings rows have no background until focused: spacing groups them, and a static fill is lit all day. |
| `look.restBrightness` | 0.62 | 1 | App icons rest dimmer than the focused one. Over black, opacity is brightness. |
| `look.driftX`, `look.driftY` | 8, 6 | 0, 0 | The scene, and the pointer hits with it, move in whole pixels from the minute of the day, on axes with periods of 20 and 13 minutes, so no edge rests on one pixel row. At most 3 px between two minutes; 99 positions in a day. |

The idle dim after 30 seconds and the screen saver's start time are not part of a theme.

`tests/golden/oled_budget.py` keeps the `oled-*` goldens inside a light budget: at most 1.6% light output (the mean linear luminance as a share of full white) and 1.0% of pixels above 80% luma. Google TV's Home is at 4.3% and 2.3%; the OLED Care goldens are at 0.85 to 1.46% and 0.3 to 0.9%.

## Typography

| | Material 3 Expressive | Native Tv |
|---|---|---|
| Scale | 30 styles: 15 baseline and 15 emphasized, in five roles (display, headline, title, body, label) of three sizes. A Major Second scale (1.125) on a 14 sp base. | 22 named styles on 13 distinct sizes (`type` in `tokens.hpp`). Each is a baseline style at 2 px per sp: `title` 32/48 is Title Medium (16/24 sp), `body` 28/40 is Body Medium, `labelL` 28/40 is Title Small (14/20 sp), `labelM` 24/32 is Label Medium, `display` 72/88 is Display Small, `headline` 56/72 is Headline Medium. `numeral` (64/72), `numeralText` (44/68) and `fieldText` (36/48) are off the scale. Home's row labels are `labelL`, one step above the 24 px app names under the circles: at Title Medium or Large they towered over the rows, and in `onSurface2` they read faint from the sofa (a B3 at 3 m). |
| Emphasis | An emphasized style keeps the size, line height and tracking of its baseline style and raises the weight one step: 400 to 500 for display, headline, title large and body; 500 to 700 (600 on the variable axis) for title medium, title small and label. Components do not use them by default. | Two weights, 400 and 500. Titles and labels use 500, which is their baseline weight. A selected item changes fill and colour, not weight. There is no 600 or 700 cut. |
| Typeface | Roboto for both the brand (display, headline) and the plain (body, label) typeface. Roboto Flex is the variable swap, "not yet part of the M3 typescale". No M3 page names Google Sans Flex as a default. | Google Sans Flex in three static cuts made by `tools/gen_fonts.py`: Display (optical size 36, from 48 px up) and Regular and Medium (optical size 18). No variable axis is used at run time. |
| Tracking | Non-zero on most styles: Label Medium and Body Large 0.5 sp, Title Medium 0.15 sp, Display Large −0.25 sp. | 0 on most styles, 0.01 em on `labelM`, `labelS` and `cap`, −0.01 and −0.02 em on the large numerals. |
| Line height | 1.2 times the size for display, headline and title; about 1.5 for body and label. | The baseline line heights at 2 px per sp. |
| TV | No minimum text size is documented for TV. | The smallest style is `labelS`, 22 px (11 sp). |

## Colour

| | Material 3 Expressive | Native Tv |
|---|---|---|
| Roles | 26 documented roles on five key colours (a sixth palette, error, in Compose), with fixed roles, surface-dim and surface-bright. Expressive changed two things: three contrast levels (standard, medium, high) and more colourful light-theme `on-*-container` roles. The dark theme is unchanged. Material Color Utilities also offers schemes such as TonalSpot, Vibrant and Expressive. | 15 colour tokens and 5 with alpha. Page `ink` #0C0B0E; `surface1` to `surface3` #16151A, #201F26, #2C2B34; `onSurface` to `onSurface3` #F1EEF4, #B9B4C2, #8A8495; `accent` #A8C7FA on `onAccent` #062E6F; `focusFill` #F1EEF4 on `onFocus` #131118; `navButton` #393B3B, the kit's surface-variant #444746 at 80% laid over `ink` and kept opaque so an icon holds its contrast over art; `warn` #FFB784; `bad` #FF8A80. No secondary or tertiary role. One dark scheme, no light theme, no dynamic colour. |
| Dark baseline | Surface is neutral tone 6 (#141218); containers lowest to highest are tones 4, 10, 12, 17 and 22. | `ink` is darker than tone 6, for the OLED panel. |
| Art colour | Not specified. | Home tints its wall and circles from an app's or title's colour (`tintOf`, `appColor` in `home_motion.cpp` and `home_view.cpp`), and falls back to a hue derived from the id. |
| Contrast | 4.5:1 for small text, 3:1 for large text and graphics. | Measured on `ink`: `onSurface` 17.1, `onSurface2` 9.7, `onSurface3` 5.4, `accent` 11.4. `onSurface3` on `surface3` is 3.9. A row label is `onSurface`, 17.1, and 8.5 dimmed to 50% (`dimmedRowAlpha`); in `onSurface2` the dimmed label was 3.2, the large-text limit only, and faint from the sofa. |
| State layers | Hover 8%, focus 10%, pressed 10%, dragged 16%. | None. Focus swaps the fill to `focusFill` and draws a ring. |

## Shape

The Material 3 Expressive scale has ten steps, in dp: none 0, XS 4, S 8, M 12, L 16, L-increased 20, XL 28, XL-increased 32, XXL 48 and full (a pill). Expressive added L-increased, XL-increased and XXL.

The client's radii are px: `sm` 8, `md` 12, `lg` 20, `xl` 28, `key` 18 and `pill`. They carry the same numbers as the dp steps, so at 2 px per dp they are 4, 6, 10, 14 and 9 dp. Of these only 4 dp is a step of the scale.

| | Material 3 Expressive | Native Tv |
|---|---|---|
| State | A list item goes from 4 dp at rest to 16 dp when focused or selected (12 dp hovered); a pressed button turns more square. | Focus never changes a shape: it scales the item and draws a ring. |
| Library | 35 shapes (Cookie, Clover, Sunny, Pill, Burst and others) for decorative elements such as avatars and image crops, used sparingly; any shape morphs into any other. | App icons are circles and tabs are pills. No library shape and no morph. |
| Containment | Cards 12 dp; dialogs and sheets 28 dp; no large corners on dense components. | Poster cards, input tiles and app tiles `md`; stat cards, settings rows and dialogs `lg`; detail panels and the Quick drawer `xl`. |

## Spacing and layout

| | Material 3 Expressive and Android TV | Native Tv |
|---|---|---|
| Scale | An 8 dp scale with `space100` = 8 dp, and nested 2, 4, 6 and 10 dp. | `space` 4, 8, 12, 16, 24, 32, 48, 64 px, which is 2 to 32 dp. |
| TV canvas | 960×540 dp, shipped as 1080p. Overscan margin 48 dp at the sides and 27 dp top and bottom (the same page also gives 58 and 28 dp). A 12-column grid of 52 dp columns with 20 dp gutters. | 1920×1080 px. Safe margin 80 px at the sides (40 dp) and 48 px top and bottom (24 dp). Rows run to the right edge of the screen. |
| Home | Not specified. | Row pitch 252 px, the label 40 px (the line height of `labelL`) from y 556 and a 12 px gap above the items at y 608, which puts the items 23 px under the label's baseline (a 20 px gap left 31, against about 20 to 24 in a Google TV capture); poster card 300×169 px; app circle 112 px, at a 60 px gap with every circle's name under it while App names is on (28 px with the names off); hero text from y 226. |
| Top bar | Not specified. | 72 px high from y 60: the avatar, the tabs, then at the right the search and settings buttons (80 px, the kit's 40 dp icon button, in `navButton`), the clock and the name Native Tv (`labelL`, the kit's on-surface-variant role, `onSurface2`) at the margin, 24 px apart. The kit lights a tab with focus and tints a selected one; Native Tv departs from it: the selected tab is a pill in `focusFill` with `onFocus` ink whether or not focus is on the bar, after the reference capture the owner supplied. |
| Apps | Not specified. | No page head. One 252 px row per kind from y 180 (48 px under the bar); the page washes in the neutral `neutralWash` colour, not the tint of the last title on Home; three rows fit and a fourth shows its top. |
| Editing apps | Not specified. | On the Apps page by kind, the lifted kind's card carries three pills in its top right corner, level with the kind's name, 48 px high and 16 px apart (Hide or Show 140 px, Move kind 196 px, Done 150 px, with the `eyeoff`, `arrowup` and `check` icons), reached with Up from the apps, and a hint band that lists the keys of the focused part; a 380×56 px pill button (Hide from Home) at the top right from y 136, the Apps page 64 px lower while it is shown, the key hints on a band of ink from y 900, and an app hidden from Home dimmed to 40%. |
| Live | Not specified. | Four columns of 422×292 px stat cards, 24 px apart. |

## Focus

| | Android TV and tv-material 1.1.0 | Native Tv |
|---|---|---|
| Indicators | Scale, border, glow and colour. Scales 1.025, 1.05 or 1.1; defaults 1.1 for buttons, cards and chips, 1.05 for list and navigation-drawer items. | Scale 1.1 for cards, app tiles, input tiles and keys; 1.12 for app circles; 1.035 for stat cards; 1.02 for settings rows. |
| Border | 3 dp on cards (6 px at 1080p), 2 dp on most others. Material Web's focus ring is 3 px wide with a 2 px outward offset. | A 3 px ring in `focusRingColor` #F1EEF4, outside the edge, no offset; a shadow of 22 px offset, 56 px blur, 62% black. |
| Timing | A tween: 300 ms focus, 500 ms unfocus, 120 ms press, easing (0, 0, 0.2, 1). | A tween: 170 ms, easing (0.23, 1, 0.32, 1). |

## Motion

Expressive replaces easing and duration with springs. A spatial spring (position, size, rotation, corner radius) overshoots; an effects spring (colour, opacity) never does. Values are damping ratio and stiffness, mass 1, from the Compose source (`ExpressiveMotionTokens.kt`, `StandardMotionTokens.kt`).

| Token | Expressive | Standard |
|---|---|---|
| Fast spatial | 0.6 / 800 | 0.9 / 1400 |
| Default spatial | 0.8 / 380 | 0.9 / 700 |
| Slow spatial | 0.8 / 200 | 0.9 / 300 |
| Fast, default and slow effects | 1.0 / 3800, 1.0 / 1600, 1.0 / 800 | the same |

| Native Tv | Damping ratio / stiffness | Drives |
|---|---|---|
| `spring::row` | 0.96 / 230 | Row scroll |
| `spring::page` | 1.0 / 170 | Page stack, grid scroll |
| `spring::list` | 1.0 / 210 | Settings and drawer lists |
| `spring::pillLead`, `pillTrail` | 0.9 / 460, 0.9 / 250 | The tab pill's two edges |

Focus, entrances and the hero are tweens (`ms::focus` 170, `ms::base` 320, `ms::hero` 900), not springs. The springs are at or near critical damping, so a position never overshoots. The client has no reduced-motion setting; Material asks for fades instead of slides and scaling, and no parallax or shape morphing, when the platform sets one.

A row's items enter one after another, `stagger::homeCell` 26 ms apart (the tenth and later together), the first `stagger::homeBase` 80 ms after the row starts. The values were 38 and 120 ms: the apps row, 14 circles wide, took 0.78 s from the start of its entrance to the end of its last circle's, which read as slow; it takes 0.63 s now. Home draws nothing until the apps' icons are in (300 ms at most), so a circle never enters as a colour and changes to its art.

## Icons

| | Material 3 Expressive | Native Tv |
|---|---|---|
| Family | Material Symbols, a variable font: weight 100 to 700, fill 0 to 1, grade, optical size 20 to 48; Outlined, Rounded and Sharp. | 62 Material Design Icons from Pictogrammers (`@mdi/js`, Apache-2.0, paths unchanged) in `native-tv/assets/icons/material`. One weight, no fill axis. |
| Size | 24 dp with a 20 dp live area. Icon buttons from 32 dp (20 dp icon) to 136 dp (40 dp icon). | Rasterised from SVG into an alpha atlas at 18, 24, 26, 28, 34, 38, 40 and 52 px (30 and 56 px on demand) and tinted when drawn. The top bar's avatar, search and settings buttons are 80 px (40 dp, the standard icon button) with a 40 px icon; at 56 px they were small targets from the sofa. |
| State | A toggle goes from outlined to filled when selected. | A selected item changes colour; the glyph stays. |

App icons are the apps' own artwork in their own colours, set in circles.

## Not adopted

* Tracking on the baseline values.
* The emphasized weights (600 and 700 cuts).
* The shape steps 4, 16, 32 and 48, and a shape that changes with focus.
* Springs that overshoot, and effects springs for colour and opacity.
* A reduced-motion setting.
* Material Symbols and the filled and outlined state.
* Secondary and tertiary colour roles, and state layers.
* A focus border of 3 dp (6 px) on cards.

Each of these changes the golden images of every screen that uses it, so each is decided on its own.

## Sources

* Typography: [type scale tokens](https://m3.material.io/styles/typography/type-scale-tokens), [applying type](https://m3.material.io/styles/typography/applying-type), [fonts](https://m3.material.io/styles/typography/fonts).
* Colour: [colour system](https://m3.material.io/styles/color/system/overview), [state layers](https://m3.material.io/foundations/interaction/states/state-layers).
* Shape: [corner radius scale](https://m3.material.io/styles/shape/corner-radius-scale), [shape morph](https://m3.material.io/styles/shape/shape-morph).
* Spacing: [spacing](https://m3.material.io/styles/spacing/overview).
* Motion: [how it works](https://m3.material.io/styles/motion/overview/how-it-works), [specs](https://m3.material.io/styles/motion/overview/specs), [applying transitions](https://m3.material.io/styles/motion/transitions/applying-transitions).
* Icons: [applying icons](https://m3.material.io/styles/icons/applying-icons).
* TV: [layouts](https://developer.android.com/design/ui/tv/guides/styles/layouts), [focus system](https://developer.android.com/design/ui/tv/guides/styles/focus-system), [typography](https://developer.android.com/design/ui/tv/guides/styles/typography).
* Overview: [Building with Material 3 Expressive](https://m3.material.io/blog/building-with-m3-expressive).
