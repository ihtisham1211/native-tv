#include "ui/application.hpp"
#include "ui/controller_helpers.hpp"
#include "ui/kit/components.hpp"
#include "ui/home_layout.hpp"
#include "ui/splash.hpp"
#include <algorithm>
#include <cmath>

namespace nativetv {
namespace {
struct WallpaperChoice {
    std::string asset;
    float opacity = 0;
};
WallpaperChoice wallpaperAt(const Json &catalogue, int hour) {
    static const struct {
        const char *slot;
        int from;
        float opacity;
    } slots[] = {{"night", 0, .55f}, {"morning", 6, .7f}, {"afternoon", 11, .62f},
                 {"evening", 17, .72f}, {"night", 21, .65f}};
    WallpaperChoice choice;
    Json wallpapers = catalogue["wallpapers"];
    if (!wallpapers["available"].boolean())
        return choice;
    int currentHour = hour < 0 ? 0 : hour % 24;
    const auto *slot = &slots[0];
    for (const auto &candidate : slots)
        if (candidate.from <= currentHour)
            slot = &candidate;
    Json available = wallpapers["slots"];
    if (available.isObject() &&
        (!available.has(slot->slot) || !available[slot->slot].boolean()))
        return choice;
    choice.asset = std::string("wallpaper:") + slot->slot;
    std::string revision = wallpapers["revision"].string();
    if (!revision.empty())
        choice.asset += "@" + revision;
    choice.opacity = slot->opacity;
    return choice;
}
std::string wallpaperSlot(const std::string &asset) {
    if (asset.compare(0, 10, "wallpaper:") != 0)
        return "";
    size_t end = asset.find('@', 10);
    return asset.substr(10, end == std::string::npos ? std::string::npos : end - 10);
}
} // namespace
void Application::wallClock(int hour, int minute, int weekday, int day, int month) {
    if (weekday < 0)
        weekday = state_.weekday;
    if (day < 0)
        day = state_.day;
    if (month < 0)
        month = state_.month;
    if (state_.hour != hour || state_.minute != minute || state_.weekday != weekday ||
        state_.day != day || state_.month != month) {
        state_.hour = hour;
        state_.minute = minute;
        state_.weekday = weekday;
        state_.day = day;
        state_.month = month;
        if (active_)
            syncWallpaper();
        dirty_ = true;
    }
}
Application::Application(Services &services, const std::string &initial)
    : services_(services), clock_([this] { return static_cast<double>(now_); }) {
    registerScreens(registry_);
    state_.home.tabs = homeTabs(Json());
    if (initial == "splash") {
        state_.screen = Screen::Splash;
        return;
    }
    services_.read("caps", Operation::Capabilities);
    route(initial);
}
void Application::toast(const std::string &value) {
    state_.toast = value;
    toastUntil_ = now_ + 4000;
    motion_.play("toast", Tween(0, 1, kit::motion::ms::fast, Easing::out()));
    dirty_ = true;
}
void Application::active(bool value) {
    active_ = value;
    if (value) {
        if (state_.screen != Screen::Splash) {
            syncWallpaper();
            refresh();
        }
    } else {
        clearPicture();
    }
    dirty_ = true;
}
bool Application::update(uint32_t now) {
    now_ = now;
    if (!active_)
        return false;
    if (state_.screen == Screen::Splash)
        return true;
    clock_.tick();
    bool moved = motion_.update(clock_);
    updateWallpaperTransition();
    if (static_cast<int32_t>(now_ - refreshAt_) >= 0)
        refresh();
    if (toastUntil_ && static_cast<int32_t>(now_ - toastUntil_) >= 0) {
        toastUntil_ = 0;
        state_.toast.clear();
        dirty_ = true;
    }
    bool screenBusy = false;
    if (state_.screen == Screen::Home || isRecents()) {
        homeUpdate();
        homeBeat(nullptr);
    }
    if (screen_)
        screenBusy = screen_->update(*this);
    // The wallpaper is shown once its picture has arrived.
    std::string pending = wallpaperPending();
    if (!pending.empty() && pictures_ &&
        pictures_->view(pending, 1600, 900, ImageFit::Cover, now_).texture != 0)
        imageReady(pending);
    bool draw = dirty_ || moved || state_.home.wallpaperTransition || screenBusy;
    ambientOnly_ = moved && motion_.ambientOnly() && !dirty_ && !state_.home.wallpaperTransition &&
                   !screenBusy;
    dirty_ = false;
    return draw;
}
void Application::attach(kit::Glyphs &glyphs, Pictures &pictures) {
    glyphs_ = &glyphs;
    pictures_ = &pictures;
    flow_.tabTitles.clear(); // the tabs' widths come from the glyphs that draw them
    dirty_ = true;
}
void Application::syncWallpaper() {
    if (!active_ || !state_.catalogue.valid())
        return;
    WallpaperChoice choice = wallpaperAt(state_.catalogue, state_.hour);
    auto &home = state_.home;
    if (home.wallpaperTarget == choice.asset &&
        std::fabs(home.wallpaperTargetOpacity - choice.opacity) < .001f)
        return;
    if (home.wallpaperTransition) {
        updateWallpaperTransition();
        if (home.wallpaperTransition)
            finishWallpaperTransition(home.wallpaperProgress >= .5f);
    }
    if (!home.wallpaperTarget.empty() && home.wallpaperTarget != choice.asset &&
        home.wallpaperTarget != home.wallpaperShown)
        services_.cancel("image:" + home.wallpaperTarget);
    home.wallpaperTarget = choice.asset;
    home.wallpaperTargetOpacity = choice.opacity;
    home.wallpaperFailed = false;
    if (home.wallpaperTarget.empty()) {
        if (!home.wallpaperShown.empty())
            startWallpaperTransition();
        else {
            home.wallpaperTransition = false;
            home.wallpaperFrom.clear();
            home.wallpaperProgress = 1;
        }
    } else {
        home.wallpaperTransition = false;
        home.wallpaperFrom.clear();
        home.wallpaperProgress = 1;
    }
    dirty_ = true;
}
void Application::startWallpaperTransition() {
    auto &home = state_.home;
    if (home.wallpaperTarget == home.wallpaperShown &&
        std::fabs(home.wallpaperTargetOpacity - home.wallpaperShownOpacity) < .001f)
        return;
    home.wallpaperFrom = home.wallpaperShown;
    home.wallpaperFromOpacity = home.wallpaperShownOpacity;
    home.wallpaperProgress = 0;
    home.wallpaperTransitionAt = now_;
    home.wallpaperTransition = true;
    dirty_ = true;
}
void Application::finishWallpaperTransition(bool chooseTarget) {
    auto &home = state_.home;
    if (chooseTarget) {
        home.wallpaperShown = home.wallpaperTarget;
        home.wallpaperShownOpacity = home.wallpaperTargetOpacity;
    } else {
        home.wallpaperShown = home.wallpaperFrom;
        home.wallpaperShownOpacity = home.wallpaperFromOpacity;
    }
    home.wallpaperFrom.clear();
    home.wallpaperTransition = false;
    home.wallpaperProgress = 1;
    dirty_ = true;
}
void Application::updateWallpaperTransition() {
    auto &home = state_.home;
    if (!home.wallpaperTransition)
        return;
    uint32_t elapsed = now_ - home.wallpaperTransitionAt;
    if (elapsed >= 2400) {
        finishWallpaperTransition(true);
        return;
    }
    // The browser's default ease, as the Home page cross-faded its wallpaper.
    home.wallpaperProgress = static_cast<float>(Easing::ease()(elapsed / 2400.0));
}
void Application::imageReady(const std::string &path) {
    if (path.compare(0, 10, "wallpaper:") != 0)
        return;
    auto &home = state_.home;
    if (path != home.wallpaperTarget)
        return;
    home.wallpaperFailed = false;
    if (active_)
        startWallpaperTransition();
    dirty_ = true;
}
std::string Application::wallpaperPending() const {
    const auto &home = state_.home;
    if (!home.wallpaperTransition && !home.wallpaperTarget.empty() &&
        (home.wallpaperTarget != home.wallpaperShown ||
         std::fabs(home.wallpaperTargetOpacity - home.wallpaperShownOpacity) >= .001f))
        return home.wallpaperTarget;
    return "";
}
void Application::imageFailed(const std::string &path) {
    if (path.compare(0, 10, "wallpaper:") != 0 || path != state_.home.wallpaperTarget)
        return;
    state_.home.wallpaperFailed = true;
    dirty_ = true;
}
bool Application::isRecents() const {
    return state_.screen == Screen::Quick && state_.menu.level == "recents";
}
// Pictures are asked for at the size they are drawn at; a node whose picture has not arrived
// stays invisible (the kit draws a placeholder under it) and fades in when it has.
void Application::resolvePictures() {
    if (!pictures_)
        return;
    const double now = static_cast<double>(now_);
    for (Node &node : scene_.nodes) {
        if (node.kind != Node::Kind::Image || node.texture || node.value.empty() ||
            node.value.compare(0, 6, "local:") == 0 || node.value.compare(0, 3, "qr:") == 0)
            continue;
        bool wide = node.rect.w >= 1800;
        int width = wide ? 1600 : static_cast<int>(std::lround(node.rect.w * 1.1f)),
            height = wide ? 900 : static_cast<int>(std::lround(node.rect.h * 1.1f));
        ImageView view = pictures_->view(node.value, width, height,
                                         node.contain ? ImageFit::Contain : ImageFit::Cover, now);
        node.value.clear();
        if (view.texture) {
            node.texture = view.texture;
            node.opacity *= view.fade;
        } else
            node.opacity = 0;
    }
}
void Application::buildFrame() {
    scene_.reset();
    kit::Paint paint{scene_, *glyphs_};
    Scene &scene = scene_;
    Frame frame{state_, motion_, static_cast<double>(now_)};
    if (state_.screen == Screen::Splash) {
        drawSplash(paint, now_);
        return;
    }
    if (screen_) {
        scene.background = kit::color::ink;
        if (!screen_->paintsWall())
            kit::ambientWall(paint, 1, state_.home.art.wallTint);
        screen_->build(*this, paint);
        if (screen_->hasBar())
            drawTopBar(paint, frame, screen_->barTab());
    } else if (state_.screen == Screen::Home)
        drawHome(paint, frame);
    else if (isRecents()) {
        drawHome(paint, frame);
        drawRecents(paint, frame);
    } else if (state_.screen == Screen::Kit)
        kit::gallery(scene, state_.kitComponent);
    else
        drawMenu(scene, state_);
    float toast = static_cast<float>(motion_.value("toast"));
    if (!state_.toast.empty() && toast > .002f) {
        scene.pushOpacity(toast);
        scene.pushTransform(Transform::translate(0, 16 * (1 - toast)));
        scene.shape({350, 940, 1220, 100}, kit::color::surface3, 20);
        paint.glyphs.text(scene, {382, 968, 1156, kit::type::title.line}, state_.toast,
                          kit::type::title, kit::color::onSurface);
        scene.popTransform();
        scene.popOpacity();
    }
}
const Scene &Application::frame() {
    buildFrame();
    resolvePictures();
    return scene_;
}
// Scripted checks read the words on screen; with the glyph atlas they are glyph quads, so the
// frame is built once more with the plain text nodes.
bool Application::shows(const std::string &text) {
    kit::Glyphs *attached = glyphs_;
    glyphs_ = &legacyGlyphs_;
    Pictures *pictures = pictures_;
    pictures_ = nullptr;
    buildFrame();
    glyphs_ = attached;
    pictures_ = pictures;
    for (const auto &node : scene_.nodes)
        if (node.kind == Node::Kind::Text && node.value.find(text) != std::string::npos)
            return true;
    return false;
}
std::string Application::screenName() const {
    if (screen_)
        return screen_->name();
    switch (state_.screen) {
    case Screen::Splash:
        return "splash";
    case Screen::Home:
        return "home";
    case Screen::Dashboard:
        return "dashboard:" + dashboardTabs()[state_.menu.tab].key;
    case Screen::Quick:
        return "quick:" + state_.menu.level;
    case Screen::Setup:
        return "setup:" + state_.menu.level;
    case Screen::Catalog:
        return "catalog";
    case Screen::Kit:
        return "kit:" + state_.kitComponent;
    case Screen::Qr:
        return "qr";
    }
    return "home";
}
Json Application::snapshot() const {
    if (screen_) {
        Json value = screen_->snapshot();
        value.set("screen", Json::text(screen_->name()));
        value.set("barFocus", Json::flag(barFocus_));
        return value;
    }
    Json value = fields({{"screen", Json::text(screenName())},
                         {"rows", Json::numeric(state_.home.rows.size())},
                         {"busy", Json::flag(state_.busy)}});
    if (state_.screen == Screen::Quick && state_.menu.level == "recents") {
        value.set("recentApps", Json::numeric(state_.recents.cards.size()));
        value.set("selected", Json::numeric(state_.recents.selected));
        value.set("recentFocus",
                  Json::text(state_.recents.focus == RecentState::Focus::All     ? "all"
                             : state_.recents.focus == RecentState::Focus::Close ? "close"
                                                                                 : "card"));
    } else if (state_.screen == Screen::Home) {
        value.set("picture", Json::text(pictureLabel(state_.home.picture)));
        value.set("row", Json::numeric(state_.home.row));
        value.set("column", Json::numeric(state_.home.column));
        const auto &home = state_.home;
        value.set("tab", Json::text(home.tabs.empty() ? "" : home.tabs[home.tab].key));
        value.set("navigation", Json::flag(home.navigation));
        value.set("nav", Json::numeric(home.nav));
        value.set("heroTitle", Json::text(home.art.text.title));
        value.set("slides", Json::numeric(home.spotlight.size()));
        value.set("idle", Json::flag(flow_.idle));
        value.set("silent", Json::flag(state_.serverSilent));
        value.set("wallpaper", Json::text(
                                    home.wallpaperTransition
                                        ? "transitioning"
                                        : !home.wallpaperShown.empty()
                                              ? "visible"
                                              : home.wallpaperFailed
                                                    ? "failed-black"
                                                    : !home.wallpaperTarget.empty() ? "loading-black"
                                                                                   : "black"));
        value.set("wallpaperSlot", Json::text(wallpaperSlot(
                                        home.wallpaperTarget.empty() ? home.wallpaperShown
                                                                     : home.wallpaperTarget)));
    } else if (state_.menu.selected < state_.menu.rows.size())
        value.set("focus", Json::text(state_.menu.rows[state_.menu.selected].label));
    return value;
}
} // namespace nativetv
