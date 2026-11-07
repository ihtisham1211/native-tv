// The TV theme through the Application as the remote drives it: the picker, the Keep sheet that
// follows a change (kept, refused with Back, or timed out), a save the server refuses, the client
// following the theme the server names, and the one-time offer of OLED Care.
#include "core/themes.hpp"
#include "overlay_harness.hpp"

using namespace overlay_test;

namespace {
std::string themeNow() {
    return kit::themeId();
}
// The catalogue the harness uses, with the server's theme (and, if asked, the offer) added.
std::string catalogueWith(const std::string &theme, bool offer = false) {
    std::string json = catalogueJson;
    json.pop_back();
    json += ",\"tvTheme\":\"" + theme + "\"" + (offer ? ",\"suggestTvTheme\":true" : "") + "}";
    return json;
}
// The newest request that saves a theme: the value it carries, or "" when none was sent.
std::string lastSavedTheme(const World &world) {
    for (size_t i = world.transport.requests.size(); i-- > 0;) {
        const auto &request = world.transport.requests[i];
        if (request.write && request.operation == Operation::Control &&
            request.body["action"].string() == "setTvTheme")
            return request.body["value"].string();
    }
    return "";
}
size_t writesOf(const World &world, const std::string &action) {
    size_t count = 0;
    for (const auto &request : world.transport.requests)
        if (request.write && request.operation == Operation::Control &&
            request.body["action"].string() == action)
            ++count;
    return count;
}
// Answers the newest write; a theme change is followed by the reads the new theme needs, so it is
// not always the last request.
void answerLastWrite(World &world, long status, const char *body) {
    for (size_t i = world.transport.requests.size(); i-- > 0;) {
        const auto request = world.transport.requests[i];
        if (!request.write)
            continue;
        world.reply(request.channel, body, status, static_cast<int>(request.generation));
        return;
    }
    check(false, "There was a write to answer");
}

void pickerChecks() {
    kit::useTheme("n-google-tv");
    World world;
    world.app.open("tvtheme");
    world.settle(600);
    check(world.screen() == "tvtheme" && world.app.shows("TV theme") && world.app.shows("OLED Care"),
          "The picker opens and names both themes");
    check(world.app.shows("In use") && themeNow() == "n-google-tv", "The theme in use is marked");

    // OK on the other theme: it shows at once, is saved, and a sheet offers to keep it.
    world.press(Key::Right, 100);
    world.press(Key::Ok, 400);
    check(themeNow() == "n-oled-care" && kit::look::ringFocus, "OK shows the theme at once");
    check(lastSavedTheme(world) == "n-oled-care", "OK saves the theme through /api/control");
    check(world.app.shows("Keep OLED Care?") && world.app.shows("Go back"), "The Keep sheet follows");
    answerLastWrite(world, 200, R"({"ok":true,"tvTheme":"n-oled-care"})");
    world.settle(300);
    check(themeNow() == "n-oled-care", "An accepted save leaves the theme on");

    // Back refuses it: the old theme returns and is saved again; the picker stays.
    world.press(Key::Back, 400);
    check(themeNow() == "n-google-tv" && lastSavedTheme(world) == "n-google-tv",
          "Back on the sheet puts the old theme back and saves it");
    check(world.screen() == "tvtheme" && !world.app.shows("Keep OLED Care?"), "The picker stays open");
    answerLastWrite(world, 200, R"({"ok":true,"tvTheme":"n-google-tv"})");

    // Keep: the sheet closes and nothing reverts later.
    world.press(Key::Ok, 400);
    check(themeNow() == "n-oled-care", "OK again chooses OLED Care");
    answerLastWrite(world, 200, R"({"ok":true,"tvTheme":"n-oled-care"})");
    world.press(Key::Ok, 300);
    check(!world.app.shows("Keep OLED Care?"), "Keep closes the sheet");
    world.settle(11000);
    check(themeNow() == "n-oled-care", "A kept theme stays");

    // No answer to the sheet: after ten seconds the TV goes back by itself.
    world.press(Key::Left, 100);
    world.press(Key::Ok, 400);
    check(themeNow() == "n-google-tv" && world.app.shows("Keep Google TV?"), "Choosing Google TV asks to keep it");
    answerLastWrite(world, 200, R"({"ok":true,"tvTheme":"n-google-tv"})");
    world.settle(9000);
    check(themeNow() == "n-google-tv", "The sheet waits");
    world.settle(1800);
    check(themeNow() == "n-oled-care" && lastSavedTheme(world) == "n-oled-care",
          "Ten seconds without a key puts the previous theme back and saves it");
}

void refusedSaveChecks() {
    kit::useTheme("n-google-tv");
    World world;
    world.app.open("tvtheme");
    world.settle(600);
    world.press(Key::Right, 100);
    world.press(Key::Ok, 400);
    check(themeNow() == "n-oled-care", "The theme shows before the server has answered");
    answerLastWrite(world, 500, R"({"ok":false,"error":"could not save setting"})");
    world.settle(600);
    check(themeNow() == "n-google-tv", "A refused save puts the old theme back");
}

void followServerChecks() {
    kit::useTheme("n-google-tv");
    World world;
    check(themeNow() == "n-google-tv", "The harness starts on the default theme");
    world.reply("catalogue", catalogueWith("n-oled-care"));
    check(themeNow() == "n-oled-care", "A catalogue that names another theme switches to it");
    world.reply("catalogue", catalogueWith("n-gone"));
    check(themeNow() == "n-oled-care", "A theme the client does not have changes nothing");
    world.reply("catalogue", catalogueWith("n-google-tv"));
    check(themeNow() == "n-google-tv", "A change from the phone reaches an open client");
    world.reply("caps", R"({"ok":true,"tvTheme":"n-oled-care"})");
    check(themeNow() == "n-oled-care", "Caps carry the theme for the first frame");
    kit::useTheme("n-google-tv");
}

// The panel's figures as /api/stats carries them.
const char *const statsWithPanel =
    R"({"ok":true,"oled":{"care":true,"panel_hours":4210,"hours_since_comp":2.7,"comp_interval_hours":4,"hours_until_refresher":1613,"refresher_interval_hours":2000,"hours_since_refresher":387,"refresher_cycles":12}})";

void compactHomeChecks() {
    kit::useTheme("n-google-tv");
    World world;
    world.settle(600);
    check(world.app.shows("Coast"), "Google TV's Home shows the feed's top pick");
    check(hasHit(world.app.scene(), "tool:2"), "Google TV's bar has the search button");
    const size_t discoverReads = world.transport.count("discover");

    // The server names OLED Care: Home becomes the apps, the inputs and the panel, and stops asking for a feed.
    world.reply("catalogue", catalogueWith("n-oled-care"));
    world.settle(300);
    check(themeNow() == "n-oled-care", "the client follows the server to OLED Care");
    check(world.transport.count("stats") >= 1, "a Home with no feed asks for the panel's figures");
    world.reply("stats", statsWithPanel);
    world.settle(900);
    check(world.app.shows("Panel hours") && world.app.shows("4,210") && world.app.shows("Compensation cycle 68%"),
          "the panel's hours and compensation cycle are a card");
    check(world.app.shows("Pixel Refresher") && world.app.shows("1,613") && world.app.shows("12 cycles done"),
          "the Pixel Refresher is a card");
    check(world.app.shows("HDMI 1") && world.app.shows("Console"), "the inputs are a row of Home");
    check(!world.app.shows("Coast") && !world.app.shows("Top picks for you"), "there is no hero text and no picks");
    check(!hasHit(world.app.scene(), "tool:2"),
          "there is no search button, though the client read the feed under Google TV");
    world.app.open("search");
    world.settle(300);
    check(world.screen() != "search", "title search does not open in OLED Care");
    world.settle(10500);
    check(world.transport.count("discover") == discoverReads, "the feed is not asked for again");
    check(world.transport.count("stats") >= 2, "the panel's figures are read again with Home's refresh");

    // OK on a panel card opens OLED Care in Settings.
    world.press({Key::Down, Key::Down});
    world.press(Key::Ok, 400);
    check(world.screen() == "dashboard:oled", "OK on a panel card opens OLED Care in Settings");

    // Without panel figures (an LCD TV that chose the theme) the row is simply left out.
    World lcd;
    lcd.reply("catalogue", catalogueWith("n-oled-care"));
    lcd.settle(600);
    lcd.reply("stats", R"({"ok":true,"oled":{"care":false}})");
    lcd.settle(600);
    check(!lcd.app.shows("Panel hours") && lcd.app.shows("HDMI 1"), "no panel figures, no panel card");

    // Back to Google TV: the feed returns and is asked for.
    World back;
    back.reply("catalogue", catalogueWith("n-oled-care"));
    back.settle(600);
    back.reply("catalogue", catalogueWith("n-google-tv"));
    back.settle(600);
    check(themeNow() == "n-google-tv" && back.transport.count("discover") >= 1, "the feed is asked for again");
    back.reply("discover", feedJson);
    back.settle(900);
    check(back.app.shows("Coast"), "Google TV's Home shows the feed again");
    kit::useTheme("n-google-tv");
}

// A client that opened in Google TV and is told OLED Care before its first frame (a server that
// answers quickly) must still ask for the panel's figures, not wait for the next refresh.
void lateThemeChecks() {
    kit::useTheme("n-google-tv");
    FakeTransport transport;
    Application app(transport);
    Response caps;
    caps.channel = "caps";
    caps.status = 200;
    caps.bytes = R"({"ok":true,"tvTheme":"n-oled-care"})";
    caps.generation = transport.revisions["caps"];
    app.response(caps);
    check(themeNow() == "n-oled-care", "caps name OLED Care before the first frame");
    const size_t statsBefore = transport.count("stats");
    run(app, 0, 600);
    check(transport.count("stats") > statsBefore,
          "a theme that arrives before the first frame asks for the panel's figures at once");
    kit::useTheme("n-google-tv");
}

void offerChecks() {
    kit::useTheme("n-google-tv");
    {
        World world;
        world.reply("catalogue", catalogueWith("n-google-tv", true));
        world.settle(600);
        check(world.screen() == "tvtheme:suggest" && world.app.shows("Use OLED Care?"),
              "An OLED panel on the default theme is offered OLED Care on Home");
        check(world.app.shows("Use it") && world.app.shows("Not now"), "The offer has two answers");
        world.press(Key::Back, 300);
        check(world.screen() == "home" && themeNow() == "n-google-tv", "Back is no thanks");
        check(writesOf(world, "dismissTvThemeSuggestion") == 1, "No thanks is saved, so the offer stops");
        world.reply("catalogue", catalogueWith("n-google-tv", true));
        world.settle(300);
        check(world.screen() == "home", "The offer is not opened twice in a run");
    }
    {
        World world;
        world.reply("catalogue", catalogueWith("n-google-tv", true));
        world.settle(600);
        world.press(Key::Right, 100);
        world.press(Key::Left, 100);
        world.press(Key::Ok, 400);
        check(themeNow() == "n-oled-care" && lastSavedTheme(world) == "n-oled-care", "Use it applies OLED Care and saves it");
        check(world.app.shows("Keep OLED Care?"), "Use it leads to the Keep sheet");
        answerLastWrite(world, 200, R"({"ok":true,"tvTheme":"n-oled-care"})");
        world.press(Key::Back, 400);
        check(themeNow() == "n-google-tv" && world.screen() == "home", "Going back from the offer returns to Home on the old theme");
    }
    {
        World world;
        world.reply("catalogue", catalogueWith("n-google-tv", true));
        world.settle(600);
        world.press(Key::Right, 100);
        world.press(Key::Ok, 400);
        check(writesOf(world, "dismissTvThemeSuggestion") == 1 && world.screen() == "home" && themeNow() == "n-google-tv",
              "Not now leaves the theme and ends the offer");
    }
    {
        World world;
        world.reply("catalogue", catalogueWith("n-oled-care", false));
        world.settle(600);
        check(world.screen() == "home" && themeNow() == "n-oled-care", "No offer, no screen");
    }
    kit::useTheme("n-google-tv");
}
} // namespace

int main() {
    try {
        pickerChecks();
        refusedSaveChecks();
        followServerChecks();
        compactHomeChecks();
        lateThemeChecks();
        offerChecks();
        std::printf("%d native theme-screen checks passed\n", checkCount());
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
