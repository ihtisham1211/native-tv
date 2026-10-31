---
description: "What Material 3 Expressive specifies for type, colour, shape, spacing, focus, motion and icons, what the Native Tv TV client uses today, and where the two differ."
---

# Material 3 Expressive

The TV client draws every screen from tokens in `native-tv/design/` (`tokens.json` and `metrics.json`; `tools/gen_tokens.py` writes `native-tv/src/ui/kit/tokens.hpp`). The tokens come from Google's TV Design Kit, which is Material 3 at 2 px per dp: the 1920×1080 canvas is 960×540 dp.

Material 3 Expressive is Google's May 2025 update to Material 3. Google's [Android TV design guide](https://developer.android.com/design/ui/tv/guides/foundations/design-for-tv) and `androidx.tv:tv-material` 1.1.0 do not document any adoption of it for TV: the library's theme has colour, shapes and typography, and no motion scheme. This page records what Expressive specifies, what the client does, and the differences. It was checked on 31 October 2026.

A TV screen takes its type, colour, shape, spacing and motion from the tokens, not from literals in a screen's code. A value that departs from the tables below is a decision, and is written here with its reason.

## Typography

| | Material 3 Expressive | Native Tv |
|---|---|---|
| Scale | 30 styles: 15 baseline and 15 emphasized, in five roles (display, headline, title, body, label) of three sizes. A Major Second scale (1.125) on a 14 sp base. | 22 named styles on 13 distinct sizes (`type` in `tokens.hpp`). Each is a baseline style at 2 px per sp: `title` 32/48 is Title Medium (16/24 sp), `body` 28/40 is Body Medium, `labelM` 24/32 is Label Medium, `display` 72/88 is Display Small, `headline` 56/72 is Headline Medium. `numeral` (64/72), `numeralText` (44/68) and `fieldText` (36/48) are off the scale. |
| Emphasis | An emphasized style keeps the size, line height and tracking of its baseline style and raises the weight one step: 400 to 500 for display, headline, title large and body; 500 to 700 (600 on the variable axis) for title medium, title small and label. Components do not use them by default. | Two weights, 400 and 500. Titles and labels use 500, which is their baseline weight. A selected item changes fill and colour, not weight. There is no 600 or 700 cut. |
| Typeface | Roboto for both the brand (display, headline) and the plain (body, label) typeface. Roboto Flex is the variable swap, "not yet part of the M3 typescale". No M3 page names Google Sans Flex as a default. | Google Sans Flex in three static cuts made by `tools/gen_fonts.py`: Display (optical size 36, from 48 px up) and Regular and Medium (optical size 18). No variable axis is used at run time. |
| Tracking | Non-zero on most styles: Label Medium and Body Large 0.5 sp, Title Medium 0.15 sp, Display Large −0.25 sp. | 0 on most styles, 0.01 em on `labelM`, `labelS` and `cap`, −0.01 and −0.02 em on the large numerals. |
| Line height | 1.2 times the size for display, headline and title; about 1.5 for body and label. | The baseline line heights at 2 px per sp. |
| TV | No minimum text size is documented for TV. | The smallest style is `labelS`, 22 px (11 sp). |

## Colour

| | Material 3 Expressive | Native Tv |
|---|---|---|
| Roles | 26 documented roles on five key colours (a sixth palette, error, in Compose), with fixed roles, surface-dim and surface-bright. Expressive changed two things: three contrast levels (standard, medium, high) and more colourful light-theme `on-*-container` roles. The dark theme is unchanged. Material Color Utilities also offers schemes such as TonalSpot, Vibrant and Expressive. | 14 colour tokens and 5 with alpha. Page `ink` #0C0B0E; `surface1` to `surface3` #16151A, #201F26, #2C2B34; `onSurface` to `onSurface3` #F1EEF4, #B9B4C2, #8A8495; `accent` #A8C7FA on `onAccent` #062E6F; `focusFill` #F1EEF4 on `onFocus` #131118; `warn` #FFB784; `bad` #FF8A80. No secondary or tertiary role. One dark scheme, no light theme, no dynamic colour. |
| Dark baseline | Surface is neutral tone 6 (#141218); containers lowest to highest are tones 4, 10, 12, 17 and 22. | `ink` is darker than tone 6, for the OLED panel. |
| Art colour | Not specified. | Home tints its wall and circles from an app's or title's colour (`tintOf`, `appColor` in `home_motion.cpp` and `home_view.cpp`), and falls back to a hue derived from the id. |
| Contrast | 4.5:1 for small text, 3:1 for large text and graphics. | Measured on `ink`: `onSurface` 17.1, `onSurface2` 9.7, `onSurface3` 5.4, `accent` 11.4. `onSurface3` on `surface3` is 3.9. A row label dimmed to 50% (`dimmedRowAlpha`) is 3.2, which meets the large-text limit only. |
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
| Home | Not specified. | Row pitch 252 px; poster card 300×169 px; app circle 112 px; hero text from y 226. |
| Apps | Not specified. | No page head. Six columns of 156 px circles at a 293×240 px pitch from y 160, or one 252 px row per kind from y 160; three rows fit and a fourth shows its top. |
| Editing apps | Not specified. | A 380×56 px pill button (Hide from Home) at the top right from y 136, the Apps page 64 px lower while it is shown, the key hints on a band of ink from y 900, and an app hidden from Home dimmed to 40%. |
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
| Size | 24 dp with a 20 dp live area. Icon buttons from 32 dp (20 dp icon) to 136 dp (40 dp icon). | Rasterised from SVG into an alpha atlas at 18, 24, 26, 28, 34, 38, 40 and 52 px (30 and 56 px on demand) and tinted when drawn. |
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
