// Home and Apps drawn on the kit from the controller's state and the motion engine's values.
#include "ui/application.hpp"
#include "ui/home_layout.hpp"
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
        hero.slot[i].tint = home.art.tint[layer];
    }
    hero.wallAlpha = static_cast<float>(motion.value("wall"));
    hero.stackY = static_cast<float>(motion.value("sy"));
    hero.source = home.art.text.source;
    hero.title = home.art.text.title;
    hero.tag = home.art.text.tag;
    hero.description = home.art.text.description;
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
    if (state.catalogue["layout"]["picture"].boolean())
        paint.glyphs.text(paint.scene, {800, 136, 1040, kit::type::labelM.line},
                          pictureLabel(home.picture), kit::type::labelM, kit::color::onSurface3,
                          Node::Align::Right);
}

namespace {
void rows(kit::Paint paint, const Frame &frame) {
    const ViewState &state = frame.state;
    const HomeState &home = state.home;
    const Motion &motion = frame.motion;
    const float stack = static_cast<float>(motion.value("sy"));
    const bool appNames = state.catalogue["layout"]["appNames"].boolean(true);
    for (size_t ri = 0; ri < home.rows.size(); ++ri) {
        const CardRow &row = home.rows[ri];
        float labelY = kit::geometry::rowLabelY(static_cast<int>(ri)) + stack;
        // Rows well above the bar or below the screen are not drawn at all.
        if (labelY > kit::canvas::height + 40 || labelY + 260 < 100)
            continue;
        kit::Row label;
        label.index = static_cast<int>(ri);
        label.label = row.title;
        label.dim = static_cast<float>(motion.value(home::id("rd", ri)));
        label.enter = enterOf(motion, home::id("ER", ri));
        paint.scene.pushTransform(Transform::translate(0, stack));
        kit::row(paint, label);
        paint.scene.popTransform();
        const float scroll = static_cast<float>(motion.value(home::id("rx", ri)));
        for (size_t ci = 0; ci < row.cards.size(); ++ci) {
            Rect rest = home::item(row, ri, ci);
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
                circle.name = appNames ? card.title : "";
                circle.icon = card.id == "lg-home" ? "home" : "apps";
                circle.image = card.image;
                circle.color = appColor(card);
                circle.focus = focus;
                circle.enter = enter;
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
                poster.focus = focus;
                poster.enter = enter;
                kit::card(paint, poster);
            }
        }
    }
}

// The app's own artwork on its tile, scaled and faded with the tile it sits on.
void tileArt(kit::Paint paint, const kit::AppTile &tile, const std::string &image) {
    kit::focus::Look look = kit::focus::lookAt(tile.rest, kit::motion::scale::tile, tile.focus);
    Scene &scene = paint.scene;
    scene.pushOpacity(tile.enter.alpha);
    scene.pushTransform(Transform::translate(0, tile.enter.dy));
    scene.pushTransform(kit::focus::transformAt(tile.rest, look.scale));
    scene.picture({tile.rest.x + layout::tilePad, tile.rest.y + layout::tilePad, 44, 44}, 0, 10)
        .value = image;
    scene.popTransform();
    scene.popTransform();
    scene.popOpacity();
}

void grid(kit::Paint paint, const Frame &frame) {
    const HomeState &home = frame.state.home;
    const Motion &motion = frame.motion;
    if (home.rows.empty())
        return;
    const CardRow &row = home.rows[0];
    kit::PageHead head;
    head.title = "Apps";
    head.subtitle = std::to_string(row.cards.size()) + " installed";
    head.enter = enterOf(motion, "EH");
    kit::pageHead(paint, head);
    const float scroll = static_cast<float>(motion.value("gy"));
    // Rows that scroll away disappear under the page title rather than over it.
    bool clipped = scroll < -.5f;
    if (clipped)
        paint.scene.pushClip({0, kit::layout::gridTop - 28, kit::canvas::width, kit::canvas::height});
    for (size_t i = 0; i < row.cards.size(); ++i) {
        Rect rest = kit::geometry::appTile(static_cast<int>(i));
        rest.y += scroll;
        if (rest.y > kit::canvas::height + 40 || rest.y + rest.h < kit::layout::gridTop - 60)
            continue;
        const Card &card = row.cards[i];
        kit::AppTile tile;
        tile.rest = rest;
        tile.id = home::hit(0, i);
        tile.name = card.title;
        tile.icon = card.image.empty() ? (card.id == "lg-home" ? "home" : "apps") : "";
        tile.color = appColor(card);
        tile.focus = static_cast<float>(motion.value(home::id("f", 0, i)));
        tile.enter = enterOf(motion, home::id("EC", 0, i));
        kit::appTile(paint, tile);
        if (!card.image.empty())
            tileArt(paint, tile, card.image);
    }
    if (clipped)
        paint.scene.popClip();
}

void banner(kit::Paint paint, float amount) {
    Scene &scene = paint.scene;
    scene.shape({0, 0, kit::canvas::width, kit::canvas::height},
                kit::focus::withAlpha(kit::alpha::scrimStrong, amount));
    scene.pushOpacity(amount);
    paint.glyphs.text(scene, {400, 430, 1120, kit::type::headline.line}, "nativeTv is not answering",
                      kit::type::headline, kit::color::onSurface, Node::Align::Center);
    paint.glyphs.text(scene, {560, 500, 800, kit::type::body.line * 2},
                      "Home comes back when nativeTv answers again.", kit::type::body,
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
        grid(paint, frame);
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
            dots.alpha = hero.text[2].alpha * parallax.textAlpha;
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
    drawTopBar(paint, frame, static_cast<int>(home.tab));
    float dim = static_cast<float>(motion.value("dim"));
    if (dim > .002f)
        scene.shape({0, 0, kit::canvas::width, kit::canvas::height}, {0, 0, 0, dim});
    float fault = static_cast<float>(motion.value("fault"));
    if (fault > .002f)
        banner(paint, fault);
}
} // namespace nativetv
