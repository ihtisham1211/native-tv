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
void answerLastWrite(World &world, long status, const char *body) {
    const auto request = world.transport.requests.back();
    check(request.write, "The last request was a write");
    world.reply(request.channel, body, status, static_cast<int>(request.generation));
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
        offerChecks();
        std::printf("%d native theme-screen checks passed\n", checkCount());
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
