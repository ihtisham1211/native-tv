#include "ui/application.hpp"
#include "ui/kit/components.hpp"
#include "ui/kit/focus.hpp"
#include "ui/kit/geometry.hpp"
#include "ui/kit/run_glyphs.hpp"
#include "ui/kit/tuning.hpp"
#include "ui/kit/widgets.hpp"
#include <cmath>
#include <cstdio>
#include <stdexcept>

using namespace nativetv;
namespace {
void check(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}
bool near(float left, float right, float tolerance = .06f) {
    return std::fabs(left - right) <= tolerance;
}
bool sameRect(Rect left, Rect right, float tolerance = .06f) {
    return near(left.x, right.x, tolerance) && near(left.y, right.y, tolerance) &&
           near(left.w, right.w, tolerance) && near(left.h, right.h, tolerance);
}
bool sameColor(Color left, Color right) {
    return near(left.r, right.r, .001f) && near(left.g, right.g, .001f) &&
           near(left.b, right.b, .001f) && near(left.a, right.a, .001f);
}
bool hasHit(const Scene &scene, const std::string &id) {
    for (const auto &hit : scene.hits)
        if (hit.id == id)
            return true;
    return false;
}
const Node *textNode(const Scene &scene, const std::string &value) {
    for (const auto &node : scene.nodes)
        if (node.kind == Node::Kind::Text && node.value == value)
            return &node;
    return nullptr;
}
const Node *textAt(const Scene &scene, const std::string &value) {
    return textNode(scene, value);
}
size_t countKind(const Scene &scene, Node::Kind kind) {
    size_t count = 0;
    for (const auto &node : scene.nodes)
        if (node.kind == kind)
            ++count;
    return count;
}
const Node *shapeAt(const Scene &scene, Rect rect, float tolerance = .06f) {
    for (const auto &node : scene.nodes)
        if (node.kind == Node::Kind::Rectangle && sameRect(node.rect, rect, tolerance))
            return &node;
    return nullptr;
}
const Node *shapeWith(const Scene &scene, Rect rect, Color color) {
    for (const auto &node : scene.nodes)
        if (node.kind == Node::Kind::Rectangle && sameRect(node.rect, rect) &&
            sameColor(node.color, color))
            return &node;
    return nullptr;
}
const Hit *hitFor(const Scene &scene, const std::string &id) {
    for (const auto &hit : scene.hits)
        if (hit.id == id)
            return &hit;
    return nullptr;
}
struct Fixture {
    Scene scene;
    kit::LegacyGlyphs glyphs;
    kit::Paint paint() {
        return {scene, glyphs};
    }
};
struct NoopServices final : Services {
    unsigned read(const std::string &, Operation, const Json &) override {
        return 0;
    }
    unsigned write(const std::string &, Operation, const Json &) override {
        return 0;
    }
    void cancel(const std::string &) override {}
};

void tokens() {
    check(sameColor(kit::color::ink, kit::rgb(0x0C0B0E)) &&
              sameColor(kit::color::focusFill, kit::rgb(0xF1EEF4)) &&
              sameColor(kit::alpha::outline, kit::rgb(0xF1EEF4, .12f)),
          "Colour tokens come from tokens.json");
    check(kit::type::display.px == 72 && kit::type::display.line == 76 &&
              kit::type::display.weight == 300 && near(kit::type::display.tracking, -.02f, 1e-6f) &&
              kit::type::key.px == 30,
          "Type tokens keep size, line, weight and tracking");
    check(kit::motion::ms::page == 420 && kit::motion::ms::focus == 170 &&
              kit::motion::spring::row.stiffness == 230 &&
              near(kit::motion::spring::row.dampingRatio, .96f, 1e-6f) &&
              kit::motion::stagger::heroText[2] == 240 && kit::motion::ms::idleAfter == 30000 &&
              near(kit::motion::scale::card, 1.1f, 1e-6f) &&
              near(kit::motion::ease::out.x1, .23f, 1e-6f) && kit::motion::ease::linear.x2 == 1,
          "Motion tokens keep durations, springs, scales and curves");
    check(kit::elevation::focusRingWidth == 3 && kit::elevation::focusShadow.dy == 22 &&
              kit::elevation::focusShadow.blur == 56 &&
              near(kit::elevation::focusShadow.alpha, .62f),
          "Elevation tokens keep the ring and the focus shadow");
}

// Milliseconds a spring takes to come to rest after a 100 px move.
double settleMs(kit::SpringTuning tuning, double *overshoot = nullptr) {
    Spring spring(kit::springParams(tuning), 0);
    spring.retarget(100);
    double peak = 0, elapsed = 0;
    while (!spring.settled() && elapsed < 3000) {
        spring.advance(1000.0 / 240);
        elapsed += 1000.0 / 240;
        peak = std::max(peak, spring.position() - 100);
    }
    if (overshoot)
        *overshoot = peak;
    return elapsed;
}

void motionTokens() {
    double overshoot = 0;
    check(settleMs(kit::motion::spring::row, &overshoot) < 600 && overshoot < 1,
          "The row spring settles within 600 ms without visible overshoot");
    check(settleMs(kit::motion::spring::page, &overshoot) < 800 && overshoot < .01,
          "The page spring is critically damped and settles within 800 ms");
    check(settleMs(kit::motion::spring::pillLead) < 400 &&
              settleMs(kit::motion::spring::pillTrail) < 550,
          "Both pill edges settle within about half a second");
    Easing out = kit::easing(kit::motion::ease::out),
           linear = kit::easing(kit::motion::ease::linear);
    check(out(.5) > .85 && out(0) == 0 && near(static_cast<float>(out(1)), 1, 1e-4f) &&
              linear(.3) == .3 && kit::easing(kit::motion::ease::inOut)(.1) < .05 &&
              kit::easing(kit::motion::ease::inOut)(.9) > .95,
          "Easing tokens become the engine's curves: strong ease-out, slow-at-both-ends in-out, "
          "linear");
}

void focusMath() {
    namespace focus = kit::focus;
    check(focus::scaleAt(1.1f, 0) == 1 && near(focus::scaleAt(1.1f, 1), 1.1f, 1e-6f) &&
              near(focus::scaleAt(1.1f, .5f), 1.05f, 1e-6f) &&
              near(focus::scaleAt(1.1f, 3), 1.1f, 1e-6f) && focus::scaleAt(1.1f, -1) == 1,
          "Focus scale interpolates between rest and the token and clamps");

    Rect card = kit::geometry::card(0, 0);
    Rect lifted = focus::scaledAbout(card, 1.1f);
    check(near(lifted.w, 224 * 1.1f) && near(lifted.h, 140 * 1.1f) &&
              near(lifted.x + lifted.w / 2, card.x + card.w / 2) &&
              near(lifted.y + lifted.h / 2, card.y + card.h / 2),
          "A focused card grows 10% about its centre");
    Rect row = kit::geometry::settingsRow(0);
    Rect pushed = focus::scaledAbout(row, 1.02f, focus::leftCentre);
    check(near(pushed.x, row.x) && near(pushed.w, 840 * 1.02f) &&
              near(pushed.y + pushed.h / 2, row.y + row.h / 2),
          "A settings row grows 2% about its left-centre");

    focus::Look rest = focus::lookAt(card, kit::motion::scale::card, 0);
    check(sameRect(rest.rect, card) && rest.ringAlpha == 0 && rest.shadowAlpha == 0 &&
              rest.ringWidth == 3,
          "An unfocused card has no ring and no shadow");
    focus::Look half = focus::lookAt(card, kit::motion::scale::card, .5f);
    check(near(half.ringAlpha, .5f) && near(half.shadowAlpha, .31f) && half.shadowDy == 22 &&
              half.shadowBlur == 56 && near(half.scale, 1.05f, 1e-6f),
          "Ring and shadow fade with focus; their shape does not change");
    focus::Look full = focus::lookAt(card, kit::motion::scale::card, 1);
    check(near(full.shadowAlpha, .62f) && sameRect(full.rect, lifted),
          "Full focus is the 10% lift with the .62 shadow");

    check(sameColor(focus::inkAt(kit::color::onSurface2, kit::color::onFocus, 0),
                    kit::color::onSurface2) &&
              sameColor(focus::inkAt(kit::color::onSurface2, kit::color::onFocus, 1),
                        kit::color::onFocus),
          "Ink cross-fades from rest to focused");
    check(focus::labelRise(0) == -4 && focus::labelRise(1) == 0 &&
              near(focus::dimmedRowAlpha(1), .5f, 1e-6f) && focus::dimmedRowAlpha(0) == 1,
          "The app name rises 4 px; other rows dim to 50%");
    check(near(focus::keyScale(1, 0), 1.1f, 1e-6f) && near(focus::keyScale(1, 1), .94f, 1e-6f) &&
              near(focus::keyScale(0, 1), .94f, 1e-6f) && focus::keyScale(0, 0) == 1,
          "A key press dips to .94 from wherever focus left it");
}

void geometryAgainstMetrics() {
    namespace geometry = kit::geometry;
    namespace home = kit::metrics::home;
    check(sameRect(geometry::avatar(), home::avatar) &&
              sameRect(geometry::settingsButton(), home::gear) &&
              sameRect(geometry::clock(), home::clock),
          "The top bar's avatar, settings button and clock sit where the prototype put them");
    std::vector<Rect> tabs =
        geometry::tabRects({home::tabHome.w - 64, home::tabApps.w - 64, home::tabLive.w - 64});
    // The prototype's widths are rounded to 0.1 each, so positions drift by up to 0.1.
    check(sameRect(tabs[0], home::tabHome, .15f) && sameRect(tabs[1], home::tabApps, .15f) &&
              sameRect(tabs[2], home::tabLive, .15f),
          "Tabs touch, each its text plus 32 px on both sides");
    check(sameRect(geometry::pill(tabs[1].x, tabs[1].x + tabs[1].w), {315.3f, 60, 134.8f, 72}) &&
              sameRect(geometry::pill(tabs[0].x, tabs[2].x + tabs[2].w),
                       {168, 60, 147.3f + 134.8f + 118.8f, 72}),
          "The pill spans its two edges at the bar height");
    check(sameRect(geometry::heroSource(), home::heroMeta) &&
              sameRect(geometry::heroTitle(), home::heroTitle) &&
              sameRect(geometry::heroSubtitle(), home::heroSubtitle),
          "The hero text block stacks source, title and subtitle");
    check(sameRect(geometry::dotSlot(0), home::dotSlot1) &&
              near(geometry::dotSlot(5).x + geometry::dotSlot(5).w, 1840) &&
              sameRect({geometry::dotSlot(0).x, 594, 6 * 28, 20}, home::dots6Slots),
          "Six pagination slots end at the right margin");
    check(sameRect(geometry::rowLabel(0), home::row1Label) &&
              sameRect(geometry::rowLabel(1), home::row2Label) &&
              sameRect(geometry::rowLabel(2), home::row3Label),
          "Row labels sit 212 px apart");
    check(sameRect(geometry::card(0, 0), home::card1) &&
              sameRect(geometry::card(0, 1), home::card2) &&
              near(geometry::card(0, 1).x - geometry::card(0, 0).x, 248),
          "Poster cards are 224x140 at a 248 px pitch");
    check(sameRect(geometry::appCircle(1, 0), home::app1) &&
              sameRect(geometry::appCircle(1, 1), home::app2),
          "App circles are 110 px at a 134 px pitch");
    check(sameRect(geometry::inputTile(2, 0), home::input1), "Input tiles sit in the third row");

    namespace apps = kit::metrics::apps;
    check(sameRect(geometry::pageTitle(apps::title.w), apps::title) &&
              sameRect(geometry::pageSubtitle(apps::subtitle.w), apps::subtitle) &&
              sameRect(geometry::appTile(0), apps::tile1) &&
              sameRect(geometry::appTile(1), apps::tile2) &&
              sameRect(geometry::appTile(6), apps::tile7Row2),
          "The Apps grid is six 272x153 tiles per row");

    namespace live = kit::metrics::live;
    check(sameRect(geometry::statCard(0), live::stat1) &&
              sameRect(geometry::statCard(1), live::stat2) &&
              sameRect(geometry::statCard(4), live::stat5Row2),
          "Live stat cards are 422x276 in four columns");

    namespace settings = kit::metrics::settings;
    check(sameRect(geometry::settingsViewport(), settings::listViewport) &&
              sameRect(geometry::settingsRow(0), settings::row1) &&
              sameRect(geometry::settingsRow(1), settings::row2) &&
              sameRect(geometry::detailPanel(), settings::detailPanel),
          "Settings rows are 840x84 at a 92 px pitch beside the detail panel");

    namespace setup = kit::metrics::setup;
    std::vector<std::string> digits(10, "1"), shiftRow = {"shift", "z", "delete"},
                                              lastRow = {"space", "done"};
    check(sameRect(geometry::keyRow(944, 340, 0, digits)[0], setup::key1) &&
              sameRect(geometry::keyRow(944, 340, 3, shiftRow)[0], setup::shiftKey) &&
              sameRect(geometry::keyRow(944, 340, 4, lastRow)[0], setup::spaceKey) &&
              sameRect(geometry::keyRow(944, 340, 4, lastRow)[1], setup::doneKey),
          "Keys are 80x74 with a 10 px gap; shift, space and done are wider");
}

void scrolling() {
    namespace geometry = kit::geometry;
    check(geometry::rowScrollTarget(0, 20, 224, 24) == 0 &&
              geometry::rowScrollTarget(3, 20, 224, 24) == 0 &&
              near(geometry::rowScrollTarget(4, 20, 224, 24), -248) &&
              near(geometry::rowScrollTarget(10, 20, 224, 24), -7 * 248),
          "A row holds still until focus passes the 4th item, then follows it");
    float lastEnd = 20 * 248 - 24 - 1760;
    check(near(geometry::rowScrollTarget(19, 20, 224, 24), -lastEnd) &&
              geometry::rowScrollTarget(5, 6, 224, 24) == 0,
          "A row stops at its end and a short row never scrolls");
    check(geometry::stackTarget(0) == 0 && geometry::stackTarget(-1) == 0 &&
              geometry::stackTarget(2) == -424,
          "The stack snaps one row pitch per content row");
    kit::geometry::HeroParallax none = geometry::heroParallax(0),
                                gone = geometry::heroParallax(-212),
                                mid = geometry::heroParallax(-106);
    check(none.textAlpha == 1 && none.scrimOpacity == 0 && near(gone.textY, -106) &&
              gone.textAlpha == 0 && near(gone.artY, -25.44f) && near(gone.scrimOpacity, .84f) &&
              near(mid.textAlpha, 1 - 106.0f / 150) && near(mid.scrimOpacity, .42f),
          "Hero parallax follows the stack offset");
    check(geometry::settingsScrollTarget(0, 12) == 0 &&
              geometry::settingsScrollTarget(3, 12) == 0 &&
              near(geometry::settingsScrollTarget(5, 12), -184) &&
              near(geometry::settingsScrollTarget(11, 12), -(12 * 92 + 16 - 756)) &&
              geometry::settingsScrollTarget(7, 4) == 0,
          "The settings list follows focus and stops at its end");
}

void cards() {
    Fixture rest;
    kit::PosterCard poster;
    poster.rest = kit::geometry::card(0, 0);
    poster.id = "card:0";
    poster.title = "Glass Season";
    poster.image = "image:one";
    kit::card(rest.paint(), poster);
    const Hit *hit = hitFor(rest.scene, "card:0");
    check(hit && sameRect(hit->rect, {80, 732, 224, 140}), "A card at rest is 224x140 at 1080p");
    const Node &base = rest.scene.nodes[0];
    check(base.kind == Node::Kind::Rectangle && base.radius == 12 &&
              sameColor(base.color, kit::color::surface2) && base.ring.outer == 0 &&
              base.shadow.color.a == 0 && base.group.identity(),
          "An unfocused card has no ring, shadow or scale");
    check(textAt(rest.scene, "Glass Season") &&
              sameRect(textAt(rest.scene, "Glass Season")->rect, {93, 828, 198, 26}),
          "The title sits 13 px in and 44 px above the bottom edge");

    Fixture lifted;
    poster.focus = 1;
    kit::card(lifted.paint(), poster);
    const Node &focused = lifted.scene.nodes[0];
    check(near(focused.group.sx, 1.1f, 1e-5f) && near(focused.group.sy, 1.1f, 1e-5f) &&
              sameRect(focused.group.apply(focused.rect), {68.8f, 725, 246.4f, 154}) &&
              near(focused.ring.outer, 3) && sameColor(focused.ring.color, kit::rgb(0xF1EEF4)) &&
              focused.shadow.dy == 22 && focused.shadow.blur == 56 &&
              near(focused.shadow.color.a, .62f) &&
              sameRect(hitFor(lifted.scene, "card:0")->rect, {68.8f, 725, 246.4f, 154}),
          "A focused card scales 1.1 about its centre and gains the ring and shadow");

    Fixture after;
    kit::card(after.paint(), poster);
    after.scene.shape({0, 0, 1, 1}, kit::color::ink);
    check(after.scene.nodes.back().group.identity() && after.scene.nodes.back().groupOpacity == 1,
          "A component leaves no transform or opacity pushed");

    Fixture entering;
    poster.focus = 0;
    poster.enter = {.5f, 28};
    kit::card(entering.paint(), poster);
    check(near(entering.scene.nodes[0].groupOpacity, .5f) && entering.scene.nodes[0].group.ty == 28,
          "An entering card carries its fade and rise");
    Fixture hidden, away;
    poster.enter = {0, 28};
    kit::card(hidden.paint(), poster);
    poster.enter = {};
    poster.rest.x = 4000;
    kit::card(away.paint(), poster);
    check(hidden.scene.nodes.empty() && hidden.scene.hits.empty() && away.scene.nodes.empty(),
          "A card that has not entered or is off the canvas draws nothing");

    // A row of 40 cards only draws the ones on the canvas.
    Fixture longRow;
    for (int i = 0; i < 40; ++i) {
        kit::PosterCard item;
        item.rest = kit::geometry::card(0, i);
        item.id = "card:" + std::to_string(i);
        kit::card(longRow.paint(), item);
    }
    check(longRow.scene.hits.size() == 8,
          "Eight 224 px cards fit the 1920 px canvas; the rest are culled");
}

void circlesAndTiles() {
    Fixture circle;
    kit::AppCircle app;
    app.rest = kit::geometry::appCircle(1, 0);
    app.id = "app:0";
    app.name = "Netflix";
    app.icon = "play";
    app.color = kit::rgb(0xE50914);
    kit::appCircle(circle.paint(), app);
    const Node &disc = circle.scene.nodes[0];
    check(sameRect(disc.rect, {80, 944, 110, 110}) && disc.radius == 55 &&
              disc.gradient.stops == 3 && disc.gradient.angle == 145 && disc.gradient.at[2] == 1 &&
              !textAt(circle.scene, "Netflix") && circle.scene.nodes.size() == 2,
          "An app circle is a 110 px disc with a 145 degree gradient and no name at rest");
    Fixture named;
    app.focus = 1;
    kit::appCircle(named.paint(), app);
    check(near(named.scene.nodes[0].group.sx, 1.1f, 1e-5f) && textAt(named.scene, "Netflix") &&
              sameRect(textAt(named.scene, "Netflix")->rect, {50, 1068, 170, 24}),
          "A focused app circle grows 10% and shows its name 124 px below its top");
    Fixture half;
    app.focus = .5f;
    kit::appCircle(half.paint(), app);
    check(near(textAt(half.scene, "Netflix")->color.a, .5f) &&
              near(textAt(half.scene, "Netflix")->rect.y, 1068 - 2),
          "The name fades in while rising 4 px");

    Fixture input;
    kit::InputTile tile;
    tile.rest = kit::geometry::inputTile(2, 0);
    tile.id = "input:1";
    tile.name = "HDMI 1";
    tile.sub = "Game Console";
    tile.live = true;
    tile.unused = true;
    kit::inputTile(input.paint(), tile);
    check(sameRect(input.scene.nodes[0].rect, {80, 1156, 224, 140}) &&
              sameColor(input.scene.nodes[0].color, kit::color::surface2) &&
              near(input.scene.nodes[0].ring.inner, 1) &&
              near(input.scene.nodes[0].groupOpacity, .62f) && textAt(input.scene, "HDMI 1") &&
              textAt(input.scene, "Game Console") &&
              shapeAt(input.scene, {80 + 224 - 26, 1172, 10, 10}),
          "An unused input tile is 62% opaque with its outline, names and a live dot 16 px from "
          "the corner");

    Fixture appTile;
    kit::AppTile gridTile;
    gridTile.rest = kit::geometry::appTile(7);
    gridTile.id = "tile:7";
    gridTile.name = "Plex";
    gridTile.icon = "play";
    kit::appTile(appTile.paint(), gridTile);
    check(sameRect(appTile.scene.nodes[0].rect, {376, 471, 272, 153}) &&
              appTile.scene.nodes[0].gradient.angle == 150 && textAt(appTile.scene, "Plex") &&
              sameRect(textAt(appTile.scene, "Plex")->rect, {396, 471 + 153 - 20 - 28, 232, 28}),
          "An app tile is 272x153 with a 150 degree gradient and its name at the bottom left");
}

void barAndHero() {
    Fixture bar;
    kit::TopBar top;
    top.avatarInitial = "I";
    top.tabs = {"Home", "Apps", "Live"};
    top.selected = 0;
    top.clock = "10:11";
    std::vector<Rect> tabs = kit::tabRects(bar.glyphs, top.tabs);
    top.pillLeft = tabs[0].x;
    top.pillRight = tabs[0].x + tabs[0].w;
    kit::topBar(bar.paint(), top);
    check(hitFor(bar.scene, "nav:avatar") && hitFor(bar.scene, "nav:tab:0") &&
              hitFor(bar.scene, "nav:tab:2") && hitFor(bar.scene, "nav:settings") &&
              sameRect(hitFor(bar.scene, "nav:settings")->rect, kit::metrics::home::gear) &&
              sameRect(hitFor(bar.scene, "nav:avatar")->rect, kit::metrics::home::avatar),
          "The top bar offers the avatar, each tab and the settings button");
    const Node *pill = &bar.scene.nodes[0];
    check(sameRect(pill->rect, {tabs[0].x, 60, tabs[0].w, 72}) && pill->radius == 36 &&
              sameColor(pill->color, kit::color::pillRest),
          "The pill rests on the selected tab in the opaque rest colour");
    check(textAt(bar.scene, "10:11") &&
              sameRect(textAt(bar.scene, "10:11")->rect, kit::metrics::home::clock) &&
              textAt(bar.scene, "10:11")->align == Node::Align::Right &&
              sameColor(textAt(bar.scene, "Home")->color, kit::color::onSurface) &&
              sameColor(textAt(bar.scene, "Apps")->color, kit::color::onSurface2),
          "The clock is right-aligned at 1840; the selected tab reads brighter");
    Fixture hot;
    top.pillHot = 1;
    top.avatarFocus = 1;
    kit::topBar(hot.paint(), top);
    check(sameColor(hot.scene.nodes[0].color, kit::color::focusFill) &&
              sameColor(textAt(hot.scene, "Home")->color, kit::color::onFocus) &&
              sameColor(textAt(hot.scene, "Apps")->color, kit::color::onSurface2),
          "With focus on the bar the pill turns light and the selected tab dark");
    check(near(hot.scene.nodes[1].group.sx, 1.12f, 1e-5f), "A focused round button grows 12%");
    Fixture gone;
    top.pillAlpha = 0;
    kit::topBar(gone.paint(), top);
    check(!shapeAt(gone.scene, {tabs[0].x, 60, tabs[0].w, 72}),
          "The pill is not drawn while Settings owns the page");

    Fixture settingsOn;
    kit::TopBar onSettings;
    onSettings.tabs = {"Home"};
    onSettings.selected = -1;
    onSettings.settingsSelected = true;
    kit::topBar(settingsOn.paint(), onSettings);
    check(shapeAt(settingsOn.scene, kit::metrics::home::gear) &&
              sameColor(shapeAt(settingsOn.scene, kit::metrics::home::gear)->color,
                        kit::alpha::accentDim),
          "The settings button takes the accent fill when it is the page");

    Fixture wall;
    kit::Hero hero;
    hero.slot[0].image = "art:1";
    hero.slot[0].opacity = 1;
    hero.slot[0].kenBurns = 1;
    hero.source = "Harbor Originals";
    hero.title = "Harbor Lights";
    hero.tag = "Trending";
    hero.description = "A ferry pilot and a lighthouse keeper";
    kit::hero(wall.paint(), hero);
    const Node *title = textAt(wall.scene, "Harbor Lights");
    check(title && sameRect(title->rect, kit::metrics::home::heroTitle) &&
              sameRect(textAt(wall.scene, "Harbor Originals")->rect, {120, 416, 1360, 30}) &&
              textAt(wall.scene, "Trending") && near(textAt(wall.scene, "Trending")->rect.x, 80) &&
              near(textAt(wall.scene, "Trending")->rect.y, 542) &&
              shapeAt(wall.scene, {80, 416 + 1, 28, 28}),
          "The hero text block sits where the prototype put it: source, title, subtitle");
    namespace home = kit::metrics::home;
    check(near(kit::geometry::heroSubtitle().y + kit::geometry::heroSubtitle().h -
                   kit::geometry::heroSource().y,
               home::heroSubtitle.y + home::heroSubtitle.h - home::heroMeta.y) &&
              near(home::heroSubtitle.y + home::heroSubtitle.h - home::heroMeta.y, 164),
          "The hero text block is 164 px tall, from the source row to the subtitle");
    const Node *art = nullptr;
    for (const auto &node : wall.scene.nodes)
        if (node.kind == Node::Kind::Image && node.value == "art:1")
            art = &node;
    check(art && art->value == "art:1" && sameRect(art->rect, {0, 0, 1920, 1080}) &&
              near(art->group.sx, 1.09f, 1e-5f) &&
              near(art->group.tx, 1344 * (1 - 1.09f) - 18, .01f) &&
              near(art->group.ty, 540 * (1 - 1.09f) - 6, .01f) && art->clipped,
          "The art covers the canvas and pushes in 9% about (70%, 50%), clipped to the screen");
    size_t gradients = 0;
    for (const auto &node : wall.scene.nodes)
        if (node.gradient.stops == 4)
            ++gradients;
    check(gradients == 1, "The bottom scrim has four stops (.97, .78, .30, 0)");

    Fixture scrolled;
    hero.stackY = -212;
    kit::hero(scrolled.paint(), hero);
    check(!textAt(scrolled.scene, "Harbor Lights") &&
              near(scrolled.scene.nodes.back().color.a, .86f * .84f, .001f),
          "Scrolled a full row the hero text is gone and the veil is up");
    Fixture mid;
    hero.stackY = -75;
    kit::heroText(mid.paint(), hero);
    check(near(textAt(mid.scene, "Harbor Lights")->group.ty, -37.5f) &&
              near(textAt(mid.scene, "Harbor Lights")->groupOpacity, .5f),
          "Halfway through the 150 px fade the title has moved by half the offset");

    Fixture dots;
    kit::Pagination pager;
    pager.count = 6;
    pager.current = {1, 0, 0, 0, 0, 0};
    pager.progress = .5f;
    kit::pagination(dots.paint(), pager);
    check(dots.scene.nodes.size() == 5 + 2 && shapeAt(dots.scene, {1672, 600, 28, 8}) &&
              shapeAt(dots.scene, {1672, 600, 14, 8}) &&
              shapeAt(dots.scene, {1700 + 10, 600, 8, 8}) &&
              near(shapeAt(dots.scene, {1700 + 10, 600, 8, 8})->color.a, .38f),
          "Pagination draws a 28 px track filled to half and five 8 px dots at 38%");
    check(kit::heroProgress(0) == 0 && near(kit::heroProgress(4500), .5f) &&
              kit::heroProgress(20000) == 1,
          "Hero progress runs over the 9 s slide");
}

void rows() {
    Fixture dimmed;
    kit::Row row;
    row.index = 1;
    row.label = "Your apps";
    row.dim = 1;
    kit::row(dimmed.paint(), row);
    check(sameRect(textAt(dimmed.scene, "Your apps")->rect, kit::metrics::home::row2Label) &&
              near(textAt(dimmed.scene, "Your apps")->color.a, .5f),
          "A row label sits 212 px per row down and dims to 50% when focus is elsewhere");
    row.scrollX = -248;
    Rect third = kit::rowItem(row, 3, 224, 24);
    check(near(third.x, 80 + 3 * 248 - 248) && near(third.y, 944),
          "Row items follow the row's scroll offset");
}

void statsAndSettings() {
    Fixture stat;
    kit::StatCard card;
    card.rest = kit::geometry::statCard(1);
    card.id = "stat:1";
    card.icon = "memory";
    card.label = "Memory";
    card.value = "1.7";
    card.unit = "of 2.9 GB";
    card.caption = "Swap off";
    card.bar = .5f;
    kit::statCard(stat.paint(), card);
    check(sameRect(stat.scene.nodes[0].rect, kit::metrics::live::stat2) &&
              stat.scene.nodes[0].radius == 20 && near(stat.scene.nodes[0].ring.inner, 1) &&
              sameRect(textAt(stat.scene, "1.7")->rect, {554, 382, 366, 68}) &&
              sameRect(textAt(stat.scene, "Swap off")->rect, kit::metrics::live::caption) &&
              textAt(stat.scene, "of 2.9 GB") && textAt(stat.scene, "Memory"),
          "A stat card is 422x276 with its value, caption and unit at the measured offsets");
    check(
        shapeAt(stat.scene, kit::metrics::live::bar) &&
            sameColor(shapeAt(stat.scene, kit::metrics::live::bar)->color, kit::color::surface3) &&
            shapeWith(stat.scene, {554, 530, 183, 8}, kit::color::accent),
        "The bar is an 8 px track with an accent fill to the fraction");
    check(shapeWith(stat.scene, {554, 316, 44, 44}, kit::color::surface3),
          "The icon well is 44 px at (28, 26)");
    Fixture hot;
    card.warn = true;
    card.bar = 1;
    kit::statCard(hot.paint(), card);
    check(sameColor(shapeAt(hot.scene, kit::metrics::live::bar)->color, kit::color::surface3) &&
              shapeWith(hot.scene, {554, 530, 366, 8}, kit::color::warn),
          "A warm bar turns the warn colour");
    Fixture days;
    card.bar = -1;
    card.days.assign(14, true);
    card.days[6] = false;
    kit::statCard(days.paint(), card);
    int grey = 0, up = 0;
    for (const auto &node : days.scene.nodes) {
        if (node.rect.h == 36 && node.radius == 4)
            ++(sameColor(node.color, kit::color::surface3) ? grey : up);
    }
    check(grey == 1 && up == 13, "The uptime strip has 14 days with one grey gap");
    Fixture spark;
    card.days.clear();
    for (int i = 0; i < 30; ++i)
        card.spark.push_back(.5f + .4f * std::sin(i * .5f));
    kit::statCard(spark.paint(), card);
    size_t columns = 0;
    for (const auto &node : spark.scene.nodes)
        if (node.rect.w == 6 && node.radius == 1.5f)
            ++columns;
    check(columns == 61, "The sparkline is drawn as 61 six-pixel columns");

    Fixture row;
    kit::SettingsRow setting;
    setting.rest = kit::geometry::settingsRow(1);
    setting.id = "setting:1";
    setting.icon = "display";
    setting.title = "Picture mode";
    setting.value = "Cinema";
    kit::settingsRow(row.paint(), setting);
    check(sameRect(row.scene.nodes[0].rect, kit::metrics::settings::row2) &&
              sameColor(row.scene.nodes[0].color, kit::color::surface1) &&
              shapeAt(row.scene, {98, 388 + 18, 48, 48}) &&
              sameRect(textAt(row.scene, "Picture mode")->rect, {166, 414, 420, 32}) &&
              textAt(row.scene, "Cinema")->align == Node::Align::Right &&
              near(textAt(row.scene, "Cinema")->rect.x + textAt(row.scene, "Cinema")->rect.w, 892),
          "A settings row is 840x84 with its well, title and right-aligned value");
    Fixture focused;
    setting.focus = 1;
    kit::settingsRow(focused.paint(), setting);
    check(near(focused.scene.nodes[0].group.sx, 1.02f, 1e-5f) &&
              near(focused.scene.nodes[0].group.tx, 80 - 80 * 1.02f, .01f) &&
              sameColor(textAt(focused.scene, "Picture mode")->color, kit::color::onFocus) &&
              sameColor(focused.scene.nodes[1].color, kit::color::focusFill),
          "A focused row grows 2% from its left edge and turns to the focus fill");
    Fixture toggled;
    setting.focus = 0;
    setting.toggle = true;
    setting.on = 1;
    kit::settingsRow(toggled.paint(), setting);
    check(shapeWith(toggled.scene, {824, 411, 68, 38}, kit::color::accent) &&
              shapeAt(toggled.scene, kit::focus::scaledAbout({824 + 36, 411 + 6, 26, 26}, 1.18f)) &&
              !textAt(toggled.scene, "Cinema"),
          "A toggle row shows the 68x38 switch with its knob at +36 grown 18%");
    Fixture off;
    kit::toggle(off.paint(), {824, 595, 68, 38}, 0);
    check(near(off.scene.nodes[0].ring.inner, 3) &&
              sameColor(off.scene.nodes[0].ring.color, kit::color::onSurface3) &&
              shapeAt(off.scene, {830, 601, 26, 26}) && off.scene.nodes.size() == 2,
          "An off toggle is a 3 px ring with the knob at +6");

    Fixture panel;
    kit::DetailPanel detail;
    detail.icon = "display";
    detail.title = "Picture mode";
    detail.body = "Choose how the screen looks.";
    detail.action = "Change";
    detail.chips = {"Cinema", "Game"};
    kit::detailPanel(panel.paint(), detail);
    check(sameRect(panel.scene.nodes[0].rect, kit::metrics::settings::detailPanel) &&
              panel.scene.nodes[0].radius == 28 &&
              shapeAt(panel.scene, kit::metrics::settings::panelIcon) &&
              sameRect(textAt(panel.scene, "Picture mode")->rect,
                       kit::metrics::settings::panelTitle) &&
              near(textAt(panel.scene, "Choose how the screen looks.")->rect.y,
                   kit::metrics::settings::panelText.y) &&
              textAt(panel.scene, "Cinema") && textAt(panel.scene, "Change") &&
              textAt(panel.scene, "OK"),
          "The detail panel lays out well, title, body, chips and hint like the prototype");
}

void galleries() {
    // Every designed screen emits a bounded number of nodes, stays balanced and is reachable
    // from `--screen kit:<name>`.
    NoopServices services;
    for (const char *name : {"home", "apps", "live", "settings", "setup"}) {
        Scene scene;
        check(kit::designGallery(scene, name) && scene.nodes.size() > 30 &&
                  scene.nodes.size() < 700 && !scene.hits.empty(),
              "Each designed screen has a bounded frame of nodes");
        scene.shape({0, 0, 1, 1}, kit::color::ink);
        check(scene.nodes.back().group.identity() && scene.nodes.back().groupOpacity == 1 &&
                  !scene.nodes.back().clipped,
              "A designed screen leaves no group state pushed");
        Application app(services, std::string("kit:") + name);
        check(app.screenName() == std::string("kit:") + name && app.scene().nodes.size() > 30,
              "The application shows a designed screen by name");
        std::printf("kit:%s %zu nodes, %zu hits\n", name, scene.nodes.size() - 1,
                    scene.hits.size());
    }
    Scene unknown;
    check(!kit::designGallery(unknown, "nothing") && unknown.nodes.empty(),
          "An unknown screen name draws nothing");
}

void realGlyphs() {
    // The same components over the Figtree atlas: text becomes glyph runs, icons atlas masks,
    // and measured tab widths land within a pixel or two of the prototype's DOM.
    std::string assets = NATIVETV_ASSETS;
    GlyphAtlas atlas(nullptr, assets + "/fonts");
    check(atlas.ok(), "The Figtree faces load");
    TextRuns runs(atlas);
    IconAtlas icons(nullptr, assets + "/icons");
    kit::RunGlyphs glyphs(runs, atlas, icons);
    namespace home = kit::metrics::home;
    std::vector<Rect> tabs = kit::tabRects(glyphs, {"Home", "Apps", "Live"});
    check(sameRect(tabs[0], home::tabHome, 2) && sameRect(tabs[1], home::tabApps, 3) &&
              sameRect(tabs[2], home::tabLive, 4),
          "Figtree tab widths match the prototype's measured tabs");

    runs.beginFrame();
    Scene scene;
    kit::Paint paint{scene, glyphs};
    kit::PosterCard poster;
    poster.rest = kit::geometry::card(0, 0);
    poster.id = "card:0";
    poster.title = "Glass Season";
    kit::card(paint, poster);
    kit::AppCircle circle;
    circle.rest = kit::geometry::appCircle(1, 0);
    circle.icon = "play";
    kit::appCircle(paint, circle);
    check(countKind(scene, Node::Kind::Run) == 1 && countKind(scene, Node::Kind::Mask) == 1 &&
              countKind(scene, Node::Kind::Text) == 0,
          "Text and icons become glyph runs and atlas masks");
    const Node *run = nullptr;
    for (const auto &node : scene.nodes)
        if (node.kind == Node::Kind::Run)
            run = &node;
    check(run && run->run != 0 && sameRect(run->rect, {93, 828, 198, 26}),
          "A run starts at the text box's top-left");
    float width = glyphs.measure("Glass Season", kit::type::cardTitle);
    check(width > 80 && width < 190, "The poster title measures as a line of 22 px text");
}

void keys() {
    Fixture plain;
    kit::KeyCap key;
    key.rest = kit::geometry::keyRow(944, 340, 0, {"1"})[0];
    key.id = "key:0:0";
    key.label = "1";
    kit::keyCap(plain.paint(), key);
    check(sameRect(plain.scene.nodes[0].rect, kit::metrics::setup::key1) &&
              plain.scene.nodes[0].radius == 18 &&
              sameColor(plain.scene.nodes[0].color, kit::color::surface2) &&
              plain.scene.nodes[0].group.identity(),
          "A key is 80x74 with an 18 px radius on the raised surface");
    Fixture lit;
    key.focus = 1;
    kit::keyCap(lit.paint(), key);
    check(near(lit.scene.nodes[0].group.sx, 1.1f, 1e-5f) &&
              sameColor(lit.scene.nodes[1].color, kit::color::focusFill) &&
              sameColor(textAt(lit.scene, "1")->color, kit::color::onFocus),
          "A focused key grows 10% and turns light");
    Fixture pressed;
    key.press = 1;
    kit::keyCap(pressed.paint(), key);
    check(near(pressed.scene.nodes[0].group.sx, .94f, 1e-5f), "A pressed key dips to .94");
    Fixture done;
    kit::KeyCap finish;
    finish.rest = kit::metrics::setup::doneKey;
    finish.id = "key:4:1";
    finish.label = "Done";
    finish.icon = "check";
    finish.accent = true;
    kit::keyCap(done.paint(), finish);
    check(sameColor(done.scene.nodes[0].color, kit::color::accent) &&
              sameColor(textAt(done.scene, "Done")->color, kit::color::onAccent) &&
              countKind(done.scene, Node::Kind::Image) == 1,
          "The Done key is accent-filled with a check and its label");
}
} // namespace

int main() {
    try {
        tokens();
        motionTokens();
        focusMath();
        geometryAgainstMetrics();
        scrolling();
        cards();
        circlesAndTiles();
        barAndHero();
        rows();
        statsAndSettings();
        keys();
        galleries();
        realGlyphs();

        Scene buttonPreview;
        kit::gallery(buttonPreview, "button");
        check(textNode(buttonPreview, "Default") && textNode(buttonPreview, "Focused") &&
                  textNode(buttonPreview, "Disabled") && hasHit(buttonPreview, "button:default") &&
                  hasHit(buttonPreview, "button:focused") &&
                  !hasHit(buttonPreview, "button:disabled"),
              "The kit button preview renders default, focused and disabled states");
        Scene drawerPreview;
        kit::gallery(drawerPreview, "drawer");
        check(textNode(drawerPreview, "Selected") && textNode(drawerPreview, "Focused") &&
                  textNode(drawerPreview, "Disabled") && hasHit(drawerPreview, "menu:0") &&
                  hasHit(drawerPreview, "menu:1") && hasHit(drawerPreview, "menu:2") &&
                  !hasHit(drawerPreview, "menu:3"),
              "The kit drawer preview demonstrates row states and disables pointer input");

        NoopServices services;
        Application buttonApp(services, "kit:button");
        buttonApp.click(1080, 260);
        check(buttonApp.screenName() == "kit:button",
              "The application controller ignores the disabled button preview");
        Application drawerApp(services, "kit:drawer");
        drawerApp.click(1552, 480);
        check(drawerApp.screenName() == "kit:drawer",
              "The application controller ignores the disabled drawer-row preview");

        std::puts("Native kit checks passed");
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
