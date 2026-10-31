#include "ui/application.hpp"
#include <clocale>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
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
    std::set<std::string> failed; // pictures the server has none of
    ImageView view(const std::string &id, int width, int height, ImageFit fit, double) override {
        asked.push_back(id + "@" + std::to_string(width) + "x" + std::to_string(height) +
                        (fit == ImageFit::Contain ? "c" : ""));
        ImageView result;
        auto found = ready.find(id);
        if (found != ready.end()) {
            result.texture = found->second;
            result.fade = .5f;
        }
        result.failed = failed.count(id) != 0;
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
        check(app.snapshot()["row"].number() == 1 && app.snapshot()["column"].number() == 0,
              "Initial selection lands on the first top pick, under the apps row");
        app.key(Key::Ok);
        check(app.screenName() == "details", "OK on a title opens details");
        check(transport.count("action") == 0, "Opening details does not launch the title");
        app.key(Key::Back);
        check(app.screenName() == "home" && app.snapshot()["row"].number() == 1,
              "Back restores Home's row selection");
        app.key(Key::Back);
        check(app.snapshot()["row"].number() == 0, "Back first returns to the first Home row");
        app.key(Key::Back);
        check(app.snapshot()["navigation"].boolean(), "Back from the first row goes to the bar");
        app.key(Key::Back);
        check(app.screenName() == "home" && app.snapshot()["navigation"].boolean() &&
                  transport.count("recents") == 0,
              "A tap of Back on the bar does nothing");
        app.longBack();
        check(app.screenName() == "quick:recents", "Back held opens recent apps");
        FakeTransport navigationTransport;
        Application navigation(navigationTransport);
        reply(navigation, "catalogue", catalogue);
        reply(navigation, "discover", feed);
        navigation.key(Key::Up); // to the apps row
        navigation.key(Key::Up); // to the bar
        for (int i = 0; i < 3; ++i)
            navigation.key(Key::Right);
        navigation.key(Key::Ok);
        check(navigation.screenName() == "search",
              "From the first tab the third Right reaches Search, drawn left of Quick settings");
        FakeTransport gearTransport;
        Application gear(gearTransport);
        reply(gear, "catalogue", catalogue);
        reply(gear, "discover", feed);
        gear.key(Key::Up);
        gear.key(Key::Up);
        for (int i = 0; i < 4; ++i)
            gear.key(Key::Right);
        gear.key(Key::Ok);
        check(gear.screenName() == "quick", "A fourth Right reaches Quick settings");
        FakeTransport avatarTransport;
        Application avatar(avatarTransport);
        reply(avatar, "catalogue", catalogue);
        reply(avatar, "discover", feed);
        avatar.key(Key::Up);
        avatar.key(Key::Up);
        avatar.key(Key::Left);
        avatar.key(Key::Ok);
        check(avatar.screenName() == "profiles",
              "The avatar is on the bar's path with one profile, and opens Who's watching?");

        std::setlocale(LC_TIME, "C");
        Json twelveHourCatalogue = Json::parse(catalogue);
        twelveHourCatalogue["layout"].set("clockFormat", Json::text("12h"));
        FakeTransport clockTransport;
        Application clock(clockTransport);
        clock.wallClock(17, 5, 0, 25, 9);
        reply(clock, "catalogue", twelveHourCatalogue.dump());
        Scene twelveHourScene = clock.scene();
        const Node *twelveHourTime = textNode(twelveHourScene, "5:05");
        check(twelveHourTime && twelveHourTime->rect.x == 1740 && twelveHourTime->rect.y == 72 &&
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
            focusedCard = focusedCard || hit.id == "card:1:0";
        check(wallpaper.snapshot()["wallpaper"].string() == "visible" &&
                  wallpaper.snapshot()["row"].number() == 1 && focusedCard && carouselTitle &&
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
        size_t recentReads = pictureTransport.count("recents");
        pictureApp.longPress();
        pictureApp.key(Key::Back);
        check(pictureTransport.count("recents") == recentReads &&
                  !pictureApp.snapshot()["editing"].boolean(),
              "Holding OK on an app, then a tap of Back, does not open the recent apps");
        pictureApp.longBack();
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
        pictureApp.longBack();
        reply(pictureApp, "recents", R"({"ok":false})", 200, pictureTransport.revisions["recents"]);
        size_t reads = pictureTransport.count("recents");
        pictureApp.key(Key::Ok);
        check(pictureTransport.count("recents") == reads + 1,
              "Unavailable recent apps can be retried without claiming the list is empty");

        // ---- Home on the kit: focus paths, tabs, hero, pages, idle and the beat ----------------
        const char *fullCatalogue =
            R"({"ok":true,"apps":[{"id":"a","title":"Alpha"},{"id":"b","title":"Beta"},{"id":"c","title":"Gamma"},{"id":"d","title":"Delta"},{"id":"e","title":"Epsilon"},{"id":"f","title":"Zeta"},{"id":"g","title":"Eta"},{"id":"h","title":"Theta"}],"devices":[{"id":"hdmi1","name":"Console","port":1},{"id":"hdmi2","name":"Stick","port":2,"unused":true}],"channels":[],"order":{"app":["a","b","c","d","e","f","g","h","lg-home"],"dev":["hdmi1","hdmi2"]},"layout":{"rows":["app","dev"],"hidden":{},"showInputs":true},"profiles":[{"id":"main","name":"Main","active":true}]})";
        const char *fullFeed =
            R"({"ok":true,"enabled":true,"titlesEnabled":true,"rows":[{"key":"trending","items":[{"id":1,"type":"movie","title":"One","overview":"First."},{"id":2,"type":"movie","title":"Two","overview":"Second."},{"id":3,"type":"tv","title":"Three","overview":"Third."},{"id":4,"type":"movie","title":"Four"},{"id":5,"type":"movie","title":"Five"},{"id":6,"type":"movie","title":"Six"},{"id":7,"type":"movie","title":"Seven"},{"id":8,"type":"movie","title":"Eight"}]},{"key":"movies","items":[{"id":9,"type":"movie","title":"Nine"}]}]})";
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
        uint32_t clockMs = run(home, 16, 200);
        check(motion.value("EC:0.0") < 1 && motion.value("EC:1.4") == 1,
              "Row 0 starts rising before row 1's staggered items");
        clockMs = run(home, clockMs, 1200);
        check(motion.value("EC:0.0") == 0 && motion.value("EC:1.5") == 0 &&
                  motion.value("ER:0") == 0,
              "The entrance finishes for every item");
        check(home.snapshot()["heroTitle"].string() == "One",
              "The hero shows the first top pick once its art is ready");
        // Focus: ring and lift follow the focused card, the row keeps the 4th slot. The top
        // picks (row 1, under the apps) start focused.
        check(motion.value("f:1.0") == 1 && motion.value("f:1.1") == 0,
              "The first top pick starts focused");
        for (int i = 0; i < 6; ++i)
            home.key(Key::Right);
        clockMs = run(home, clockMs, 900);
        check(motion.value("f:1.6") == 1 && motion.value("f:1.0") == 0,
              "Focus moves with the keys and the old card drops its ring");
        float rowTarget = kit::geometry::rowScrollTarget(6, 8, 300, 24);
        check(rowTarget < 0 && std::fabs(motion.value("rx:1") - rowTarget) < .5,
              "The row scrolls so the focused card keeps the keyline slot");
        check(home.snapshot()["heroTitle"].string() == "Seven",
              "The hero follows a focused title after the key settles");
        home.key(Key::Down);
        clockMs = run(home, clockMs, 900);
        check(std::fabs(motion.value("sy") + 252) < .5 && motion.value("rd:1") == 1 &&
                  motion.value("rd:2") == 0,
              "Down snaps the stack one row up and dims the row left behind");
        Scene stacked = home.scene();
        check(hasHit(stacked, "card:2:0"), "The rows follow the focus");
        home.key(Key::Up);
        check(std::fabs(motion.value("sy")) < .5 || motion.moving("sy"),
              "Up to the top picks brings the stack back");
        home.key(Key::Up);
        check(!home.snapshot()["navigation"].boolean() && home.snapshot()["row"].number() == 0,
              "Up from the top picks reaches the apps row");
        clockMs = run(home, clockMs, 900);
        check(!home.snapshot()["heroTitle"].string().empty() && home.snapshot()["heroTitle"].string() != "Alpha",
              "An app has no preview in the hero: it stays on a top pick");
        home.key(Key::Up);
        check(home.snapshot()["navigation"].boolean(), "Up from the apps row focuses the bar");
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

        // Back goes to the first tab from any other, then up one level at a time, and stops at the
        // bar; it never opens the recent apps.
        home.key(Key::Back);
        clockMs = run(home, clockMs, 600);
        check(home.snapshot()["tab"].string() == "home" && !home.snapshot()["navigation"].boolean() &&
                  home.snapshot()["row"].number() == 1,
              "Back from a grid tile goes to the Home tab, focus on its top picks");
        home.key(Key::Back);
        check(home.snapshot()["row"].number() == 0 && !home.snapshot()["navigation"].boolean(),
              "Back from the top picks goes to the apps row");
        home.key(Key::Back);
        check(home.snapshot()["navigation"].boolean(), "Back from the first row goes to the bar");
        home.key(Key::Back);
        check(home.screenName() == "home" && home.snapshot()["navigation"].boolean(),
              "Back on the bar does nothing");

        // Apps sorted by kind: one row per kind. Holding OK lifts the focused row; Down moves it,
        // Back puts the server's order back, OK saves the kinds' order.
        const char *kindCatalogue =
            R"({"ok":true,"apps":[{"id":"a","title":"Alpha","kind":"video"},{"id":"b","title":"Beta","kind":"music"},{"id":"c","title":"Gamma","kind":"games"}],"devices":[],"channels":[],"kinds":[{"key":"video","label":"Movies & TV"},{"key":"music","label":"Music & audio"},{"key":"games","label":"Games"}],"order":{"app":["a","b","c"],"dev":[]},"layout":{"rows":["app"],"hidden":{},"sort":{"app":"kind"},"showInputs":false,"showLgHome":false},"profiles":[{"id":"main","name":"Main","active":true}]})";
        FakeTransport kindTransport;
        Application kinds(kindTransport);
        kinds.update(0);
        reply(kinds, "catalogue", kindCatalogue);
        reply(kinds, "discover", fullFeed);
        uint32_t kindMs = run(kinds, 16, 1500);
        kinds.key(Key::Up);
        kinds.key(Key::Up);
        kinds.key(Key::Right);
        kinds.key(Key::Ok);
        kindMs = run(kinds, kindMs, 1200);
        check(kinds.snapshot()["tab"].string() == "apps" && kinds.snapshot()["rows"].number() == 3 &&
                  kinds.snapshot()["rowGroup"].string() == "video",
              "The Apps tab sorted by kind opens on the first kind's row");
        kinds.longPress();
        check(kinds.snapshot()["arranging"].boolean() && kinds.screenName() == "home",
              "Holding OK on a row lifts it instead of opening the recent apps");
        kinds.key(Key::Down);
        check(kinds.snapshot()["row"].number() == 1 && kinds.snapshot()["rowGroup"].string() == "video",
              "Down moves the lifted row under the next one, the focus with it");
        // The ten-second refresh answers while a row is held: the move stays.
        reply(kinds, "catalogue", kindCatalogue);
        reply(kinds, "discover", fullFeed);
        check(kinds.snapshot()["arranging"].boolean() && kinds.snapshot()["row"].number() == 1 &&
                  kinds.snapshot()["rowGroup"].string() == "video",
              "A refresh that lands while a row is lifted does not put it back");
        kinds.key(Key::Back);
        check(!kinds.snapshot()["arranging"].boolean() && kinds.snapshot()["row"].number() == 0 &&
                  kinds.snapshot()["rowGroup"].string() == "video",
              "Back puts the rows back and keeps the focus on the one that was lifted");
        kinds.longPress();
        kinds.key(Key::Down);
        kinds.key(Key::Down);
        kinds.key(Key::Down); // no row below the last: nothing moves
        size_t writes = kindTransport.requests.size();
        kinds.key(Key::Ok);
        const Json &saved = kindTransport.requests.back().body;
        check(!kinds.snapshot()["arranging"].boolean() && kindTransport.requests.size() == writes + 1 &&
                  kindTransport.requests.back().channel == "action" &&
                  saved["kinds"].size() == 3 && saved["kinds"][0].string() == "music" &&
                  saved["kinds"][1].string() == "games" && saved["kinds"][2].string() == "video",
              "OK saves the kinds in their new order");

        const auto replaced = [](std::string text, const std::string &from, const std::string &to) {
            text.replace(text.find(from), from.size(), to);
            return text;
        };
        // The Apps page has its own hidden apps (`hidden.appsTab`) apart from Home's row
        // (`hidden.app`). An Apps page by kind loses a kind whose apps are all hidden; arranging
        // saves the kinds with the missing one where it was.
        {
            const std::string hiddenKinds = replaced(kindCatalogue, R"("hidden":{})",
                                                     R"("hidden":{"app":[],"appsTab":["b"]})");
            FakeTransport hiddenTransport;
            Application hiddenApps(hiddenTransport);
            hiddenApps.update(0);
            reply(hiddenApps, "catalogue", hiddenKinds);
            reply(hiddenApps, "discover", fullFeed);
            uint32_t hiddenMs = run(hiddenApps, 16, 1500);
            hiddenApps.key(Key::Up);
            hiddenApps.key(Key::Up);
            hiddenApps.key(Key::Right);
            hiddenApps.key(Key::Ok);
            hiddenMs = run(hiddenApps, hiddenMs, 1200);
            check(hiddenApps.snapshot()["tab"].string() == "apps" &&
                      hiddenApps.snapshot()["rows"].number() == 2,
                  "A kind whose apps are all hidden has no row on the Apps page");
            check(!hasHit(hiddenApps.scene(), "card:2:0") && hasHit(hiddenApps.scene(), "card:1:0"),
                  "The hidden app has no tile");
            hiddenApps.longPress();
            hiddenApps.key(Key::Down);
            hiddenApps.key(Key::Ok);
            const Json &order = hiddenTransport.requests.back().body;
            check(order["kinds"].size() == 3 && order["kinds"][0].string() == "games" &&
                      order["kinds"][1].string() == "music" && order["kinds"][2].string() == "video",
                  "Arranging the visible kinds leaves the kind with no row where it was");
            // Back on the Apps page is a step back to the first tab, not to the recent apps.
            hiddenApps.key(Key::Back);
            hiddenMs = run(hiddenApps, hiddenMs, 600);
            check(hiddenApps.snapshot()["tab"].string() == "home" &&
                      !hiddenApps.snapshot()["navigation"].boolean() &&
                      hiddenTransport.count("recents") == 0,
                  "Back from the Apps page by kind goes to the Home tab with focus in its content");
            hiddenApps.key(Key::Up); // to the apps row
            hiddenMs = run(hiddenApps, hiddenMs, 200);
            check(hasHit(hiddenApps.scene(), "card:0:2"),
                  "An app hidden on the Apps page is still on Home's row");
            hiddenApps.key(Key::Right);
            hiddenApps.key(Key::Up);
            hiddenApps.key(Key::Up);
            hiddenApps.key(Key::Right);
            hiddenApps.key(Key::Ok);
            run(hiddenApps, hiddenMs, 1200);
            hiddenApps.longBack();
            check(hiddenApps.screenName() == "quick:recents" && hiddenTransport.count("recents") == 1,
                  "Back held opens the recent apps from the Apps tab");
        }
        // The other way round: an app hidden on Home's row stays on the Apps page.
        {
            const std::string hiddenOnHome = replaced(kindCatalogue, R"("hidden":{})",
                                                      R"("hidden":{"app":["b"],"appsTab":[]})");
            FakeTransport homeTransport;
            Application homeHidden(homeTransport);
            homeHidden.update(0);
            reply(homeHidden, "catalogue", hiddenOnHome);
            reply(homeHidden, "discover", fullFeed);
            uint32_t homeHiddenMs = run(homeHidden, 16, 1500);
            homeHidden.key(Key::Up); // Home's row: Alpha and Gamma only
            homeHiddenMs = run(homeHidden, homeHiddenMs, 200);
            check(hasHit(homeHidden.scene(), "card:0:1") && !hasHit(homeHidden.scene(), "card:0:2"),
                  "An app hidden on Home's row is not on it");
            homeHidden.key(Key::Up);
            homeHidden.key(Key::Right);
            homeHidden.key(Key::Ok);
            run(homeHidden, homeHiddenMs, 1200);
            check(homeHidden.snapshot()["tab"].string() == "apps" &&
                      homeHidden.snapshot()["rows"].number() == 3,
                  "and the Apps page by kind still has its row");
        }
        // The Apps page by kind has no head: three rows fit from y 160 and the page follows the
        // focus once it passes the second row, ending with the last rows flush with the screen.
        {
            const char *fiveKinds =
                R"({"ok":true,"apps":[{"id":"a","title":"A","kind":"video"},{"id":"b","title":"B","kind":"music"},{"id":"c","title":"C","kind":"games"},{"id":"d","title":"D","kind":"home"},{"id":"e","title":"E","kind":"tools"}],"devices":[],"channels":[],"kinds":[{"key":"video","label":"Movies & TV"},{"key":"music","label":"Music & audio"},{"key":"games","label":"Games"},{"key":"home","label":"Smart home"},{"key":"tools","label":"Tools"}],"order":{"app":["a","b","c","d","e"],"dev":[]},"layout":{"rows":["app"],"hidden":{"app":[]},"sort":{"app":"kind"},"showInputs":false,"showLgHome":false},"profiles":[{"id":"main","name":"Main","active":true}]})";
            FakeTransport fiveTransport;
            Application five(fiveTransport);
            five.update(0);
            reply(five, "catalogue", fiveKinds);
            reply(five, "discover", fullFeed);
            uint32_t fiveMs = run(five, 16, 1500);
            five.key(Key::Up);
            five.key(Key::Up);
            five.key(Key::Right);
            five.key(Key::Ok);
            fiveMs = run(five, fiveMs, 1200);
            check(five.snapshot()["rows"].number() == 5, "Five kinds are five rows");
            Motion &fiveMotion = five.animation();
            five.key(Key::Down);
            fiveMs = run(five, fiveMs, 800);
            check(std::fabs(fiveMotion.value("sy")) < .5, "The first two kind rows stay where they are");
            five.key(Key::Down);
            fiveMs = run(five, fiveMs, 800);
            check(std::fabs(fiveMotion.value("sy") + kit::layout::rowPitch) < .5,
                  "On the third row the page moves up one row so the next one shows");
            for (int i = 0; i < 2; ++i)
                five.key(Key::Down);
            fiveMs = run(five, fiveMs, 800);
            check(five.snapshot()["row"].number() == 4 &&
                      std::fabs(fiveMotion.value("sy") + 2 * kit::layout::rowPitch) < .5,
                  "The last rows end flush with the screen: three stay in view");
        }
        // The Apps grid keeps its focus in range when apps are hidden under it.
        {
            const std::string eightApps =
                R"({"ok":true,"apps":[{"id":"a","title":"A"},{"id":"b","title":"B"},{"id":"c","title":"C"},{"id":"d","title":"D"},{"id":"e","title":"E"},{"id":"f","title":"F"},{"id":"g","title":"G"},{"id":"h","title":"H"}],"devices":[],"channels":[],"order":{"app":["a","b","c","d","e","f","g","h"],"dev":[]},"layout":{"rows":["app"],"hidden":{"app":[],"appsTab":[]},"showInputs":false,"showLgHome":false},"profiles":[{"id":"main","name":"Main","active":true}]})";
            FakeTransport gridTransport;
            Application grid(gridTransport);
            grid.update(0);
            reply(grid, "catalogue", eightApps);
            reply(grid, "discover", fullFeed);
            uint32_t gridMs = run(grid, 16, 1500);
            grid.key(Key::Up);
            grid.key(Key::Up);
            grid.key(Key::Right);
            grid.key(Key::Ok);
            gridMs = run(grid, gridMs, 1200);
            grid.key(Key::Down); // the second grid row: apps 7 and 8
            grid.key(Key::Right);
            check(grid.snapshot()["focusCard"].string() == "h", "The last app of the grid is focused");
            const std::string fewer =
                replaced(eightApps, R"("appsTab":[])", R"("appsTab":["g","h"])");
            reply(grid, "catalogue", fewer);
            run(grid, gridMs, 100);
            check(grid.snapshot()["focusCard"].string() == "f" &&
                      grid.snapshot()["column"].number() == 5 && !hasHit(grid.scene(), "card:0:6"),
                  "Hiding the focused app and the one before it puts the focus on the last left");
        }

        // Editing the apps: holding OK on an app starts it, the arrows swap the app with its
        // neighbour, and OK, Back or Red ends it with one request. A hidden app keeps its place.
        const char *editCatalogue =
            R"({"ok":true,"apps":[{"id":"a","title":"Alpha"},{"id":"b","title":"Beta"},{"id":"c","title":"Gamma"},{"id":"h","title":"Hidden"}],"devices":[],"channels":[],"order":{"app":["a","h","b","c"],"dev":[]},"layout":{"rows":["app"],"hidden":{"app":["h"]},"sort":{"app":"recent"},"showInputs":false,"showLgHome":false},"profiles":[{"id":"main","name":"Main","active":true}]})";
        const auto layoutWrites = [](const FakeTransport &transport) {
            std::vector<Json> bodies;
            for (const auto &request : transport.requests)
                if (request.channel == "action" && request.operation == Operation::SetHomeLayout)
                    bodies.push_back(request.body);
            return bodies;
        };
        // `tab`: 0 holds OK on Home's apps row, 1 on the Apps page's grid.
        const auto editable = [&](Application &app, const std::string &catalogueJson, int tab) {
            app.update(0);
            reply(app, "catalogue", catalogueJson);
            reply(app, "discover", feed);
            uint32_t at = run(app, 16, 1500);
            app.key(Key::Up); // from the top picks to the apps row
            if (tab) {
                app.key(Key::Up);
                app.key(Key::Right);
                app.key(Key::Ok);
                at = run(app, at, 1200);
            }
            app.longPress();
            run(app, at, 900); // the page settles under the focus
        };
        {
            FakeTransport transport;
            Application app(transport);
            editable(app, editCatalogue, 0);
            check(app.snapshot()["editing"].boolean() && textNode(app.scene(), "Moving apps"),
                  "Holding OK on an app of Home starts editing and the row says so");
            app.key(Key::Right);
            app.key(Key::Ok);
            const auto writes = layoutWrites(transport);
            check(writes.size() == 1 &&
                      writes[0].dump() == R"({"order":{"app":["b","h","a","c"]},"sort":{"app":"custom"}})",
                  "OK saves the new order, the hidden app where it was, and Your order");
            check(!app.snapshot()["editing"].boolean() && app.snapshot()["rowHead"].string() == "b,a,c",
                  "The row shows the saved order at once");
        }
        {
            FakeTransport transport;
            Application app(transport);
            editable(app, editCatalogue, 0);
            app.key(Key::Right);
            app.key(Key::Back);
            check(layoutWrites(transport).size() == 1 && !app.snapshot()["editing"].boolean(),
                  "Back ends editing and saves the move");
        }
        {
            FakeTransport transport;
            Application app(transport);
            editable(app, editCatalogue, 0);
            check(hasHit(app.scene(), "card:0:3"), "The hidden app is on the row while editing");
            app.key(Key::Ok);
            run(app, 3000, 400);
            check(layoutWrites(transport).empty() && !textNode(app.scene(), "Moving apps") &&
                      !hasHit(app.scene(), "card:0:3"),
                  "Ending with no change saves nothing, and the hidden app leaves the row");
        }
        {
            FakeTransport transport;
            Application app(transport);
            editable(app, editCatalogue, 0);
            app.key(Key::Red);
            const auto writes = layoutWrites(transport);
            check(writes.size() == 1 && writes[0].dump() == R"({"hidden":{"app":["h","a"]}})",
                  "Red takes the app off Home: it joins the hidden ones and the order is not sent");
            check(app.snapshot()["rowHead"].string() == "b,c" && app.snapshot()["focusCard"].string() == "b",
                  "The hidden app is gone from the row, the focus on the one after it");
            // Hiding on Home leaves the Apps page alone.
            app.key(Key::Up);
            app.key(Key::Right);
            app.key(Key::Ok);
            run(app, 4000, 1200);
            check(app.snapshot()["tab"].string() == "apps" && app.snapshot()["rowHead"].string() == "a,b,c",
                  "An app hidden on Home is still on the Apps page");
        }
        {
            // The button above the page hides the focused app (Up from the first row, then OK).
            FakeTransport transport;
            Application app(transport);
            editable(app, editCatalogue, 0);
            check(textNode(app.scene(), "Hide from Home"), "While editing, a button says Hide from Home");
            app.key(Key::Up);
            check(app.snapshot()["editing"].boolean() && app.snapshot()["editHide"].boolean(),
                  "Up from the row goes to the button, still editing");
            app.key(Key::Down);
            check(!app.snapshot()["editHide"].boolean(), "Down from the button returns to the app");
            app.key(Key::Up);
            app.key(Key::Ok);
            const auto writes = layoutWrites(transport);
            check(writes.size() == 1 && writes[0].dump() == R"({"hidden":{"app":["h","a"]}})",
                  "OK on the button hides the app, as Red does");
        }
        {
            // The apps hidden from Home follow the others while editing, and only take the focus.
            FakeTransport transport;
            Application app(transport);
            editable(app, editCatalogue, 0);
            app.key(Key::Right);
            app.key(Key::Right);
            app.key(Key::Right);
            check(textNode(app.scene(), "Show on Home"), "On a hidden app the button says Show on Home");
            app.key(Key::Left);
            app.key(Key::Right);
            app.key(Key::Right);
            check(textNode(app.scene(), "Show on Home") && layoutWrites(transport).empty(),
                  "A hidden app is not swapped into the order, and Right at the end stays on it");
            app.key(Key::Ok);
            const auto writes = layoutWrites(transport);
            check(writes.size() == 1 &&
                      writes[0].dump() ==
                          R"({"order":{"app":["b","c","a","h"]},"sort":{"app":"custom"},"hidden":{"app":[]}})",
                  "OK on a hidden app shows it again, after the app that was carried to the end");
            check(!textNode(app.scene(), "Show on Home") && app.snapshot()["rowHead"].string() == "b,c,a",
                  "The button goes when editing ends and the row lists the shown app");
        }
        {
            // LG Home has its own switch.
            const std::string withHome =
                replaced(replaced(editCatalogue, R"("order":{"app":["a",)", R"("order":{"app":["lg-home","a",)"),
                         R"("showLgHome":false)", R"("showLgHome":true)");
            FakeTransport transport;
            Application app(transport);
            editable(app, withHome, 0);
            check(app.snapshot()["focusCard"].string() == "lg-home", "Editing starts on the focused tile");
            app.key(Key::Red);
            check(layoutWrites(transport).empty() && !app.snapshot()["editing"].boolean(),
                  "LG Home cannot be hidden from the remote");
        }
        {
            // With the apps sorted by kind an app moves among its own kind; any move keeps the sort.
            const char *kindEdit =
                R"({"ok":true,"apps":[{"id":"a","title":"Alpha","kind":"video"},{"id":"b","title":"Beta","kind":"music"},{"id":"c","title":"Gamma","kind":"video"}],"devices":[],"channels":[],"kinds":[{"key":"video","label":"Movies & TV"},{"key":"music","label":"Music & audio"}],"order":{"app":["a","b","c"],"dev":[]},"layout":{"rows":["app"],"hidden":{"app":[]},"sort":{"app":"kind"},"showInputs":false,"showLgHome":false},"profiles":[{"id":"main","name":"Main","active":true}]})";
            FakeTransport transport;
            Application app(transport);
            editable(app, kindEdit, 0);
            app.key(Key::Right); // a and c are both Movies & TV: they swap
            app.key(Key::Right); // Beta is Music: the focus passes to it, nothing swaps
            check(app.snapshot()["focusCard"].string() == "b" && app.snapshot()["rowHead"].string() == "c,a,b",
                  "An app is swapped with the apps of its kind only");
            app.key(Key::Ok);
            const auto writes = layoutWrites(transport);
            check(writes.size() == 1 && writes[0].dump() == R"({"order":{"app":["c","a","b"]}})",
                  "A move among the apps of a kind saves their order and keeps the apps by kind");
        }
        {
            // The Apps page's grid has its own hidden apps: Home hides `h`, the Apps page `b`. While
            // editing it lists its hidden app, dimmed and last, to be shown again.
            const std::string pageCatalogue = replaced(editCatalogue, R"("hidden":{"app":["h"]})",
                                                       R"("hidden":{"app":["h"],"appsTab":["b"]})");
            FakeTransport transport;
            Application app(transport);
            editable(app, pageCatalogue, 1);
            check(app.snapshot()["editing"].boolean() && app.snapshot()["rowHead"].string() == "a,h,c" &&
                      hasHit(app.scene(), "card:0:3") && !hasHit(app.scene(), "card:0:4"),
                  "The Apps page lists the apps hidden on Home but not the ones hidden on it, until editing");
            check(textNode(app.scene(), "Hide from Apps"), "The Apps page's button says where it hides");
            app.key(Key::Red);
            const auto writes = layoutWrites(transport);
            check(writes.size() == 1 && writes[0].dump() == R"({"hidden":{"appsTab":["b","a"]}})" &&
                      app.snapshot()["rowHead"].string() == "h,c",
                  "Red on the Apps page hides the app there and sends only the Apps page's list");
            // The apps hidden on the Apps page do not change Home's row.
            app.key(Key::Back);
            run(app, 4000, 600);
            app.key(Key::Up);
            check(app.snapshot()["tab"].string() == "home" && app.snapshot()["rowHead"].string() == "a,b,c",
                  "An app hidden on the Apps page is still on Home's row");
        }
        {
            FakeTransport transport;
            Application app(transport);
            const std::string pageCatalogue = replaced(editCatalogue, R"("hidden":{"app":["h"]})",
                                                       R"("hidden":{"app":["h"],"appsTab":["b"]})");
            editable(app, pageCatalogue, 1);
            app.key(Key::Right);
            app.key(Key::Right);
            app.key(Key::Right);
            check(textNode(app.scene(), "Show on Apps"), "On an app hidden there the button says Show on Apps");
            app.key(Key::Ok);
            const auto writes = layoutWrites(transport);
            check(writes.size() == 1 &&
                      writes[0].dump() ==
                          R"({"order":{"app":["h","c","a","b"]},"sort":{"app":"custom"},"hidden":{"appsTab":[]}})" &&
                      app.snapshot()["rowHead"].string() == "h,c,a",
                  "OK on it shows the app on the Apps page again, after the apps that were carried to the end");
        }
        {
            // Holding OK anywhere but on an app does nothing; Back held is the only way to the
            // recent apps, and it is ignored while apps or rows are moved.
            FakeTransport transport;
            Application app(transport);
            app.update(0);
            reply(app, "catalogue", editCatalogue);
            reply(app, "discover", feed);
            run(app, 16, 1500);
            app.longPress(); // on a title of the top picks
            check(app.screenName() == "home" && !app.snapshot()["editing"].boolean() &&
                      transport.count("recents") == 0,
                  "Holding OK on a title does nothing");
            app.key(Key::Up);
            app.key(Key::Up); // the bar
            app.longPress();
            check(!app.snapshot()["editing"].boolean() && transport.count("recents") == 0,
                  "Holding OK on the bar does nothing");
            app.key(Key::Down);
            app.longPress();
            check(app.snapshot()["editing"].boolean(), "Holding OK on an app edits the apps");
            app.longBack();
            check(app.snapshot()["editing"].boolean() && transport.count("recents") == 0,
                  "Back held is ignored while apps are edited");
            app.key(Key::Back);
            app.longBack();
            check(app.screenName() == "quick:recents" && transport.count("recents") == 1,
                  "Back held opens the recent apps from Home");
        }

        // The Native Tv tile is this client, already in front while Home shows it: OK opens the
        // settings in the client and asks the server for no launch, wherever the tile is; the
        // other apps still launch.
        const std::string ownApps =
            R"("apps":[{"id":"com.tvweb.dashboard","title":"Native Tv","kind":"tools"},{"id":"a","title":"Alpha","kind":"video"}],"devices":[],"channels":[],"kinds":[{"key":"tools","label":"Tools"},{"key":"video","label":"Movies & TV"}],"order":{"app":["com.tvweb.dashboard","a"],"dev":[]},)";
        const std::string ownProfiles = R"("profiles":[{"id":"main","name":"Main","active":true}]})";
        for (int layoutCase = 0; layoutCase < 3; ++layoutCase) {
            // 0: the apps row on Home's first page; 1: the Apps tab's grid; 2: the Apps tab by kind.
            const std::string sort = layoutCase == 2 ? R"("sort":{"app":"kind"},)" : "";
            const std::string layout = R"("layout":{"rows":["app"],"hidden":{},)" + sort +
                                       R"("showInputs":false,"showLgHome":false},)";
            FakeTransport ownTransport;
            Application own(ownTransport);
            own.update(0);
            reply(own, "catalogue", R"({"ok":true,)" + ownApps + layout + ownProfiles);
            reply(own, "discover", fullFeed);
            uint32_t ownMs = run(own, 16, 1500);
            own.key(Key::Up); // to the apps row
            if (layoutCase) {
                own.key(Key::Up);
                own.key(Key::Right); // the Apps tab, after Home's
                own.key(Key::Ok);
                ownMs = run(own, ownMs, 1200);
                check(own.snapshot()["tab"].string() == "apps", "The Apps tab opens for the tile");
            }
            check(own.snapshot()["column"].number() == 0, "The Native Tv tile is the one focused");
            size_t launches = ownTransport.count("action");
            own.key(Key::Ok);
            check(own.screenName() == "dashboard" && ownTransport.count("action") == launches,
                  "OK on the Native Tv tile opens the settings and sends no launch");
            own.key(Key::Back);
            check(own.screenName() == "home", "Back from the settings returns to Home");
            run(own, ownMs, 600);
            own.key(layoutCase == 2 ? Key::Down : Key::Right); // by kind, Alpha has its own row
            own.key(Key::Ok);
            check(ownTransport.count("action") == launches + 1 &&
                      ownTransport.requests.back().operation == Operation::OpenHomeItem &&
                      ownTransport.requests.back().body["kind"].string() == "app" &&
                      ownTransport.requests.back().body["id"].string() == "a",
                  "OK on another app still launches it through the server");
        }

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
        for (const char *app : {"a", "b", "c", "d", "e", "f", "g", "h"})
            pictures.ready[std::string("app:") + app] = 3; // the apps' icons are not this check's
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
            askedCard = askedCard || id == "tmdb:w500:/b1.jpg@330x186";
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
        // The apps lead Home and enter on their icons, ahead of the feed: nothing is drawn until
        // the icons are in (a circle would enter as a colour and change under the eye), at most
        // 300 ms, and an icon the server has none of is not waited for.
        {
            FakeTransport iconTransport;
            Application iconed(iconTransport);
            FakePictures icons;
            iconed.attach(pictureGlyphs, icons);
            iconed.update(0);
            reply(iconed, "catalogue", fullCatalogue);
            uint32_t iconClock = run(iconed, 16, 200);
            check(!hasHit(iconed.scene(), "card:0:0"),
                  "The apps row is not drawn while its icons are on the way");
            bool askedIcon = false;
            for (const auto &id : icons.asked)
                askedIcon = askedIcon || id == "app:a@123x123";
            check(askedIcon, "The icons are asked for as soon as the rows exist, at the circles' size");
            for (const char *app : {"a", "b", "c", "d", "e", "f", "g", "h"})
                icons.ready[std::string("app:") + app] = 3;
            iconClock = run(iconed, iconClock, 300);
            check(hasHit(iconed.scene(), "card:0:0") && iconed.snapshot()["rows"].number() == 1,
                  "The apps enter on their icons, without the feed");
            reply(iconed, "discover", fullFeed);
            iconClock = run(iconed, iconClock, 300);
            check(iconed.snapshot()["rows"].number() == 3 && iconed.snapshot()["row"].number() == 1 &&
                      hasHit(iconed.scene(), "card:1:0"),
                  "The feed's rows enter when it answers and the focus lands on the top picks");

            FakeTransport lateTransport;
            Application late(lateTransport);
            FakePictures never;
            late.attach(pictureGlyphs, never);
            late.update(0);
            reply(late, "catalogue", fullCatalogue);
            uint32_t lateClock = run(late, 16, 250);
            check(!hasHit(late.scene(), "card:0:0"), "An icon that is slow holds its row back");
            run(late, lateClock, 500);
            check(hasHit(late.scene(), "card:0:0"),
                  "The apps enter without an icon that does not arrive within 300 ms");

            FakeTransport noneTransport;
            Application none(noneTransport);
            FakePictures missing;
            for (const char *app : {"a", "b", "c", "d", "e", "f", "g", "h"})
                missing.failed.insert(std::string("app:") + app);
            none.attach(pictureGlyphs, missing);
            none.update(0);
            reply(none, "catalogue", fullCatalogue);
            run(none, 16, 250);
            check(hasHit(none.scene(), "card:0:0"), "An icon the server has none of is not waited for");
        }
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
