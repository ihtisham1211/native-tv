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
    bar.settingsSelected = pageTab < 0 && !home.navigation;
    bar.pillLeft = static_cast<float>(motion.value("pill:0"));
    bar.pillRight = static_cast<float>(motion.value("pill:1"));
    bar.pillHot = static_cast<float>(motion.value("pill:hot"));
    bar.avatarFocus = static_cast<float>(motion.value("f:av"));
    bar.settingsFocus = static_cast<float>(motion.value("f:gr"));
    bar.clock = clockText(state);
    kit::topBar(paint, bar);
    if (state.discover["titlesEnabled"].boolean()) {
        kit::RoundButton search;
        Rect gear = kit::geometry::settingsButton();
        search.rest = {gear.x - layout::circle - 24, gear.y, layout::circle, layout::circle};
        search.id = "tool:2";
        search.icon = "search";
        search.focus = static_cast<float>(motion.value("f:sr"));
        kit::roundButton(paint, search);
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
    const bool appNames = state.catalogue["layout"]["appNames"].boolean(true);
    // On the Apps page every circle carries its name; on Home only the focused one does.
    const bool namedApps = home.tabs[home.tab].key == "apps";
    for (size_t ri = 0; ri < home.rows.size(); ++ri) {
        const CardRow &row = home.rows[ri];
        float labelY = kit::geometry::rowLabelY(static_cast<int>(ri)) + stack;
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
                const Rect last = home::item(row, ri, row.cards.size() - 1, namedApps);
                const float overhang = namedApps ? (layout::appGapNamed - 8) / 2 : 0;
                right = std::min(right, last.x + last.w + overhang + kPad +
                                            static_cast<float>(motion.value(home::id("rx", ri))));
                right = std::max(right, left + 480);
            }
            const float height = layout::rowLabelH + layout::rowLabelGap +
                                 (home::isApps(row) ? layout::appD + 56 : layout::cardH + 12);
            Node &slab = paint.scene.shape({left, labelY - 12, right - left, height + 36},
                                           kit::color::surface2, kit::radius::lg);
            slab.ring = {1, 0, kit::alpha::outline};
            slab.shadow = {0, kit::elevation::raised.dy, kit::elevation::raised.blur, 0,
                           {0, 0, 0, .5f}};
        }
        kit::Row label;
        label.index = static_cast<int>(ri);
        label.label = home.editing && ri == home.row && home::isApps(row) ? "Moving apps" : row.title;
        label.dim = static_cast<float>(motion.value(home::id("rd", ri)));
        label.enter = enterOf(motion, home::id("ER", ri));
        // The next row's name is not shown cut by the screen's edge: it fades in as it rises.
        label.enter.alpha *=
            kit::focus::clamp01((kit::canvas::height - 40 - labelY) / 48);
        paint.scene.pushTransform(Transform::translate(0, stack));
        kit::row(paint, label);
        paint.scene.popTransform();
        const float scroll = static_cast<float>(motion.value(home::id("rx", ri)));
        for (size_t ci = 0; ci < row.cards.size(); ++ci) {
            Rect rest = home::item(row, ri, ci, namedApps);
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
                circle.name = appNames || namedApps ? card.title : "";
                circle.icon = card.id == "lg-home" ? "home" : "apps";
                circle.image = card.image;
                circle.color = appColor(card);
                circle.named = namedApps;
                if (namedApps)
                    circle.nameWidth = layout::appD + layout::appGapNamed - 8;
                circle.focus = focus;
                circle.enter = enter;
                if (card.hidden) // hidden from Home: listed only while the apps are edited
                    circle.enter.alpha *= .4f;
                if (home.arranging && ri != home.row) // the rows behind the lifted one
                    circle.enter.alpha *= .55f;
                kit::appCircle(paint, circle);
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

// While the apps are edited the Apps page makes room, above its first row, for the button that
// hides the focused app.
constexpr float kEditLift = 64;
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
    kit::hintBar(paint, {kit::canvas::safeX, 998, 0, 30}, hints);
}

// The Apps page: the same circles as Home's apps row, larger, with their names, six to a row.
// It has no head: the first row starts under the bar.
void grid(kit::Paint paint, const Frame &frame) {
    const HomeState &home = frame.state.home;
    const Motion &motion = frame.motion;
    if (home.rows.empty())
        return;
    const CardRow &row = home.rows[0];
    const float lift = kEditLift * static_cast<float>(motion.value("lift"));
    const float scroll = static_cast<float>(motion.value("gy")) + lift;
    // Rows that scroll away disappear under the bar rather than over it.
    const float clipTop = kit::layout::appsTop + lift - 16;
    bool clipped = scroll - lift < -.5f;
    if (clipped)
        paint.scene.pushClip({0, clipTop, kit::canvas::width, kit::canvas::height});
    for (size_t i = 0; i < row.cards.size(); ++i) {
        Rect rest = kit::geometry::appTile(static_cast<int>(i));
        rest.y += scroll;
        if (rest.y > kit::canvas::height + 40 || rest.y + rest.h < clipTop - 44)
            continue;
        const Card &card = row.cards[i];
        kit::AppCircle circle;
        circle.rest = rest;
        circle.id = home::hit(0, i);
        circle.name = card.title;
        circle.icon = card.id == "lg-home" ? "home" : "apps";
        circle.image = card.image;
        circle.color = appColor(card);
        circle.named = true;
        circle.nameWidth = layout::tilePitchX - 40;
        circle.focus = static_cast<float>(motion.value(home::id("f", 0, i)));
        circle.enter = enterOf(motion, home::id("EC", 0, i));
        if (card.hidden) // hidden from the Apps page: listed only while its apps are edited
            circle.enter.alpha *= .4f;
        kit::appCircle(paint, circle);
    }
    if (clipped)
        paint.scene.popClip();
}

// The Apps page with its apps grouped by kind: rows as on Home from under the bar, which pass
// under it once they scroll.
void appRows(kit::Paint paint, const Frame &frame) {
    const HomeState &home = frame.state.home;
    const Motion &motion = frame.motion;
    const float lift = kEditLift * static_cast<float>(motion.value("lift"));
    bool clipped = motion.value("sy") < -.5;
    if (clipped)
        paint.scene.pushClip({0, layout::appsTop + lift - 16, kit::canvas::width, kit::canvas::height});
    rows(paint, frame, layout::appsTop - layout::rowsTop + lift);
    if (clipped)
        paint.scene.popClip();
    if (home.arranging)
        hintBand(paint, {{"", "Move"}, {"OK", "Save"}, {"Back", "Cancel"}});
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
    kit::ambientWall(paint, 1 - wall, home.art.wallTint);
    if (apps) {
        kit::Hero none = heroOf(frame);
        none.wallAlpha = wall;
        kit::heroWall(paint, none);
        if (home.grid)
            grid(paint, frame);
        else
            appRows(paint, frame);
    } else {
        kit::Hero hero = heroOf(frame);
        kit::heroWall(paint, hero);
        kit::heroText(paint, hero);
        if (home.spotlight.size() > 1) {
            kit::Pagination dots;
            dots.count = static_cast<int>(home.spotlight.size());
            for (int i = 0; i < dots.count; ++i)
                dots.current.push_back(static_cast<float>(motion.value(home::id("dot", i))));
            dots.progress = static_cast<float>(motion.value("hp"));
            kit::geometry::HeroParallax parallax = kit::geometry::heroParallax(hero.stackY);
            dots.alpha = hero.text[2].alpha * parallax.textAlpha *
                         static_cast<float>(motion.value("dots"));
            dots.y = parallax.textY;
            kit::pagination(paint, dots);
        }
        rows(paint, frame);
        if (home.rows.empty())
            paint.glyphs.text(scene, {kit::canvas::safeX, 800, 1600, kit::type::body.line},
                              state.catalogue.valid() ? "No items to show. Open Settings to arrange Home."
                                                      : "Loading Home…",
                              kit::type::body, kit::color::onSurface2);
    }
    if (home.editing && home.row < home.rows.size() && home.column < home.rows[home.row].cards.size()) {
        // Editing the apps: the button above the page hides the focused app, or shows it again when
        // it was one of the hidden ones; Red does the same.
        const bool hidden = home.rows[home.row].cards[home.column].hidden;
        // Home's row and the Apps page each have their own hidden apps.
        const bool onApps = home.tabs[home.tab].key == "apps";
        const std::string label = hidden ? (onApps ? "Show on Apps" : "Show on Home")
                                         : (onApps ? "Hide from Apps" : "Hide from Home");
        hintBand(paint, {{"", "Move"}, {"OK", "Done"}, {"Red", label}});
        kit::PillButton hide;
        hide.rest = {kit::canvas::width - kit::canvas::safeX - 380, 136, 380, 56};
        hide.id = "edit:hide";
        hide.label = label;
        hide.icon = hidden ? "eye" : "eyeoff";
        hide.focus = static_cast<float>(motion.value("f:eh"));
        kit::pillButton(paint, hide);
    }
    drawTopBar(paint, frame, static_cast<int>(home.tab));
    float fault = static_cast<float>(motion.value("fault"));
    if (fault > .002f)
        banner(paint, fault);
}
} // namespace nativetv
