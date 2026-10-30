#pragma once
#include "core/services.hpp"
#include "domain/models.hpp"
#include "ui/kit/widgets.hpp"
#include "ui/motion/motion.hpp"
#include "ui/optimistic.hpp"
#include "ui/pictures.hpp"
#include "ui/scene.hpp"
#include "ui/screen.hpp"
#include <cstdint>
#include <map>
#include <memory>

namespace nativetv {
enum class Screen {
    Splash,
    Home,
    Dashboard,
    Quick, // the recent apps (the drawer, Profiles, Details and the editors are registered screens)
    Setup,
    Catalog,
    Qr,
    Kit
};
// What the hero shows: the lines of a card and the artwork behind them, as two layers that
// cross-fade (the newest is drawn last).
struct HeroText {
    std::string source, title, tag, description;
    bool operator==(const HeroText &other) const {
        return source == other.source && title == other.title && tag == other.tag &&
               description == other.description;
    }
};
struct HeroArt {
    std::string image[2];
    Color tint[2] = {{0.07f, 0.19f, 0.24f, 1}, {0.07f, 0.19f, 0.24f, 1}};
    int newest = 0;       // the layer that took the latest picture
    std::string shownId;  // card the layers are showing
    HeroText text;        // the lines on screen; changes after the old ones have left
    Color wallTint = {0.07f, 0.19f, 0.24f, 1}; // what pages without art wash in
};
struct HomeState {
    size_t tab = 0, row = 0, column = 0, nav = 0;
    bool navigation = false;
    std::vector<Tab> tabs;
    std::vector<CardRow> rows;
    Card hero;                 // the card the hero is showing or about to show
    std::vector<Card> spotlight; // the top picks the hero cycles through
    size_t spotIndex = 0;
    HeroArt art;
    Json picture;
    std::string pictureKey;
    std::string wallpaperShown, wallpaperTarget, wallpaperFrom;
    float wallpaperShownOpacity = 0, wallpaperTargetOpacity = 0, wallpaperFromOpacity = 0,
          wallpaperProgress = 1;
    uint32_t wallpaperTransitionAt = 0;
    bool wallpaperTransition = false, wallpaperFailed = false;
    unsigned sourceGeneration = 0, pictureGeneration = 0;
};
struct MenuState {
    size_t tab = 0, selected = 0;
    std::string level, sub;
    std::vector<MenuRow> rows;
};
struct RecentState {
    std::vector<Card> cards;
    size_t selected = 0;
    enum class Focus { Card, Close, All };
    Focus focus = Focus::Card;
    std::string status;
    unsigned revision = 0;
};
struct ViewState {
    Screen screen = Screen::Home;
    HomeState home;
    MenuState menu;
    RecentState recents;
    std::string toast, qr, kitComponent;
    bool busy = false, light = false, serverSilent = false;
    bool installRoot = false, installReplace = false;
    int hour = 0, minute = 0, weekday = 0, day = 1, month = 0;
    Json catalogue, discover, guide, stats, caps, setup, menuData, catalog, install;
};
// What a view reads to draw a frame: the state the controllers keep and the animated values the
// motion engine owns. Views never change either.
struct Frame {
    const ViewState &state;
    const Motion &motion;
    double nowMs;
};
void drawHome(kit::Paint paint, const Frame &frame);
void drawTopBar(kit::Paint paint, const Frame &frame, int pageTab);
void drawRecents(kit::Paint paint, const Frame &frame);
void drawMenu(Scene &scene, const ViewState &state);
class Application : private ScreenHost {
    Services &services_;
    ViewState state_;
    bool dirty_ = true, active_ = true, running_ = true;
    uint32_t now_ = 0, refreshAt_ = 0, toastUntil_ = 0, confirmUntil_ = 0;
    // Whether the server has said if first-run setup is done (/api/caps). Until it does, the
    // question is asked again every two seconds, and the client's own first frames stay behind the
    // ink cover for at most 2.5 s so the page that is about to be replaced is not seen.
    bool capsAnswered_ = false, holdForCaps_ = false;
    uint32_t capsAt_ = 0;
    std::string initial_;
    unsigned recentSnapshot_ = 0;
    std::string confirmId_;
    Command pending_, confirming_;
    std::map<std::string, std::pair<size_t, size_t>> homePositions_;
    // Animation. One Motion holds every animated value; the clock reads the frame time handed to
    // update(), so a --time capture advances by whole frames and nothing else.
    Motion motion_;
    MotionClock clock_;
    Optimistic optimistic_; // after clock_: it reads the frame's time
    kit::LegacyGlyphs legacyGlyphs_;
    kit::Glyphs *glyphs_ = &legacyGlyphs_;
    Pictures *pictures_ = nullptr;
    Scene scene_;
    ScreenRegistry registry_;
    std::unique_ptr<ScreenView> screen_;
    bool barFocus_ = false, ambientOnly_ = false;
    std::string currentRoute_, cameFrom_; // the registered screen showing, and where it was opened from
    std::map<std::string, Json> memory_;  // what screens keep between openings
    // Home's choreography (home_motion.cpp).
    struct HomeFlow {
        bool entered = false;        // the page's entrance has played
        size_t shown = 0;            // tab whose rows are on screen
        int pendingTab = -1;         // tab to show once the old page has left
        uint32_t swapAt = 0, commitAt = 0, textSwapAt = 0, wantSince = 0, lastKeyAt = 0, beatAt = 0,
                 waitFrom = 0;
        bool idle = false /* dimmed: any screen, see idleUpdate() */, heroWaiting = false, textPending = false, tabPending = false,
             waiting = false;
        std::string focusId, wantId;
        Card want;
        std::vector<std::string> tabTitles;
        unsigned beatFails = 0;
        std::string build, layoutVersion;
    } flow_;
    // ScreenHost
    Services &services() override {
        return services_;
    }
    Motion &motion() override {
        return motion_;
    }
    const ViewState &view() const override {
        return state_;
    }
    double nowMs() const override {
        return clock_.nowMs();
    }
    void toast(const std::string &value) override;
    bool barFocused() const override {
        return barFocus_;
    }
    void invalidate() override {
        dirty_ = true;
    }
    std::string cameFrom() const override {
        return cameFrom_;
    }
    std::map<std::string, Json> &memory() override {
        return memory_;
    }
    Optimistic &optimistic() override {
        return optimistic_;
    }
    bool isRecents() const;
    void homeStart();
    void homeData();
    void homeFocus(bool jump = false);
    void homeKeyMoved(Key key);
    void homeSelectTab(size_t tab);
    void homeUpdate();
    void homePlayEntrance();
    void homeRetargetPill(bool jump);
    void homeRetargetHero(const Card &card, bool instant);
    void homeSwapText();
    // Idle dimming, for every screen but the splash (application.cpp).
    void idleUpdate();
    bool wake();
    void drawIdleDim(kit::Paint &paint);
    void homeBeat(const Response *reply);
    void resolvePictures();
    void buildFrame();
    void askCaps();
    bool coveredForCaps() const;
    void rebuildHome();
    void rebuildMenu();
    void refresh();
    void syncWallpaper();
    void startWallpaperTransition();
    void finishWallpaperTransition(bool chooseTarget);
    void updateWallpaperTransition();
    void clearPicture();
    void pictureResponse(const Response &response);
    void recentResponse(const Json &data);
    void recentKey(Key key);
    void recentsFocus(bool jump = false);
    void route(const std::string &path);
    void execute(const Command &command);
    void openDetails(const Json &item);
    void back();
    void screenKey(Key key);

  public:
    // Follows a route as a key press would ("home", "quick", "search", or a registered screen's).
    // The route is copied: a screen passes one of its own strings, and route() destroys the screen.
    void open(const std::string &path) override {
        route(std::string(path));
    }
    // Where screens are registered; the client fills it in registerScreens(), tests add their own.
    ScreenRegistry &screens() {
        return registry_;
    }
    explicit Application(Services &services, const std::string &initial = "home");
    // The real client keeps its first frames behind the ink until the server has said whether
    // first-run setup is needed (up to 2.5 s), so the page about to be replaced is never seen. Not
    // for the kit gallery, setup itself or the splash, which do not ask.
    void holdForSetupCheck() {
        holdForCaps_ = initial_.compare(0, 4, "kit:") != 0 && initial_.compare(0, 6, "setup:") != 0 &&
                       initial_ != "splash";
        dirty_ = true;
    }
    void key(Key key);
    void longPress();
    void input(const std::string &text);
    void click(float x, float y);
    bool activate(const std::string &id);
    void imageReady(const std::string &path);
    void imageFailed(const std::string &path);
    std::string wallpaperPending() const;
    void active(bool value);
    void wallClock(int hour, int minute, int weekday = -1, int day = -1, int month = -1);
    // Advances the animation to `now`; true while the screen changes and must be drawn.
    bool update(uint32_t now);
    // Text and icons come from the glyph atlas and pictures from the image loader once the
    // client attaches them; without them (tests) text stays legacy nodes and pictures stay empty.
    void attach(kit::Glyphs &glyphs, Pictures &pictures);
    // The frame as of the last update(). The reference is valid until the next call.
    const Scene &frame();
    Scene scene() {
        return frame();
    }
    // True when only Ken Burns and the hero's progress moved: half the frame rate is enough.
    bool ambientOnly() const {
        return ambientOnly_;
    }
    // Whether the screen shows this text (for scripted checks; ignores the glyph atlas).
    bool shows(const std::string &text);
    Motion &animation() {
        return motion_;
    }
    bool running() const {
        return running_;
    }
    std::string screenName() const;
    Json snapshot() const;
    void response(const Response &response);
};
} // namespace nativetv
