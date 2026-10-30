// Live, Settings and Catalog: what the models say of the TV's real readings (snapshots against
// tests/fixtures/tv), and what the screens do with keys and replies. The snapshots are text files
// beside the fixtures; NATIVETV_UPDATE_SNAPSHOTS=1 writes them again after a deliberate change.
#include "domain/live_cards.hpp"
#include "domain/settings_items.hpp"
#include "ui/application.hpp"
#include "ui/kit/tuning.hpp"
#include "ui/kit/widgets.hpp"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>

using namespace nativetv;
namespace {
int checks = 0;
void check(bool condition, const char *message) {
    ++checks;
    if (!condition)
        throw std::runtime_error(message);
}
std::string readFile(const std::string &path) {
    std::ifstream file(path, std::ios::binary);
    if (!file)
        throw std::runtime_error("cannot read " + path);
    std::stringstream text;
    text << file.rdbuf();
    return text.str();
}
const std::string kFixtures = std::string(NATIVETV_FIXTURES) + "/tv/";
Json fixture(const std::string &name) {
    Json value = Json::parse(readFile(kFixtures + name));
    if (!value.valid())
        throw std::runtime_error("bad JSON in " + name);
    return value;
}
Json section(const std::string &name) {
    return fixture("dashboard/" + name + ".json");
}
// The options page reads the TV's stats with the settings the server adds to them.
Json optionsStats() {
    Json stats = fixture("stats-c4-webos9.json"), extra = section("options-extra");
    for (const auto &key : extra.keys())
        stats.set(key, extra[key]);
    return stats;
}
void snapshot(const std::string &name, const std::string &text) {
    std::string path = kFixtures + "dashboard/snapshots/" + name + ".txt";
    if (std::getenv("NATIVETV_UPDATE_SNAPSHOTS")) {
        std::ofstream(path, std::ios::binary) << text;
        return;
    }
    std::string expected = readFile(path);
    if (expected != text)
        throw std::runtime_error("snapshot " + name + " differs; actual:\n" + text);
    ++checks;
}

struct FakeTransport final : Services {
    struct Request {
        std::string channel;
        Operation operation;
        Json body;
        unsigned generation;
    };
    std::vector<Request> requests;
    std::map<std::string, unsigned> revisions;
    bool refuse = false;
    std::string session; // the admin session the client was told to send
    void adminSession(const std::string &value) override {
        session = value;
    }
    unsigned read(const std::string &channel, Operation operation,
                  const Json &arguments = Json()) override {
        unsigned generation = refuse ? 0 : ++revisions[channel];
        requests.push_back({channel, operation, arguments, generation});
        return generation;
    }
    unsigned write(const std::string &channel, Operation operation, const Json &body) override {
        unsigned generation = refuse ? 0 : ++revisions[channel];
        requests.push_back({channel, operation, body, generation});
        return generation;
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
    // The writes the optimistic layer sent, oldest first (they travel on "w:<key>" channels).
    std::vector<const Request *> writes() const {
        std::vector<const Request *> found;
        for (const auto &request : requests)
            if (request.channel.compare(0, 2, "w:") == 0)
                found.push_back(&request);
        return found;
    }
    const Request *last(const std::string &channel) const {
        for (auto it = requests.rbegin(); it != requests.rend(); ++it)
            if (it->channel == channel)
                return &*it;
        return nullptr;
    }
};
void reply(Application &app, const std::string &channel, const std::string &json,
           long status = 200) {
    Response response;
    response.channel = channel;
    response.status = status;
    response.bytes = json;
    app.response(response);
}
void reply(Application &app, const std::string &channel, const Json &json) {
    reply(app, channel, json.dump());
}
// Frames at 60 Hz from `from` for `ms`.
uint32_t run(Application &app, uint32_t from, uint32_t ms) {
    uint32_t at = from;
    for (uint32_t step = 0; step <= ms * 60 / 1000; ++step) {
        at = from + step * 1000 / 60;
        app.update(at);
    }
    return at;
}

// ---- the models, as text -----------------------------------------------------------------------

std::string number(double value, int decimals) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.*f", decimals, value);
    return text;
}
std::string describe(const LiveView &view) {
    std::string text = view.title + " | " + view.subtitle + "\n";
    for (const LiveCard &card : view.cards) {
        text += card.key + ": " + (card.numeric ? liveNumber(card, card.number) : card.text);
        if (!card.unit.empty())
            text += " " + card.unit;
        text += " | " + card.caption;
        if (!card.caption2.empty())
            text += " | " + card.caption2;
        if (card.bar >= 0)
            text += " | bar " + number(card.bar, 2) + (card.warn ? " warn" : "");
        if (card.spark)
            text += " | spark";
        if (!card.days.empty()) {
            text += " | days ";
            for (bool up : card.days)
                text += up ? '#' : '.';
        }
        for (const auto &chip : card.chips)
            text += " [" + chip + "]";
        text += "\n";
    }
    return text;
}
std::string describe(const std::vector<SettingPage> &pages) {
    static const char *const kinds[] = {"heading", "info", "toggle", "choice", "action", "link"};
    std::string text;
    for (const SettingPage &page : pages) {
        if (pages.size() > 1)
            text += "## " + page.title + "\n";
        for (const SettingItem &item : page.items) {
            std::string kind = kinds[static_cast<int>(item.kind)];
            if (item.kind == SettingItem::Kind::Heading) {
                text += "== " + item.title + "\n";
                continue;
            }
            text += kind + std::string(8 - kind.size(), ' ') + item.title;
            if (!item.value.empty())
                text += " = " + item.value;
            if (item.kind == SettingItem::Kind::Toggle)
                text += item.on ? " [on]" : " [off]";
            if (!item.action.empty())
                text += " -> " + item.action;
            if (item.command.confirm)
                text += " (confirms)";
            if (!item.choices.empty())
                text += " {" + std::to_string(item.choices.size()) + " values}";
            text += " <" + item.icon + ">\n";
        }
    }
    return text;
}

void models() {
    for (const char *name : {"stats-b8-webos4", "stats-cx-webos5", "stats-c4-webos9",
                             "stats-c4-webos9-hdr", "stats-c4-webos9-standby", "stats-g4-webos9"}) {
        LiveView view = liveView(fixture(std::string(name) + ".json"), 10, 9, 4);
        check(view.cards.size() == 7, "Live shows seven cards for every TV");
        snapshot(std::string("live-") + name, describe(view));
    }
    check(liveView(Json(), 0, 0, 0).cards.empty(), "A reply with nothing in it has no cards");
    Json sparse = Json::parse(R"({"ok":true,"load":12})");
    LiveView few = liveView(sparse, 0, 0, 0);
    check(few.cards.size() == 7 && few.cards[1].text == "\xe2\x80\x94" && few.cards[3].word,
          "A TV that reports little shows dashes, not zeros");
    check(liveLoad(sparse) == 12 && liveLoad(Json::parse(R"({"ok":true})")) < 0,
          "The load is read only where the TV gives one");

    Json caps = Json::object();
    caps.set("allowPower", Json::flag(true));
    std::map<std::string, Json> data = {{"system", fixture("stats-c4-webos9.json")},
                                        {"apps", section("apps")},
                                        {"oled", section("oled")},
                                        {"game", section("game")},
                                        {"saver", section("saver")},
                                        {"home", section("home")},
                                        {"privacy", section("privacy")},
                                        {"service", section("service")},
                                        {"options", optionsStats()},
                                        {"settings", section("setup")}};
    for (const SettingGroup &group : settingGroups()) {
        auto rows = dashboardRows(group.key, data[group.key], caps);
        check(!rows.empty(), "Every section has rows");
        snapshot("rows-" + group.key, describe(settingPages(group.key, rows)));
        check(!settingGroupStatus(group.key, data[group.key]).empty() || group.key == "game" ||
                  group.key == "apps" || group.key == "options",
              "A loaded section tells a few words of itself");
    }
    snapshot("rows-privacy-consents",
             describe(settingPages("privacy",
                                   dashboardRows("privacy", data["privacy"], caps, "consents"))));
    snapshot("rows-privacy-services",
             describe(settingPages("privacy",
                                   dashboardRows("privacy", data["privacy"], caps, "services"))));
    check(settingGroups().size() == dashboardTabs().size(),
          "Settings lists the ten dashboard sections");
    for (size_t i = 0; i < settingGroups().size(); ++i)
        check(settingGroups()[i].key == dashboardTabs()[i].key,
              "The sections keep the dashboard's keys and order");
    bool on = false;
    check(settingToggleValue("Shown", on) && on && settingToggleValue("Not on Home", on) && !on &&
              !settingToggleValue("Browse", on),
          "A row's value says whether it is a switch");
}

// ---- the kit's panel, as Settings uses it
// ----------------------------------------------------------

size_t count(const Scene &scene, const std::string &text) {
    size_t found = 0;
    for (const auto &node : scene.nodes)
        if (node.kind == Node::Kind::Text && node.value == text)
            ++found;
    return found;
}
void panel() {
    kit::LegacyGlyphs glyphs;
    Scene plain, longer;
    kit::DetailPanel detail;
    detail.icon = "info";
    detail.title = "Title";
    detail.body = "Body";
    detail.chips = {"Chip"};
    detail.action = "Change";
    kit::detailPanel({plain, glyphs}, detail);
    check(count(plain, "OK") == 1 && count(plain, "Change") == 1, "The panel says what OK does");
    detail.action.clear();
    detail.bodyLines = 5;
    kit::detailPanel({longer, glyphs}, detail);
    check(count(longer, "OK") == 0, "A row OK does nothing on has no OK hint");
    float chipsAt = 0, chipsLower = 0;
    for (const auto &node : plain.nodes)
        if (node.kind == Node::Kind::Text && node.value == "Chip")
            chipsAt = node.rect.y;
    for (const auto &node : longer.nodes)
        if (node.kind == Node::Kind::Text && node.value == "Chip")
            chipsLower = node.rect.y;
    check(chipsLower > chipsAt, "A longer body moves the chips down");
}

// ---- Live
// ------------------------------------------------------------------------------------------

void live() {
    FakeTransport transport;
    Application app(transport, "live");
    check(app.screenName() == "live", "The Live route opens the readings");
    check(transport.count("live:stats") == 1 &&
              transport.last("live:stats")->operation == Operation::Metrics,
          "Live asks for /api/stats as it opens");
    run(app, 16, 300);
    check(app.shows("Reading the TV"), "Live says it is reading while nothing has come");
    reply(app, "live:stats", fixture("stats-c4-webos9.json"));
    uint32_t clock = run(app, 400, 1500);
    check(app.snapshot()["loaded"].boolean() && app.snapshot()["cards"].number() == 7,
          "The reading becomes seven cards");
    check(app.shows("LG OLED55C4") && app.shows("Processor") && app.shows("Memory") &&
              app.shows("PC") && app.shows("HDMI 4") && app.shows("updated just now"),
          "The cards and the title are drawn");
    check(transport.count("live:stats") == 1, "The next reading waits");
    clock = run(app, clock, 4200);
    check(transport.count("live:stats") == 2, "Live reads again every few seconds");
    run(app, clock, 6000);
    check(transport.count("live:stats") == 2, "…but not while a reading is still out");
    Json next = fixture("stats-c4-webos9.json");
    next.set("load", Json::numeric(47));
    reply(app, "live:stats", next);
    reply(app, "live:stats", next);
    check(app.snapshot()["samples"].number() == 3,
          "Each reading adds a sample to the processor's line");
    clock = run(app, clock + 2000, 800);
    check(app.shows("47"), "The numeral counts to the new reading");
    reply(app, "live:stats", "", 503);
    check(app.snapshot()["failed"].boolean() && app.snapshot()["loaded"].boolean(),
          "A failed reading keeps the last cards");
    run(app, clock, 200);

    // Focus: a 4 by 2 grid, with seven cards.
    check(app.snapshot()["focus"].number() == 0, "Focus starts on the first card");
    app.key(Key::Right);
    app.key(Key::Right);
    check(app.snapshot()["focus"].number() == 2, "Right moves along the row");
    app.key(Key::Down);
    check(app.snapshot()["focus"].number() == 6, "Down goes to the card below");
    app.key(Key::Right);
    check(app.snapshot()["focus"].number() == 6, "The last card has nothing to its right");
    app.key(Key::Down);
    check(app.snapshot()["focus"].number() == 6 && !app.snapshot()["barFocus"].boolean(),
          "Down at the bottom stays");
    app.key(Key::Up);
    app.key(Key::Up);
    check(app.snapshot()["focus"].number() == 2 && app.snapshot()["barFocus"].boolean(),
          "Up from the top row gives the focus to the bar");
    size_t reads = transport.count("live:stats");
    app.key(Key::Down);
    check(!app.snapshot()["barFocus"].boolean(), "Down from the bar returns to the page");
    app.key(Key::Ok);
    run(app, clock + 300, 100);
    check(transport.count("live:stats") == reads + 1, "OK reads again now");
    app.key(Key::Back);
    check(app.screenName() == "home", "Back leaves Live for Home");

    // The first reading failing says so.
    FakeTransport broken;
    Application cold(broken, "live");
    reply(cold, "live:stats", "", 503);
    run(cold, 16, 600);
    check(cold.snapshot()["failed"].boolean() && !cold.snapshot()["loaded"].boolean() &&
              cold.shows("did not answer"),
          "With nothing to show, Live says the server did not answer");
    FakeTransport full;
    full.refuse = true;
    Application queued(full, "live");
    run(queued, 16, 1500);
    check(full.count("live:stats") >= 2, "A refused read is asked for again");
    // The tab commit from the bar replaces the page: the route a registered tab answers.
    FakeTransport tabbed;
    Application bar(tabbed);
    bar.open("tab:live");
    check(bar.screenName() == "live", "tab:live is the same page");
}

// ---- Settings ---------------------------------------------------------------------------------

std::string zone(Application &app) {
    return app.snapshot()["zone"].string();
}
// The hit of the first row of a list ("sec:row:0"): where the list is on the canvas.
const Hit *hitOf(const Scene &scene, const std::string &id) {
    for (const auto &hit : scene.hits)
        if (hit.id == id)
            return &hit;
    return nullptr;
}
// Sends the reply of the newest write the layer has made, or of the one before it.
void answerWrite(Application &app, const FakeTransport &transport, bool ok = true,
                 const std::string &error = "", size_t from_end = 0, const std::string &body = "") {
    auto writes = transport.writes();
    check(writes.size() > from_end, "A write was made to answer");
    const FakeTransport::Request &request = *writes[writes.size() - 1 - from_end];
    Response response;
    response.channel = request.channel;
    response.generation = request.generation;
    response.status = ok ? 200 : 503;
    response.bytes = !body.empty() ? body
                     : ok          ? R"({"ok":true})"
                                   : std::string(R"({"ok":false,"error":")") + error + "\"}";
    app.response(response);
}

void settings() {
    FakeTransport transport;
    Application app(transport, "dashboard");
    uint32_t clock = run(app, 16, 300);
    reply(app, "caps", R"({"ok":true,"allowPower":true})");
    check(app.screenName() == "dashboard" && zone(app) == "sections" &&
              app.snapshot()["section"].string() == "system",
          "The dashboard route opens the list of sections, on the first");
    for (const char *key : {"oled", "game", "saver", "home"})
        check(transport.count(std::string("settings:data:") + key) == 1,
              "The sections a TV may lack are asked once");
    check(transport.count("settings:data:system") == 1, "The section in view is read as it opens");
    check(app.snapshot()["sections"].number() == 10 && app.snapshot()["loading"].boolean(),
          "All ten sections are listed at first, and the page waits for its data");
    check(app.shows("Loading…"), "A section being read shows the loading indicator");
    {
        // The indicator is the kit's turning arc: a few dozen nodes, moving from frame to frame.
        auto dots = [](const Scene &scene) {
            std::vector<float> xs;
            for (const auto &node : scene.nodes)
                if (node.kind == Node::Kind::Rectangle && node.rect.x > 1100 &&
                    node.rect.x < 1400 && node.rect.y > 450 && node.rect.y < 700 &&
                    node.rect.w < 12)
                    xs.push_back(node.rect.x);
            return xs;
        };
        std::vector<float> first = dots(app.frame());
        clock = run(app, clock, 350);
        std::vector<float> later = dots(app.frame());
        check(first.size() >= 16 && first.size() <= 40 && first.size() == later.size() &&
                  first != later,
              "The loading indicator is a small turning arc, not a layer");
    }
    reply(app, "settings:data:oled", R"({"ok":true,"available":false})");
    reply(app, "settings:data:game", section("game"));
    reply(app, "settings:data:system", fixture("stats-c4-webos9.json"));
    clock = run(app, clock, 800);
    check(app.snapshot()["sections"].number() == 9,
          "A section that says it is absent leaves the list");
    check(!app.snapshot()["loading"].boolean() && app.snapshot()["pages"].number() == 3,
          "Metrics arrive as three subsections");
    check(app.shows("Settings") && app.shows("Changes apply as soon as they are made.") &&
              app.shows("Load") && app.shows("Memory and storage") && app.shows("This TV"),
          "The list, the title and the subsections' tabs are drawn");

    // The page is 30% list and 70% content, both in view; every row is inside its pane.
    {
        const Scene &scene = app.frame();
        const Hit *section = hitOf(scene, "sec:row:0"), *row = hitOf(scene, "set:row:0");
        check(section && row, "Both lists are drawn");
        check(section->rect.x == 80 && section->rect.x + section->rect.w <= 600 &&
                  row->rect.x >= 640 && row->rect.x + row->rect.w <= 1840,
              "The sections are the left 30% and the rows the right 70%, inside the margins");
        check(row->rect.w > 2 * section->rect.w,
              "The rows are more than twice as wide as the sections");
    }

    // Up and Down in the list move the section, and the page follows.
    app.key(Key::Down);
    check(app.snapshot()["section"].string() == "apps" && zone(app) == "sections",
          "Down moves to the next section");
    check(transport.count("settings:data:apps") == 0 && app.snapshot()["loading"].boolean(),
          "The page waits for the focus to settle before it reads");
    clock = run(app, clock, 300);
    check(transport.count("settings:data:apps") == 1, "…then reads the section");
    reply(app, "settings:data:apps", section("apps"));
    clock = run(app, clock, 700);
    check(app.snapshot()["pages"].number() == 6 && app.snapshot()["page"].string() == "Installed",
          "Apps are six short subsections, the first showing");
    check(app.shows("Quick Access") && app.shows("Kept ready") && app.shows("Services"),
          "…named after what the dashboard grouped");
    app.key(Key::Up);
    check(app.snapshot()["section"].string() == "system" && !app.snapshot()["loading"].boolean(),
          "Up moves back; a section already read shows at once");

    // Left and Right never change the section.
    app.key(Key::Down);
    app.key(Key::Left);
    check(app.snapshot()["section"].string() == "apps" && zone(app) == "sections",
          "Left in the list changes nothing");
    app.key(Key::Right);
    check(zone(app) == "rows" && app.snapshot()["section"].string() == "apps",
          "Right moves the focus into the page");
    app.key(Key::Right);
    app.key(Key::Right);
    check(zone(app) == "rows" && app.snapshot()["section"].string() == "apps",
          "Right in the page does not open another section");
    app.key(Key::Left);
    check(zone(app) == "sections" && app.snapshot()["section"].string() == "apps",
          "Left in the page returns to the list");
    app.key(Key::Ok);
    check(zone(app) == "rows", "OK on a section moves into it as well");
    app.key(Key::Back);
    check(zone(app) == "sections" && app.screenName() == "dashboard",
          "Back in the page returns to the list");

    // Subsections: a row of tabs over the rows, and the one last shown is remembered.
    app.key(Key::Right);
    check(app.snapshot()["focus"].string() == "Install homebrew apps",
          "The page opens on its first row");
    app.key(Key::Up);
    check(zone(app) == "tabs", "Up from the first row reaches the tabs");
    app.key(Key::Right);
    app.key(Key::Right);
    check(app.snapshot()["page"].string() == "Quick Access" &&
              app.snapshot()["focus"].string() == "1",
          "Right on the tabs shows the next subsection");
    app.key(Key::Left);
    check(app.snapshot()["page"].string() == "Home screen", "Left steps back");
    app.key(Key::Down);
    check(zone(app) == "rows" && app.snapshot()["focus"].string() == "HDMI 1",
          "Down returns to the rows of that subsection");
    app.key(Key::Back);
    app.key(Key::Down);
    clock = run(app, clock, 300);
    app.key(Key::Up);
    check(app.snapshot()["section"].string() == "apps" &&
              app.snapshot()["page"].string() == "Home screen",
          "A section opens on the subsection it was left on");
    app.key(Key::Right);
    app.key(Key::Up);
    app.key(Key::Left);
    check(zone(app) == "tabs" && app.snapshot()["page"].string() == "Installed",
          "Left on the first tab ends on the first subsection");
    app.key(Key::Left);
    check(zone(app) == "sections", "…and then returns to the list");
    app.key(Key::Back);
    check(app.screenName() == "home", "Back in the list closes Settings, to where it was opened");

    // Directly into a section: the page has the focus, Back returns to the list and then leaves.
    FakeTransport direct;
    Application oled(direct, "dashboard:oled");
    run(oled, 16, 100);
    check(oled.screenName() == "dashboard:oled" && zone(oled) == "rows" &&
              direct.last("settings:data:oled")->operation == Operation::OledCare,
          "A section route opens its page");
    reply(oled, "settings:data:oled", section("oled"));
    uint32_t t = run(oled, 200, 700);
    check(oled.snapshot()["sections"].number() == 10 && oled.snapshot()["pages"].number() == 3 &&
              oled.snapshot()["focus"].string() == "Total power-on hours",
          "The list is there too, and the focus is on the page's first row");
    oled.key(Key::Back);
    check(zone(oled) == "sections", "Back goes to the list first");
    oled.key(Key::Back);
    check(oled.screenName() == "home", "…then closes Settings");
    (void)t;

    // A refresh keeps the focus and the scroll where they were, also on the last row, and a row
    // that shares its title with another stays itself.
    FakeTransport longer;
    Application home(longer, "dashboard:home");
    reply(home, "caps", R"({"ok":true})");
    reply(home, "settings:data:home", section("home"));
    t = run(home, 16, 700);
    home.key(Key::Up);
    check(zone(home) == "tabs", "The tabs are above the rows");
    home.key(Key::Right);
    check(home.snapshot()["page"].string() == "Appearance", "Appearance has the switches");
    home.key(Key::Down);
    for (int i = 0; i < 12; ++i)
        home.key(Key::Down);
    t = run(home, t, 1200);
    std::string last = home.snapshot()["focusId"].string();
    float scrolled = static_cast<float>(home.animation().value("set:sy"));
    check(last == "Profile#0" && scrolled < -50,
          "The last row is focused and the list has scrolled");
    for (int i = 0; i < 3; ++i) {
        reply(home, "settings:data:home", section("home"));
        t = run(home, t, 600);
        check(home.snapshot()["focusId"].string() == last && zone(home) == "rows" &&
                  std::fabs(home.animation().value("set:sy") - scrolled) < .5 &&
                  home.snapshot()["page"].string() == "Appearance",
              "A refresh while on the last row keeps the focus, the page and the scroll");
    }
    // Pressing Down past the last row leaves everything where it is.
    home.key(Key::Down);
    home.key(Key::Down);
    t = run(home, t, 600);
    check(home.snapshot()["focusId"].string() == last &&
              std::fabs(home.animation().value("set:sy") - scrolled) < .5,
          "Down on the last row stays on it");

    // Inputs has its own Order row, with the title the Apps page's has: it stays itself.
    FakeTransport twin;
    Application order(twin, "dashboard:home");
    reply(order, "settings:data:home", section("home"));
    t = run(order, 16, 700);
    order.key(Key::Up);
    for (int i = 0; i < 3; ++i)
        order.key(Key::Right);
    check(order.snapshot()["page"].string() == "Inputs", "The Inputs subsection");
    order.key(Key::Down);
    check(order.snapshot()["focusId"].string() == "Order#1",
          "Its Order row is the second of that title");
    reply(order, "settings:data:home", section("home"));
    run(order, t, 600);
    check(order.snapshot()["focusId"].string() == "Order#1" &&
              order.snapshot()["page"].string() == "Inputs",
          "A refresh leaves the focus on the Order it was on, not the first one");
}

// Every change shows at once, goes through the layer, and is taken back with a message when the
// TV refuses.
void writes() {
    FakeTransport transport;
    Application app(transport, "dashboard:options");
    reply(app, "caps", R"({"ok":true,"allowPower":true})");
    reply(app, "settings:data:options", optionsStats());
    uint32_t clock = run(app, 16, 700);
    check(app.snapshot()["page"].string() == "Power & standby" &&
              app.snapshot()["focus"].string() == "Quick Boot" &&
              app.snapshot()["focusOn"].boolean(),
          "Quick Boot is on, and first");
    check(transport.writes().empty(), "Nothing is written by looking");

    // A switch flips in the same frame as the key, before any reply.
    app.key(Key::Ok);
    check(!app.snapshot()["focusOn"].boolean() && app.snapshot()["focusPending"].boolean(),
          "The switch is already off, and marked as on its way");
    check(transport.writes().size() == 1 &&
              transport.writes()[0]->operation == Operation::Control &&
              transport.writes()[0]->body["action"].string() == "quickBoot" &&
              !transport.writes()[0]->body["value"].boolean(true),
          "…and the write is the same one the dashboard made");
    clock = run(app, clock, 400);
    check(!app.snapshot()["focusOn"].boolean() && app.snapshot()["focusPending"].boolean(),
          "With no reply it stays that way");
    // Refused: back, with the reason.
    answerWrite(app, transport, false, "not allowed here");
    clock = run(app, clock, 200);
    check(app.snapshot()["focusOn"].boolean() && !app.snapshot()["focusPending"].boolean(),
          "A refused change goes back");
    check(app.shows("That did not work") && app.shows("not allowed here"), "…and says why");

    // Accepted: it stays, and the TV is read again for the value it holds.
    app.key(Key::Ok);
    clock = run(app, clock, 200);
    answerWrite(app, transport);
    clock = run(app, clock, 50);
    check(!app.snapshot()["focusOn"].boolean() && !app.snapshot()["focusPending"].boolean(),
          "An accepted change stays");
    size_t reads = transport.count("settings:data:options");
    clock = run(app, clock, 1000);
    check(transport.count("settings:data:options") == reads + 1,
          "The TV is read once more after the answer");
    Json flipped = optionsStats();
    flipped.set("quickBoot", Json::flag(false));
    reply(app, "settings:data:options", flipped);
    clock = run(app, clock, 400);
    check(!app.snapshot()["focusOn"].boolean(), "What the TV says is what stays");

    // A second press before the first is answered is the newest wish; a late reply of the older
    // write does not move the switch back.
    size_t before = transport.writes().size();
    app.key(Key::Ok);
    check(app.snapshot()["focusOn"].boolean(), "The first press turns it on");
    app.key(Key::Ok);
    check(!app.snapshot()["focusOn"].boolean(), "The second turns it off again, in the same frame");
    clock = run(app, clock, 400);
    auto sent = transport.writes();
    check(sent.size() == before + 2 && sent[before]->body["value"].boolean() &&
              !sent[before + 1]->body["value"].boolean(true),
          "Both are sent, the newest last");
    answerWrite(app, transport, true, "", 1); // the older write answered late
    clock = run(app, clock, 100);
    check(!app.snapshot()["focusOn"].boolean(), "A late answer of an older write changes nothing");
    answerWrite(app, transport);
    clock = run(app, clock, 100);
    check(!app.snapshot()["focusOn"].boolean() && !app.snapshot()["focusPending"].boolean(),
          "The newest write's answer settles it");

    // A stepper shows its next value at once.
    FakeTransport timers;
    Application sleep(timers, "dashboard:options");
    reply(sleep, "settings:data:options", optionsStats());
    uint32_t at = run(sleep, 16, 700);
    sleep.key(Key::Up);
    sleep.key(Key::Right);
    check(sleep.snapshot()["page"].string() == "Timers", "The Timers subsection");
    sleep.key(Key::Down);
    check(sleep.snapshot()["focus"].string() == "Sleep timer" &&
              sleep.snapshot()["focusValue"].string() == "30 min",
          "The sleep timer reads 30 minutes");
    sleep.key(Key::Ok);
    check(sleep.snapshot()["focusValue"].string() == "60 min" &&
              sleep.snapshot()["focusPending"].boolean(),
          "OK steps it to 60 at once");
    check(timers.writes().size() == 1 &&
              timers.writes()[0]->body["action"].string() == "sleepTimer" &&
              timers.writes()[0]->body["value"].string() == "60",
          "…and writes that value");
    answerWrite(sleep, timers, false, "busy");
    at = run(sleep, at, 200);
    check(sleep.snapshot()["focusValue"].string() == "30 min", "A refusal puts the old value back");
    sleep.key(Key::Ok);
    check(sleep.snapshot()["focusValue"].string() == "60 min", "OK steps it again");
    answerWrite(sleep, timers);
    at = run(sleep, at, 400);
    sleep.key(Key::Ok);
    check(sleep.snapshot()["focusValue"].string() == "90 min", "The next press goes on from 60");
    at = run(sleep, at, 200);

    // An action asks twice, is pending until the TV answers, and says when it did not work.
    FakeTransport power;
    Application off(power, "dashboard:options");
    reply(off, "caps", R"({"ok":true,"allowPower":true})");
    reply(off, "settings:data:options", optionsStats());
    uint32_t when = run(off, 16, 700);
    off.key(Key::Up);
    for (int i = 0; i < 4; ++i)
        off.key(Key::Right);
    check(off.snapshot()["page"].string() == "Power", "The Power subsection");
    off.key(Key::Down);
    check(off.snapshot()["focus"].string() == "Power off", "Power off");
    off.key(Key::Ok);
    check(off.snapshot()["confirming"].boolean() && power.writes().empty(),
          "The first OK only asks");
    off.key(Key::Back);
    check(!off.snapshot()["confirming"].boolean() && zone(off) == "rows", "Back cancels it");
    off.key(Key::Ok);
    off.key(Key::Ok);
    check(power.writes().size() == 1 && power.writes()[0]->body["action"].string() == "powerOff" &&
              off.snapshot()["focusPending"].boolean(),
          "The second OK makes the write, and the row is pending at once");
    off.key(Key::Ok);
    check(power.writes().size() == 1, "A second press while it is pending is dropped");
    answerWrite(off, power, false, "refused");
    when = run(off, when, 200);
    check(!off.snapshot()["focusPending"].boolean() && off.shows("refused"),
          "A refusal clears the pending state and says so");

    // The network switch says it is switching, and waits for the server to come back.
    FakeTransport network;
    Application web(network, "dashboard:settings");
    reply(web, "settings:data:settings", section("setup"));
    uint32_t w = run(web, 16, 700);
    check(web.snapshot()["focus"].string() == "Web dashboard" &&
              web.snapshot()["focusOn"].boolean(),
          "The web dashboard is on");
    web.key(Key::Ok);
    check(!web.snapshot()["focusOn"].boolean() && web.shows("Switching…") &&
              network.writes().size() == 1 &&
              network.writes()[0]->body["action"].string() == "network" &&
              !network.writes()[0]->body["open"].boolean(true),
          "It reads Switching… the moment OK is pressed, and writes the opposite to /api/setup");
    answerWrite(web, network);
    w = run(web, w, 1800);
    check(network.count("settings:wait") == 1 && web.snapshot()["busy"].boolean(),
          "Then it waits for the server to answer again");
    reply(web, "settings:wait", "", 503);
    w = run(web, w, 1100);
    check(network.count("settings:wait") == 2, "…asking once a second");
    size_t asked = network.count("settings:data:settings");
    reply(web, "settings:wait", section("setup"));
    check(!web.snapshot()["busy"].boolean() && network.count("settings:data:settings") == asked + 1,
          "…and reads the section when it does");
    // A screen saver choice steps at once, and starting one waits for the OK to be let go.
    FakeTransport savers;
    Application saver(savers, "dashboard:saver");
    reply(saver, "settings:data:saver", section("saver"));
    uint32_t sv = run(saver, 16, 700);
    saver.key(Key::Up);
    saver.key(Key::Right);
    check(saver.snapshot()["page"].string() == "Options", "The saver's Options subsection");
    saver.key(Key::Down);
    check(saver.snapshot()["focus"].string() == "Brightness" &&
              saver.snapshot()["focusValue"].string() == "Dim",
          "Brightness reads Dim");
    saver.key(Key::Ok);
    check(saver.snapshot()["focusValue"].string() != "Dim" &&
              saver.snapshot()["focusPending"].boolean() && savers.writes().size() == 1,
          "OK steps the brightness at once and writes it");
    answerWrite(saver, savers, false, "no screen");
    sv = run(saver, sv, 200);
    check(saver.snapshot()["focusValue"].string() == "Dim" && saver.shows("no screen"),
          "A refused brightness goes back to Dim and says why");
    saver.key(Key::Down);
    check(saver.snapshot()["focus"].string() == "Start screen saver", "Start screen saver");
    saver.key(Key::Ok);
    sv = run(saver, sv, 100);
    check(savers.writes().size() == 1 && saver.shows("Starting the screen saver"),
          "Starting one only says so until the OK is let go");
    sv = run(saver, sv, 1100);
    check(savers.writes().size() == 2 &&
              savers.writes()[1]->body["action"].string() == "screensaver" &&
              saver.snapshot()["focusPending"].boolean(),
          "Then it is asked for, and the row is pending");
    answerWrite(saver, savers, false, "not now");
    sv = run(saver, sv, 200);
    check(!saver.snapshot()["focusPending"].boolean() && saver.shows("not now"),
          "A refused start clears the pending state and says why");
    // A switch the server refuses is put back, and the row can be pressed again.
    FakeTransport refusedNetwork;
    Application refusedWeb(refusedNetwork, "dashboard:settings");
    reply(refusedWeb, "settings:data:settings", section("setup"));
    uint32_t refusedAt = run(refusedWeb, 16, 700);
    refusedWeb.key(Key::Ok);
    check(refusedWeb.shows("Switching…") && refusedWeb.snapshot()["busy"].boolean(),
          "A refused network switch also says it is switching until the answer");
    answerWrite(refusedWeb, refusedNetwork, false, "not allowed");
    refusedAt = run(refusedWeb, refusedAt, 200);
    check(refusedWeb.snapshot()["focusOn"].boolean() && !refusedWeb.shows("Switching…") &&
              !refusedWeb.snapshot()["busy"].boolean() && refusedWeb.shows("not allowed"),
          "A refused network switch goes back to on, says why, and is not left switching");
    refusedWeb.key(Key::Ok);
    check(refusedNetwork.writes().size() == 2, "The row can be pressed again");
    // A page left while a change is pending still has it answered.
    FakeTransport left;
    Application away(left, "dashboard:options");
    reply(away, "settings:data:options", optionsStats());
    uint32_t gone = run(away, 16, 700);
    away.key(Key::Ok);
    away.key(Key::Left);
    away.key(Key::Back);
    check(away.screenName() == "home" && left.writes().size() == 1,
          "Leaving does not lose the write");
    answerWrite(away, left);
    run(away, gone, 100);
}

// ---- Catalog
// ---------------------------------------------------------------------------------------

void catalog() {
    FakeTransport transport;
    Application app(transport, "catalog");
    run(app, 16, 100);
    check(app.screenName() == "catalog" &&
              transport.last("catalog:list")->operation == Operation::AppCatalog &&
              transport.last("catalog:status")->operation == Operation::InstallStatus,
          "The catalog reads the list and the install status");
    run(app, 150, 200);
    check(app.shows("Loading the catalog…"), "It says it is loading");
    reply(app, "catalog:list", section("catalog"));
    reply(app, "catalog:status", R"({"ok":true,"state":"idle"})");
    uint32_t clock = run(app, 400, 600);
    check(app.snapshot()["apps"].number() == 7 &&
              app.snapshot()["focus"].string() == "org.example.kodi",
          "An update comes first");
    check(app.shows("Update") && app.shows("Installed") && app.shows("Review and update"),
          "The tiles and the focused app are described");
    app.key(Key::Right);
    check(app.snapshot()["focus"].string() == "org.example.clock", "Titles follow the updates");
    app.key(Key::Down);
    check(app.snapshot()["focus"].string() == "org.example.weather",
          "Down goes to the next row, which is shorter");
    app.key(Key::Up);
    app.key(Key::Up);
    check(app.snapshot()["barFocus"].boolean(), "Up from the first row gives the focus to the bar");
    app.key(Key::Down);
    // An app that comes from the Homebrew Channel is not installed here.
    for (int i = 0; i < 6 && app.snapshot()["focus"].string() != "org.webosbrew.hbchannel"; ++i)
        app.key(Key::Right);
    size_t writes = transport.writes().size();
    app.key(Key::Ok);
    check(transport.writes().size() == writes, "The Homebrew Channel updates itself");
    // An installed app is uninstalled with a second OK.
    app.key(Key::Left);
    check(app.snapshot()["focus"].string() == "org.example.devmanager", "Dev Manager is installed");
    app.key(Key::Ok);
    check(transport.writes().size() == writes, "The first OK on an installed app only asks");
    app.key(Key::Ok);
    check(transport.writes().back()->operation == Operation::UninstallApp &&
              transport.writes().back()->body["id"].string() == "org.example.devmanager",
          "The second OK uninstalls it");
    clock = run(app, clock, 100);
    check(app.shows("Removing…"), "The app is marked as being removed at once");
    answerWrite(app, transport, false, "busy");
    clock = run(app, clock, 100);
    check(!app.shows("Removing…") && app.shows("busy") && transport.count("catalog:list") == 1,
          "A refused removal puts the tile back and says why");
    app.key(Key::Ok);
    app.key(Key::Ok);
    check(transport.writes().back()->operation == Operation::UninstallApp,
          "It can be asked for again");
    answerWrite(app, transport);
    clock = run(app, clock, 50);
    check(transport.count("catalog:list") == 2 && app.shows("Uninstalled Dev Manager"),
          "The list is read again, and the toast names the app");

    // Choosing a new app fetches it; the preview waits for OK on Install.
    for (int i = 0; i < 8 && app.snapshot()["focus"].string() != "org.example.photos"; ++i)
        app.key(Key::Right);
    check(app.snapshot()["focus"].string() == "org.example.photos", "Photo Frame is a new app");
    app.key(Key::Ok);
    check(transport.writes().back()->operation == Operation::FetchPackage &&
              transport.writes().back()->body["id"].string() == "org.example.photos" &&
              transport.writes().back()->body["auto"].boolean(),
          "OK on a new app asks the server to fetch it");
    check(app.snapshot()["view"].string() == "busy" && app.shows("Starting…"),
          "…and the page is already under way, before the server answers");
    Json preview = section("install-preview");
    Json fetched = Json::object();
    fetched.set("ok", Json::flag(true));
    fetched.set("install", preview);
    answerWrite(app, transport, true, "", 0, fetched.dump());
    clock = run(app, clock + 800, 600);
    check(app.snapshot()["view"].string() == "preview" &&
              app.snapshot()["focus"].string() == "Install",
          "The server's preview opens with Install focused");
    check(app.shows("Install Photo Frame") &&
              app.shows("Nothing is installed until OK is pressed here."),
          "The preview says what happens");
    app.key(Key::Up);
    app.key(Key::Up);
    check(app.snapshot()["focus"].string() == "Run services as root", "Root is a switch");
    app.key(Key::Ok);
    check(app.snapshot()["root"].boolean(), "OK on it turns it on");
    app.key(Key::Down);
    app.key(Key::Down);
    app.key(Key::Ok);
    check(transport.writes().back()->operation == Operation::ConfirmPackage &&
              transport.writes().back()->body["jobId"].string() == "job-1" &&
              transport.writes().back()->body["elevate"].boolean() &&
              !transport.writes().back()->body["replaceStore"].boolean(true),
          "Install confirms the job with the chosen privileges");
    check(app.snapshot()["view"].string() == "busy", "…and the progress card is up at once");
    Json running = Json::parse(
        R"({"ok":true,"install":{"state":"downloading","jobId":"job-1","progress":{"bytes":2600000,"total":5200000}}})");
    answerWrite(app, transport, true, "", 0, running.dump());
    clock = run(app, clock + 100, 600);
    check(app.snapshot()["view"].string() == "busy" && app.shows("Downloading 2 MB of 5 MB"),
          "A running install shows its progress");
    app.key(Key::Back);
    check(app.snapshot()["view"].string() == "busy" && app.screenName() == "catalog",
          "Back does nothing while it runs");
    check(transport.count("catalog:status") >= 1, "The status is asked for");
    size_t listReads = transport.count("catalog:list");
    reply(
        app, "catalog:status",
        R"({"ok":true,"state":"installed","jobId":"job-1","result":{"title":"Photo Frame","version":"1.4.0"}})");
    clock = run(app, clock + 200, 600);
    check(app.snapshot()["view"].string() == "result" && app.shows("Installed Photo Frame v1.4.0."),
          "A finished install is reported");
    check(transport.count("catalog:list") == listReads + 1,
          "The catalog is read again when a job ends");
    app.key(Key::Ok);
    check(app.snapshot()["view"].string() == "list", "OK returns to the catalog");

    // Back on the preview discards the download.
    FakeTransport second;
    Application again(second, "catalog");
    reply(again, "catalog:list", section("catalog"));
    reply(again, "catalog:status", section("install-preview"));
    run(again, 16, 600);
    check(again.snapshot()["view"].string() == "preview",
          "A preview that is waiting is shown on opening");
    again.key(Key::Back);
    check(second.writes().back()->operation == Operation::CancelPackage &&
              second.writes().back()->body["jobId"].string() == "job-1",
          "Back on the preview cancels the job");
    check(again.snapshot()["view"].string() == "list", "…and the preview is gone at once");
    answerWrite(again, second, true, "", 0, R"({"ok":true,"install":{"state":"idle"}})");
    run(again, 700, 100);
    check(again.snapshot()["view"].string() == "list", "…and shows the catalog");
    again.key(Key::Back);
    check(again.screenName() == "dashboard:apps", "Back from the catalog returns to Apps");

    // A failed install, and a catalog that does not load.
    FakeTransport failing;
    Application failed(failing, "catalog");
    reply(failed, "catalog:list", "", 503);
    reply(failed, "catalog:status",
          R"({"ok":true,"state":"error","jobId":"job-2","error":"checksum mismatch"})");
    run(failed, 16, 300);
    check(failed.snapshot()["apps"].number() == 0 && failed.shows("Catalog unavailable"),
          "A catalog that does not load says so");
    size_t listed = failing.count("catalog:list");
    failed.key(Key::Ok);
    check(failing.count("catalog:list") == listed + 1, "OK tries again");

    // A refused fetch goes back to the grid with a message; one refused on the preview goes back
    // to the preview.
    {
        FakeTransport refusing;
        Application tried(refusing, "catalog");
        reply(tried, "catalog:list", section("catalog"));
        reply(tried, "catalog:status", R"({"ok":true,"state":"idle"})");
        uint32_t at = run(tried, 16, 600);
        for (int i = 0; i < 8 && tried.snapshot()["focus"].string() != "org.example.photos"; ++i)
            tried.key(Key::Right);
        tried.key(Key::Ok);
        check(tried.snapshot()["view"].string() == "busy", "The fetch shows at once");
        answerWrite(tried, refusing, false, "no space");
        at = run(tried, at, 300);
        check(tried.snapshot()["view"].string() == "list" && tried.shows("no space") &&
                  tried.shows("The install could not be started"),
              "A refused fetch returns to the grid and says why");
        // The preview, then Install refused: the preview is back.
        tried.key(Key::Ok);
        Json fetched = Json::object();
        fetched.set("ok", Json::flag(true));
        fetched.set("install", section("install-preview"));
        answerWrite(tried, refusing, true, "", 0, fetched.dump());
        at = run(tried, at, 600);
        check(tried.snapshot()["view"].string() == "preview", "The preview is up");
        tried.key(Key::Ok);
        check(tried.snapshot()["view"].string() == "busy", "Install is under way at once");
        answerWrite(tried, refusing, false, "disk full");
        run(tried, at, 300);
        check(tried.snapshot()["view"].string() == "preview" && tried.shows("disk full"),
              "A refused install returns to the preview");
        // Once an admin PIN is set the install answers 403 with pin: true: the PIN prompt opens
        // over the catalog, and the session it unlocks sends the person back to a fresh catalog.
        tried.key(Key::Ok);
        const FakeTransport::Request &installing = *refusing.writes().back();
        Response denied;
        denied.channel = installing.channel;
        denied.generation = installing.generation;
        denied.status = 403;
        denied.bytes = R"({"ok":false,"error":"admin PIN required","pin":true})";
        tried.response(denied);
        at = run(tried, at, 700);
        check(tried.screenName() == "pin" && tried.shows("Enter the admin PIN") &&
                  tried.shows("admin PIN required"),
              "An install the server wants the PIN for opens the PIN prompt");
        tried.input("0000");
        for (Key key : {Key::Down, Key::Right, Key::Ok}) {
            tried.key(key);
            at = run(tried, at, 40);
        }
        check(refusing.last("pin:unlock") && refusing.last("pin:unlock")->body["pin"].string() == "0000" &&
                  refusing.last("pin:unlock")->operation == Operation::AdminUnlock,
              "Done asks the server to unlock with the digits");
        reply(tried, "pin:unlock", R"({"ok":true,"session":"0f3a9c0f3a9c0f3a9c0f3a9c0f3a9cab"})");
        run(tried, at, 700);
        check(tried.screenName() == "catalog" && refusing.session == "0f3a9c0f3a9c0f3a9c0f3a9c0f3a9cab" &&
                  tried.shows("Unlocked. Do that again."),
              "An unlock returns to the catalog, keeps the session and does not install by itself");
    }
}
// ---- Idle dimming: every screen, not only Home -------------------------------------------------

// The OLED must not hold a still picture: thirty quiet seconds dim the screen, and the first key
// only wakes it.
bool dimmed(Application &app, float expected) {
    const Scene &scene = app.frame();
    if (scene.nodes.empty())
        return expected == 0;
    const Node &last = scene.nodes.back();
    bool veil = last.rect.w == 1920 && last.rect.h == 1080 && last.color.r == 0 &&
                last.color.g == 0 && last.color.b == 0 && last.color.a > .002f;
    return expected == 0 ? !veil : veil && std::fabs(last.color.a - expected) < .01f;
}
void idle() {
    {
        FakeTransport transport;
        Application app(transport, "live");
        reply(app, "live:stats", fixture("stats-c4-webos9.json"));
        uint32_t clock = run(app, 16, 1500);
        app.key(Key::Right);
        check(app.snapshot()["focus"].number() == 1, "Live: a key moves the focus");
        clock = run(app, clock, 29000);
        check(!app.snapshot()["idle"].boolean() && dimmed(app, 0),
              "Live is not dimmed before thirty seconds without a key");
        clock = run(app, clock, 4000);
        check(app.snapshot()["idle"].boolean() && dimmed(app, .66f),
              "Live dims to two thirds black after thirty seconds without a key");
        app.key(Key::Right);
        check(app.snapshot()["focus"].number() == 1 && !app.snapshot()["idle"].boolean(),
              "The first key after the dim only wakes the screen");
        clock = run(app, clock, 400);
        check(dimmed(app, 0), "The dim lifts on the first key");
        app.key(Key::Right);
        check(app.snapshot()["focus"].number() == 2, "The next key acts");
        run(app, clock, 31000);
        check(app.snapshot()["idle"].boolean(), "A key restarts the thirty seconds");
    }
    for (const char *route : {"dashboard", "catalog", "setup:welcome"}) {
        FakeTransport transport;
        Application app(transport, route);
        run(app, 16, 33000);
        check(app.snapshot()["idle"].boolean() && dimmed(app, .66f),
              (std::string(route) + " dims after thirty seconds without a key").c_str());
    }
}

} // namespace

int main() {
    try {
        models();
        panel();
        live();
        settings();
        writes();
        catalog();
        idle();
        std::printf("%d dashboard screen checks passed\n", checks);
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
