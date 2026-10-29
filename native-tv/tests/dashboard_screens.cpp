// Live, Settings and Catalog: what the models say of the TV's real readings (snapshots against
// tests/fixtures/tv), and what the screens do with keys and replies. The snapshots are text files
// beside the fixtures; NATIVETV_UPDATE_SNAPSHOTS=1 writes them again after a deliberate change.
#include "domain/live_cards.hpp"
#include "domain/settings_items.hpp"
#include "ui/application.hpp"
#include "ui/kit/widgets.hpp"
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
    };
    std::vector<Request> requests;
    std::map<std::string, unsigned> revisions;
    bool refuse = false;
    unsigned read(const std::string &channel, Operation operation,
                  const Json &arguments = Json()) override {
        requests.push_back({channel, operation, arguments});
        return refuse ? 0 : ++revisions[channel];
    }
    unsigned write(const std::string &channel, Operation operation, const Json &body) override {
        requests.push_back({channel, operation, body});
        return refuse ? 0 : ++revisions[channel];
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
std::string describe(const std::vector<SettingItem> &items) {
    static const char *const kinds[] = {"heading", "info", "toggle", "choice", "action", "link"};
    std::string text;
    for (const SettingItem &item : items) {
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
        text += " <" + item.icon + ">\n";
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
        snapshot("rows-" + group.key, describe(settingItems(group.key, rows)));
        check(!settingGroupStatus(group.key, data[group.key]).empty() || group.key == "game" ||
                  group.key == "apps" || group.key == "options",
              "A loaded section tells a few words of itself");
    }
    snapshot("rows-privacy-consents",
             describe(settingItems("privacy",
                                   dashboardRows("privacy", data["privacy"], caps, "consents"))));
    snapshot("rows-privacy-services",
             describe(settingItems("privacy",
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

// ---- Settings
// --------------------------------------------------------------------------------------

std::string focus(Application &app) {
    return app.snapshot()["focus"].string();
}
// Moves down until the focused row is `title`; false when it never is.
bool goTo(Application &app, const std::string &title) {
    for (int i = 0; i < 80 && focus(app) != title; ++i)
        app.key(Key::Down);
    return focus(app) == title;
}
void settings() {
    FakeTransport transport;
    Application app(transport, "dashboard");
    uint32_t clock = run(app, 16, 300);
    reply(app, "caps", R"({"ok":true,"allowPower":true})");
    check(app.screenName() == "dashboard" && app.snapshot()["level"].string() == "groups",
          "The dashboard route lists the sections");
    for (const char *key : {"oled", "game", "saver", "home"})
        check(transport.count(std::string("settings:probe:") + key) == 1,
              "The sections a TV may lack are asked once");
    check(app.snapshot()["rows"].number() == 10, "All ten sections are listed at first");
    reply(app, "settings:probe:oled", R"({"ok":true,"available":false})");
    reply(app, "settings:probe:game", section("game"));
    clock = run(app, clock, 300);
    check(app.snapshot()["rows"].number() == 9, "A section that says it is absent leaves the list");
    check(app.shows("Metrics") && app.shows("Changes apply as soon as they are made."),
          "The list is drawn");

    app.key(Key::Down);
    check(focus(app) == "Apps", "Down moves along the sections");
    app.key(Key::Ok);
    check(app.screenName() == "dashboard:apps" &&
              transport.last("settings:data")->operation == Operation::Apps,
          "OK opens a section and reads its data");
    run(app, clock, 200);
    check(app.shows("Loading…"), "The section says it is loading");
    reply(app, "settings:data", section("apps"));
    clock = run(app, clock + 300, 500);
    check(app.snapshot()["rows"].number() > 8 && app.shows("Uninstall Applications"),
          "The rows are listed");

    // A change that is hard to undo asks twice.
    check(goTo(app, "Homebrew Channel"), "An installed app is a row");
    size_t writes = transport.count("settings:write");
    app.key(Key::Ok);
    check(app.snapshot()["confirming"].boolean() && transport.count("settings:write") == writes,
          "The first OK on an uninstall only asks");
    run(app, clock, 100);
    check(app.shows("Confirm?"), "The row says it is waiting for a second OK");
    app.key(Key::Back);
    check(!app.snapshot()["confirming"].boolean() && app.screenName() == "dashboard:apps",
          "Back cancels the question and stays on the page");
    app.key(Key::Ok);
    app.key(Key::Ok);
    check(transport.count("settings:write") == writes + 1 &&
              transport.last("settings:write")->operation == Operation::UninstallApp &&
              transport.last("settings:write")->body["id"].string() == "org.webosbrew.hbchannel",
          "The second OK uninstalls through the same route the dashboard used");
    size_t reads = transport.count("settings:data");
    app.key(Key::Ok);
    check(transport.count("settings:write") == writes + 1,
          "Nothing else is sent while a write is out");
    reply(app, "settings:write", R"({"ok":true})");
    check(transport.count("settings:data") == reads + 1, "A change reads the section again");
    run(app, clock + 200, 100);
    check(app.shows("Saved"), "A change that is not a switch says it was saved");

    // A refused change says why, and the rows come back.
    check(goTo(app, "Promotions"), "A Home screen part is a switch");
    app.key(Key::Ok);
    check(transport.last("settings:write")->operation == Operation::SetLgHomeLayout &&
              transport.last("settings:write")->body["part"].string() == "promotions" &&
              transport.last("settings:write")->body["shown"].boolean(),
          "A switch writes its opposite");
    reads = transport.count("settings:data");
    reply(app, "settings:write", R"({"ok":false,"error":"Not allowed on this firmware"})");
    run(app, clock + 400, 100);
    check(app.shows("Not allowed on this firmware") &&
              transport.count("settings:data") == reads + 1,
          "A refusal is shown and the section is read again");

    // Left and Right step through the sections; Back returns to the list.
    app.key(Key::Right);
    check(app.screenName() == "dashboard:saver" || app.screenName() == "dashboard:game",
          "Right opens the next section that exists");
    app.key(Key::Left);
    app.key(Key::Left);
    check(app.screenName() == "dashboard:system",
          "Left steps back, over the section that is absent");
    app.key(Key::Back);
    check(app.screenName() == "dashboard" && app.snapshot()["level"].string() == "groups",
          "Back returns to the list of sections");
    app.key(Key::Back);
    check(app.screenName() == "home", "Back from the list leaves for Home");

    // A section that does not load says so, and OK asks again.
    FakeTransport dead;
    Application unreachable(dead, "dashboard:service");
    reply(unreachable, "settings:data", "", 503);
    run(unreachable, 16, 400);
    check(unreachable.shows("Could not load this screen") && unreachable.shows("did not answer"),
          "A section that does not answer says so");
    size_t asked = dead.count("settings:data");
    unreachable.key(Key::Ok);
    check(dead.count("settings:data") == asked + 1, "OK on it asks again");
    reply(unreachable, "settings:data", section("service"));
    run(unreachable, 500, 400);
    check(unreachable.shows("Open EZ Adjust"), "…and the rows come once it answers");

    // Metrics link to Live.
    FakeTransport metrics;
    Application readings(metrics, "dashboard:system");
    reply(readings, "settings:data", fixture("stats-c4-webos9.json"));
    run(readings, 16, 400);
    check(focus(readings) == "Live", "Metrics start with a way to the Live cards");
    readings.key(Key::Ok);
    check(readings.screenName() == "live", "OK opens Live");

    // A section opened by its route goes straight to its rows and Back leaves.
    FakeTransport direct;
    Application oled(direct, "dashboard:oled");
    run(oled, 16, 100);
    check(oled.screenName() == "dashboard:oled" &&
              direct.last("settings:data")->operation == Operation::OledCare,
          "A section route opens its rows");
    reply(oled, "settings:data", section("oled"));
    run(oled, 200, 400);
    check(oled.shows("Total power-on hours"), "The panel's readings are rows");
    check(goTo(oled, "Global sticky reduction"), "A protection is a row");
    oled.key(Key::Ok);
    check(oled.snapshot()["confirming"].boolean(), "Switching a protection off asks first");
    oled.key(Key::Ok);
    check(direct.last("settings:write")->operation == Operation::Control &&
              direct.last("settings:write")->body["action"].string() == "oledProtection" &&
              !direct.last("settings:write")->body["value"]["enabled"].boolean(true),
          "…and then sends it");
    reply(oled, "settings:write", R"({"ok":true})");
    reply(oled, "settings:data", section("oled"));
    oled.key(Key::Back);
    check(oled.screenName() == "home", "Back from a section opened by its route goes Home");

    // Read-only rows do nothing; a toggle slides.
    FakeTransport system;
    Application options(system, "dashboard:options");
    reply(options, "caps", R"({"ok":true,"allowPower":true})");
    reply(options, "settings:data", optionsStats());
    uint32_t at = run(options, 16, 400);
    check(goTo(options, "Quick Boot"), "A power setting is a row");
    size_t sent = system.count("settings:write");
    options.key(Key::Ok);
    check(system.count("settings:write") == sent + 1 &&
              system.last("settings:write")->body["action"].string() == "quickBoot" &&
              !system.last("settings:write")->body["value"].boolean(true),
          "OK on a switch writes the opposite");
    Json flipped = optionsStats();
    flipped.set("quickBoot", Json::flag(false));
    reply(options, "settings:write", R"({"ok":true})");
    reply(options, "settings:data", flipped);
    at = run(options, at, 100);
    run(options, at, 400);
    check(options.snapshot()["rows"].number() > 10, "The rows stay after a change");
    check(goTo(options, "Power off"), "Power actions are offered when the server allows them");
    options.key(Key::Ok);
    check(options.snapshot()["confirming"].boolean(), "Power off asks twice");
    options.key(Key::Back);
    // The system page refreshes by itself.
    size_t polled = system.count("settings:data");
    run(options, at + 400, 11000);
    check(system.count("settings:data") > polled, "System refreshes every ten seconds while open");

    // A list inside a section: Privacy's consents.
    FakeTransport privacy;
    Application consents(privacy, "dashboard:privacy");
    reply(consents, "settings:data", section("privacy"));
    run(consents, 16, 400);
    check(goTo(consents, "Consents switched on"), "Privacy links its consents");
    consents.key(Key::Ok);
    check(consents.snapshot()["sub"].string() == "consents" &&
              consents.shows("Viewing information"),
          "OK opens the consents");
    consents.key(Key::Left);
    check(consents.snapshot()["sub"].string().empty() &&
              consents.screenName() == "dashboard:privacy",
          "Left from a list inside a section returns to the section");
    check(goTo(consents, "Consents switched on"), "…at the row that opened it");
    consents.key(Key::Ok);
    consents.key(Key::Back);
    check(consents.snapshot()["sub"].string().empty(), "So does Back");

    // The server restarting after the dashboard is opened to the network.
    FakeTransport network;
    Application web(network, "dashboard:settings");
    reply(web, "settings:data", section("setup"));
    uint32_t t = run(web, 16, 400);
    check(goTo(web, "Web dashboard"), "The web dashboard is a switch");
    web.key(Key::Ok);
    check(network.last("settings:write")->operation == Operation::Setup &&
              network.last("settings:write")->body["action"].string() == "network" &&
              !network.last("settings:write")->body["open"].boolean(true),
          "It writes the opposite to /api/setup");
    reply(web, "settings:write", R"({"ok":true})");
    t = run(web, t, 1800);
    check(network.count("settings:wait") == 1 && web.snapshot()["busy"].boolean(),
          "Then it waits for the server to answer again");
    reply(web, "settings:wait", "", 503);
    t = run(web, t, 1100);
    check(network.count("settings:wait") == 2, "…asking once a second");
    reply(web, "settings:wait", section("setup"));
    check(!web.snapshot()["busy"].boolean() && network.count("settings:data") >= 2,
          "…and reads the section when it does");
    // Anything that leaves the page: Home Assistant is B3's page.
    check(goTo(web, "Home Assistant"), "Home Assistant is a row");
    web.key(Key::Ok);
    check(web.screenName() != "dashboard:settings", "Its row opens another page");
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
    size_t writes = transport.count("catalog:write");
    app.key(Key::Ok);
    check(transport.count("catalog:write") == writes, "The Homebrew Channel updates itself");
    // An installed app is uninstalled with a second OK.
    app.key(Key::Left);
    check(app.snapshot()["focus"].string() == "org.example.devmanager", "Dev Manager is installed");
    app.key(Key::Ok);
    check(transport.count("catalog:write") == writes, "The first OK on an installed app only asks");
    app.key(Key::Ok);
    check(transport.last("catalog:write")->operation == Operation::UninstallApp &&
              transport.last("catalog:write")->body["id"].string() == "org.example.devmanager",
          "The second OK uninstalls it");
    reply(app, "catalog:write", R"({"ok":true})");
    check(transport.count("catalog:list") == 2, "The list is read again");

    // Choosing a new app fetches it; the preview waits for OK on Install.
    for (int i = 0; i < 8 && app.snapshot()["focus"].string() != "org.example.photos"; ++i)
        app.key(Key::Right);
    check(app.snapshot()["focus"].string() == "org.example.photos", "Photo Frame is a new app");
    app.key(Key::Ok);
    check(transport.last("catalog:write")->operation == Operation::FetchPackage &&
              transport.last("catalog:write")->body["id"].string() == "org.example.photos" &&
              transport.last("catalog:write")->body["auto"].boolean(),
          "OK on a new app asks the server to fetch it");
    Json preview = section("install-preview");
    Json fetched = Json::object();
    fetched.set("ok", Json::flag(true));
    fetched.set("install", preview);
    reply(app, "catalog:write", fetched);
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
    check(transport.last("catalog:write")->operation == Operation::ConfirmPackage &&
              transport.last("catalog:write")->body["jobId"].string() == "job-1" &&
              transport.last("catalog:write")->body["elevate"].boolean() &&
              !transport.last("catalog:write")->body["replaceStore"].boolean(true),
          "Install confirms the job with the chosen privileges");
    Json running = Json::parse(
        R"({"ok":true,"install":{"state":"downloading","jobId":"job-1","progress":{"bytes":2600000,"total":5200000}}})");
    reply(app, "catalog:write", running);
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
    check(second.last("catalog:write")->operation == Operation::CancelPackage &&
              second.last("catalog:write")->body["jobId"].string() == "job-1",
          "Back on the preview cancels the job");
    reply(again, "catalog:write", R"({"ok":true,"install":{"state":"idle"}})");
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
}
} // namespace

int main() {
    try {
        models();
        panel();
        live();
        settings();
        catalog();
        std::printf("%d dashboard screen checks passed\n", checks);
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
