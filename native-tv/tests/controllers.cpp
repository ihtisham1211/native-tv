#include "ui/application.hpp"
#include <clocale>
#include <cmath>
#include <cstdio>
#include <map>
#include <stdexcept>

using namespace nativetv;
namespace {
int checks = 0;
void check(bool condition, const char *message) {
    ++checks;
    if (!condition)
        throw std::runtime_error(message);
}
struct FakeTransport final : Services {
    struct Request {
        std::string channel;
        Operation operation;
        Json body;
    };
    std::vector<Request> requests;
    std::map<std::string, unsigned> revisions;
    unsigned read(const std::string &channel, Operation operation,
                  const Json &arguments = Json()) override {
        requests.push_back({channel, operation, arguments});
        return ++revisions[channel];
    }
    unsigned write(const std::string &channel, Operation operation, const Json &body) override {
        requests.push_back({channel, operation, body});
        return ++revisions[channel];
    }
    void cancel(const std::string &channel) override {
        ++revisions[channel];
    }
    size_t count(const std::string &channel) const {
        size_t count = 0;
        for (const auto &request : requests)
            if (request.channel == channel)
                ++count;
        return count;
    }
};
void reply(Application &app, const std::string &channel, const std::string &json, long status = 200,
           unsigned generation = 0) {
    Response response;
    response.generation = generation;
    response.channel = channel;
    response.status = status;
    response.bytes = json;
    app.response(response);
}
const char *catalogue =
    R"({"ok":true,"apps":[{"id":"a","title":"Alpha"}],"devices":[{"id":"hdmi1","name":"Console","port":1}],"channels":[],"order":{"app":["a"],"dev":["hdmi1"]},"layout":{"rows":["app","dev"],"hidden":{},"showInputs":true},"profiles":[{"id":"main","name":"Main","active":true}]})";
const char *feed =
    R"({"ok":true,"enabled":true,"titlesEnabled":true,"rows":[{"key":"trending","items":[{"id":7,"type":"movie","title":"Coast","overview":"A journey."}]}]})";
const Node *imageNode(const Scene &scene, const std::string &path) {
    for (const auto &node : scene.nodes)
        if (node.kind == Node::Kind::Image && node.value == path)
            return &node;
    return nullptr;
}
const Node *textNode(const Scene &scene, const std::string &value) {
    for (const auto &node : scene.nodes)
        if (node.kind == Node::Kind::Text && node.value == value)
            return &node;
    return nullptr;
}
// Frames at 60 Hz from `from` for `ms`: the animation advances by what the clock shows.
uint32_t run(Application &app, uint32_t from, uint32_t ms) {
    uint32_t at = from;
    for (uint32_t step = 0; step <= ms * 60 / 1000; ++step) {
        at = from + step * 1000 / 60;
        app.update(at);
    }
    return at;
}
// Pictures the test decides the readiness of.
struct FakePictures final : Pictures {
    std::vector<std::string> asked;
    std::map<std::string, TextureId> ready;
    ImageView view(const std::string &id, int width, int height, ImageFit fit, double) override {
        asked.push_back(id + "@" + std::to_string(width) + "x" + std::to_string(height) +
                        (fit == ImageFit::Contain ? "c" : ""));
        ImageView result;
        auto found = ready.find(id);
        if (found != ready.end()) {
            result.texture = found->second;
            result.fade = .5f;
        }
        return result;
    }
};
// A registered screen, to show what a screen added in its own file sees of the application.
struct ProbeScreen final : ScreenView {
    std::vector<Key> keys;
    std::vector<std::string> clicks, responses;
    bool entered = false, left = false, backHandled = false, focused = false;
    std::string name() const override {
        return "probe";
    }
    int barTab() const override {
        return -1;
    }
    void enter(ScreenHost &) override {
        entered = true;
    }
    void leave(ScreenHost &) override {
        left = true;
    }
    bool key(ScreenHost &, Key key) override {
        keys.push_back(key);
        return key == Key::Down || key == Key::Left;
    }
    bool back(ScreenHost &) override {
        bool handled = backHandled;
        backHandled = false;
        return handled;
    }
    void focus(ScreenHost &) override {
        focused = true;
    }
    void click(ScreenHost &, const std::string &id) override {
        clicks.push_back(id);
    }
    void response(ScreenHost &, const Response &response) override {
        responses.push_back(response.channel);
    }
    void build(ScreenHost &, kit::Paint paint) override {
        paint.scene.shape({100, 400, 300, 100}, kit::color::surface2, 12);
        paint.scene.hits.push_back({{100, 400, 300, 100}, "probe:hit"});
    }
};
ProbeScreen *probe = nullptr;
bool hasHit(const Scene &scene, const std::string &id) {
    for (const auto &hit : scene.hits)
        if (hit.id == id)
            return true;
    return false;
}
} // namespace
int main() {
    try {
        FakeTransport splashTransport;
        Application splash(splashTransport, "splash");
        check(splash.screenName() == "splash", "Native launch selects the OpenGL splash");
        check(splashTransport.requests.empty(), "The splash does not wait on daemon readiness");
        splash.update(1000);
        Scene splashScene = splash.scene();
        check(splashScene.background.r == kit::color::ink.r &&
                  splashScene.background.g == kit::color::ink.g &&
                  splashScene.background.b == kit::color::ink.b,
              "The startup splash covers the screen in Home's ink");
        check(splashScene.nodes.size() == 5 &&
                  imageNode(splashScene, "local:icons/material/mark.svg"),
              "The splash renders its Native Tv mark, its dot and three breathing dots");
        splash.key(Key::Back);
        check(splash.screenName() == "splash", "Remote keys do not dismiss startup splash");
        splash.active(false);
        check(!splash.update(1500), "A background splash pauses its animation");
        splash.active(true);
        check(splash.update(2000), "A resumed splash redraws its animation");

        FakeTransport transport;
        Application app(transport);
        app.update(1000);
        reply(app, "catalogue", catalogue);
        reply(app, "discover", feed);
        check(app.snapshot()["rows"].number() == 2, "Live catalogue and feed rows are combined");
        check(app.snapshot()["column"].number() == 0,
              "Initial selection remains on the first card");
        app.key(Key::Ok);
        check(app.screenName() == "details", "OK on a title opens details");
        check(transport.count("action") == 0, "Opening details does not launch the title");
        app.key(Key::Back);
        check(app.screenName() == "home" && app.snapshot()["row"].number() == 0,
              "Back restores Home's row selection");
        app.key(Key::Down);
        check(app.snapshot()["row"].number() == 1, "Down moves to the apps row");
        app.key(Key::Back);
        check(app.snapshot()["row"].number() == 0, "Back first returns to the first Home row");
        app.key(Key::Back);
        app.key(Key::Back);
        check(app.screenName() == "quick:recents", "Back from the bar opens recent apps");
        FakeTransport navigationTransport;
        Application navigation(navigationTransport);
        reply(navigation, "catalogue", catalogue);
        reply(navigation, "discover", feed);
        navigation.key(Key::Up);
        for (int i = 0; i < 3; ++i)
            navigation.key(Key::Right);
        navigation.key(Key::Ok);
        check(navigation.screenName() == "quick",
              "Header navigation skips the absent single-profile avatar");
        check(!navigation.activate("tool:0"), "An invisible avatar cannot be activated");

        std::setlocale(LC_TIME, "C");
        Json twelveHourCatalogue = Json::parse(catalogue);
        twelveHourCatalogue["layout"].set("clockFormat", Json::text("12h"));
        FakeTransport clockTransport;
        Application clock(clockTransport);
        clock.wallClock(17, 5, 0, 25, 9);
        reply(clock, "catalogue", twelveHourCatalogue.dump());
        Scene twelveHourScene = clock.scene();
        const Node *twelveHourTime = textNode(twelveHourScene, "5:05");
        check(twelveHourTime && twelveHourTime->rect.x == 1740 && twelveHourTime->rect.y == 78 &&
                  twelveHourTime->rect.w == 100 && twelveHourTime->align == Node::Align::Right,
              "The 12-hour clock sits right-aligned at the bar's right margin");
        Json twentyFourHourCatalogue = Json::parse(catalogue);
        FakeTransport clock24Transport;
        Application clock24(clock24Transport);
        clock24.wallClock(17, 5, 0, 25, 9);
        reply(clock24, "catalogue", twentyFourHourCatalogue.dump());
        check(textNode(clock24.scene(), "17:05") != nullptr,
              "The clock keeps the catalogue's 24-hour format");
        FakeTransport blackWallpaperTransport;
        Application blackWallpaper(blackWallpaperTransport);
        blackWallpaper.wallClock(6, 0);
        reply(blackWallpaper, "catalogue", catalogue);
        Scene blackScene = blackWallpaper.scene();
        bool hasWallpaper = false;
        for (const auto &node : blackScene.nodes)
            hasWallpaper = hasWallpaper ||
                           (node.kind == Node::Kind::Image &&
                            node.value.compare(0, 10, "wallpaper:") == 0);
        check(!hasWallpaper && blackWallpaper.snapshot()["wallpaper"].string() == "black",
              "A Home with no uploaded wallpaper keeps its OLED-black background");
        Json wallpapers = Json::parse(catalogue);
        wallpapers.set(
            "wallpapers",
            Json::parse(
                R"({"available":true,"slots":{"night":true,"morning":true,"afternoon":true,"evening":true},"revision":"r1"})"));
        FakeTransport wallpaperTransport;
        Application wallpaper(wallpaperTransport);
        wallpaper.update(1000);
        wallpaper.wallClock(5, 0);
        reply(wallpaper, "catalogue", wallpapers.dump());
        reply(wallpaper, "discover", feed);
        Scene loadingWallpaper = wallpaper.scene();
        const Node *nightPending = imageNode(loadingWallpaper, "wallpaper:night@r1");
        check(nightPending && nightPending->color.a == 0 &&
                  wallpaper.snapshot()["wallpaper"].string() == "loading-black",
              "A requested slot remains black until its image is decoded");
        wallpaper.imageFailed("wallpaper:night@r1");
        check(wallpaper.snapshot()["wallpaper"].string() == "failed-black" &&
                  wallpaper.scene().background.r < .06f,
              "A missing or offline wallpaper leaves the near-black ink fallback");
        wallpaper.imageReady("wallpaper:night@r1");
        check(wallpaper.snapshot()["wallpaper"].string() == "transitioning",
              "A successfully decoded first wallpaper fades in");
        wallpaper.update(1600);
        Scene fadingWallpaper = wallpaper.scene();
        const Node *fadingNight = imageNode(fadingWallpaper, "wallpaper:night@r1");
        check(fadingNight && fadingNight->color.a > 0 && fadingNight->color.a < .55f,
              "The first slot blends from black over the timed transition");
        wallpaper.update(3400);
        check(wallpaper.snapshot()["wallpaper"].string() == "visible",
              "The first wallpaper reaches its configured night opacity");
        struct SlotCheck {
            int hour;
            const char *slot;
            float opacity;
        } slotChecks[] = {{6, "morning", .7f}, {11, "afternoon", .62f},
                          {17, "evening", .72f}, {21, "night", .65f}};
        uint32_t slotTime = 3400;
        for (const auto &expected : slotChecks) {
            wallpaper.wallClock(expected.hour, 0);
            std::string asset = std::string("wallpaper:") + expected.slot + "@r1";
            wallpaper.imageReady(asset);
            slotTime += 2400;
            wallpaper.update(slotTime);
            Scene changedWallpaper = wallpaper.scene();
            const Node *visibleWallpaper = imageNode(changedWallpaper, asset);
            check(wallpaper.snapshot()["wallpaperSlot"].string() == expected.slot &&
                      wallpaper.snapshot()["wallpaper"].string() == "visible" &&
                      visibleWallpaper && std::fabs(visibleWallpaper->color.a - expected.opacity) < .01f,
                  "The Home clock selects and fades to the correct wallpaper time slot");
        }
        run(wallpaper, slotTime, 1500);
        Scene focusedWallpaper = wallpaper.scene();
        const Node *carouselTitle = textNode(focusedWallpaper, "Coast");
        bool focusedCard = false;
        for (const auto &hit : focusedWallpaper.hits)
            focusedCard = focusedCard || hit.id == "card:0:0";
        check(wallpaper.snapshot()["wallpaper"].string() == "visible" &&
                  wallpaper.snapshot()["row"].number() == 0 && focusedCard && carouselTitle &&
                  carouselTitle->rect.x == 80 && carouselTitle->rect.w == 1400 &&
                  carouselTitle->color.a == 1,
              "The hero title keeps the design's geometry over the wallpaper");
        check(imageNode(focusedWallpaper, "wallpaper:night@r1"),
              "The uploaded wallpaper stays behind the hero");
        Json partialWallpapers = Json::parse(catalogue);
        partialWallpapers.set(
            "wallpapers",
            Json::parse(
                R"({"available":true,"slots":{"morning":true},"revision":"r1"})"));
        FakeTransport partialWallpaperTransport;
        Application partialWallpaper(partialWallpaperTransport);
        partialWallpaper.wallClock(1, 0);
        reply(partialWallpaper, "catalogue", partialWallpapers.dump());
        check(partialWallpaper.snapshot()["wallpaper"].string() == "black" &&
                  !imageNode(partialWallpaper.scene(), "wallpaper:night@r1"),
              "A time slot without an upload remains OLED black");
        FakeTransport pictureTransport;
        Application pictureApp(pictureTransport);
        reply(pictureApp, "catalogue", catalogue);
        check(pictureTransport.count("home-now") == 0, "Picture polling remains opt-in");
        Json withPicture = Json::parse(catalogue);
        withPicture["layout"].set("picture", Json::flag(true));
        reply(pictureApp, "catalogue", withPicture.dump());
        reply(pictureApp, "home-now", R"({"kind":"input","port":4})", 200,
              pictureTransport.revisions["home-now"]);
        check(pictureTransport.requests.back().body["port"].number() == 4,
              "Picture reads target the active input, independently of the focused card");
        unsigned oldPicture = pictureTransport.revisions["home-picture"];
        const char *hdr =
            R"({"ok":true,"signal":{"width":3840,"height":2160,"hz":24,"hdr":"hdr10"},"mode":{"display":"HDR Filmmaker"}})";
        reply(pictureApp, "home-picture", hdr, 200, oldPicture);
        check(pictureApp.snapshot()["picture"].string().find("3840 × 2160 at 24 Hz") !=
                  std::string::npos,
              "The native plate formats resolution and refresh rate");
        reply(pictureApp, "catalogue", withPicture.dump());
        reply(pictureApp, "home-now", R"({"kind":"app","appId":"netflix"})", 200,
              pictureTransport.revisions["home-now"]);
        check(pictureApp.snapshot()["picture"].string().empty(),
              "A changed video source clears the previous plate");
        check(pictureTransport.requests.back().body["app"].string() == "netflix",
              "App video metadata uses the active app ID");
        reply(pictureApp, "home-picture", hdr, 200, oldPicture);
        check(pictureApp.snapshot()["picture"].string().empty(),
              "A delayed input reply cannot replace the app picture");
        reply(
            pictureApp, "home-picture",
            R"({"ok":true,"signal":{"width":1920,"height":1080,"hz":23.98,"hdr":"dolby_vision"},"mode":{"display":"Cinema"}})",
            200, pictureTransport.revisions["home-picture"]);
        check(pictureApp.snapshot()["picture"].string().find("23.98 Hz  ·  Dolby Vision") !=
                  std::string::npos,
              "Fractional rates and Dolby Vision remain readable");
        reply(pictureApp, "catalogue", withPicture.dump());
        reply(pictureApp, "home-now", R"({"kind":"input","port":2})", 200,
              pictureTransport.revisions["home-now"]);
        reply(pictureApp, "home-picture", R"({"ok":true,"signal":null,"mode":null})", 200,
              pictureTransport.revisions["home-picture"]);
        check(pictureApp.snapshot()["picture"].string().empty(),
              "An input without a signal hides the plate");
        reply(pictureApp, "catalogue", withPicture.dump());
        reply(pictureApp, "home-now", R"({"kind":"input","port":4})", 200,
              pictureTransport.revisions["home-now"]);
        oldPicture = pictureTransport.revisions["home-picture"];
        reply(pictureApp, "catalogue", catalogue);
        reply(pictureApp, "home-picture", hdr, 200, oldPicture);
        check(pictureApp.snapshot()["picture"].string().empty(),
              "A delayed reply cannot restore a disabled plate");
        pictureApp.longPress();
        const char *recent =
            R"({"ok":true,"apps":[{"id":"one","title":"One","image":true},{"id":"two","title":"Two","image":false}]})";
        reply(pictureApp, "recents", recent, 200, pictureTransport.revisions["recents"]);
        check(pictureApp.snapshot()["recentApps"].number() == 2, "Recents expose screenshot cards");
        run(pictureApp, 1000, 400);
        std::string image;
        for (const auto &node : pictureApp.scene().nodes)
            if (node.kind == Node::Kind::Image && node.value.find("recent:") == 0)
                image = node.value;
        check(!image.empty(), "A screenshot becomes a native image node");
        reply(pictureApp, "recents", recent, 200, pictureTransport.revisions["recents"]);
        std::string refreshed;
        for (const auto &node : pictureApp.scene().nodes)
            if (node.kind == Node::Kind::Image && node.value.find("recent:") == 0)
                refreshed = node.value;
        check(image != refreshed, "A refreshed screenshot gets a new renderer cache reference");
        pictureApp.key(Key::Right);
        pictureApp.key(Key::Down);
        check(pictureApp.snapshot()["recentFocus"].string() == "close",
              "Down focuses the selected card's Close action");
        size_t actions = pictureTransport.count("action");
        pictureApp.key(Key::Ok);
        pictureApp.key(Key::Ok);
        check(pictureTransport.count("action") == actions + 1 &&
                  pictureTransport.requests.back().body["id"].string() == "two",
              "Close targets its own card and held OK cannot duplicate it");
        reply(pictureApp, "action", R"({"ok":false,"error":"Refused"})", 400);
        check(pictureApp.snapshot()["recentApps"].number() == 2,
              "A failed close retains cards for retry");
        pictureApp.key(Key::Up);
        pictureApp.key(Key::Up);
        pictureApp.key(Key::Ok);
        check(pictureTransport.requests.back().body["action"].string() == "closeAll",
              "Up from the card selects Close all");
        reply(pictureApp, "action", R"({"ok":true,"apps":[]})");
        check(pictureApp.screenName() == "home", "Successful Close all returns to Home");
        pictureApp.longPress();
        reply(pictureApp, "recents", R"({"ok":false})", 200, pictureTransport.revisions["recents"]);
        size_t reads = pictureTransport.count("recents");
        pictureApp.key(Key::Ok);
        check(pictureTransport.count("recents") == reads + 1,
              "Unavailable recent apps can be retried without claiming the list is empty");

        // ---- Home on the kit: focus paths, tabs, hero, pages, idle and the beat ----------------
        const char *fullCatalogue =
            R"({"ok":true,"apps":[{"id":"a","title":"Alpha"},{"id":"b","title":"Beta"},{"id":"c","title":"Gamma"},{"id":"d","title":"Delta"},{"id":"e","title":"Epsilon"},{"id":"f","title":"Zeta"},{"id":"g","title":"Eta"},{"id":"h","title":"Theta"}],"devices":[{"id":"hdmi1","name":"Console","port":1},{"id":"hdmi2","name":"Stick","port":2,"unused":true}],"channels":[],"order":{"app":["a","b","c","d","e","f","g","h","lg-home"],"dev":["hdmi1","hdmi2"]},"layout":{"rows":["app","dev"],"hidden":{},"showInputs":true},"profiles":[{"id":"main","name":"Main","active":true}]})";
        const char *fullFeed =
            R"({"ok":true,"enabled":true,"titlesEnabled":true,"rows":[{"key":"trending","items":[{"id":1,"type":"movie","title":"One","overview":"First."},{"id":2,"type":"movie","title":"Two","overview":"Second."},{"id":3,"type":"tv","title":"Three","overview":"Third."},{"id":4,"type":"movie","title":"Four"},{"id":5,"type":"movie","title":"Five"},{"id":6,"type":"movie","title":"Six"},{"id":7,"type":"movie","title":"Seven"},{"id":8,"type":"movie","title":"Eight"}]}]})";
        FakeTransport homeTransport;
        Application home(homeTransport);
        home.update(0);
        reply(home, "catalogue", fullCatalogue);
        reply(home, "discover", fullFeed);
        check(home.snapshot()["slides"].number() == 6,
              "The hero cycles through at most six top picks");
        // Entrance: nothing shows until its stagger delay, then each row's items rise in turn.
        Motion &motion = home.animation();
        check(motion.value("EC:0.0") == 1 && motion.value("ER:1") == 1,
              "Every item of a page that is entering starts hidden");
        uint32_t clockMs = run(home, 16, 250);
        check(motion.value("EC:0.0") < 1 && motion.value("EC:1.4") == 1,
              "Row 0 starts rising before row 1's staggered items");
        clockMs = run(home, clockMs, 1200);
        check(motion.value("EC:0.0") == 0 && motion.value("EC:1.5") == 0 &&
                  motion.value("ER:0") == 0,
              "The entrance finishes for every item");
        check(home.snapshot()["heroTitle"].string() == "One",
              "The hero shows the first top pick once its art is ready");
        // Focus: ring and lift follow the focused card, the row keeps the 4th slot.
        check(motion.value("f:0.0") == 1 && motion.value("f:0.1") == 0,
              "The first card starts focused");
        for (int i = 0; i < 6; ++i)
            home.key(Key::Right);
        clockMs = run(home, clockMs, 900);
        check(motion.value("f:0.6") == 1 && motion.value("f:0.0") == 0,
              "Focus moves with the keys and the old card drops its ring");
        float rowTarget = kit::geometry::rowScrollTarget(6, 8, 224, 24);
        check(rowTarget < 0 && std::fabs(motion.value("rx:0") - rowTarget) < .5,
              "The row scrolls so the focused card keeps the keyline slot");
        check(home.snapshot()["heroTitle"].string() == "Seven",
              "The hero follows a focused title after the key settles");
        home.key(Key::Down);
        clockMs = run(home, clockMs, 900);
        check(std::fabs(motion.value("sy") + 212) < .5 && motion.value("rd:0") == 1 &&
                  motion.value("rd:1") == 0,
              "Down snaps the stack one row up and dims the row left behind");
        Scene stacked = home.scene();
        check(hasHit(stacked, "card:1:6") && !textNode(stacked, "Seven\u00a0"),
              "The apps row follows the focus");
        home.key(Key::Up);
        home.key(Key::Up);
        check(home.snapshot()["navigation"].boolean(), "Up from the first row focuses the bar");
        clockMs = run(home, clockMs, 900);
        check(std::fabs(motion.value("sy")) < .5 && motion.value("pill:hot") == 1,
              "The bar returns the stack to the top and turns the pill hot");

        // The bar: the pill leads with the focus, the page follows after the focus rests.
        kit::LegacyGlyphs glyphs;
        std::vector<Rect> tabBoxes = kit::tabRects(glyphs, {"Home", "Apps", "Inputs"});
        check(std::fabs(motion.value("pill:0") - tabBoxes[0].x) < .5 &&
                  std::fabs(motion.value("pill:1") - (tabBoxes[0].x + tabBoxes[0].w)) < .5,
              "The pill rests on the selected tab");
        home.key(Key::Right);
        clockMs = run(home, clockMs, 100);
        check(home.snapshot()["tab"].string() == "home" && motion.moving("pill:1"),
              "The pill leaves at once while the page waits for the focus to rest");
        double lead = motion.value("pill:1") - (tabBoxes[0].x + tabBoxes[0].w),
               trail = motion.value("pill:0") - tabBoxes[0].x;
        check(lead > trail,
              "The leading edge runs ahead of the trailing one, so the pill stretches");
        clockMs = run(home, clockMs, 600);
        check(home.snapshot()["tab"].string() == "apps" &&
                  std::fabs(motion.value("pill:0") - tabBoxes[1].x) < .5,
              "The page opens once the focus has rested on its tab");
        check(motion.value("pgx") == 0 && motion.value("wall") == 0,
              "The Apps page has no hero wall");
        home.key(Key::Right);
        home.key(Key::Left);
        clockMs = run(home, clockMs, 600);
        check(home.snapshot()["tab"].string() == "apps",
              "Moving along the tabs and back does not reload the page");
        // Home again: its hero comes back with the page.
        home.key(Key::Left);
        clockMs = run(home, clockMs, 2000);
        check(home.snapshot()["tab"].string() == "home" && home.snapshot()["heroTitle"].string() == "One" &&
                  motion.value("wall") == 1,
              "Coming back to Home shows its hero and wall again");
        home.key(Key::Right);
        clockMs = run(home, clockMs, 1000);

        // Apps: a six-column grid, no horizontal scroll.
        home.key(Key::Down);
        clockMs = run(home, clockMs, 800);
        check(home.snapshot()["navigation"].boolean() == false && home.snapshot()["row"].number() == 0 &&
                  home.snapshot()["column"].number() == 0,
              "Down from the Apps tab lands on the first tile");
        home.key(Key::Right);
        home.key(Key::Down);
        check(home.snapshot()["column"].number() == 7, "Down moves one grid row of six");
        for (int i = 0; i < 2; ++i)
            home.key(Key::Down);
        check(home.snapshot()["column"].number() == 7,
              "The grid stops at its last row");
        clockMs = run(home, clockMs, 800);
        check(motion.value("f:0.7") == 1 && motion.value("f:0.0") == 0,
              "The tile focus follows the grid");

        // Back goes up one level at a time.
        home.key(Key::Back);
        check(home.snapshot()["row"].number() == 0 && home.snapshot()["navigation"].boolean(),
              "Back from a grid tile goes to the bar");

        // Idle: after thirty quiet seconds the screen dims, and the first key only wakes it.
        clockMs = run(home, clockMs, 33000);
        check(home.snapshot()["idle"].boolean() && std::fabs(motion.value("dim") - .66) < .01,
              "The screen dims after thirty seconds without a key");
        size_t position = home.snapshot()["nav"].number();
        home.key(Key::Right);
        check(home.snapshot()["nav"].number() == position && !home.snapshot()["idle"].boolean(),
              "The first key after dimming only wakes the screen");
        clockMs = run(home, clockMs, 400);
        check(motion.value("dim") == 0, "The dim lifts quickly on the first key");
        home.key(Key::Right);
        check(home.snapshot()["nav"].number() != position, "The next key acts");

        // The beat: every five seconds, three silent beats raise the banner, a new build reloads.
        FakeTransport beatTransport;
        Application beating(beatTransport);
        beating.update(0);
        reply(beating, "catalogue", fullCatalogue);
        reply(beating, "discover", fullFeed);
        run(beating, 16, 4000);
        check(beatTransport.count("beat") == 0, "The beat waits five seconds");
        uint32_t beatClock = run(beating, 4016, 1500);
        check(beatTransport.count("beat") == 1 &&
                  beatTransport.requests.back().operation == Operation::HomeBeat,
              "Home beats the server every five seconds");
        reply(beating, "beat", R"({"ok":true,"build":"one","layoutVersion":"1","wallpapers":{"available":false}})");
        size_t catalogueReads = beatTransport.count("catalogue");
        beatClock = run(beating, beatClock, 5200);
        reply(beating, "beat", R"({"ok":true,"build":"one","layoutVersion":"2"})");
        check(beatTransport.count("catalogue") > catalogueReads,
              "A changed layout version reloads the catalogue");
        catalogueReads = beatTransport.count("catalogue");
        beatClock = run(beating, beatClock, 5200);
        reply(beating, "beat", R"({"ok":true,"build":"two","layoutVersion":"2"})");
        check(beatTransport.count("catalogue") > catalogueReads,
              "A new server build reloads Home's data");
        for (int i = 0; i < 3; ++i) {
            beatClock = run(beating, beatClock, 5200);
            reply(beating, "beat", "", 503);
        }
        check(beating.snapshot()["silent"].boolean(),
              "Three silent beats raise the not-answering banner");
        run(beating, beatClock, 400);
        check(beating.shows("Native Tv is not answering"), "The banner names the problem");
        reply(beating, "beat", R"({"ok":true,"build":"two","layoutVersion":"2"})");
        check(!beating.snapshot()["silent"].boolean(), "An answer takes the banner away");

        // Screens registered in their own files are hosted without Home changing.
        FakeTransport hostTransport;
        Application host(hostTransport);
        host.update(0);
        reply(host, "catalogue", fullCatalogue);
        reply(host, "discover", fullFeed);
        host.screens().add([](const std::string &route) -> std::unique_ptr<ScreenView> {
            if (route != "probe")
                return nullptr;
            probe = new ProbeScreen();
            return std::unique_ptr<ScreenView>(probe);
        });
        host.open("probe");
        check(host.screenName() == "probe" && probe && probe->entered,
              "A registered screen is entered by its route");
        host.key(Key::Down);
        check(probe->keys.size() == 1 && !host.snapshot()["barFocus"].boolean(),
              "The screen gets the keys first and keeps the ones it uses");
        host.key(Key::Up);
        check(host.snapshot()["barFocus"].boolean(),
              "An unused Up gives the focus to the bar");
        host.key(Key::Down);
        check(probe->focused && !host.snapshot()["barFocus"].boolean(),
              "Down from the bar hands the focus back to the screen");
        Scene probeScene = host.scene();
        check(hasHit(probeScene, "probe:hit") && hasHit(probeScene, "nav:tab:0"),
              "The page is drawn under the top bar");
        host.click(200, 450);
        check(probe->clicks.size() == 1 && probe->clicks[0] == "probe:hit",
              "A click on the screen's hit reaches it");
        reply(host, "stats", R"({"ok":true})");
        check(probe->responses.size() >= 1 && probe->responses.back() == "stats",
              "Replies reach the screen");
        probe->backHandled = true;
        host.key(Key::Back);
        check(host.screenName() == "probe", "A screen can use Back for a level of its own");
        host.key(Key::Back);
        check(host.screenName() == "home", "Back leaves a registered screen for Home");

        // ---- Pictures: asked for at their drawn size, waited for by the hero, faded in -------
        const char *artFeed =
            R"({"ok":true,"enabled":true,"titlesEnabled":true,"rows":[{"key":"trending","items":[{"id":1,"type":"movie","title":"One","backdrop":"/b1.jpg"},{"id":2,"type":"movie","title":"Two","backdrop":"/b2.jpg"}]}]})";
        FakeTransport pictureHomeTransport;
        Application pictured(pictureHomeTransport);
        kit::LegacyGlyphs pictureGlyphs;
        FakePictures pictures;
        pictured.attach(pictureGlyphs, pictures);
        pictured.update(0);
        reply(pictured, "catalogue", fullCatalogue);
        reply(pictured, "discover", artFeed);
        uint32_t pictureClock = run(pictured, 16, 300);
        check(pictured.snapshot()["heroTitle"].string().empty(),
              "The hero keeps its place until the incoming art has been decoded");
        Scene waiting = pictured.scene();
        bool cardPicture = false;
        for (const auto &node : waiting.nodes)
            if (node.kind == Node::Kind::Image && node.texture == 0 && node.opacity == 0 &&
                node.value.empty())
                cardPicture = true;
        check(cardPicture, "A picture that has not arrived draws nothing over its placeholder");
        bool askedCard = false, askedHero = false;
        for (const auto &id : pictures.asked) {
            askedCard = askedCard || id == "tmdb:w500:/b1.jpg@246x154";
            askedHero = askedHero || id == "tmdb:w1280:/b1.jpg@1600x900";
        }
        check(askedCard && askedHero,
              "Cards are asked for at their focus-scaled size and the hero at 1600x900");
        pictures.ready["tmdb:w1280:/b1.jpg"] = 7;
        pictures.ready["tmdb:w500:/b1.jpg"] = 8;
        pictureClock = run(pictured, pictureClock, 200);
        check(pictured.snapshot()["heroTitle"].string() == "One", "The hero shows once its art is ready");
        Scene shown = pictured.scene();
        bool faded = false, hero = false;
        for (const auto &node : shown.nodes) {
            if (node.kind == Node::Kind::Image && node.texture == 8 && node.opacity == .5f)
                faded = true;
            if (node.kind == Node::Kind::Image && node.texture == 7 && node.rect.w == 1920)
                hero = true;
        }
        check(faded && hero, "Ready pictures carry the loader's fade-in and their texture");
        // A picture that never arrives holds the hero back only briefly.
        FakeTransport slowTransport;
        Application slow(slowTransport);
        FakePictures nothing;
        slow.attach(pictureGlyphs, nothing);
        slow.update(0);
        reply(slow, "catalogue", fullCatalogue);
        reply(slow, "discover", artFeed);
        run(slow, 16, 2500);
        check(slow.snapshot()["heroTitle"].string() == "One",
              "The hero is not held back for ever by art that does not arrive");
        // The wallpaper starts fading in by itself once its picture is ready.
        FakeTransport wallpaperHomeTransport;
        Application wallpaperHome(wallpaperHomeTransport);
        FakePictures wallpaperPictures;
        wallpaperHome.attach(pictureGlyphs, wallpaperPictures);
        wallpaperHome.update(0);
        wallpaperHome.wallClock(5, 0);
        reply(wallpaperHome, "catalogue", wallpapers.dump());
        run(wallpaperHome, 16, 200);
        check(wallpaperHome.snapshot()["wallpaper"].string() == "loading-black",
              "The wallpaper waits for its picture");
        wallpaperPictures.ready["wallpaper:night@r1"] = 9;
        run(wallpaperHome, 300, 100);
        check(wallpaperHome.snapshot()["wallpaper"].string() == "transitioning",
              "The wallpaper fades in as soon as the loader has it");
        FakeTransport idleTransport;
        Application idle(idleTransport);
        idle.update(1000);
        idle.update(1200);
        check(!idle.update(1300), "Idle UI does not request a continuous redraw");
        std::printf("%d native controller checks passed\n", checks);
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
