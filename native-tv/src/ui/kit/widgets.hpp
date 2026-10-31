#pragma once
#include "ui/kit/focus.hpp"
#include "ui/kit/geometry.hpp"
#include <string>
#include <vector>

// The TV design kit's components (spec section 8) as pure functions: state in (focus 0..1 and the
// other animated values the motion engine owns), Scene v2 nodes and hit rectangles out. Nothing
// here keeps state, so a view stays a thin read of its animated values.
//
// Components draw at their resting rect and scale by pushing a scene transform, so text and
// icons grow with the card as they do in the prototype. A component whose entrance has not
// started (alpha 0) or that is wholly off the canvas draws nothing.
namespace nativetv {
namespace kit {

// Text and icons come from the glyph and icon modules; the kit asks for them by style and name.
class Glyphs {
  public:
    virtual ~Glyphs() = default;
    // Advance of one line in canvas px, for tab and chip widths.
    virtual float measure(const std::string &text, const TypeStyle &style) const = 0;
    // `box` is the text's layout box; the line ends in an ellipsis where it overflows the width.
    virtual void text(Scene &scene, Rect box, const std::string &text, const TypeStyle &style,
                      Color color, Node::Align align = Node::Align::Left, int maxLines = 1) = 0;
    // A Material icon by file name ("settings", "hdmi"), square in `box`.
    virtual void icon(Scene &scene, Rect box, const std::string &name, Color tint) = 0;
};

// Text and icons as the legacy Text and Image nodes, which the renderer draws today. It ignores
// tracking and measures by a per-character estimate; the glyph-atlas implementation replaces it.
class LegacyGlyphs final : public Glyphs {
  public:
    float measure(const std::string &text, const TypeStyle &style) const override;
    void text(Scene &scene, Rect box, const std::string &text, const TypeStyle &style, Color color,
              Node::Align align, int maxLines) override;
    void icon(Scene &scene, Rect box, const std::string &name, Color tint) override;
};

struct Paint {
    Scene &scene;
    Glyphs &glyphs;
};

// A title's facts line ("Film  ·  2026  ·  ★ 6.4") with the star drawn as an icon; a line too
// wide for `box` loses whole items from its end.
void factsLine(Paint paint, Rect box, const std::string &facts, const TypeStyle &style, Color ink);

// Page entrance and exit: fades in while rising from `dy` px below (spec 5.6).
struct Enter {
    float alpha = 1, dy = 0;
};

// ---- Top bar --------------------------------------------------------------------------------

struct TopBar {
    std::string avatarInitial;
    std::vector<std::string> tabs;
    // The selected tab keeps the pill; -1 when another page owns the bar (Settings).
    int selected = 0;
    // The pill's two edges are animated separately so it stretches toward its target.
    float pillLeft = 0, pillRight = 0;
    float pillHot = 0;   // 0..1: focus is on the bar, so the pill turns from rest to focus fill
    float pillAlpha = 1; // fades out while Settings or Setup is showing
    float avatarFocus = 0, settingsFocus = 0;
    bool settingsSelected = false;
    std::string clock;
    Enter enter;
};
// Tab boxes for the bar's tabs, so the view can aim the pill's springs at them.
std::vector<Rect> tabRects(const Glyphs &glyphs, const std::vector<std::string> &tabs);
// Hits are "nav:avatar", "nav:tab:<i>" and "nav:settings".
void topBar(Paint paint, const TopBar &bar);
void tabPill(Paint paint, float left, float right, float hot, float alpha = 1);

struct RoundButton {
    Rect rest;
    std::string id;
    std::string icon;    // Material icon, or
    std::string text;    // a letter (the avatar)
    bool avatar = false; // gradient fill instead of `fill`
    Color fill = color::surface3; // opaque: the icon keeps its contrast over bright art
    Color ink = color::onSurface;
    float focus = 0; // fill fades to focus fill, ink to on-focus, scale to 1.12
    Enter enter;
};
void roundButton(Paint paint, const RoundButton &button);

// ---- Hero and pagination ---------------------------------------------------------------------

struct HeroSlot {
    std::string image;  // art; empty draws only the tint
    float opacity = 0;  // crossfade
    float kenBurns = 0; // 0..1 over the 14 s push
    float artAlpha = 1; // the picture alone: dimmed to a wallpaper behind a focused app
    Color tint = color::ink;
};
struct Hero {
    HeroSlot slot[2];    // outgoing and incoming art
    float wallAlpha = 1; // 0 off the Home page
    float stackY = 0;    // the rows' vertical offset (<= 0) that drives the parallax
    // A title: source row, title, facts and a two-line description. An app: its icon beside
    // the lines, and the key hint in place of the description.
    std::string source, title, facts, description, image, hint;
    // Source row, title and the lines under it enter one after another (spec 5.3).
    Enter text[3];
};
// The artwork, tints, legibility scrims and the veil that rises with the rows.
void heroWall(Paint paint, const Hero &hero);
// The wall of pages without a hero: two soft washes, the second in the page's tint.
void ambientWall(Paint paint, float amount, Color tint = color::ink);
// Source row, title and "tag | description", fading with the stack's parallax.
void heroText(Paint paint, const Hero &hero);
inline void hero(Paint paint, const Hero &state) {
    heroWall(paint, state);
    heroText(paint, state);
}

// Progress of the current slide, 0..1, from the time since it appeared.
constexpr float heroProgress(float elapsedMs) {
    return focus::clamp01(elapsedMs / motion::ms::heroAdvance);
}
struct Pagination {
    int count = 0;
    std::vector<float> current; // per slot, 0..1: dot turns into the progress track
    float progress = 0;
    float alpha = 1, y = 0; // y: parallax offset
};
void pagination(Paint paint, const Pagination &dots);

// ---- Home rows --------------------------------------------------------------------------------

struct Row {
    int index = 0;
    std::string label;
    float scrollX = 0; // the row's horizontal spring
    float dim = 0;     // 0..1: focus is in another row
    Enter enter;
};
// The label only; items scroll, the label stays.
void row(Paint paint, const Row &row);
// An item's box inside the scrolled row.
inline Rect rowItem(const Row &row, int index, float width, float gap) {
    return {canvas::safeX + index * (width + gap) + row.scrollX, geometry::rowItemsY(row.index),
            width, 0};
}

struct PosterCard {
    Rect rest;
    std::string id, image, title;
    std::string subtitle; // the facts line that appears under the title on focus
    float focus = 0;
    Enter enter;
};
void card(Paint paint, const PosterCard &card);

struct AppCircle {
    Rect rest;
    std::string id, name;
    std::string icon;  // glyph on the circle
    std::string image; // the app's own artwork, masked to the circle
    Color color = color::surface3;
    bool named = false; // the name stays under the circle (the Apps page); else it shows on focus
    float nameWidth = 0; // the name's box; 0 is a box a little wider than the circle
    float focus = 0;
    Enter enter;
};
void appCircle(Paint paint, const AppCircle &circle);

struct InputTile {
    Rect rest;
    std::string id, name, sub;
    std::string icon = "hdmi";
    bool live = false, unused = false;
    float focus = 0;
    Enter enter;
};
void inputTile(Paint paint, const InputTile &tile);

// ---- Apps and Live ------------------------------------------------------------------------------

struct PageHead {
    std::string title, subtitle;
    Enter enter;
};
void pageHead(Paint paint, const PageHead &head);

struct AppTile {
    Rect rest;
    std::string id, name, icon;
    Color color = color::surface3;
    // A banner, as on Google TV and Apple TV: one flat colour (the app's own), its icon or art
    // (drawn over the tile by the page) large in the middle, and no name on the tile.
    bool centred = false;
    float focus = 0;
    Enter enter;
};
void appTile(Paint paint, const AppTile &tile);

struct StatCard {
    Rect rest;
    std::string id, icon, label, value, unit, caption, caption2;
    bool textValue = false;   // a word ("HDMI 1") rather than a number
    float bar = -1;           // 0..1 fill, or < 0 for none
    bool warn = false;        // the bar turns warn-coloured
    std::vector<float> spark; // 0..1 samples; drawn under the caption when not empty
    float sparkShift = 0;     // 0..1 of a sample: the line slides left between updates
    std::vector<bool> days;   // uptime strip: true = up, false = a grey gap day
    float focus = 0;
    Enter enter;
};
void statCard(Paint paint, const StatCard &card);
// A 3 px line over a faded fill, drawn as 6 px columns (the scene has no polyline).
void sparkline(Paint paint, Rect area, const std::vector<float> &samples, float shift);

// ---- Settings ----------------------------------------------------------------------------------

struct SettingsRow {
    Rect rest;
    std::string id, icon, title, value;
    bool toggle = false;
    float on = 0; // toggle position 0..1
    float focus = 0;
    Enter enter;
};
void settingsRow(Paint paint, const SettingsRow &row);
// 68x38 in `rect`. `rowFocus` darkens the ring and knob once the row's fill turns light.
void toggle(Paint paint, Rect rect, float on, float rowFocus = 0);

struct DetailPanel {
    std::string icon, title, body, action; // an empty action leaves the OK hint out
    std::vector<std::string> chips;
    int bodyLines = 3; // the chips move down with a longer body
    Enter enter;
};
void detailPanel(Paint paint, const DetailPanel &panel);
// A pill with an optional accent dot; returns its width so chips can flow.
float pillChip(Paint paint, float x, float y, const std::string &label, bool dot = true,
               float alpha = 1);

// ---- Loading
// -------------------------------------------------------------------------------------

// The indeterminate circular progress indicator of Material and Google TV: a thin arc in the accent
// colour that turns at a constant rate, with an optional label under it. The scene has no arcs or
// rotation, so the arc is drawn as small dots along it that thin out toward the tail; the whole
// thing is under forty nodes and needs no layer of its own.
struct Loading {
    Rect area;         // the indicator is centred in it, and the label sits under
    std::string label; // "Loading..." or what is being read
    float phase = 0;   // 0..1 of a turn: spinnerPhase() in tuning.hpp turns it at a constant rate
    Enter enter;
};
void loadingIndicator(Paint paint, const Loading &loading);

// ---- Keyboard -----------------------------------------------------------------------------------

struct KeyCap {
    Rect rest;
    std::string id, label, icon;
    bool accent = false; // the Done key
    bool hint = false;   // quiet label (space)
    bool latched = false; // shift while capitals are on: accent ink on a tinted fill
    float focus = 0;
    float press = 0; // 0 at rest, 1 at the bottom of the .94 dip
    Enter enter;
};
void keyCap(Paint paint, const KeyCap &key);

// ---- Progress -----------------------------------------------------------------------------------

// The circular indeterminate progress indicator, for a value that is being read or a change that
// is on its way: twelve dots round a circle, the head one bright and the rest fading behind it,
// one turn in 1.25 s. `timeMs` is the frame's clock; the box is square.
void spinner(Paint paint, Rect box, double timeMs, Color ink = color::onSurface2, float alpha = 1);

// Static frames of the designed screens for capture review: "home", "apps", "live", "settings"
// and "setup". False for any other name.
bool designGallery(Paint paint, const std::string &name);
// The same over the legacy text and icon nodes.
bool designGallery(Scene &scene, const std::string &name);

} // namespace kit
} // namespace nativetv
