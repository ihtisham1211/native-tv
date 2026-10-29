// The Quick drawer, Profiles, title details and the search and profile editors, driven through the
// Application as the remote drives them: keys in, requests and replies at the Services seam, the
// scene and the snapshot read back.
#include "ui/application.hpp"
#include "ui/kit/overlay.hpp"
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
        bool write;
    };
    std::vector<Request> requests;
    std::map<std::string, unsigned> revisions;
    bool refuse = false; // a full queue: the request is not taken
    unsigned read(const std::string &channel, Operation operation, const Json &arguments = Json()) override {
        if (refuse)
            return 0;
        requests.push_back({channel, operation, arguments, false});
        return ++revisions[channel];
    }
    unsigned write(const std::string &channel, Operation operation, const Json &body) override {
        if (refuse)
            return 0;
        requests.push_back({channel, operation, body, true});
        return ++revisions[channel];
    }
    void cancel(const std::string &channel) override {
        ++revisions[channel];
    }
    size_t count(const std::string &channel) const {
        size_t total = 0;
        for (const auto &request : requests)
            if (request.channel == channel)
                ++total;
        return total;
    }
    const Request &last(const std::string &channel) const {
        for (size_t i = requests.size(); i-- > 0;)
            if (requests[i].channel == channel)
                return requests[i];
        throw std::runtime_error("no request on " + channel);
    }
};
uint32_t run(Application &app, uint32_t from, uint32_t ms) {
    uint32_t at = from;
    for (uint32_t step = 0; step <= ms * 60 / 1000; ++step) {
        at = from + step * 1000 / 60;
        app.update(at);
    }
    return at;
}
bool hasHit(const Scene &scene, const std::string &id) {
    for (const auto &hit : scene.hits)
        if (hit.id == id)
            return true;
    return false;
}

const char *catalogueJson =
    R"({"ok":true,"apps":[{"id":"a","title":"Alpha"}],"devices":[{"id":"hdmi1","name":"Console","port":1},{"id":"hdmi2","name":"Stick","port":2,"unused":true}],"channels":[],"order":{"app":["a"],"dev":["hdmi1","hdmi2"]},"layout":{"rows":["app","dev"],"hidden":{},"showInputs":true,"clockFormat":"12h"},"profiles":[{"id":"main","name":"Main","active":true},{"id":"guest","name":"Guest","active":false}]})";
const char *feedJson =
    R"({"ok":true,"enabled":true,"titlesEnabled":true,"rows":[{"key":"trending","items":[{"id":7,"type":"movie","title":"Coast","overview":"A journey."},{"id":8,"type":"tv","title":"Harbor"},{"id":9,"type":"movie","title":"Orbit"}]}]})";
const char *statsJson =
    R"({"ok":true,"volume":24,"muted":false,"oled":{"care":true},"sleepTimer":"off","picture":{"mode":"Cinema","mode_raw":"cinema","backlight":70,"modes":[{"value":"vivid","label":"Vivid"},{"value":"cinema","label":"Cinema"},{"value":"game","label":"Game"}]},"sound":{"output":"TV Speaker","output_raw":"tv_speaker","outputs":[{"id":"tv_speaker","name":"TV Speaker"},{"id":"arc","name":"HDMI ARC"}]}})";
const char *detailsJson =
    R"({"ok":true,"enabled":true,"netflixEnabled":true,"item":{"id":7,"type":"movie","title":"Coast","overview":"A journey across the water.","runtime":121,"trailer":"dQw4w9WgXcQ","cast":[{"name":"Ada"},{"name":"Ben"}],"similar":[{"id":20,"type":"movie","title":"Horizon"},{"id":21,"type":"tv","title":"Tide"}]}})";

// An application with Home's data in, its entrance played and the clock at a known time.
struct World {
    FakeTransport transport;
    Application app;
    uint32_t clock = 0;
    explicit World(const char *catalogue = catalogueJson) : app(transport) {
        app.update(0);
        reply("catalogue", catalogue, 0);
        reply("discover", feedJson, 0);
        clock = run(app, 16, 1500);
    }
    // The answer to the latest request on a channel (generation 0 asks for none in particular).
    void reply(const std::string &channel, const std::string &json, long status = 200,
               int generation = -1) {
        Response response;
        response.channel = channel;
        response.status = status ? status : 200;
        response.bytes = json;
        response.generation = generation >= 0 ? static_cast<unsigned>(generation)
                                              : transport.revisions[channel];
        app.response(response);
    }
    void settle(uint32_t ms) {
        clock = run(app, clock, ms);
    }
    void press(Key key, uint32_t after = 0) {
        app.key(key);
        if (after)
            settle(after);
    }
    void press(std::initializer_list<Key> keys) {
        for (Key key : keys)
            press(key);
    }
    Json snapshot() {
        return app.snapshot();
    }
    std::string screen() {
        return app.screenName();
    }
    double motion(const std::string &id) {
        return app.animation().value(id);
    }
    // The toast is a tween: it shows after a few frames.
    bool toasted(const std::string &text) {
        settle(120);
        return app.shows(text);
    }
    void openQuick() {
        check(app.activate("tool:1"), "The gear is on the bar");
        reply("quick:stats", statsJson);
        reply("quick:now", R"({"ok":true,"kind":"input","port":1})");
        settle(900);
    }
    void openProfiles() {
        check(app.activate("tool:0"), "The avatar is on the bar");
        settle(500);
    }
    void openSearch() {
        check(app.activate("tool:2"), "The search button is on the bar");
        settle(500);
    }
    void openDetails(const char *answer = detailsJson) {
        press(Key::Ok);
        reply("title:details", answer);
        settle(500);
    }
};
} // namespace

// ---- Kit parts --------------------------------------------------------------------------------

Rect hitOf(const Scene &scene, const std::string &id) {
    for (const auto &hit : scene.hits)
        if (hit.id == id)
            return hit.rect;
    return {};
}
void kitChecks() {
    kit::LegacyGlyphs glyphs;
    {
        Scene scene;
        kit::PillButton button;
        button.rest = {100, 100, 300, 64};
        button.id = "pill";
        button.label = "Open";
        kit::pillButton({scene, glyphs}, button);
        check(hitOf(scene, "pill").w == 300, "A button at rest is its own size");
        Scene focused;
        button.focus = 1;
        kit::pillButton({focused, glyphs}, button);
        Rect lifted = hitOf(focused, "pill");
        check(lifted.w > 310 && lifted.w < 325 && lifted.x < 100, "A focused button grows about its centre");
        bool ring = false;
        for (const auto &node : focused.nodes)
            ring = ring || node.ring.outer > 0;
        check(ring, "A focused button has its ring");
        Scene hidden;
        button.enter = {0, 28};
        kit::pillButton({hidden, glyphs}, button);
        check(hidden.nodes.empty() && hidden.hits.empty(), "A button that has not entered draws nothing");
    }
    {
        Scene scene;
        kit::veil({scene, glyphs}, 0);
        check(scene.nodes.empty(), "A veil at zero draws nothing");
        kit::veil({scene, glyphs}, .5f);
        check(scene.nodes.size() == 1 && scene.nodes[0].rect.w == 1920 && scene.nodes[0].color.a == .5f,
              "A veil is one full-screen quad");
    }
    {
        Scene off, on;
        kit::radio({off, glyphs}, {0, 0, 28, 28}, 0);
        kit::radio({on, glyphs}, {0, 0, 28, 28}, 1);
        check(off.nodes.size() == 1 && on.nodes.size() == 2, "A radio mark has a dot only when it is on");
    }
    {
        Scene plain, loading;
        kit::StatusPanel panel;
        panel.rest = {1000, 200, 800, 380};
        panel.title = "Searching…";
        kit::statusPanel({plain, glyphs}, panel);
        panel.sweep = .5f;
        kit::statusPanel({loading, glyphs}, panel);
        check(loading.nodes.size() == plain.nodes.size() + 2, "A loading panel adds its bar and the light on it");
        panel.sweep = 0;
        Scene start;
        kit::statusPanel({start, glyphs}, panel);
        check(start.nodes.size() == plain.nodes.size() + 1, "The light is not drawn before it has entered the bar");
    }
    {
        Scene scene;
        float width = kit::keyHint({scene, glyphs}, 80, 1000, "Back", "Return", "hint");
        Rect box = hitOf(scene, "hint");
        check(width > 60 && std::fabs(box.w - width) < .5f && box.x == 80, "A hint is as wide as it says and clickable");
    }
}

// ---- Details ----------------------------------------------------------------------------------

void detailsChecks() {
    World world;
    world.press(Key::Ok);
    check(world.screen() == "details", "OK on a title opens its details");
    check(world.transport.count("action") == 0 && world.transport.count("title:details") == 1,
          "Opening details reads them and launches nothing");
    check(world.transport.last("title:details").operation == Operation::TitleDetails &&
              world.transport.last("title:details").body["id"].string() == "7" &&
              world.transport.last("title:details").body["type"].string() == "movie",
          "The details are read by the title's type and id");
    check(world.app.shows("Loading details…") && world.app.shows("Coast") && world.app.shows("A journey."),
          "The card's own lines show while the details load");
    check(world.snapshot()["contentState"].string() == "loading", "Details start in their loading state");

    world.reply("title:details", detailsJson);
    world.settle(600);
    check(world.snapshot()["contentState"].string() == "ready" && world.snapshot()["focus"].string() == "Open" &&
              world.app.shows("A journey across the water.") && world.app.shows("Cast: Ada, Ben") &&
              world.app.shows("More like this"),
          "A reply fills the page, with Open focused and the similar titles below");
    world.press(Key::Right);
    check(world.snapshot()["focus"].string() == "Trailer", "Trailer follows Open");
    world.reply("title:netflix", R"({"ok":true,"netflixId":"80057281"})");
    check(world.snapshot()["focus"].string() == "Trailer",
          "A late Netflix link keeps the focus on the action that had it");
    world.press(Key::Left);
    check(world.snapshot()["focus"].string() == "Open on Netflix",
          "The Netflix action is inserted before Trailer");
    world.press(Key::Ok);
    check(world.transport.count("title:open") == 1 && world.transport.last("title:open").body["kind"].string() == "netflix" &&
              !world.transport.last("title:open").body.has("netflixId") &&
              world.transport.last("title:open").body["id"].string() == "7",
          "Open on Netflix posts the title and leaves the mapping to the server");
    world.press(Key::Ok);
    check(world.transport.count("title:open") == 1, "A second OK while it is opening is ignored");
    world.reply("title:open", R"({"ok":false,"error":"Refused"})", 400);
    check(world.toasted("That did not open: Refused"), "A refused open says so, with the daemon's reason");
    world.press(Key::Left);
    world.press(Key::Ok);
    check(world.transport.count("title:open") == 2 && world.transport.last("title:open").body["kind"].string() == "title",
          "Open posts the title to the open route once the first reply is in");
    world.reply("title:open", R"({"ok":true})");
    world.press(Key::Right);
    world.press(Key::Right);
    check(world.snapshot()["focus"].string() == "Trailer", "Trailer is reachable");
    world.press(Key::Ok);
    check(world.transport.count("title:open") == 3 && world.transport.last("title:open").body["kind"].string() == "youtube" &&
              world.transport.last("title:open").body["id"].string() == "dQw4w9WgXcQ",
          "Trailer opens the video");

    // Similar titles open on top of this one; Back peels them off.
    world.press(Key::Down);
    check(world.snapshot()["focus"].string() == "Horizon", "Down moves to the similar titles");
    world.press(Key::Right);
    world.press(Key::Ok);
    check(world.snapshot()["depth"].number() == 2 && world.transport.last("title:details").body["id"].string() == "21",
          "A similar title opens as a second level and is read");
    world.press(Key::Back);
    check(world.screen() == "details" && world.snapshot()["depth"].number() == 1 &&
              world.snapshot()["titleId"].string() == "7",
          "Back returns to the title underneath");
    world.press(Key::Back);
    check(world.screen() == "home" && world.snapshot()["column"].number() == 0,
          "Back from the first title returns to Home as it was");

    // A reply for a title that was left behind does not fill the one on screen.
    World stale;
    stale.press(Key::Ok);
    unsigned first = stale.transport.revisions["title:details"];
    stale.reply("title:details", detailsJson, 200, static_cast<int>(first) - 1);
    check(stale.snapshot()["contentState"].string() == "loading", "A reply to an older request is ignored");
}

void detailsFailureChecks() {
    World world;
    world.press(Key::Ok);
    world.reply("title:details", R"({"ok":false,"error":"Metadata is warming up"})", 503);
    world.settle(300);
    check(world.screen() == "details" && world.snapshot()["contentState"].string() == "error" &&
              world.app.shows("Metadata is warming up") && world.app.shows("Retry"),
          "A failed read keeps the page, says why and offers Retry");
    for (int i = 0; i < 4; ++i)
        world.press(Key::Right);
    check(world.snapshot()["focus"].string() == "Retry", "Retry is the last action");
    world.press(Key::Ok);
    check(world.transport.count("title:details") == 2 && world.snapshot()["contentState"].string() == "loading",
          "Retry reads the details again without leaving");
    world.reply("title:details", R"({"ok":true,"enabled":true,"netflixEnabled":false,"item":{"id":7,"type":"movie","title":"Coast","overview":""}})");
    world.settle(300);
    check(world.snapshot()["contentState"].string() == "ready" && world.app.shows("No overview available"),
          "A title without an overview says so");
    World stale;
    stale.press(Key::Ok);
    stale.reply("title:details", R"({"ok":true,"enabled":true,"stale":true,"item":{"id":7,"type":"movie","title":"Coast","overview":"Kept."}})");
    check(stale.snapshot()["contentStatus"].string() == "Saved details · offline",
          "Details served from the saved copy say they are offline");
    World busy;
    busy.transport.refuse = true;
    busy.press(Key::Ok);
    check(busy.snapshot()["contentState"].string() == "error",
          "A request the transport cannot take is an error, not a spinner");
}

void detailsOverviewChecks() {
    World world;
    world.openDetails();
    world.press({Key::Right, Key::Right});
    check(world.snapshot()["focus"].string() == "Full overview", "Full overview is an action");
    world.press(Key::Ok);
    world.settle(300);
    check(world.snapshot()["overview"].boolean() && world.app.shows("A journey across the water."),
          "The full overview opens as a sheet over the page");
    world.press(Key::Down);
    check(world.snapshot()["overviewPage"].number() == 0, "A short overview has one page");
    world.press(Key::Back);
    check(world.screen() == "details" && !world.snapshot()["overview"].boolean(),
          "Back closes the sheet and stays on the title");
    world.press(Key::Back);
    check(world.screen() == "home", "Back again leaves the title");
    // A long overview pages by words, never in the middle of one.
    std::string long_text;
    for (int i = 0; i < 400; ++i)
        long_text += "wordy ";
    World paged;
    paged.press(Key::Ok);
    paged.reply("title:details", std::string(R"({"ok":true,"enabled":true,"item":{"id":7,"type":"movie","title":"Coast","overview":")") +
                                     long_text + R"("}})");
    paged.press({Key::Right});
    paged.press(Key::Ok);
    paged.press(Key::PageDown);
    check(paged.snapshot()["overviewPage"].number() == 1, "A long overview has more than one page");
    paged.press(Key::PageUp);
    check(paged.snapshot()["overviewPage"].number() == 0, "Page up goes back");
}

void detailsDepthChecks() {
    World world;
    world.openDetails();
    for (int i = 0; i < 7; ++i) {
        world.press(Key::Down);
        world.press(Key::Ok);
        world.reply("title:details", detailsJson);
    }
    check(world.snapshot()["depth"].number() == 8, "Titles open up to eight deep");
    world.press(Key::Down);
    world.press(Key::Ok);
    check(world.snapshot()["depth"].number() == 8 && world.toasted("Go back before opening another title"),
          "A ninth title is refused with a message");
}

void detailsFocusReturnChecks() {
    World world;
    world.press({Key::Right, Key::Right});
    world.settle(300);
    check(world.snapshot()["column"].number() == 2, "Home focus moved to the third card");
    world.press(Key::Ok);
    check(world.screen() == "details", "The third card opens");
    world.press(Key::Back);
    check(world.screen() == "home" && world.snapshot()["column"].number() == 2,
          "Back lands on the card that opened the details");
}

// ---- Profiles ---------------------------------------------------------------------------------

void profilesChecks() {
    World world;
    world.openProfiles();
    check(world.screen() == "profiles" && world.snapshot()["selected"].number() == 0,
          "The avatar opens the profile page on the profile in use");
    world.press(Key::Right);
    world.press(Key::Ok);
    check(world.transport.count("profiles:set") == 1 && world.transport.last("profiles:set").operation == Operation::SetProfile &&
              world.transport.last("profiles:set").body["action"].string() == "switch" &&
              world.transport.last("profiles:set").body["id"].string() == "guest",
          "OK on another profile posts a switch to it");
    world.press(Key::Ok);
    check(world.transport.count("profiles:set") == 1 && world.snapshot()["busy"].boolean(),
          "A second OK while switching is ignored");
    world.reply("profiles:set", R"({"ok":false,"error":"Not allowed"})", 403);
    check(world.screen() == "profiles" && world.toasted("That did not work: Not allowed"),
          "A refused switch stays on the page and says why");
    world.press(Key::Ok);
    world.reply("profiles:set", R"({"ok":true})");
    world.settle(500);
    check(world.screen() == "home" && world.transport.count("catalogue") >= 3,
          "A switch reads the layout again and starts Home over");
    // Add profile, and OK on the one in use.
    World adding;
    adding.openProfiles();
    adding.press({Key::Right, Key::Right});
    adding.press(Key::Ok);
    check(adding.transport.last("profiles:set").body["action"].string() == "add" &&
              adding.transport.last("profiles:set").body["switch"].boolean(),
          "Add profile posts an add that switches to it");
    adding.reply("profiles:set", R"({"ok":true})");
    check(adding.toasted("Profile added"), "A new profile is announced");
    World same;
    same.openProfiles();
    same.press(Key::Ok);
    check(same.screen() == "home" && same.transport.count("profiles:set") == 0,
          "OK on the profile in use only closes the page");
    World back;
    back.openProfiles();
    back.press(Key::Back);
    check(back.screen() == "home", "Back returns Home");
    World rename;
    rename.openProfiles();
    rename.press(Key::Down);
    rename.press(Key::Ok);
    check(rename.screen() == "rename", "Rename profile opens the editor");
    rename.press(Key::Back);
    check(rename.screen() == "profiles", "Back from the editor returns to the profiles");
}

// ---- Editors ----------------------------------------------------------------------------------

void searchChecks() {
    World world;
    world.openSearch();
    check(world.screen() == "search" && world.app.shows("Enter at least 2 characters"),
          "The search button opens the editor with its prompt");
    world.app.input("s");
    world.settle(500);
    check(world.transport.count("title:search") == 0, "A single character searches nothing");
    world.app.input("t");
    world.settle(300);
    check(world.transport.count("title:search") == 0 && world.snapshot()["contentState"].string() == "loading",
          "The search waits for its debounce");
    world.settle(200);
    check(world.transport.count("title:search") == 1 && world.transport.last("title:search").body["query"].string() == "st",
          "One request goes after the last key");
    world.reply("title:search", R"({"ok":true,"enabled":true,"items":[{"id":7,"type":"movie","title":"Coast"},{"id":8,"type":"tv","title":"Harbor"}]})");
    world.settle(300);
    check(world.snapshot()["contentState"].string() == "ready" && world.snapshot()["results"].number() == 2 &&
              world.app.shows("2 results"),
          "Results replace the prompt");
    // Done moves to the results; OK opens one on top of the search.
    check(world.app.activate("editor:kb:4:4"), "The Done key is on the keyboard");
    check(world.snapshot()["zone"].string() == "results", "Done with results moves to them");
    world.press(Key::Ok);
    check(world.screen() == "details" && world.snapshot()["origin"].string() == "search",
          "OK on a result opens its details");
    world.press(Key::Back);
    check(world.screen() == "search" && world.snapshot()["results"].number() == 2 &&
              world.snapshot()["query"].string() == "st" && world.snapshot()["zone"].string() == "results",
          "Back from the details restores the search as it was");
    world.press(Key::Back);
    check(world.snapshot()["zone"].string() == "keys", "Back from the results returns to the keyboard");
    unsigned before = world.transport.revisions["title:search"];
    world.press(Key::Back);
    check(world.screen() == "home" && world.transport.revisions["title:search"] > before,
          "Back from the keyboard returns Home and invalidates a pending reply");
}

void searchStateChecks() {
    World world;
    world.openSearch();
    world.app.input("Wa");
    world.settle(400);
    unsigned first = world.transport.revisions["title:search"];
    world.reply("title:search", R"({"ok":false,"error":"Search service is starting"})", 503);
    world.settle(300);
    Scene error = world.app.scene();
    check(world.snapshot()["contentState"].string() == "error" && world.snapshot()["query"].string() == "Wa" &&
              world.app.shows("Search is unavailable. Try again.") && hasHit(error, "editor:retry"),
          "A failed search keeps the query and offers a retry");
    world.app.activate("editor:retry");
    check(world.transport.count("title:search") == 2 && world.snapshot()["contentState"].string() == "loading",
          "Retry asks again without leaving the editor");
    world.reply("title:search", R"({"ok":true,"enabled":true,"items":[]})");
    world.settle(300);
    check(world.snapshot()["contentState"].string() == "empty" && world.app.shows("No films or series found"),
          "An empty answer has its own state");
    world.reply("title:search", R"({"ok":true,"enabled":true,"items":[{"id":1,"type":"movie","title":"Late"}]})", 200,
                static_cast<int>(first));
    check(world.snapshot()["results"].number() == 0, "A reply to an older search is dropped");
    world.press(Key::Up);
    world.press(Key::Up);
    world.app.activate("editor:clear");
    check(world.snapshot()["query"].string().empty() && world.app.shows("Enter at least 2 characters to see matching titles."),
          "Clear empties the field and brings back the prompt");
    world.reply("title:search", R"({"ok":true,"enabled":false,"items":[]})");
    World disabled;
    disabled.openSearch();
    disabled.app.input("ab");
    disabled.settle(500);
    disabled.reply("title:search", R"({"ok":true,"enabled":false,"items":[]})");
    check(disabled.snapshot()["contentState"].string() == "error" && disabled.app.shows("Search is unavailable"),
          "Search turned off on the server is reported");
    World busy;
    busy.openSearch();
    busy.transport.refuse = true;
    busy.app.input("ab");
    busy.settle(500);
    check(busy.snapshot()["contentState"].string() == "error", "A request the transport cannot take is an error");
}

void searchGridChecks() {
    World world;
    world.openSearch();
    world.app.input("ab");
    world.settle(500);
    world.reply("title:search", R"({"ok":true,"enabled":true,"items":[{"id":1,"type":"movie","title":"A"},{"id":2,"type":"movie","title":"B"},{"id":3,"type":"movie","title":"C"},{"id":4,"type":"movie","title":"D"},{"id":5,"type":"movie","title":"E"}]})");
    world.press(Key::Down);
    world.press(Key::Down);
    world.press(Key::Down);
    check(world.snapshot()["zone"].string() == "keys" && world.snapshot()["focusedKey"].string() == "symbols",
          "Down along the keyboard reaches its last row");
    world.press(Key::Down);
    check(world.snapshot()["zone"].string() == "clear", "Down from the last row reaches the Clear button");
    world.press(Key::Up);
    check(world.snapshot()["zone"].string() == "keys", "Up returns to the keyboard");
    world.app.activate("editor:kb:4:4");
    world.press(Key::Down);
    check(world.snapshot()["selected"].number() == 2, "Down follows the results' two columns");
    world.press(Key::Right);
    check(world.snapshot()["selected"].number() == 3, "Right selects the neighbour");
    world.press(Key::Down);
    check(world.snapshot()["selected"].number() == 4, "Down to a shorter last row lands on its last card");
    world.settle(900);
    check(std::fabs(world.motion("editor:rs") + 254) < .5,
          "The grid scrolls a row up once the focus is on its third row");
    world.press(Key::Up);
    world.press(Key::Left);
    check(world.snapshot()["zone"].string() == "keys", "Left from the first column returns to the keyboard");
    world.settle(900);
    check(std::fabs(world.motion("editor:rs")) < .5, "The grid returns to the top with the focus");
}

void keyboardChecks() {
    World world;
    world.openSearch();
    // The key under the focus is typed with OK; the focus moves by the nearest key.
    check(world.app.activate("editor:kb:1:4"), "The keys are targets");
    check(world.snapshot()["query"].string() == "t", "A key is typed");
    world.press(Key::Down);
    world.press(Key::Down);
    world.press(Key::Right);
    world.press(Key::Ok);
    check(world.snapshot()["query"].string().size() == 2, "Arrow keys move over the keyboard and OK types");
    world.press(Key::Delete);
    check(world.snapshot()["query"].string() == "t", "The Delete key erases a character");
    world.app.input("héllo");
    check(world.snapshot()["query"].string() == "théllo", "Typed text is inserted whole");
    world.app.activate("editor:kb:3:0");
    world.app.activate("editor:kb:2:0");
    check(world.snapshot()["query"].string() == "théllo" + std::string("A"),
          "Shift turns the next letter to capitals");
    world.app.activate("editor:kb:4:0");
    check(world.app.activate("editor:kb:1:0") && world.snapshot()["query"].string().back() == '!',
          "The 123? key switches to symbols");
}

void renameChecks() {
    World world;
    world.openProfiles();
    world.press(Key::Down);
    world.press(Key::Ok);
    check(world.screen() == "rename" && world.snapshot()["query"].string() == "Main",
          "Rename starts with the current name");
    world.app.activate("editor:clear");
    check(world.snapshot()["contentState"].string() == "empty" && world.app.shows("Enter a name"),
          "An empty name is explained");
    world.app.activate("editor:kb:4:4");
    check(world.transport.count("editor:save") == 0, "An empty name is not sent");
    world.app.input("Sasha");
    world.app.activate("editor:kb:4:4");
    check(world.transport.count("editor:save") == 1 && world.transport.last("editor:save").operation == Operation::SetProfile &&
              world.transport.last("editor:save").body["action"].string() == "rename" &&
              world.transport.last("editor:save").body["id"].string() == "main" &&
              world.transport.last("editor:save").body["name"].string() == "Sasha" &&
              world.snapshot()["contentState"].string() == "loading",
          "Save posts the new name to the profile route");
    world.app.input("!");
    world.press(Key::Delete);
    check(world.snapshot()["query"].string() == "Sasha", "The name cannot change while it saves");
    world.reply("editor:save", R"({"ok":false,"error":"Profile service is starting"})", 503);
    check(world.screen() == "rename" && world.snapshot()["contentState"].string() == "error" &&
              world.snapshot()["query"].string() == "Sasha" && world.app.shows("Profile service is starting") &&
              world.app.shows("Retry"),
          "A refused save keeps the name and offers Retry");
    world.app.activate("editor:kb:4:4");
    check(world.transport.count("editor:save") == 2, "Retry posts the name again");
    world.reply("editor:save", R"({"ok":true})");
    world.settle(300);
    check(world.screen() == "profiles" && world.transport.count("catalogue") >= 3,
          "A saved name reads the catalogue and returns to the profiles");
    World limit;
    limit.openProfiles();
    limit.press(Key::Down);
    limit.press(Key::Ok);
    limit.app.input("aaaaaaaaaaaaaaaa");
    limit.app.input("b");
    check(limit.snapshot()["query"].string() == "Mainaaaaaaaaaaaaaaaa", "A name is limited to twenty characters");
}

// ---- Quick drawer -----------------------------------------------------------------------------

void quickChecks() {
    World world;
    check(world.app.activate("tool:1"), "The gear opens the drawer");
    check(world.screen() == "quick" && world.transport.count("quick:stats") == 1 && world.transport.count("quick:now") == 1,
          "The drawer reads what the TV is on when it opens");
    check(world.motion("quick:x") > 700, "The drawer starts off the right edge");
    double previous = world.motion("quick:x");
    bool monotonic = true;
    for (int frame = 0; frame < 40; ++frame) {
        world.clock += 17;
        world.app.update(world.clock);
        double now = world.motion("quick:x");
        monotonic = monotonic && now <= previous + 1e-6 && now >= -.01;
        previous = now;
    }
    check(monotonic && previous < .5, "The drawer springs in without overshoot");
    world.reply("quick:stats", statsJson);
    world.reply("quick:now", R"({"ok":true,"kind":"input","port":1})");
    world.settle(300);
    check(world.snapshot()["volume"].number() == 24 && world.snapshot()["light"].number() == 70 &&
              world.snapshot()["rows"].number() == 11 && world.snapshot()["focus"].string() == "Volume",
          "The drawer shows the volume and light the TV reported");
    check(world.app.shows("Picture mode") && world.app.shows("Cinema") && world.app.shows("OLED light") &&
              world.app.shows("Console"),
          "The rows carry what is set now");
    // A stale reply is dropped.
    world.reply("quick:stats", R"({"ok":true,"volume":99})", 200, 0);
    check(world.snapshot()["volume"].number() == 24, "A reply to an older read is dropped");
}

void quickVolumeChecks() {
    World world;
    world.openQuick();
    for (int i = 0; i < 5; ++i)
        world.press(Key::Right);
    check(world.snapshot()["volume"].number() == 29, "The volume moves at once on screen");
    check(world.transport.count("quick:vol") == 1 && world.transport.last("quick:vol").operation == Operation::Control &&
              world.transport.last("quick:vol").body["action"].string() == "volumeStep" &&
              world.transport.last("quick:vol").body["value"].number() == 1,
          "The first step goes out at once");
    world.reply("quick:vol", R"({"ok":true})");
    check(world.transport.count("quick:vol") == 2 && world.transport.last("quick:vol").body["value"].number() == 4,
          "The steps pressed meanwhile go in one request");
    world.press(Key::Left);
    check(world.transport.count("quick:vol") == 2, "Nothing goes while a step is on its way");
    world.reply("quick:vol", R"({"ok":true})");
    check(world.transport.count("quick:vol") == 3 && world.transport.last("quick:vol").body["value"].number() == -1,
          "A step the other way goes after it");
    world.reply("quick:vol", R"({"ok":true})");
    check(world.transport.count("quick:vol") == 3, "No more requests once nothing is left to send");
    size_t reads = world.transport.count("quick:stats");
    world.settle(800);
    check(world.transport.count("quick:stats") == reads + 1, "The TV is read again shortly after the last change");
    // A refused step is reported and the volume read back.
    world.press(Key::Right);
    size_t after = world.transport.count("quick:stats");
    world.reply("quick:vol", R"({"ok":false,"error":"Volume locked"})", 409);
    world.settle(100);
    check(world.transport.count("quick:stats") > after, "A refused step reads the volume back");
    check(world.toasted("That did not work: Volume locked"), "A refused volume step says why");
    // The ends of the range.
    World ends;
    ends.openQuick();
    for (int i = 0; i < 30; ++i)
        ends.press(Key::Left);
    check(ends.snapshot()["volume"].number() == 0, "The volume stops at zero");
    // A pending step goes when the drawer closes.
    World closing;
    closing.openQuick();
    closing.press(Key::Right);
    closing.press(Key::Right);
    closing.press(Key::Back);
    closing.settle(900);
    check(closing.screen() == "home" && closing.transport.count("quick:vol") == 2 &&
              closing.transport.last("quick:vol").body["value"].number() == 1,
          "A step not yet sent is sent as the drawer closes");
}

void quickMuteAndLightChecks() {
    World world;
    world.openQuick();
    world.press(Key::Ok);
    check(world.transport.last("quick:mute").body["action"].string() == "mute" &&
              world.transport.last("quick:mute").body["value"].boolean() && world.snapshot()["muted"].boolean(),
          "OK on the volume toggles mute");
    world.reply("quick:mute", R"({"ok":false,"error":"Nope"})", 500);
    check(!world.snapshot()["muted"].boolean(), "A refused mute is taken back");
    // The light waits for the key to rest.
    world.press(Key::Down);
    world.press(Key::Down);
    check(world.snapshot()["focus"].string() == "OLED light", "The light is the third row");
    world.press(Key::Right);
    world.press(Key::Right);
    world.settle(200);
    check(world.transport.count("quick:light") == 0 && world.snapshot()["light"].number() == 80,
          "The light moves on screen while the request waits");
    world.settle(300);
    check(world.transport.count("quick:light") == 1 && world.transport.last("quick:light").body["action"].string() == "backlight" &&
              world.transport.last("quick:light").body["value"].number() == 80,
          "One request carries the final value");
}

void quickLevelChecks() {
    World world;
    world.openQuick();
    world.press(Key::Down);
    world.press(Key::Ok);
    check(world.screen() == "quick:picture" && world.snapshot()["focus"].string() == "Cinema",
          "Picture mode opens its list on the mode in force");
    world.press(Key::Down);
    world.press(Key::Ok);
    check(world.transport.last("quick:set").body["action"].string() == "pictureMode" &&
              world.transport.last("quick:set").body["value"].string() == "game",
          "Choosing a mode posts it");
    world.press(Key::Ok);
    check(world.transport.count("quick:set") == 1, "A second choice waits for the first to answer");
    world.reply("quick:set", R"({"ok":true})");
    world.settle(300);
    check(world.screen() == "quick" && world.snapshot()["focus"].string() == "Picture mode" && world.app.shows("Game"),
          "The list closes on its row, with the new mode shown");
    // Back and Left return to the main list; a refusal stays.
    world.press(Key::Ok);
    world.press(Key::Left);
    check(world.screen() == "quick", "Left returns from a list");
    world.press(Key::Down);
    world.press(Key::Down);
    world.press(Key::Ok);
    check(world.screen() == "quick:sound", "Sound output opens its list");
    world.press(Key::Down);
    world.press(Key::Ok);
    world.reply("quick:set", R"({"ok":false,"error":"Not connected"})", 502);
    check(world.screen() == "quick:sound" && world.toasted("That did not work: Not connected"),
          "A refused choice stays on its list and says why");
    world.press(Key::Back);
    check(world.screen() == "quick" && world.snapshot()["focus"].string() == "Sound output", "Back returns to the main list");
    // Sleep timer.
    world.press(Key::Down);
    world.press(Key::Down);
    world.press(Key::Ok);
    check(world.screen() == "quick:sleep" && world.snapshot()["focus"].string() == "Off", "Sleep timer opens on Off");
    world.press({Key::Down, Key::Down});
    world.press(Key::Ok);
    check(world.transport.last("quick:set").body["action"].string() == "sleepTimer" &&
              world.transport.last("quick:set").body["value"].string() == "30",
          "A sleep time posts its minutes");
}

void quickInputChecks() {
    World world;
    world.openQuick();
    for (int i = 0; i < 4; ++i)
        world.press(Key::Down);
    world.press(Key::Ok);
    check(world.screen() == "quick:input" && world.snapshot()["focus"].string() == "Console",
          "Input lists the devices, on the one in use");
    world.press(Key::Down);
    world.settle(400);
    check(world.snapshot()["focus"].string() == "HDMI 2" && world.app.shows("Nothing connected"),
          "A port with nothing on it is named by its port");
    world.press(Key::Up);
    world.press(Key::Ok);
    check(world.transport.last("quick:act").operation == Operation::OpenHomeItem &&
              world.transport.last("quick:act").body["kind"].string() == "dev" &&
              world.transport.last("quick:act").body["id"].string() == "hdmi1",
          "Choosing an input opens it");
    world.reply("quick:act", R"({"ok":true})");
    world.settle(900);
    check(world.screen() == "home", "The drawer closes once the input is open");
}

void quickSwitchChecks() {
    World world;
    world.openQuick();
    for (int i = 0; i < 10; ++i)
        world.press(Key::Down);
    world.settle(400);
    check(world.snapshot()["focus"].string() == "Home settings" && world.motion("quick:sy") < -100,
          "The list scrolls to keep the focused row in view");
    world.press(Key::Ok);
    world.settle(400);
    check(world.screen() == "quick:layout" && world.app.shows("24-hour clock") && world.app.shows("App names"),
          "Home settings lists the switches");
    world.press(Key::Down);
    world.press(Key::Ok);
    check(world.transport.last("quick:layout").operation == Operation::SetHomeLayout &&
              world.transport.last("quick:layout").body["appNames"].isBoolean() &&
              !world.transport.last("quick:layout").body["appNames"].boolean(),
          "A switch posts its new value to the layout route");
    world.settle(300);
    check(world.motion("quick:t:1") < .5, "The switch moves at once");
    world.reply("quick:layout", R"({"ok":false,"error":"Read-only"})", 500);
    world.settle(300);
    check(world.motion("quick:t:1") > .5 && world.toasted("That did not work: Read-only"),
          "A refused switch moves back and says why");
    world.press(Key::Up);
    world.press(Key::Ok);
    check(world.transport.last("quick:layout").body["clockFormat"].string() == "24h", "The clock switch posts 24h");
    size_t catalogues = world.transport.count("catalogue");
    world.reply("quick:layout", R"({"ok":true})");
    check(world.transport.count("catalogue") == catalogues + 1, "A saved switch reads the layout again");
}

void quickActionChecks() {
    World world;
    world.openQuick();
    for (int i = 0; i < 6; ++i)
        world.press(Key::Down);
    check(world.snapshot()["focus"].string() == "Screensaver", "Screensaver is a row");
    world.press(Key::Ok);
    check(world.transport.last("quick:act").body["action"].string() == "screensaver", "It starts the screensaver");
    world.reply("quick:act", R"({"ok":false,"error":"Busy"})", 503);
    check(world.screen() == "quick" && world.toasted("That did not work: Busy"), "A refusal keeps the drawer open");
    world.press(Key::Down);
    world.press(Key::Ok);
    check(world.transport.last("quick:act").body["action"].string() == "screenOff", "Screen off posts its action");
    world.reply("quick:act", R"({"ok":true})");
    world.settle(900);
    check(world.screen() == "home", "The drawer closes once the TV has acted");
}

void quickNavigationChecks() {
    World recents;
    recents.openQuick();
    for (int i = 0; i < 8; ++i)
        recents.press(Key::Down);
    check(recents.snapshot()["focus"].string() == "Recent apps", "Recent apps is a row");
    recents.press(Key::Ok);
    recents.settle(900);
    check(recents.screen() == "quick:recents", "The row opens the recent apps");
    World profile;
    profile.openQuick();
    for (int i = 0; i < 9; ++i)
        profile.press(Key::Down);
    check(profile.snapshot()["focus"].string() == "Profile" && profile.app.shows("Main"), "The profile row names the profile in use");
    profile.press(Key::Ok);
    profile.settle(900);
    check(profile.screen() == "profiles", "The row opens the profiles");
    profile.press(Key::Back);
    check(profile.screen() == "home", "Back from there goes Home, not into a second drawer");
    // Close: the drawer slides out, Home is as it was, and the gear has the focus.
    World world;
    world.press({Key::Right, Key::Right});
    world.settle(300);
    world.press(Key::Up);
    world.press({Key::Right, Key::Right, Key::Right});
    world.press(Key::Ok);
    world.reply("quick:stats", statsJson);
    world.settle(600);
    check(world.screen() == "quick", "The gear opens the drawer from the bar");
    world.press(Key::Back);
    check(world.screen() == "quick" && world.snapshot()["closing"].boolean(), "Back starts the slide out");
    world.settle(900);
    check(world.screen() == "home" && world.snapshot()["navigation"].boolean() &&
              world.snapshot()["nav"].number() == 4 && world.snapshot()["column"].number() == 2,
          "The drawer closes onto Home as it was, with the gear focused");
    world.press(Key::Down);
    check(!world.snapshot()["navigation"].boolean(), "Home answers its keys again");
    // A click beside the drawer closes it.
    World click;
    click.openQuick();
    click.app.click(300, 500);
    click.settle(900);
    check(click.screen() == "home", "A click beside the drawer closes it");
}

int main() {
    try {
        kitChecks();
        detailsChecks();
        detailsFailureChecks();
        detailsOverviewChecks();
        detailsDepthChecks();
        detailsFocusReturnChecks();
        profilesChecks();
        searchChecks();
        searchStateChecks();
        searchGridChecks();
        keyboardChecks();
        renameChecks();
        quickChecks();
        quickVolumeChecks();
        quickMuteAndLightChecks();
        quickLevelChecks();
        quickInputChecks();
        quickSwitchChecks();
        quickActionChecks();
        quickNavigationChecks();
        std::printf("%d native overlay-screen checks passed\n", checks);
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
