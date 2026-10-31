#include "domain/models.hpp"
#include "ui/keyboard.hpp"
#include <cstdio>
#include <stdexcept>

using namespace nativetv;
namespace {
int checks = 0;
void check(bool condition, const char *message) {
    ++checks;
    if (!condition)
        throw std::runtime_error(message);
}
const MenuRow &find(const std::vector<MenuRow> &rows, const std::string &label) {
    for (const auto &row : rows)
        if (row.label == label)
            return row;
    throw std::runtime_error("Missing row: " + label);
}
} // namespace
int main() {
    try {
        Json parsed = Json::parse("{\"title\":\"Café 🌿\",\"items\":[1,true,null]}");
        check(parsed["title"].string() == "Café 🌿", "JSON preserves Unicode");
        check(parsed["items"].size() == 3, "JSON arrays remain bounded and indexable");
        check(!Json::parse("{broken").valid(), "Malformed JSON is rejected");
        check(!Json::parse(std::string(8 * 1024 * 1024 + 1, 'x')).valid(),
              "Oversized JSON is rejected");
        Json copy = parsed;
        parsed = Json();
        check(copy["title"].valid(), "JSON ownership survives independent copies");
        check(encode("a b&/é") == "a%20b%26%2F%C3%A9",
              "Query encoding handles UTF-8 and delimiters");
        Keyboard keyboard;
        keyboard.reset("é🌿", 3);
        keyboard.press("left");
        check(keyboard.cursor() == 2, "Cursor moves across a complete UTF-8 code point");
        check(keyboard.erase() && keyboard.value() == "🌿",
              "Backspace removes a complete UTF-8 code point");
        check(keyboard.insert("a") && keyboard.value() == "a🌿", "Insertion respects the cursor");
        check(keyboard.insert("b") && !keyboard.insert("c"),
              "Character limits count Unicode characters");
        check(!keyboard.insert("\n"), "Input excludes control characters");
        keyboard.reset("", 20);
        keyboard.press("shift");
        keyboard.press("a");
        check(keyboard.value() == "A", "Shift changes letter input");
        keyboard.press("symbols");
        check(keyboard.rows()[1][0] == "!", "Symbol layer is available");
        Json catalogue = Json::parse(
            R"({"apps":[{"id":"a","title":"Alpha"},{"id":"b","title":"Beta"}],"devices":[{"id":"hdmi1","name":"Console","port":1}],"channels":[],"order":{"app":["b","a","lg-home"],"dev":["hdmi1"],"ch":[]},"layout":{"rows":["app","dev"],"hidden":{"app":["b"]},"showInputs":true,"showLive":false,"showLgHome":false},"sources":{"channels":0}})");
        auto tabs = homeTabs(catalogue);
        check(tabs.size() == 3 && tabs[2].key == "inputs",
              "Home tabs honor tuner availability and layout");
        auto rows = homeRows("home", catalogue, Json(), Json());
        check(rows.size() == 1 && rows[0].cards.size() == 1 && rows[0].cards[0].id == "a",
              "Hidden apps and LG Home remain hidden");
        auto inputs = homeRows("inputs", catalogue, Json(), Json());
        check(inputs[0].cards[0].command.body["kind"].string() == "dev",
              "Input launch uses the existing API contract");
        Json byKind = Json::parse(
            R"({"apps":[{"id":"a","title":"Alpha","kind":"video"},{"id":"b","title":"Beta","kind":"tools"},{"id":"c","title":"Gamma","kind":"video"}],"devices":[],"channels":[],"order":{"app":["a","b","c"],"dev":[],"ch":[]},"kinds":[{"key":"tools","label":"Tools"},{"key":"video","label":"Movies & TV"}],"layout":{"rows":["app"],"hidden":{"app":[]},"sort":{"app":"kind"},"showLgHome":false},"sources":{"channels":0}})");
        auto kinds = homeRows("apps", byKind, Json(), Json());
        check(kinds.size() == 2 && kinds[0].group == "tools" && kinds[0].title == "Tools" &&
                  kinds[0].cards.size() == 1 && kinds[1].cards.size() == 2 &&
                  kinds[1].cards[1].id == "c",
              "Apps sorted by kind come as one row per kind, in the owner's order");
        auto flat = homeRows("home", byKind, Json(), Json());
        check(flat.size() == 1 && flat[0].cards.size() == 3 && flat[0].cards[0].id == "b",
              "Home's apps row follows the kinds' order");
        // The Apps page and Home's row have their own hidden apps (`hidden.appsTab` and `hidden.app`):
        // a kind whose apps are all hidden on the page loses its row there, and nothing carries over.
        Json hiddenByKind = Json::parse(
            R"({"apps":[{"id":"a","title":"Alpha","kind":"video"},{"id":"b","title":"Beta","kind":"tools"},{"id":"c","title":"Gamma","kind":"video"}],"devices":[],"channels":[],"order":{"app":["a","b","c"],"dev":[],"ch":[]},"kinds":[{"key":"tools","label":"Tools"},{"key":"video","label":"Movies & TV"}],"layout":{"rows":["app"],"hidden":{"app":[],"appsTab":["b"]},"sort":{"app":"kind"},"showLgHome":false},"sources":{"channels":0}})");
        auto kindRows = homeRows("apps", hiddenByKind, Json(), Json());
        check(kindRows.size() == 1 && kindRows[0].group == "video" && kindRows[0].cards.size() == 2,
              "A kind whose apps are all hidden on the Apps page has no row there");
        auto homeApps = homeRows("home", hiddenByKind, Json(), Json());
        check(homeApps.size() == 1 && homeApps[0].cards.size() == 3,
              "An app hidden on the Apps page is still on Home's row");
        Json hiddenGrid = hiddenByKind;
        hiddenGrid.set("layout", Json::parse(
            R"({"rows":["app"],"hidden":{"app":["c"],"appsTab":["a"]},"sort":{"app":"custom"},"showLgHome":false})"));
        auto gridRows = homeRows("apps", hiddenGrid, Json(), Json());
        check(gridRows.size() == 1 && gridRows[0].cards.size() == 2 &&
                  gridRows[0].cards[0].id == "b" && gridRows[0].cards[1].id == "c",
              "The Apps grid leaves out what the Apps page hides, and keeps what only Home hides");
        homeApps = homeRows("home", hiddenGrid, Json(), Json());
        check(homeApps[0].cards.size() == 2 && homeApps[0].cards[0].id == "a" &&
                  homeApps[0].cards[1].id == "b",
              "Home's row leaves out what Home hides, and keeps what only the Apps page hides");
        CardRow pageShown = appsRow(hiddenGrid, Json(), false, "appsTab"),
                pageListed = appsRow(hiddenGrid, Json(), true, "appsTab"),
                homeListed = appsRow(hiddenGrid, Json(), true);
        check(pageShown.cards.size() == 2 && pageListed.cards.size() == 3 &&
                  pageListed.cards[2].id == "a" && pageListed.cards[2].hidden &&
                  !pageListed.cards[0].hidden && homeListed.cards.size() == 3 &&
                  homeListed.cards[2].id == "c" && homeListed.cards[2].hidden,
              "Editing lists the apps hidden from that list last, flagged; otherwise they are left out");
        // A catalogue from before the Apps page had a list hides there what Home hides.
        Json oldCatalogue = hiddenByKind;
        oldCatalogue.set("layout", Json::parse(
            R"({"rows":["app"],"hidden":{"app":["b"]},"sort":{"app":"custom"},"showLgHome":false})"));
        check(homeRows("apps", oldCatalogue, Json(), Json())[0].cards.size() == 2,
              "Without an Apps page list the page hides what Home hides");
        Json movedOrder = Json::parse(R"(["a","h","b","c"])");
        std::vector<std::string> moved = orderAfterMove(movedOrder, {"c", "b", "a"});
        check(moved == std::vector<std::string>({"c", "h", "b", "a"}),
              "A moved order keeps the hidden app where it was");
        moved = orderAfterMove(movedOrder, {"a", "b", "c", "n"});
        check(moved.size() == 5 && moved[4] == "n", "An app the order did not list follows it");
        Json feed = Json::parse(
            R"({"rows":[{"key":"toprated","items":[{"id":42,"type":"tv","title":"A title","year":"2026","rating":8.5,"backdrop":"/abc.jpg"}]}]})");
        rows = homeRows("home", catalogue, feed, Json());
        check(rows.size() == 2 && rows[1].key == "toprated",
              "Derived feed rows are consumed without a new data source");
        check(rows[1].cards[0].image == "tmdb:w500:/abc.jpg",
              "Artwork remains proxied through the server");
        check(rows[1].cards[0].subtitle == "Series  ·  2026  ·  ★ 8.5",
              "A title's facts lead with its kind and star the rating");
        Json topPicks = Json::parse(
            R"({"rows":[{"key":"trending","items":[{"id":1,"type":"movie","title":"One","poster":"/p.jpg"}]}]})");
        rows = homeRows("home", catalogue, topPicks, Json());
        check(rows.size() == 2 && rows[0].key == "app" && rows[1].key == "trending" &&
                  rows[1].title == "Top picks for you",
              "The apps lead Home and the top picks follow them");
        check(rows[1].cards[0].image == "tmdb:w342:/p.jpg",
              "A title with no backdrop shows its poster on the landscape card");
        Json oled = Json::parse(
            R"({"ok":true,"isOled":true,"writable":true,"serviceControls":true,"gsr":true,"panelHours":123})");
        auto menu = dashboardRows("oled", oled, Json());
        check(find(menu, "Global sticky reduction").command.confirm,
              "Disabling OLED protection requires confirmation");
        check(
            find(menu, "Global sticky reduction").command.body["value"]["enabled"].boolean(true) ==
                false,
            "Protection commands retain typed booleans");
        Json apps = Json::parse(
            R"({"ok":true,"writable":true,"installed":[{"id":"org.test","title":"Test"}],"tileHidingEnabled":false,"tileHidingHeld":true,"systemTiles":[{"id":"stock"}]})");
        menu = dashboardRows("apps", apps, Json());
        check(find(menu, "Test").command.confirm, "Uninstall requires confirmation");
        check(find(menu, "System App Hiding").command.empty(),
              "Firmware-held controls remain unavailable");
        Json caps = fields({{"allowPower", Json::flag(false)}});
        menu = dashboardRows("options", Json::parse("{\"ok\":true}"), caps);
        bool power = false;
        for (const auto &item : menu)
            if (item.label == "Power off")
                power = true;
        check(!power, "Power actions honor server capabilities");
        caps.set("allowPower", Json::flag(true));
        menu = dashboardRows("options", Json::parse("{\"ok\":true}"), caps);
        check(find(menu, "Power off").command.confirm, "Power actions retain confirmation");
        for (const auto &tab : dashboardTabs())
            check(!dashboardRows(tab.key, Json::parse("{\"ok\":true}"), caps).empty(),
                  "Every dashboard tab has a defined empty state");
        auto setup = setupRows("network", Json());
        check(setup[0].command.body["open"].boolean(),
              "Network setup sends the server's open field");
        setup = setupRows("standby", Json());
        check(setup[0].command.body["on"].boolean(), "Standby setup sends the server's on field");
        setup = setupRows("summary", Json());
        check(setup.back().command.body["action"].string() == "done",
              "Setup completion uses the existing action");
        std::printf("%d native model checks passed\n", checks);
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
