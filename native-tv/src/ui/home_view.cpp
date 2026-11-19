// Home and Apps drawn on the kit from the controller's state and the motion engine's values.
#include "ui/application.hpp"
#include "ui/home_layout.hpp"
#include "ui/kit/entry.hpp"
#include "ui/kit/overlay.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace nativetv {
namespace {
namespace layout = kit::layout;

// A row's items enter one after another; a page leaving takes them all with it.
kit::Enter enterOf(const Motion &motion, const std::string &id) {
    double hidden = std::max(motion.value(id), motion.value("pgx"));
    return {static_cast<float>(1 - hidden), static_cast<float>(28 * hidden)};
}
// Hero text: positive is rising in from below (18 px), negative is leaving upward (8 px).
kit::Enter textEnter(const Motion &motion, int index) {
    double value = motion.value(home::id("hx", index)), leaving = motion.value("pgx");
    double alpha = (value >= 0 ? 1 - value : 1 + value) * (1 - leaving);
    return {static_cast<float>(std::max(0.0, alpha)),
            static_cast<float>(value >= 0 ? 18 * value : 8 * value)};
}
Color appColor(const Card &card) {
    if (card.id.compare(0, 5, "tool:") == 0) // Search and Settings on a dock: not an app's colour
        return kit::rgb(0x34343C);
    std::string value = card.data["color"].string();
    if (!value.empty() && value[0] == '#')
        value.erase(0, 1);
    if (value.size() == 6 && value.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos)
        return kit::rgb(static_cast<unsigned>(std::strtoul(value.c_str(), nullptr, 16)));
    // No colour from the TV: a steady one per app, dark enough for white text.
    return home::hsl(static_cast<float>(home::hashOf(card.id) % 360), .5f, .36f);
}
std::string clockText(const ViewState &state) {
    char text[16];
    if (state.catalogue["layout"]["clockFormat"].string() == "12h")
        std::snprintf(text, sizeof(text), "%d:%02d", state.hour % 12 ? state.hour % 12 : 12,
                      state.minute);
    else
        std::snprintf(text, sizeof(text), "%02d:%02d", state.hour, state.minute);
    return text;
}
std::string initialOf(const ViewState &state) {
    std::string name;
    for (const auto &person : state.catalogue["profiles"].items())
        if (person["active"].boolean())
            name = person["name"].string();
    if (name.empty())
        return "M";
    size_t end = 1;
    while (end < name.size() && (static_cast<unsigned char>(name[end]) & 0xc0) == 0x80)
        ++end;
    return name.substr(0, end);
}

void wallpaper(Scene &scene, const HomeState &home) {
    const Rect stage = {0, 0, kit::canvas::width, kit::canvas::height};
    if (home.wallpaperTransition) {
        if (!home.wallpaperFrom.empty())
            scene.image(stage, home.wallpaperFrom, 0, false,
                        {1, 1, 1, home.wallpaperFromOpacity * (1 - home.wallpaperProgress)});
        if (!home.wallpaperTarget.empty())
            scene.image(stage, home.wallpaperTarget, 0, false,
                        {1, 1, 1, home.wallpaperTargetOpacity * home.wallpaperProgress});
    } else if (!home.wallpaperShown.empty()) {
        scene.image(stage, home.wallpaperShown, 0, false, {1, 1, 1, home.wallpaperShownOpacity});
        if (!home.wallpaperTarget.empty() && home.wallpaperTarget != home.wallpaperShown)
            scene.image(stage, home.wallpaperTarget, 0, false, {1, 1, 1, 0});
    } else if (!home.wallpaperTarget.empty()) {
        // A zero-alpha node keeps the picture requested without showing an empty frame.
        scene.image(stage, home.wallpaperTarget, 0, false, {1, 1, 1, 0});
    }
}

kit::Hero heroOf(const Frame &frame) {
    const HomeState &home = frame.state.home;
    const Motion &motion = frame.motion;
    kit::Hero hero;
    for (int i = 0; i < 2; ++i) {
        // The newest layer is last, so it covers the one it replaces.
        int layer = i == 0 ? 1 - home.art.newest : home.art.newest;
        hero.slot[i].image = home.art.image[layer];
        hero.slot[i].opacity = static_cast<float>(motion.value(home::id("ha", layer)));
        hero.slot[i].kenBurns = static_cast<float>(motion.value(home::id("hk", layer)));
        hero.slot[i].artAlpha = home.art.artAlpha[layer];
        hero.slot[i].tint = home.art.tint[layer];
    }
    hero.wallAlpha = static_cast<float>(motion.value("wall"));
    hero.stackY = static_cast<float>(motion.value("sy"));
    hero.dock = !home.tabs.empty() && home::onDock(home.tabs[home.tab].key);
    hero.source = home.art.text.source;
    hero.title = home.art.text.title;
    hero.facts = home.art.text.facts;
    hero.description = home.art.text.description;
    hero.image = home.art.text.image;
    hero.hint = home.art.text.hint;
    for (int i = 0; i < 3; ++i)
        hero.text[i] = textEnter(motion, i);
    return hero;
}

} // namespace

// pageTab: the bar's tab a registered screen belongs to; -1 keeps the pill off every tab. Home
// passes its own selected tab.
void drawTopBar(kit::Paint paint, const Frame &frame, int pageTab) {
    const ViewState &state = frame.state;
    const HomeState &home = state.home;
    const Motion &motion = frame.motion;
    kit::TopBar bar;
    bar.avatarInitial = initialOf(state);
    for (const auto &tab : home.tabs)
        bar.tabs.push_back(tab.title);
    bar.selected = home.navigation && home.nav < home.tabs.size() ? static_cast<int>(home.nav)
                                                                  : pageTab;
    bar.pillAlpha = bar.selected < 0 ? 0.0f : 1.0f;
    bar.barFocused = home.navigation && home.nav < home.tabs.size();
    bar.settingsSelected = pageTab < 0 && !home.navigation;
    bar.pillLeft = static_cast<float>(motion.value("pill:0"));
    bar.pillRight = static_cast<float>(motion.value("pill:1"));
    bar.avatarFocus = static_cast<float>(motion.value("f:av"));
    bar.settingsFocus = static_cast<float>(motion.value("f:gr"));
    bar.clock = clockText(state);
    // A theme with a dock rests Home's bar as a pill and brings the bar in while focus is on it.
    bar.compact = state.screen == Screen::Home && pageTab >= 0 && pageTab == static_cast<int>(home.tab) &&
                  home.tab < home.tabs.size() && home::onDock(home.tabs[home.tab].key);
    bar.reveal = bar.compact ? static_cast<float>(motion.value("bar")) : 1.0f;
    // The pill goes with the page: it is not over the grid once the dock has risen to its top.
    if (bar.compact)
        bar.enter.alpha *= kit::focus::clamp01(1 + static_cast<float>(motion.value("sy")) / 150);
    kit::topBar(paint, bar);
    if (state.titleSearch() && bar.reveal > .002f) {
        paint.scene.pushOpacity(bar.reveal);
        kit::RoundButton search;
        Rect gear = kit::geometry::settingsButton();
        search.rest = {gear.x - kit::look::barButton - 24, gear.y, kit::look::barButton, kit::look::barButton};
        search.id = "tool:2";
        search.icon = "search";
        search.focus = static_cast<float>(motion.value("f:sr"));
        kit::roundButton(paint, search);
        paint.scene.popOpacity();
    }
    if (state.catalogue["layout"]["picture"].boolean() && !home.editing)
        paint.glyphs.text(paint.scene, {800, 136, 1040, kit::type::labelM.line},
                          pictureLabel(home.picture), kit::type::labelM, kit::color::onSurface3,
                          Node::Align::Right);
}

namespace {
// The rows of cards; `top` moves them all, so the Apps page can start its rows under its head.
void rows(kit::Paint paint, const Frame &frame, float top = 0) {
    const ViewState &state = frame.state;
    const HomeState &home = state.home;
    const Motion &motion = frame.motion;
    const float stack = static_cast<float>(motion.value("sy")) + top;
    // Every circle carries its name on the Apps page, and on Home while App names is on.
    const bool namedApps = home::namesShown(home.tabs[home.tab].key, state.catalogue["layout"]);
    // A theme with a dock lays Home out on it (home_layout.hpp): its tiles name themselves on focus,
    // and the first row, which rests on the glass capsule, never does.
    const bool dock = home::onDock(home.tabs[home.tab].key);
    for (size_t ri = 0; ri < home.rows.size(); ++ri) {
        const CardRow &row = home.rows[ri];
        float labelY = (dock ? kit::geometry::dockLabelY(static_cast<int>(ri))
                             : kit::geometry::rowLabelY(static_cast<int>(ri))) + stack;
        const bool dockRow = dock && ri == 0 && home::isApps(row) && !home.editing;
        // Rows well above the bar or below the screen are not drawn at all.
        if (labelY > kit::canvas::height + 40 || labelY + 260 < 100)
            continue;
        if (home.arranging && ri == home.row) {
            // The lifted row sits on a raised slab while it is being moved. The slab is as wide as
            // the row, with room all round for the focused name, which is wider than its circle.
            constexpr float kPad = 40;
            const float left = kit::canvas::safeX - kPad;
            float right = kit::canvas::width - kit::canvas::safeX + kPad / 2;
            if (!row.cards.empty()) {
                const Rect last = home::item(row, ri, row.cards.size() - 1, namedApps, dock);
                const float overhang = namedApps ? (kit::look::appGapNamed - 8) / 2 : 0;
                right = std::min(right, last.x + last.w + overhang + kPad +
                                            static_cast<float>(motion.value(home::id("rx", ri))));
                right = std::max(right, left + 800);
            }
            const float height = layout::rowLabelH + layout::rowLabelGap +
                                 (home::isApps(row) ? kit::look::appH + 56 : layout::cardH + 12);
            Node &slab = paint.scene.shape({left, labelY - 12, right - left, height + 36},
                                           kit::color::surface2, kit::radius::lg);
            slab.ring = {1, 0, kit::alpha::outline};
            slab.shadow = {0, kit::elevation::raised.dy, kit::elevation::raised.blur, 0,
                           {0, 0, 0, .5f}};
            // The card's buttons in its top right corner, level with the row's name: Hide (Show,
            // on a hidden app) for the focused app, Move kind, which picks the card up, and Done.
            // Up from the apps reaches them; the pointer clicks them.
            const bool focusedHidden = home.column < row.cards.size() && row.cards[home.column].hidden;
            kit::PillButton done, move, hide;
            done.rest = {right - 20 - 150, labelY - 4, 150, 48};
            done.id = "arrange:done";
            done.label = "Done";
            done.icon = "check";
            done.focus = static_cast<float>(motion.value("f:ad"));
            move.rest = {done.rest.x - 16 - 196, labelY - 4, 196, 48};
            move.id = "arrange:move";
            move.label = home.carryingKind ? "Put down" : "Move kind";
            move.icon = "arrowup";
            move.focus = static_cast<float>(motion.value("f:am"));
            hide.rest = {move.rest.x - 16 - 140, labelY - 4, 140, 48};
            hide.id = "arrange:hide";
            hide.label = focusedHidden ? "Show" : "Hide";
            hide.icon = focusedHidden ? "eye" : "eyeoff";
            hide.focus = static_cast<float>(motion.value("f:ah"));
            kit::pillButton(paint, hide);
            kit::pillButton(paint, move);
            kit::pillButton(paint, done);
        }
        if (dockRow) {
            paint.scene.pushTransform(Transform::translate(0, stack));
            kit::dockCapsule(paint, enterOf(motion, home::id("ER", ri)));
            paint.scene.popTransform();
        }
        kit::Row label;
        label.index = static_cast<int>(ri);
        label.label = home.editing && ri == home.row && home::isApps(row) && home.column < row.cards.size()
                          ? "Moving " + row.cards[home.column].title
                          : row.title;
        if (home.arranging && home.carrying && ri == home.row && home.column < row.cards.size())
            label.label = "Moving " + row.cards[home.column].title;
        label.dim = static_cast<float>(motion.value(home::id("rd", ri)));
        label.enter = enterOf(motion, home::id("ER", ri));
        // The next row's name is not shown cut by the screen's edge: it fades in as it rises.
        label.enter.alpha *=
            kit::focus::clamp01((kit::canvas::height - 40 - labelY) / 48);
        // The dock's rows of apps carry no title; the feed's rows keep theirs.
        if (!(dock && home::isApps(row) && !home.editing)) {
            // The label sits where the row's own pitch puts it, not where a Google TV row's would.
            const float shift = dock ? kit::geometry::dockLabelY(static_cast<int>(ri)) -
                                           kit::geometry::rowLabelY(static_cast<int>(ri))
                                     : 0;
            paint.scene.pushTransform(Transform::translate(0, stack + shift));
            kit::row(paint, label);
            paint.scene.popTransform();
        }
        const float scroll = static_cast<float>(motion.value(home::id("rx", ri)));
        for (size_t ci = 0; ci < row.cards.size(); ++ci) {
            Rect rest = home::item(row, ri, ci, namedApps, dock);
            rest.x += scroll;
            rest.y += stack;
            if (rest.x + rest.w < -140 || rest.x > kit::canvas::width + 140)
                continue;
            const Card &card = row.cards[ci];
            std::string id = home::hit(ri, ci);
            float focus = static_cast<float>(motion.value(home::id("f", ri, ci)));
            kit::Enter enter = enterOf(motion, home::id("EC", ri, ci));
            if (row.key == "app") {
                kit::AppCircle circle;
                circle.rest = rest;
                circle.id = id;
                circle.name = dockRow ? "" : namedApps || dock ? card.title : "";
                circle.icon = card.id == "lg-home"         ? "home"
                              : card.id == "tool:search"   ? "search"
                              : card.id == "tool:settings" ? "settings"
                                                           : "apps";
                circle.image = card.image;
                circle.color = appColor(card);
                circle.named = namedApps && !dock;
                if (namedApps && !dock)
                    circle.nameWidth = kit::look::appW + kit::look::appGapNamed - 8;
                circle.focus = focus;
                circle.enter = enter;
                if (card.hidden) // hidden from Home: listed only while the apps are edited
                    circle.enter.alpha *= .4f;
                if (home.arranging && ri != home.row) // the rows behind the lifted one
                    circle.enter.alpha *= .55f;
                kit::appCircle(paint, circle);
            } else if (home::isPanel(row)) {
                kit::InfoCard info;
                info.rest = rest;
                info.id = id;
                info.title = card.title;
                info.value = card.data["value"].string();
                info.unit = card.data["unit"].string();
                info.caption = card.subtitle;
                info.bar = static_cast<float>(card.data["bar"].number(-1));
                info.focus = focus;
                info.enter = enter;
                kit::infoCard(paint, info);
            } else if (row.key == "dev" || row.key == "ch") {
                kit::InputTile tile;
                tile.rest = rest;
                tile.id = id;
                bool device = row.key == "dev";
                bool unused = device && card.data["unused"].boolean();
                tile.name = device ? "HDMI " + card.data["port"].string("") : card.title;
                if (device && card.data["port"].string("").empty())
                    tile.name = card.title;
                tile.sub = device ? (unused ? "Not connected" : card.title) : card.subtitle;
                if (tile.sub == tile.name)
                    tile.sub.clear();
                tile.icon = device ? "hdmi" : "tv";
                tile.unused = unused;
                tile.focus = focus;
                tile.enter = enter;
                kit::inputTile(paint, tile);
            } else {
                kit::PosterCard poster;
                poster.rest = rest;
                poster.id = id;
                poster.image = card.image;
                poster.title = card.title;
                poster.subtitle = card.subtitle;
                poster.focus = focus;
                poster.enter = enter;
                kit::card(paint, poster);
            }
        }
    }
}

// The top of the hint keys at the foot of the page.
constexpr float kHintTop = 998;
// The hint keys on a band of ink, so a row that reaches the bottom passes under them.
void hintBand(kit::Paint paint, const std::vector<kit::Hint> &hints) {
    Node &band = paint.scene.shape({0, 900, kit::canvas::width, 180}, kit::color::ink);
    band.gradient.angle = 0;
    band.gradient.stops = 3;
    band.gradient.color[0] = kit::color::ink;
    band.gradient.at[0] = 0;
    band.gradient.color[1] = kit::color::ink;
    band.gradient.at[1] = .5f;
    band.gradient.color[2] = kit::focus::withAlpha(kit::color::ink, 0);
    band.gradient.at[2] = 1;
    kit::hintBar(paint, {kit::canvas::safeX, kHintTop, 0, 30}, hints);
}

// Where the focused item rests on screen, as rows() places it, before its focus scale.
Rect focusedItem(const Frame &frame) {
    const HomeState &home = frame.state.home;
    const std::string &tab = home.tabs[home.tab].key;
    const bool dock = home::onDock(tab);
    Rect rest = home::item(home.rows[home.row], home.row, home.column,
                           home::namesShown(tab, frame.state.catalogue["layout"]), dock);
    rest.x += static_cast<float>(frame.motion.value(home::id("rx", home.row)));
    rest.y += static_cast<float>(frame.motion.value("sy")) +
              (tab == "apps" ? layout::appsTop - kit::look::homeTop : 0);
    return rest;
}

// The focused app's menu (a long press on it): below the app, clear of its name, where it fits
// above the hint keys; above the app otherwise.
void appMenu(kit::Paint paint, const Frame &frame) {
    const HomeState &home = frame.state.home;
    if (!home.appMenu || home.row >= home.rows.size() ||
        home.column >= home.rows[home.row].cards.size())
        return;
    const bool onApps = home.tabs[home.tab].key == "apps";
    std::vector<kit::MenuItem> items;
    for (size_t i = 0; i < home.appMenuItems.size(); ++i) {
        const std::string &key = home.appMenuItems[i];
        kit::MenuItem item;
        item.id = "appmenu:" + std::to_string(i);
        item.icon = key == "move" ? "arrowright" : key == "hide" ? "eyeoff" : "eye";
        item.label = key == "move"   ? "Move"
                     : key == "hide" ? (onApps ? "Hide from Apps" : "Hide from Home")
                                     : "Hidden apps";
        item.focus = static_cast<float>(frame.motion.value(home::id("f:mi", i)));
        items.push_back(item);
    }
    const Rect size = kit::menuSize(paint, items);
    const Rect app = focusedItem(frame);
    // The focused circle and the name under it, at their focus scale.
    const float middle = app.y + app.h / 2, scale = kit::motion::scale::app;
    const float bottom =
        middle + (app.h / 2 + kit::layout::appNameGap + kit::type::labelM.line) * scale;
    const float top = middle - app.h / 2 * scale;
    kit::PopupMenu menu;
    menu.items = items;
    menu.appear = static_cast<float>(frame.motion.value("appmenu"));
    menu.y = bottom + kit::space::x16;
    if (menu.y + size.h > kHintTop - kit::space::x16) {
        menu.y = top - kit::space::x16 - size.h;
        menu.above = true;
    }
    menu.x = std::max(kit::canvas::safeX,
                      std::min(app.x + app.w / 2 - size.w / 2,
                               kit::canvas::width - kit::canvas::safeX - size.w));
    kit::popupMenu(paint, menu);
}

// The Apps page with its apps grouped by kind: rows as on Home from under the bar, which pass
// under it once they scroll.
void appRows(kit::Paint paint, const Frame &frame) {
    const HomeState &home = frame.state.home;
    const Motion &motion = frame.motion;
    bool clipped = motion.value("sy") < -.5;
    if (clipped)
        paint.scene.pushClip({0, layout::appsTop - 16, kit::canvas::width, kit::canvas::height});
    rows(paint, frame, layout::appsTop - kit::look::homeTop);
    if (clipped)
        paint.scene.popClip();
    if (home.arranging) {
        const bool hidden = home.row < home.rows.size() && home.column < home.rows[home.row].cards.size() &&
                            home.rows[home.row].cards[home.column].hidden;
        if (home.carryingKind)
            hintBand(paint, {{"Up/Down", "Move kind"}, {"OK", "Put down"}});
        else if (home.carrying)
            hintBand(paint, {{"Left/Right", "Move app"}, {"OK", "Put down"}});
        else if (home.arrangeFocus)
            hintBand(paint, {{"Left/Right", "Choose"},
                             {"OK", home.arrangeFocus == 1   ? (hidden ? "Show app" : "Hide app")
                                    : home.arrangeFocus == 2 ? "Move kind"
                                                             : "Done"},
                             {"Down", "Back to the apps"},
                             {"Back", "Done"}});
        else
            hintBand(paint, {{"Left/Right", "Select"},
                             {"OK", "Pick up"},
                             {"Up", "Hide, move kind, done"},
                             {"Back", "Done"}});
    }
}

void banner(kit::Paint paint, float amount) {
    Scene &scene = paint.scene;
    scene.shape({0, 0, kit::canvas::width, kit::canvas::height},
                kit::focus::withAlpha(kit::alpha::scrimStrong, amount));
    scene.pushOpacity(amount);
    paint.glyphs.text(scene, {400, 430, 1120, kit::type::headline.line}, "Native Tv is not answering",
                      kit::type::headline, kit::color::onSurface, Node::Align::Center);
    paint.glyphs.text(scene, {560, 500, 800, kit::type::body.line * 2},
                      "Home comes back when Native Tv answers again.", kit::type::body,
                      kit::color::onSurface2, Node::Align::Center, 2);
    scene.popOpacity();
}
} // namespace

void drawHome(kit::Paint paint, const Frame &frame) {
    const ViewState &state = frame.state;
    const HomeState &home = state.home;
    const Motion &motion = frame.motion;
    Scene &scene = paint.scene;
    scene.background = kit::color::ink;
    if (home.tabs.empty())
        return;
    const bool apps = home.tabs[home.tab].key == "apps";
    wallpaper(scene, home);
    float wall = static_cast<float>(motion.value("wall"));
    // The Apps page, and a page with nothing to show, wash in the neutral colour: the tint is the last
    // hero's, which they would otherwise keep.
    kit::ambientWall(paint, 1 - wall, apps || home.rows.empty() ? neutralWash() : home.art.wallTint);
    if (apps) {
        kit::Hero none = heroOf(frame);
        none.wallAlpha = wall;
        kit::heroWall(paint, none);
        appRows(paint, frame);
    } else if (!kit::look::discover) {
        // No feed, so no hero and no dots: the rows start near the top.
        rows(paint, frame);
        if (home.rows.empty())
            paint.glyphs.text(scene, {kit::canvas::safeX, 400, 1600, kit::type::body.line},
                              !state.catalogue.valid() ? "Loading Home…" : "No items to show. Open Settings to arrange Home.",
                              kit::type::body, kit::color::onSurface2);
    } else {
        kit::Hero hero = heroOf(frame);
        kit::heroWall(paint, hero);
        // The hero's lines and dots belong to the rows under them: with none, the last ones would stay.
        if (!home.rows.empty())
            kit::heroText(paint, hero);
        if (home.spotlight.size() > 1 && !home.rows.empty()) {
            kit::Pagination dots;
            dots.count = static_cast<int>(home.spotlight.size());
            for (int i = 0; i < dots.count; ++i)
                dots.current.push_back(static_cast<float>(motion.value(home::id("dot", i))));
            dots.progress = static_cast<float>(motion.value("hp"));
            kit::geometry::HeroParallax parallax = kit::geometry::heroParallax(hero.stackY);
            dots.alpha = hero.text[2].alpha * parallax.textAlpha *
                         static_cast<float>(motion.value("dots"));
            dots.y = parallax.textY;
            dots.dock = hero.dock;
            kit::pagination(paint, dots);
        }
        rows(paint, frame);
        if (home.rows.empty()) {
            const bool live = home.tabs[home.tab].key == "live";
            paint.glyphs.text(scene, {kit::canvas::safeX, 800, 1600, kit::type::body.line},
                              !state.catalogue.valid() ? "Loading Home…"
                              : live ? "No channels added yet. Add some on the dashboard's Native Tv Home tab."
                                     : "No items to show. Open Settings to arrange Home.",
                              kit::type::body, kit::color::onSurface2);
        }
    }
    if (home.appMenu)
        hintBand(paint, {{"Up/Down", "Choose"}, {"OK", "Select"}, {"Back", "Close"}});
    else if (home.editing)
        hintBand(paint, {{"Left/Right", "Move"}, {"OK", "Done"}});
    drawTopBar(paint, frame, static_cast<int>(home.tab));
    appMenu(paint, frame);
    float fault = static_cast<float>(motion.value("fault"));
    if (fault > .002f)
        banner(paint, fault);
}
} // namespace nativetv
