// Seeded random walks through the TV client's navigation. Each seed builds a catalogue and a feed
// (grid or kind rows, one to three profiles, inputs, live channels, hidden apps), then drives the
// Application as a remote and a pointer would: every key, OK held, Back held, clicks on the hits
// the scene exposes, catalogue changes under the page, replies that come late, out of order, fail
// or never come. After every step the invariants below hold; a failure prints the seed, the step
// and the trail of actions, and NAVWALK_SEED=<n> replays that one seed with the whole trail.
//
// Invariants: the screen is a known one; every focus index is inside what exists; the focused
// thing has a hit; from any state a few taps of Back reach the first Home tab, never through the
// recent apps, and a tap of Back on the top bar changes nothing; the recent apps open only from
// Back held; no key is swallowed for good (Back always makes progress); a dimmed screen is woken
// by its first key and that key does nothing else.
//
// Known failures (a bug waiting for its fix) are listed in kKnown by invariant tag: they are
// counted and printed but do not fail the run, unless NAVWALK_STRICT=1.
#include "overlay_harness.hpp"
#include <cstdlib>
#include <fstream>
#include <random>
#include <set>
#include <sstream>

using namespace overlay_test;

namespace {
std::string number(double value) {
    return std::to_string(static_cast<int>(value));
}
const char *const kKnown[] = {"none"}; // no known failure at the moment

bool isKnown(const std::string &tag) {
    if (std::getenv("NAVWALK_STRICT"))
        return false;
    for (const char *known : kKnown)
        if (tag == known)
            return true;
    return false;
}
struct Failure : std::runtime_error {
    std::string tag;
    Failure(const std::string &tagged, const std::string &message)
        : std::runtime_error(message), tag(tagged) {}
};
std::map<std::string, unsigned> coverage; // steps spent in each screen and mode
std::map<std::string, unsigned> knownSeen;
std::map<std::string, std::string> knownFirst;
// A known failure is counted and the walk goes on; any other stops the seed.
void expect(bool condition, const char *tag, const std::string &message) {
    ++checkCount();
    if (condition)
        return;
    if (isKnown(tag)) {
        if (!knownSeen[tag]++)
            knownFirst[tag] = message;
        return;
    }
    throw Failure(tag, message);
}

std::string readFixture(const std::string &name) {
    std::ifstream file(std::string(NATIVETV_FIXTURES) + "/tv/" + name, std::ios::binary);
    std::stringstream text;
    text << file.rdbuf();
    return text.str();
}

// ---- What the server says: a catalogue and a feed that change during the walk -------------------

struct Server {
    std::vector<int> apps;
    int nextApp = 0, devices = 2, channels = 0, profiles = 1, trending = 4, movies = 2, youtube = 0;
    bool sortKind = false, showLive = false, showInputs = true, lgHome = true, titles = true,
         nativeTile = false;
    std::set<int> hiddenHome, hiddenTab;
    bool setupNeeded = false; // said once, by one reply to caps

    static const char *kindOfApp(int id) {
        static const char *const kinds[] = {"video", "music", "games", "tools"};
        return kinds[id % 4];
    }
    std::string catalogue() const {
        std::ostringstream out;
        out << R"({"ok":true,"apps":[)";
        std::string order;
        auto app = [&](const std::string &id, const std::string &title, const char *kind) {
            out << (order.empty() ? "" : ",") << R"({"id":")" << id << R"(","title":")" << title
                << R"(","kind":")" << kind << "\"}";
            order += (order.empty() ? "" : ",") + std::string("\"") + id + "\"";
        };
        for (int id : apps)
            app("app" + std::to_string(id), "App " + std::to_string(id), kindOfApp(id));
        if (nativeTile)
            app("com.tvweb.dashboard", "Native Tv", "tools");
        if (lgHome)
            order += (order.empty() ? "" : ",") + std::string("\"lg-home\"");
        out << R"(],"devices":[)";
        std::string devOrder;
        for (int i = 0; i < devices; ++i) {
            out << (i ? "," : "") << R"({"id":"hdmi)" << i + 1 << R"(","name":"Input )" << i + 1
                << R"(","port":)" << i + 1 << (i == 1 ? R"(,"unused":true})" : "}");
            devOrder += (i ? "," : "") + std::string("\"hdmi") + std::to_string(i + 1) + "\"";
        }
        out << R"(],"channels":[)";
        std::string chOrder;
        for (int i = 0; i < channels; ++i) {
            out << (i ? "," : "") << R"({"id":"ch)" << i + 1 << R"(","name":"Channel )" << i + 1
                << R"(","number":")" << i + 1 << "\"}";
            chOrder += (i ? "," : "") + std::string("\"ch") + std::to_string(i + 1) + "\"";
        }
        out << R"(],"kinds":[{"key":"video","label":"Movies & TV"},{"key":"music","label":"Music"},)"
            << R"({"key":"games","label":"Games"},{"key":"tools","label":"Tools"},{"key":"tv","label":"TV"}],)"
            << R"("order":{"app":[)" << order << R"(],"dev":[)" << devOrder << R"(],"ch":[)"
            << chOrder << R"(]},"layout":{"rows":["app","dev"],"hidden":{"app":[)";
        auto list = [&](const std::set<int> &ids) {
            std::string text;
            for (int id : ids)
                text += (text.empty() ? "" : ",") + std::string("\"app") + std::to_string(id) + "\"";
            return text;
        };
        out << list(hiddenHome) << R"(],"appsTab":[)" << list(hiddenTab) << R"(]})";
        if (sortKind)
            out << R"(,"sort":{"app":"kind"})";
        out << R"(,"showInputs":)" << (showInputs ? "true" : "false") << R"(,"showLive":)"
            << (showLive ? "true" : "false") << R"(,"showLgHome":)" << (lgHome ? "true" : "false")
            << R"(,"clockFormat":"12h"},"sources":{"channels":)" << channels << R"(},"profiles":[)";
        for (int i = 0; i < profiles; ++i)
            out << (i ? "," : "") << R"({"id":"p)" << i << R"(","name":"Person )" << i
                << R"(","active":)" << (i == 0 ? "true" : "false") << "}";
        out << "]}";
        return out.str();
    }
    std::string feed() const {
        std::ostringstream out;
        out << R"({"ok":true,"enabled":true,"titlesEnabled":)" << (titles ? "true" : "false")
            << R"(,"rows":[)";
        auto items = [&](int count, int first) {
            std::string text;
            for (int i = 0; i < count; ++i)
                text += (i ? "," : "") + std::string(R"({"id":)") + std::to_string(first + i) +
                        R"(,"type":")" + (i % 2 ? "tv" : "movie") + R"(","title":"Title )" +
                        std::to_string(first + i) + R"(","overview":"About it."})";
            return text;
        };
        std::string rows;
        auto row = [&](const char *key, int count, int first) {
            if (count)
                rows += (rows.empty() ? "" : ",") + std::string(R"({"key":")") + key +
                        R"(","items":[)" + items(count, first) + "]}";
        };
        row("trending", trending, 100);
        row("movies", movies, 200);
        if (youtube) {
            std::string videos;
            for (int i = 0; i < youtube; ++i)
                videos += (i ? "," : "") + std::string(R"({"id":"yt)") + std::to_string(i) +
                          R"(","title":"Video )" + std::to_string(i) + "\"}";
            rows += (rows.empty() ? "" : ",") + std::string(R"({"key":"youtube","items":[)") + videos + "]}";
        }
        out << rows << "]}";
        return out.str();
    }
};

// ---- The walk ----------------------------------------------------------------------------------

struct Walk {
    Server server;
    std::mt19937 rng;
    std::unique_ptr<World> world;
    std::vector<std::string> trail;
    std::vector<FakeTransport::Request> deferred;
    uint32_t lastInputAt = 0;
    bool reliable = false; // at rest the server answers everything at once
    size_t served = 0;
    size_t recentApps = 2;
    bool verbose = false;
    bool sawRecentsRow = false; // the Quick drawer focused its "Recent apps" row since it opened

    explicit Walk(unsigned seed) : rng(seed * 2654435761u + 12345u) {
        server.nextApp = 2 + pick(11);
        for (int i = 0; i < server.nextApp; ++i)
            server.apps.push_back(i);
        server.devices = pick(4);
        server.profiles = 1 + pick(3);
        server.sortKind = pick(2) == 0;
        server.showInputs = pick(5) != 0;
        server.showLive = pick(3) == 0;
        server.channels = server.showLive ? pick(4) : 0;
        server.lgHome = pick(4) != 0;
        server.titles = pick(5) != 0;
        server.nativeTile = pick(4) == 0;
        server.trending = pick(9);
        server.movies = pick(5);
        server.youtube = pick(3) == 0 ? 3 : 0;
        for (int id : server.apps) {
            if (pick(6) == 0)
                server.hiddenHome.insert(id);
            if (pick(6) == 0)
                server.hiddenTab.insert(id);
        }
        recentApps = static_cast<size_t>(pick(4));
        world.reset(new World(server.catalogue().c_str()));
        world->reply("discover", server.feed());
        world->settle(1200);
        served = world->transport.requests.size();
    }
    size_t pick(size_t count) {
        return static_cast<size_t>(rng() % count);
    }
    int pick(int count) {
        return static_cast<int>(rng() % static_cast<unsigned>(count));
    }
    Application &app() {
        return world->app;
    }

    // ---- Failing with a replayable trail --------------------------------------------------------

    std::string trailText(size_t last = 30) const {
        std::string text;
        size_t from = trail.size() > last ? trail.size() - last : 0;
        if (from)
            text = "... ";
        for (size_t i = from; i < trail.size(); ++i)
            text += (i > from ? " > " : "") + trail[i];
        return text;
    }

    // ---- The server's side of the transport -----------------------------------------------------

    std::string detailsFor(const Json &body) {
        std::string id = body["id"].string("100");
        int number = std::atoi(id.c_str());
        std::ostringstream out;
        out << R"({"ok":true,"enabled":true,"netflixEnabled":true,"item":{"id":)" << id
            << R"(,"type":")" << body["type"].string("movie") << R"(","title":"Title )" << id
            << R"(","overview":"A journey.","runtime":100,"trailer":"abc","cast":[{"name":"Ada"}],"similar":[)";
        int similar = number % 3; // some titles have none
        for (int i = 0; i < similar + 1 && number % 5 != 0; ++i)
            out << (i ? "," : "") << R"({"id":)" << number + 1 + i << R"(,"type":"movie","title":"Related )"
                << number + 1 + i << "\"}";
        out << "]}}";
        return out.str();
    }
    // The reply to a request, or "" when the server has none to give.
    std::string bodyFor(const FakeTransport::Request &request, long &status) {
        const std::string &channel = request.channel;
        status = 200;
        if (channel == "catalogue")
            return server.catalogue();
        if (channel == "discover")
            return server.feed();
        if (channel == "caps") {
            bool setup = server.setupNeeded;
            server.setupNeeded = false;
            return std::string(R"({"ok":true,"setupNeeded":)") + (setup ? "true" : "false") +
                   R"(,"origin":"http://tv.local:3000","key":"k"})";
        }
        if (channel == "guide")
            return R"({"ch1":[{"title":"News"}]})";
        if (channel == "recents" || channel == "action") {
            std::string action = request.body["action"].string();
            if (channel == "action" && request.operation != Operation::Recents)
                return R"({"ok":true})";
            if (action == "open" || action == "closeAll")
                return R"({"ok":true})";
            std::string apps;
            size_t count = channel == "action" && recentApps ? recentApps - 1 : recentApps;
            for (size_t i = 0; i < count; ++i)
                apps += (i ? "," : "") + std::string(R"({"id":"r)") + std::to_string(i) +
                        R"(","title":"Recent )" + std::to_string(i) + R"(","image":false})";
            return R"({"ok":true,"apps":[)" + apps + "]}";
        }
        if (channel == "quick:stats" || channel == "profiles:stats")
            return statsJson;
        if (channel == "quick:now")
            return R"({"ok":true,"kind":"input","port":1})";
        if (channel == "live:stats")
            return readFixture("stats-c4-webos9.json");
        if (channel == "title:details")
            return detailsFor(request.body);
        if (channel == "title:netflix")
            return R"({"ok":true,"netflixId":"80057281"})";
        if (channel == "title:search")
            return R"({"ok":true,"enabled":true,"items":[{"id":300,"type":"movie","title":"Found A"},{"id":301,"type":"tv","title":"Found B"}]})";
        if (channel.compare(0, 14, "settings:data:") == 0) {
            std::string key = channel.substr(14);
            if (key == "system")
                return readFixture("stats-c4-webos9.json");
            std::string text = readFixture("dashboard/" + key + ".json");
            if (key == "options") {
                Json stats = Json::parse(readFixture("stats-c4-webos9.json")),
                     extra = Json::parse(readFixture("dashboard/options-extra.json"));
                for (const auto &name : extra.keys())
                    stats.set(name, extra[name]);
                return stats.dump();
            }
            return text.empty() ? R"({"ok":true,"available":false})" : text;
        }
        if (channel == "catalog:list")
            return readFixture("dashboard/catalog.json");
        if (channel == "setup" || channel.compare(0, 6, "setup:") == 0) {
            std::string text = readFixture("dashboard/setup.json");
            return text.empty() ? R"({"ok":true})" : text;
        }
        return R"({"ok":true})";
    }
    void answer(const FakeTransport::Request &request, bool fail = false) {
        long status = 200;
        std::string body = bodyFor(request, status);
        if (fail) {
            status = 503;
            body = R"({"ok":false,"error":"Down"})";
        } else if (request.channel == "action" && pick(60) == 0) {
            status = 403; // the admin PIN is wanted
            body = R"({"ok":false,"pin":true,"error":"PIN required"})";
        }
        world->reply(request.channel, body, status, static_cast<int>(request.generation));
    }
    // Takes every request the application has made since: most are answered at once, some fail, some
    // are held for later (and so arrive out of order), some never come (writes always do).
    void serve() {
        for (int guard = 0; served < world->transport.requests.size(); ++guard) {
            expect(guard < 400, "storm", "the application keeps asking: a request storm");
            FakeTransport::Request request = world->transport.requests[served++];
            const bool must = request.channel == "action" || request.channel.compare(0, 2, "w:") == 0 ||
                              request.channel == "beat";
            const bool home = request.channel == "catalogue" || request.channel == "discover" ||
                              request.channel == "caps";
            int roll = reliable ? 0 : pick(100);
            if (roll < (home ? 90 : 70))
                answer(request);
            else if (roll < (home ? 94 : 80))
                answer(request, true);
            else if (roll < 92 || must)
                deferred.push_back(request);
        }
    }
    void flushDeferred(bool all) {
        while (!deferred.empty()) {
            size_t at = pick(deferred.size());
            FakeTransport::Request request = deferred[at];
            deferred.erase(deferred.begin() + static_cast<long>(at));
            answer(request, pick(8) == 0);
            if (!all)
                return;
        }
        serve();
    }
    // The server answers as the frames go by, not all at the end of the pause.
    void settle(uint32_t ms) {
        for (uint32_t done = 0; done < ms || done == 0; done += 100) {
            world->settle(std::min<uint32_t>(100, ms - std::min(done, ms)));
            serve();
            if (ms == 0)
                break;
        }
    }

    // ---- Actions --------------------------------------------------------------------------------

    static const char *keyName(Key key) {
        switch (key) {
        case Key::Left:
            return "Left";
        case Key::Right:
            return "Right";
        case Key::Up:
            return "Up";
        case Key::Down:
            return "Down";
        case Key::Ok:
            return "OK";
        case Key::Back:
            return "Back";
        case Key::Red:
            return "Red";
        case Key::PageUp:
            return "PageUp";
        case Key::PageDown:
            return "PageDown";
        case Key::Delete:
            return "Delete";
        }
        return "?";
    }
    void press(Key key) {
        trail.push_back(keyName(key));
        app().key(key);
    }
    void clickHit(bool barOnly) {
        std::vector<Hit> hits = app().frame().hits;
        std::vector<Hit> candidates;
        for (const Hit &hit : hits)
            if (!barOnly || hit.id.compare(0, 4, "nav:") == 0 || hit.id.compare(0, 5, "tool:") == 0)
                candidates.push_back(hit);
        if (candidates.empty()) {
            trail.push_back("click:none");
            return;
        }
        const Hit &hit = candidates[pick(candidates.size())];
        trail.push_back("click:" + hit.id);
        app().click(hit.rect.x + hit.rect.w / 2, hit.rect.y + hit.rect.h / 2);
    }
    void clickAnywhere() {
        float x = static_cast<float>(pick(1920)), y = static_cast<float>(pick(1080));
        trail.push_back("click@" + std::to_string(static_cast<int>(x)) + "," +
                        std::to_string(static_cast<int>(y)));
        app().click(x, y);
    }
    void mutate() {
        Server &s = server;
        std::string what;
        switch (pick(12)) {
        case 0:
            if (!s.apps.empty()) {
                s.apps.erase(s.apps.begin() + static_cast<long>(pick(s.apps.size())));
                what = "removeApp";
            }
            break;
        case 1:
            s.apps.push_back(s.nextApp++);
            what = "addApp";
            break;
        case 2:
            if (!s.apps.empty()) {
                s.hiddenHome.insert(s.apps[pick(s.apps.size())]);
                what = "hideOnHome";
            }
            break;
        case 3:
            if (!s.apps.empty()) {
                s.hiddenTab.insert(s.apps[pick(s.apps.size())]);
                what = "hideOnTab";
            }
            break;
        case 4:
            s.hiddenHome.clear();
            s.hiddenTab.clear();
            what = "unhideAll";
            break;
        case 5:
            s.sortKind = !s.sortKind;
            what = "toggleKindSort";
            break;
        case 6:
            s.devices = pick(4);
            what = "devices" + std::to_string(s.devices);
            break;
        case 7:
            s.profiles = 1 + pick(3);
            what = "profiles" + std::to_string(s.profiles);
            break;
        case 8:
            s.titles = !s.titles;
            what = "toggleTitles";
            break;
        case 9:
            s.trending = pick(9);
            s.movies = pick(5);
            what = "feedShrinks";
            break;
        case 10:
            s.showLive = !s.showLive;
            s.channels = s.showLive ? 1 + pick(3) : 0;
            what = "toggleLive";
            break;
        case 11:
            s.showInputs = !s.showInputs;
            what = "toggleInputs";
            break;
        }
        trail.push_back("mutate:" + what);
        world->reply("catalogue", s.catalogue());
        world->reply("discover", s.feed());
    }
    void dim() {
        trail.push_back("idle");
        for (int i = 0; i < 32; ++i)
            settle(1000);
        std::string before = signature(true);
        std::string name = app().screenName();
        press(Key::Back);
        settle(16);
        expect(signature(true) == before && app().screenName() == name, "wake",
               "the first key after the screen dimmed did more than wake it");
        expect(!app().snapshot()["idle"].boolean(), "wake", "a key did not wake the dimmed screen");
    }

    // ---- What must hold -------------------------------------------------------------------------

    std::string signature(bool withoutIdle = false) {
        Json value = app().snapshot();
        // The hero's slideshow moves on with time whatever is pressed, and a dimmed screen is dimmed
        // by time too: neither is a key's doing.
        value.set("heroTitle", Json::text(""));
        if (withoutIdle)
            value.set("idle", Json::flag(false));
        return app().screenName() + " " + value.dump();
    }
    size_t tabHits(const Scene &scene) const {
        size_t tabs = 0;
        for (const Hit &hit : scene.hits)
            tabs += hit.id.compare(0, 8, "nav:tab:") == 0;
        return tabs;
    }
    static bool known(const std::string &name) {
        static const std::set<std::string> exact = {
            "home", "quick", "quick:recents", "profiles", "details", "rename", "search", "pin",
            "live", "catalog", "qr", "dashboard"};
        if (exact.count(name))
            return true;
        for (const char *prefix : {"dashboard:", "quick:", "setup:"})
            if (name.compare(0, std::string(prefix).size(), prefix) == 0)
                return true;
        return false;
    }
    void invariants(bool quiet, bool finished = false) {
        Application &application = app();
        const std::string name = application.screenName();
        expect(known(name), "screen", "unknown screen " + name);
        Json snap = application.snapshot();
        const Scene &scene = application.frame();
        ++coverage[name];
        if (name == "home") {
            ++coverage["home tab " + snap["tab"].string()];
            for (const char *mode : {"navigation", "editing", "editHide", "arranging"})
                if (snap[mode].boolean())
                    ++coverage[std::string("home ") + mode];
        } else if (name == "details" && snap["depth"].number() >= 2)
            ++coverage["details depth 2+"];
        else if (name == "dashboard" || name.compare(0, 10, "dashboard:") == 0)
            ++coverage["settings zone " + snap["zone"].string()];
        for (const Hit &hit : scene.hits)
            expect(!hit.id.empty() && hit.rect.w > 0 && hit.rect.h > 0 && hit.rect.x == hit.rect.x &&
                       hit.rect.y == hit.rect.y,
                   "hit", "a hit with no id or no area: '" + hit.id + "'");
        const double rows = snap["rows"].number();
        if (name == "home") {
            static const std::set<std::string> tabs = {"home", "live", "apps", "inputs"};
            expect(tabs.count(snap["tab"].string()) != 0, "home.tab", "unknown tab " + snap["tab"].string());
            expect(snap["row"].number() < std::max(1.0, rows), "home.row", "row beyond the rows");
            expect(!snap["arranging"].boolean() || snap["tab"].string() == "apps", "home.arranging",
                   "rows lifted away from the Apps tab");
            expect(!snap["editing"].boolean() || !snap["navigation"].boolean(), "home.editing",
                   "editing apps while the focus is on the bar");
            expect(!snap["editHide"].boolean() || snap["editing"].boolean(), "home.editing",
                   "the hide button focused outside editing");
            const size_t tabCount = tabHits(scene);
            if (tabCount)
                expect(snap["nav"].number() <= static_cast<double>(tabCount) + 2, "home.nav",
                       "bar focus beyond the bar's stops");
            if (!snap["navigation"].boolean() && rows > 0 && !snap["editHide"].boolean())
                expect(!snap["focusCard"].string().empty(), "home.focus",
                       "the focus is on a card that does not exist (row " +
                           std::to_string(static_cast<int>(snap["row"].number())) + ", column " +
                           std::to_string(static_cast<int>(snap["column"].number())) + ") " + snap.dump());
        } else if (name == "quick:recents") {
            const double count = snap["recentApps"].number();
            expect(count == 0 || snap["selected"].number() < count, "recents.selected",
                   "selected card beyond the recent apps");
        } else if (name == "quick") {
            expect(snap["rows"].number() == 0 || snap["selected"].number() < snap["rows"].number(),
                   "quick.selected", "focus beyond the drawer's rows");
        } else if (name == "live") {
            expect(snap["cards"].number() == 0 || snap["focus"].number() < snap["cards"].number(),
                   "live.focus", "focus beyond the live cards");
        } else if (name == "search") {
            expect(snap["results"].number() == 0 || snap["selected"].number() < snap["results"].number(),
                   "search.selected", "focus beyond the results");
        } else if (name == "profiles") {
            expect(snap["selected"].number() <= static_cast<double>(server.profiles) + 1, "profiles.selected",
                   "focus beyond the profiles");
        } else if (name == "details") {
            expect(snap["depth"].number() >= 1 || snap["depth"].number() == 0, "details.depth", "no depth");
        }
        if (!quiet)
            return;
        // At rest the focused thing is on screen: it has a hit.
        const bool waiting = name.compare(0, 6, "setup:") == 0 &&
                             (!snap["loaded"].boolean() || snap["busy"].boolean());
        expect(!scene.hits.empty() || name == "pin" || waiting, "hits",
               "a screen at rest with nothing to click: " + signature());
        auto hitRect = [&](const std::string &id, Rect &found) {
            for (const Hit &hit : scene.hits)
                if (hit.id == id) {
                    found = hit.rect;
                    return true;
                }
            return false;
        };
        // The focused thing is on the canvas, not scrolled away: at least half of it shows.
        auto shown = [&](const std::string &id, const char *tag, const std::string &what) {
            Rect rect;
            if (!hitRect(id, rect)) {
                std::string ids;
                for (const Hit &hit : scene.hits)
                    ids += hit.id + " ";
                expect(false, tag, what + " has no hit (" + id + ") in " + signature() + " hits " + ids + " set:sy " +
                                    std::to_string(application.animation().value("set:sy")) + " E0 " +
                                    std::to_string(application.animation().value("set:E:0")) + " E1 " +
                                    std::to_string(application.animation().value("set:E:1")));
                return;
            }
            const float width = std::max(0.0f, std::min(rect.x + rect.w, 1920.0f) - std::max(rect.x, 0.0f)),
                        height = std::max(0.0f, std::min(rect.y + rect.h, 1080.0f) - std::max(rect.y, 0.0f));
            expect(width * height >= rect.w * rect.h * 0.5f, tag,
                   what + " is off the screen (" + id + " at " + number(rect.x) + "," + number(rect.y) + " " +
                       number(rect.w) + "x" + number(rect.h) + ")");
        };
        if (name == "quick" && snap["rows"].number() > 0 && !snap["loading"].boolean())
            shown("quick:row:" + number(snap["selected"].number()), "quick.focusshown", "the drawer's focus");
        else if (name == "live" && snap["cards"].number() > 0)
            shown("live:card:" + number(snap["focus"].number()), "live.focusshown", "the live focus");
        else if ((name == "dashboard" || name.compare(0, 10, "dashboard:") == 0) &&
                 !snap["loading"].boolean() && snap["zone"].string() != "tabs") {
            const bool sections = snap["zone"].string() == "sections";
            if ((sections ? snap["sections"].number() : snap["rows"].number()) > 0)
                shown(std::string(sections ? "sec" : "set") + ":row:" + number(snap["focusIndex"].number()),
                      "settings.focusshown", "the settings focus");
        }
        if (name == "home" && !snap["editHide"].boolean()) {
            auto hasHit = [&](const std::string &id) {
                Rect rect;
                return hitRect(id, rect);
            };
            const std::string row = std::to_string(static_cast<int>(snap["row"].number())),
                              column = std::to_string(static_cast<int>(snap["column"].number()));
            if (snap["navigation"].boolean()) {
                const size_t tabCount = tabHits(scene);
                const size_t nav = static_cast<size_t>(snap["nav"].number());
                const std::string id = nav < tabCount          ? "nav:tab:" + std::to_string(nav)
                                       : nav == tabCount       ? "nav:avatar"
                                       : nav == tabCount + 1   ? "nav:settings"
                                                               : "tool:2";
                expect(hasHit(id), "home.navhit", "the focus is on the bar at " + id + " which has no hit");
            } else if (rows > 0 && !snap["focusCard"].string().empty())
                shown("card:" + row + ":" + column, "home.cardhit",
                      "the focused card " + row + ":" + column + " (" + snap["focusCard"].string() + ")");
        }
        // Nothing asked of the server is left half done: no write waits for a reply that was given.
        if (finished && name == "home")
            expect(!snap["busy"].boolean(), "busy", "Home is busy with nothing outstanding");
    }

    // From wherever the walk is, Back leads to the first Home tab: a few taps, never through the
    // recent apps, never swallowed, and at the top bar it does nothing.
    bool atRest() {
        Json snap = app().snapshot();
        return app().screenName() == "home" && snap["tab"].string() == "home" &&
               snap["navigation"].boolean() && snap["nav"].number() == 0 && !snap["editing"].boolean() && !snap["arranging"].boolean();
    }
    void backChain() {
        trail.push_back("[backChain]");
        Json first = app().snapshot();
        size_t depth = static_cast<size_t>(first["depth"].number(1));
        const size_t limit = 12 + (depth > 1 ? depth - 1 : 0);
        std::string previous = signature();
        int stalled = 0;
        const bool startedInRecents = app().screenName() == "quick:recents" || sawRecentsRow;
        for (size_t taps = 0;; ++taps) {
            if (atRest())
                break;
            expect(taps < limit, "backchain",
                   "Back did not reach Home's first tab in " + std::to_string(limit) + " taps (at " +
                       app().screenName() + ")");
            press(Key::Back);
            settle(700);
            if (verbose && std::getenv("NAVWALK_TRACE"))
                std::printf("    chain Back %s\n", signature().substr(0, 200).c_str());
            if (signature() == previous) { // a page waiting on a reply gets it
                flushDeferred(true);
                settle(1500);
            }
            invariants(false);
            expect(startedInRecents || app().screenName() != "quick:recents", "back.recents",
                   "a tap of Back opened the recent apps");
            std::string now = signature();
            stalled = now == previous ? stalled + 1 : 0;
            expect(stalled < 3, "swallowed", "Back changed nothing three times in a row at " + now);
            previous = now;
        }
        // At the top bar a tap of Back does nothing.
        Json bar = app().snapshot();
        if (bar["rows"].number() > 0 && bar["focusCard"].string().empty())
            return; // a focus that is on no card is reported as home.focus; the page mends it
        const std::string before = signature();
        const size_t requests = world->transport.requests.size();
        press(Key::Back);
        settle(200);
        expect(signature() == before, "back.bar", "Back on the top bar changed " + before + " into " + signature());
        for (size_t i = requests; i < world->transport.requests.size(); ++i)
            expect(world->transport.requests[i].channel != "recents" &&
                       world->transport.requests[i].channel != "action",
                   "back.bar", "Back on the top bar asked for " + world->transport.requests[i].channel);
    }

    // ---- The walk itself ------------------------------------------------------------------------

    // Where Home's focus is, as a person would say it.
    std::string focusText() {
        Json s = app().snapshot();
        // On the bar the page may still be catching up with the tab the pill has reached.
        if (s["navigation"].boolean())
            return "on the bar at " + number(s["nav"].number());
        return s["tab"].string() + ", row " + number(s["row"].number()) + ", column " +
               number(s["column"].number()) + ", card " + s["focusCard"].string();
    }
    // A move and its way back lead to where they started (when the move moved).
    bool probe() {
        Json s = app().snapshot();
        if (app().screenName() != "home" || s["navigation"].boolean() || s["editing"].boolean() ||
            s["arranging"].boolean() || s["rows"].number() == 0 || s["focusCard"].string().empty())
            return false;
        static const Key pairs[][2] = {{Key::Right, Key::Left}, {Key::Left, Key::Right},
                                       {Key::Down, Key::Up},    {Key::Up, Key::Down}};
        const int which = pick(4);
        const bool across = which < 2;
        const std::string start = focusText();
        const double row = s["row"].number(), column = s["column"].number();
        trail.push_back(std::string("probe ") + keyName(pairs[which][0]) + "/" + keyName(pairs[which][1]));
        app().key(pairs[which][0]);
        settle(16);
        Json mid = app().snapshot();
        const bool stillOnPage = !mid["navigation"].boolean() && mid["screen"].string() == "home";
        const bool moved = mid["row"].number() != row || mid["column"].number() != column;
        if (stillOnPage && moved) {
            app().key(pairs[which][1]);
            settle(16);
            Json end = app().snapshot();
            // Up and down may pass a shorter row: the column is only held where the row had it.
            const bool columnHeld = across || mid["column"].number() == column;
            expect(end["row"].number() == row && (!columnHeld || end["column"].number() == column),
                   "probe", std::string(keyName(pairs[which][0])) + " then " + keyName(pairs[which][1]) +
                                " from (" + start + ") ended at (" + focusText() + ")");
        }
        return true;
    }

    // Home as it was: what opens over Home and is closed with Back gives Home back with the focus
    // where it was.
    struct Anchor {
        bool valid = false;
        std::string overlay, focus;
    } anchor;
    static bool isOverlay(const std::string &name) {
        return name == "details" || name == "quick" || name == "profiles" || name == "search" ||
               name == "quick:recents";
    }
    void trackAnchor(const std::string &before, const std::string &after, const std::string &focusBefore,
                     const std::string &action, bool plain, bool calm) {
        if (!plain) {
            anchor.valid = false;
            return;
        }
        if (before == "home" && isOverlay(after) && action.compare(0, 10, "click:card") != 0) {
            const bool onNoCard = focusBefore.size() >= 7 && focusBefore.compare(focusBefore.size() - 7, 7, ", card ") == 0;
            anchor = {calm && !onNoCard, after, focusBefore};
            return;
        }
        if (!anchor.valid)
            return;
        if (after == "home" && action == "Back" && before == anchor.overlay) {
            expect(focusText() == anchor.focus, "home.restore",
                   "Back from " + anchor.overlay + " gave Home back at (" + focusText() + ") instead of (" +
                       anchor.focus + ")");
            anchor.valid = false;
        } else if (!(after == anchor.overlay &&
                     (action == "Back" || action == "Up" || action == "Down" || action == "Left" ||
                      action == "Right" || action == "PageUp" || action == "PageDown" || action == "wait")))
            anchor.valid = false;
    }

    void step() {
        if (pick(14) == 0 && probe())
            return;
        // A page change or a tab on its way (under 300 ms) leaves Home in flux for a moment.
        const bool calm = world->clock - lastInputAt >= 400;
        lastInputAt = world->clock;
        const std::string before = app().screenName();
        const std::string focusBefore = before == "home" ? focusText() : "";
        // Leaving while apps are moved ends the move: Home then differs on purpose.
        const bool moving = before == "home" && (app().snapshot()["editing"].boolean() ||
                                                  app().snapshot()["arranging"].boolean());
        const size_t recentReads = world->transport.count("recents");
        bool heldBack = false, plain = false;
        int roll = pick(100);
        if (roll < 52) {
            plain = true;
            static const Key keys[] = {Key::Up,   Key::Up,    Key::Down,     Key::Down, Key::Left,
                                       Key::Left, Key::Right, Key::Right,    Key::Ok,   Key::Ok,
                                       Key::Ok,   Key::Back,  Key::Back,     Key::Back, Key::Red,
                                       Key::PageUp, Key::PageDown, Key::Delete};
            press(keys[pick(sizeof keys / sizeof keys[0])]);
        } else if (roll < 58) {
            plain = true;
            trail.push_back("OK held");
            app().longPress();
        } else if (roll < 63) {
            plain = true;
            trail.push_back("Back held");
            heldBack = true;
            app().longBack();
        } else if (roll < 77) {
            plain = true;
            clickHit(false);
        } else if (roll < 83) {
            plain = true;
            clickHit(true);
        }
        else if (roll < 85)
            clickAnywhere();
        else if (roll < 88)
            mutate();
        else if (roll < 91) {
            trail.push_back("replies");
            flushDeferred(pick(3) == 0);
        }
        else if (roll < 92) {
            static const char *const routes[] = {"catalog", "qr:address", "setup:welcome", "pin"};
            std::string route = routes[pick(4)];
            trail.push_back("open:" + route);
            app().open(route);
        } else if (roll < 93) {
            server.setupNeeded = true;
            trail.push_back("serverSaysSetup");
            settle(2200);
            return;
        } else if (roll < 95) {
            trail.push_back("text");
            app().input("a");
        } else if (roll < 96) {
            if (pick(3) == 0)
                dim();
            else
                trail.push_back("wait");
        } else if (roll < 98) {
            // Into the page: a few keys that go deeper.
            static const Key deeper[] = {Key::Down, Key::Right, Key::Ok, Key::Ok, Key::Down};
            for (int i = 2 + pick(4); i > 0; --i) {
                press(deeper[pick(5)]);
                settle(16);
            }
        } else {
            // To the bar and along it.
            for (int i = 0; i < 4; ++i)
                press(Key::Up);
            for (int i = pick(5); i > 0; --i)
                press(Key::Right);
            press(Key::Ok);
        }
        {
            // The drawer's own "Recent apps" row (OK or a click on it) is the other way in.
            const std::string &action = trail.back();
            Json drawer = app().snapshot();
            if (before == "quick" && drawer["closing"].boolean() && drawer["focus"].string() == "Recent apps" &&
                (action == "OK" || action.compare(0, 5, "click") == 0))
                sawRecentsRow = true;
            else if (before != "quick")
                sawRecentsRow = false;
        }
        static const uint32_t pauses[] = {16, 16, 40, 120, 300, 500, 700, 1200};
        settle(pauses[pick(8)]);
        invariants(false);
        const std::string after = app().screenName();
        if (verbose && after != before)
            trail.back() += "=>" + after;
        if (verbose && std::getenv("NAVWALK_TRACE"))
            std::printf("%3zu %-28s %s\n", trail.size(), trail.back().c_str(), signature().substr(0, 230).c_str());
        if (!heldBack && !sawRecentsRow && after == "quick:recents" && before != "quick:recents")
            throw Failure("recents.open", "the recent apps opened without Back held (from " + before + ")");
        std::string action = trail.back();
        if (action.find("=>") != std::string::npos)
            action.erase(action.find("=>"));
        trackAnchor(before, after, focusBefore, action, plain, calm && !moving);
        (void)recentReads;
    }
    // At rest: replies given, animations played. A page whose first reply came in the last moment
    // gets one more pause before it is held to the invariants.
    void rest() {
        reliable = true;
        flushDeferred(true);
        settle(800);
        settle(1800);
        for (int attempt = 0;; ++attempt) {
            try {
                invariants(true);
                reliable = false;
                return;
            } catch (const Failure &) {
                if (std::getenv("NAVWALK_DBG"))
                    std::printf("DBG attempt %d t=%u E0=%f E1=%f rows=%s %s\n", attempt, world->clock,
                                app().animation().value("set:E:0"), app().animation().value("set:E:1"),
                                app().snapshot()["rows"].dump().c_str(), trailText(6).c_str());
                if (attempt == 2) {
                    reliable = false;
                    throw;
                }
                flushDeferred(true);
                settle(2500);
            }
        }
    }
    void run(int steps) {
        for (int i = 0; i < steps; ++i) {
            if (i % 9 == 8)
                rest();
            if (i % 20 == 19)
                backChain();
            step();
        }
        // Everything outstanding is answered and the screen rests: nothing waits for good.
        rest();
        reliable = true;
        flushDeferred(true);
        settle(3000);
        invariants(true, true);
        reliable = false;
        backChain();
    }
};

} // namespace

int main() {
    const char *only = std::getenv("NAVWALK_SEED");
    // NAVWALK_SEEDS and NAVWALK_STEPS widen a run (a longer search than CI makes).
    const unsigned seeds = std::getenv("NAVWALK_SEEDS") ? static_cast<unsigned>(std::atoi(std::getenv("NAVWALK_SEEDS"))) : 300;
    const int steps = std::getenv("NAVWALK_STEPS") ? std::atoi(std::getenv("NAVWALK_STEPS")) : 80;
    std::map<std::string, unsigned> failures;
    std::map<std::string, std::string> firstFailure;
    unsigned failedSeeds = 0;
    for (unsigned seed = only ? static_cast<unsigned>(std::atoi(only)) : 0;
         seed < (only ? static_cast<unsigned>(std::atoi(only)) + 1 : seeds); ++seed) {
        Walk walk(seed);
        walk.verbose = only != nullptr;
        try {
            walk.run(steps);
        } catch (const Failure &failure) {
            std::ostringstream text;
            text << "seed " << seed << " step " << walk.trail.size() << " [" << failure.tag << "] "
                 << failure.what() << "\n    on " << walk.app().screenName() << ": trail "
                 << walk.trailText(only ? walk.trail.size() : 24);
            ++failedSeeds;
            if (!failures[failure.tag]++)
                firstFailure[failure.tag] = text.str();
            if (only)
                std::printf("%s\n", text.str().c_str());
        } catch (const std::exception &error) {
            ++failedSeeds;
            std::ostringstream text;
            text << "seed " << seed << " [exception] " << error.what() << "\n    on "
                 << walk.app().screenName() << ": trail " << walk.trailText(only ? walk.trail.size() : 24);
            if (!failures["exception"]++)
                firstFailure["exception"] = text.str();
        }
    }
    for (const auto &entry : failures)
        std::printf("%s: %u seeds, first %s\n", entry.first.c_str(), entry.second,
                    firstFailure[entry.first].c_str());
    for (const auto &entry : knownSeen)
        std::printf("known %s: %u times, first: %s\n", entry.first.c_str(), entry.second,
                    knownFirst[entry.first].c_str());
    std::printf("coverage (steps):");
    for (const auto &entry : coverage)
        std::printf(" [%s] %u;", entry.first.c_str(), entry.second);
    std::printf("\n");
    if (failedSeeds) {
        std::printf("navigation walk: %u seeds failed\n", failedSeeds);
        return 1;
    }
    std::printf("navigation walk ok: %u checks\n", checkCount());
    return 0;
}
