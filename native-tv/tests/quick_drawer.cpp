// The Quick drawer through the Application: what it shows while it loads and from last time, and
// that every change shows at once and goes to the TV through the optimistic-update layer, with
// replies that come late, out of order or not at all.
#include "overlay_harness.hpp"

using namespace overlay_test;

namespace {
std::string statsWith(int volume, int backlight = 70, const char *sleep = "off", const char *mode = "cinema") {
    std::string label = std::string(mode) == "game" ? "Game" : std::string(mode) == "vivid" ? "Vivid" : "Cinema";
    return std::string(R"({"ok":true,"volume":)") + std::to_string(volume) +
           R"(,"muted":false,"oled":{"care":true},"device":{"model":"OLED65B3PUA.DUSQLJR"},"sleepTimer":")" + sleep +
           R"(","picture":{"mode":")" + label + R"(","mode_raw":")" + mode + R"(","backlight":)" +
           std::to_string(backlight) +
           R"(,"modes":[{"value":"vivid","label":"Vivid"},{"value":"cinema","label":"Cinema"},{"value":"game","label":"Game"}]},"sound":{"output":"TV Speaker","output_raw":"tv_speaker","outputs":[{"id":"tv_speaker","name":"TV Speaker"},{"id":"arc","name":"HDMI ARC"}]}})";
}
struct Quick : World {
    explicit Quick(const std::string &stats = statsWith(24)) {
        check(app.activate("tool:1"), "The gear opens the drawer");
        reply("quick:stats", stats);
        reply("quick:now", R"({"ok":true,"kind":"input","port":1})");
        settle(900);
    }
    double volume() {
        return snapshot()["volume"].number();
    }
    double light() {
        return snapshot()["light"].number();
    }
    void focusRow(size_t row) {
        for (size_t i = 0; i < row; ++i)
            press(Key::Down);
    }
};
// Counts the dots of a spinner on the screen: the small round nodes of one size.
size_t spinnerDots(const Scene &scene) {
    size_t dots = 0;
    for (const auto &node : scene.nodes)
        if (node.kind == Node::Kind::Rectangle && node.rect.w == node.rect.h && std::fabs(node.rect.w - 6.72f) < .05f &&
            node.radius == node.rect.w / 2)
            ++dots;
    return dots;
}
} // namespace

// ---- Loading ----------------------------------------------------------------------------------

void loadingChecks() {
    World world;
    check(world.app.activate("tool:1"), "The gear opens the drawer");
    world.settle(400);
    check(world.screen() == "quick" && world.snapshot()["loading"].boolean(), "Before the first read the drawer is loading");
    check(!world.app.shows("Cinema") && !world.app.shows("—"),
          "No empty values and no dashes: the rows wait for the read");
    check(spinnerDots(world.app.scene()) >= 12 * 6, "Each row that reads the TV shows the circular spinner");
    // A key on a row that is not read yet does nothing.
    world.press(Key::Right);
    world.press(Key::Ok);
    check(world.screen() == "quick" && world.on("w:quick.volume").empty(), "Keys on a row still loading do nothing");
    world.reply("quick:stats", statsWith(24));
    world.settle(200);
    check(!world.snapshot()["loading"].boolean() && world.app.shows("Cinema") && world.app.shows("24"),
          "The values replace the spinners");
    check(spinnerDots(world.app.scene()) == 0, "and no spinner is left");
    // The next opening shows what was read last time at once.
    world.press(Key::Back);
    world.settle(900);
    check(world.screen() == "home", "The drawer closes");
    size_t reads = world.transport.count("quick:stats");
    check(world.app.activate("tool:1"), "and opens again");
    check(!world.snapshot()["loading"].boolean() && world.snapshot()["volume"].number() == 24,
          "The second opening has its values before any reply");
    world.settle(300);
    check(world.app.shows("Cinema") && spinnerDots(world.app.scene()) == 0, "and draws them");
    check(world.transport.count("quick:stats") == reads + 1, "while it reads again behind them");
    world.reply("quick:stats", statsWith(31));
    world.settle(100);
    check(world.snapshot()["volume"].number() == 31, "Today's value replaces last time's");
    // A read that fails with nothing to show: dashes, a message and another try.
    World failed;
    failed.app.activate("tool:1");
    failed.reply("quick:stats", R"({"ok":false})", 503);
    failed.settle(300);
    check(failed.snapshot()["readFailed"].boolean() && !failed.snapshot()["loading"].boolean() &&
              failed.app.shows("—") && failed.app.shows("The TV did not answer"),
          "A read that fails shows dashes and says so");
    size_t tries = failed.transport.count("quick:stats");
    failed.settle(3200);
    check(failed.transport.count("quick:stats") == tries + 1, "and it asks again");
    failed.reply("quick:stats", statsWith(24));
    failed.settle(200);
    check(!failed.snapshot()["readFailed"].boolean() && failed.app.shows("Cinema"), "An answer ends it");
}

void openChecks() {
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
    world.reply("quick:stats", statsWith(24));
    world.reply("quick:now", R"({"ok":true,"kind":"input","port":1})");
    world.settle(300);
    check(world.snapshot()["volume"].number() == 24 && world.snapshot()["light"].number() == 70 &&
              world.snapshot()["rows"].number() == 11 && world.snapshot()["focus"].string() == "Volume",
          "The drawer shows the volume and light the TV reported");
    check(world.app.shows("Picture mode") && world.app.shows("Cinema") && world.app.shows("OLED light") &&
              world.app.shows("Console"),
          "The rows carry what is set now");
    world.reply("quick:stats", R"({"ok":true,"volume":99})", 200, 0);
    check(world.snapshot()["volume"].number() == 24, "A reply to an older read is dropped");
}

// ---- Volume and light: shown at once, written at a pace, never moved backwards ----------------------

void shownAtOnceChecks() {
    Quick world;
    world.press(Key::Right);
    check(world.volume() == 25 && world.app.shows("25"), "The first press shows the new volume in the very next frame");
    check(world.on("w:quick.volume").size() == 1 && world.on("w:quick.volume")[0].operation == Operation::Control &&
              world.on("w:quick.volume")[0].body["action"].string() == "volume" &&
              world.on("w:quick.volume")[0].body["value"].number() == 25,
          "and the write is an absolute target, sent at once");
    for (int i = 0; i < 4; ++i)
        world.press(Key::Right);
    check(world.volume() == 29 && world.app.shows("29"), "Every repeat shows at once, with no reply at all");
    check(world.on("w:quick.volume").size() == 1, "while the writes wait for their interval");
    world.settle(200);
    check(world.on("w:quick.volume").size() == 2 && world.on("w:quick.volume")[1].body["value"].number() == 29,
          "One write with the newest value follows");
    world.settle(3000);
    check(world.volume() == 29, "With no reply for seconds the value stays");
    check(world.motion("quick:x") < .5, "(the drawer is open)");
    // Ends of the range.
    Quick ends(statsWith(1));
    for (int i = 0; i < 5; ++i)
        ends.press(Key::Left);
    check(ends.volume() == 0 && ends.on("w:quick.volume").size() >= 1, "The volume stops at zero");
    size_t sent = ends.on("w:quick.volume").size();
    ends.press(Key::Left);
    ends.settle(300);
    check(ends.on("w:quick.volume").size() == sent, "A key at the end of the range sends nothing more");
}

// A key held down from one end to the other, replies late and out of order and stale reads between.
void heldChecks(bool light, bool up) {
    const int from = up ? 5 : 95, to = up ? 95 : 5, direction = up ? 1 : -1, stepSize = light ? 5 : 1;
    const char *channel = light ? "w:quick.light" : "w:quick.volume";
    Quick world(light ? statsWith(24, from) : statsWith(from));
    if (light)
        world.focusRow(2);
    check(world.snapshot()["focus"].string() == (light ? "OLED light" : "Volume"), "The row is focused");
    auto shown = [&] { return light ? world.light() : world.volume(); };
    double previous = from;
    std::deque<FakeTransport::Request> unanswered;
    size_t seen = 0;
    int pressed = 0, expected = from;
    while (expected != to) {
        world.press(up ? Key::Right : Key::Left);
        expected += direction * stepSize;
        ++pressed;
        world.settle(light ? 60 : 30); // a repeat every 30 ms (60 for the bigger steps)
        for (auto writes = world.on(channel); seen < writes.size(); ++seen)
            unanswered.push_back(writes[seen]);
        if (pressed % 7 == 0) {
            // The replies that piled up come back newest first, and a read of an intermediate value
            // reaches the page.
            while (!unanswered.empty()) {
                world.answer(unanswered.back());
                unanswered.pop_back();
            }
            int stale = from + direction * stepSize * (pressed / 2);
            world.reply("quick:stats", light ? statsWith(24, stale) : statsWith(stale));
        }
        double now = shown();
        check(up ? now >= previous : now <= previous, "The shown value never goes backwards while the key is held");
        check(now == expected, "and it is the value the key reached");
        previous = now;
    }
    world.settle(260);
    std::vector<FakeTransport::Request> writes = world.on(channel);
    check(writes.back().body["value"].number() == to, "The last write carries the final value");
    check(writes.size() <= static_cast<size_t>(pressed * (light ? 60 : 30) / 120) + 3,
          "Writes are limited to one every 120 ms");
    for (size_t i = 0; i + 1 < writes.size(); ++i)
        world.answer(writes[i]); // the late ones, oldest first: they change nothing
    check(shown() == to, "Late replies to older writes change nothing");
    world.answer(writes.back());
    check(shown() == to, "The newest reply leaves the value where it is");
    check(world.snapshot()[light ? "lightBackwards" : "volumeBackwards"].number() == 0,
          "The drawer never moved it against the key");
    size_t reads = world.transport.count("quick:stats");
    world.settle(900);
    check(world.transport.count("quick:stats") == reads + 1, "One re-read follows the last answer");
    world.reply("quick:stats", light ? statsWith(24, to) : statsWith(to));
    world.settle(200);
    check(shown() == to, "and what it says is what shows");
    // The TV capped it: its answer wins.
    Quick capped(light ? statsWith(24, 50) : statsWith(50));
    if (light)
        capped.focusRow(2);
    capped.press(Key::Right);
    capped.settle(10);
    capped.answer(capped.on(channel).back());
    capped.settle(900);
    capped.reply("quick:stats", light ? statsWith(24, 50) : statsWith(50));
    capped.settle(100);
    check((light ? capped.light() : capped.volume()) == 50, "A TV that capped the value is believed after the re-read");
}

void failureChecks() {
    Quick world;
    world.press(Key::Right);
    check(world.volume() == 25, "The key shows at once");
    world.answer(world.on("w:quick.volume")[0], false, "Volume locked");
    world.settle(40);
    check(world.volume() == 24, "A refusal puts the volume back");
    check(world.toasted("That did not work: Volume locked"), "and says why");
    // An older refusal while a newer target waits is not shown.
    Quick busy;
    busy.press(Key::Right);
    busy.press(Key::Right);
    busy.answer(busy.on("w:quick.volume")[0], false, "Busy");
    busy.settle(40);
    check(busy.volume() == 26 && !busy.app.shows("Busy"), "An older write's refusal does not move the value");
    // Silence.
    Quick silent;
    silent.press(Key::Right);
    silent.settle(8300);
    check(silent.volume() == 24 && silent.toasted("did not answer"), "Eight seconds of silence give the value back");
    // Mute is shown at once and given back on a refusal.
    Quick mute;
    mute.press(Key::Ok);
    check(mute.snapshot()["muted"].boolean() && mute.app.shows("Muted"), "OK on the volume mutes at once");
    check(mute.on("w:quick.mute").size() == 1 && mute.on("w:quick.mute")[0].body["action"].string() == "mute" &&
              mute.on("w:quick.mute")[0].body["value"].boolean(),
          "and posts the mute");
    mute.answer(mute.on("w:quick.mute")[0], false, "Nope");
    mute.settle(40);
    check(!mute.snapshot()["muted"].boolean(), "A refusal takes it back");
}

// ---- Choices and switches -----------------------------------------------------------------------

void choiceChecks() {
    Quick world;
    world.press(Key::Down);
    world.press(Key::Ok);
    check(world.screen() == "quick:picture" && world.snapshot()["focus"].string() == "Cinema",
          "Picture mode opens its list on the mode in force");
    world.press(Key::Down);
    world.press(Key::Ok);
    check(world.screen() == "quick" && world.snapshot()["picture"].string() == "game" && world.snapshot()["focus"].string() == "Picture mode",
          "Choosing a mode shows it at once and returns to the main list");
    world.settle(300);
    check(world.app.shows("Game"), "with the new mode on its row");
    check(world.on("w:quick.picture").size() == 1 && world.on("w:quick.picture")[0].body["action"].string() == "pictureMode" &&
              world.on("w:quick.picture")[0].body["value"].string() == "game",
          "and the write posts the mode");
    world.answer(world.on("w:quick.picture")[0], false, "Not now");
    world.settle(200);
    check(world.snapshot()["picture"].string() == "cinema" && world.toasted("That did not work: Not now"),
          "A refusal gives the mode back and says why");
    // Two choices in a row: the newest is the one sent and the older reply changes nothing.
    Quick quick;
    quick.press(Key::Down);
    quick.press(Key::Ok);
    quick.press(Key::Down);
    quick.press(Key::Ok); // game
    quick.press(Key::Ok); // the list again, on game
    quick.press({Key::Up, Key::Up});
    quick.press(Key::Ok); // vivid
    quick.settle(40);
    check(quick.snapshot()["picture"].string() == "vivid", "The newest choice shows");
    quick.settle(200);
    auto picture = quick.on("w:quick.picture");
    check(picture.size() == 2 && picture.front().body["value"].string() == "game" &&
              picture.back().body["value"].string() == "vivid",
          "and is the last one sent");
    quick.answer(picture.front());
    quick.settle(40);
    check(quick.snapshot()["picture"].string() == "vivid", "A reply to an older choice changes nothing");
    // Sleep timer and sound output.
    Quick sleep;
    sleep.focusRow(5);
    sleep.press(Key::Ok);
    check(sleep.screen() == "quick:sleep" && sleep.snapshot()["focus"].string() == "Off", "Sleep timer opens on Off");
    sleep.press({Key::Down, Key::Down});
    sleep.press(Key::Ok);
    check(sleep.snapshot()["sleep"].string() == "30" && sleep.screen() == "quick", "A sleep time shows at once");
    sleep.settle(300);
    check(sleep.app.shows("30 min"), "on its row");
    check(sleep.on("w:quick.sleep")[0].body["action"].string() == "sleepTimer" &&
              sleep.on("w:quick.sleep")[0].body["value"].string() == "30",
          "and posts its minutes");
    sleep.answer(sleep.on("w:quick.sleep")[0], false, "Locked");
    sleep.settle(40);
    check(sleep.snapshot()["sleep"].string() == "off", "A refusal gives it back");
    Quick sound;
    sound.focusRow(3);
    sound.press(Key::Ok);
    sound.press(Key::Down);
    sound.press(Key::Ok);
    sound.settle(300);
    check(sound.app.shows("HDMI ARC") && sound.on("w:quick.sound")[0].body["action"].string() == "soundOutput" &&
              sound.on("w:quick.sound")[0].body["value"].string() == "arc",
          "A sound output shows at once and is posted");
    // Back and Left go up a level.
    Quick levels;
    levels.press(Key::Down);
    levels.press(Key::Ok);
    levels.press(Key::Back);
    check(levels.screen() == "quick" && levels.snapshot()["focus"].string() == "Picture mode", "Back returns to the main list");
    levels.press(Key::Ok);
    levels.press(Key::Left);
    check(levels.screen() == "quick", "so does Left");
}

void switchChecks() {
    Quick world;
    world.focusRow(10);
    world.settle(400);
    check(world.snapshot()["focus"].string() == "Home settings" && world.motion("quick:sy") < -100,
          "The list scrolls to keep the focused row in view");
    world.press(Key::Ok);
    world.settle(400);
    check(world.screen() == "quick:layout" && world.app.shows("24-hour clock") && world.app.shows("App names"),
          "Home settings lists the switches");
    world.press(Key::Down);
    world.press(Key::Ok);
    world.settle(300);
    check(world.motion("quick:t:1") < .5, "A switch moves the moment OK is pressed");
    check(world.on("w:layout.appNames").size() == 1 && world.on("w:layout.appNames")[0].operation == Operation::SetHomeLayout &&
              world.on("w:layout.appNames")[0].body["appNames"].isBoolean() &&
              !world.on("w:layout.appNames")[0].body["appNames"].boolean(),
          "and posts its new value to the layout route");
    // A read of the old layout while the write is on its way does not switch it back.
    world.reply("catalogue", catalogueJson);
    world.settle(300);
    check(world.motion("quick:t:1") < .5, "A catalogue read meanwhile does not move it back");
    // Pressed twice in a row: the last value is the one sent, the older reply is ignored.
    world.press(Key::Ok);
    world.press(Key::Ok);
    world.settle(200);
    auto writes = world.on("w:layout.appNames");
    check(writes.back().body["appNames"].boolean() == false, "The last press decides what is sent");
    world.answer(writes.front());
    check(world.motion("quick:t:1") < .5, "A reply to an older press changes nothing");
    size_t catalogues = world.transport.count("catalogue");
    world.answer(writes.back());
    world.settle(900);
    check(world.transport.count("catalogue") > catalogues, "The layout is read once to settle it");
    std::string changed = catalogueJson;
    changed.insert(changed.find("\"clockFormat\""), "\"appNames\":false,");
    world.reply("catalogue", changed);
    world.settle(300);
    check(world.motion("quick:t:1") < .5, "The catalogue that says it too leaves it off");
    // Refusal.
    world.press(Key::Up);
    world.press(Key::Ok);
    world.answer(world.on("w:layout.clockFormat")[0], false, "Read-only");
    world.settle(300);
    check(world.on("w:layout.clockFormat")[0].body["clockFormat"].string() == "24h" && world.motion("quick:t:0") < .5 &&
              world.toasted("That did not work: Read-only"),
          "A refused switch moves back and says why");
}

// ---- Actions ----------------------------------------------------------------------------------------

void actionChecks() {
    Quick world;
    world.focusRow(6);
    check(world.snapshot()["focus"].string() == "Screensaver", "Screensaver is a row");
    world.press(Key::Ok);
    check(world.snapshot()["closing"].boolean(), "OK starts it and the drawer closes at once");
    check(world.on("w:quick.act")[0].body["action"].string() == "screensaver", "The write starts the screensaver");
    world.settle(900);
    check(world.screen() == "home", "Home is back");
    world.answer(world.on("w:quick.act")[0], false, "Busy");
    check(world.toasted("That did not work: Busy"), "A refusal after the drawer has gone is still said");
    Quick off;
    off.focusRow(7);
    off.press(Key::Ok);
    check(off.on("w:quick.act")[0].body["action"].string() == "screenOff", "Screen off posts its action");
    Quick input;
    input.focusRow(4);
    input.press(Key::Ok);
    check(input.screen() == "quick:input" && input.snapshot()["focus"].string() == "Console",
          "Input lists the devices, on the one in use");
    input.settle(400);
    input.press(Key::Down);
    check(input.snapshot()["focus"].string() == "HDMI 2" && input.app.shows("Nothing connected"),
          "A port with nothing on it is named by its port");
    input.press(Key::Up);
    input.press(Key::Ok);
    check(input.on("w:quick.act")[0].operation == Operation::OpenHomeItem &&
              input.on("w:quick.act")[0].body["kind"].string() == "dev" &&
              input.on("w:quick.act")[0].body["id"].string() == "hdmi1" && input.snapshot()["closing"].boolean(),
          "Choosing an input opens it and closes the drawer");
}

void navigationChecks() {
    Quick recents;
    recents.focusRow(8);
    check(recents.snapshot()["focus"].string() == "Recent apps", "Recent apps is a row");
    recents.press(Key::Ok);
    recents.settle(900);
    check(recents.screen() == "quick:recents", "The row opens the recent apps");
    Quick profile;
    profile.focusRow(9);
    check(profile.snapshot()["focus"].string() == "Profile" && profile.app.shows("B3"),
          "The profile row names the profile after the TV");
    profile.press(Key::Ok);
    profile.settle(900);
    check(profile.screen() == "profiles", "The row opens the profiles");
    profile.press(Key::Back);
    check(profile.screen() == "home", "Back from there goes Home, not into a second drawer");
    // Close: the drawer slides out, Home is as it was, and the gear has the focus.
    World world;
    world.press({Key::Right, Key::Right});
    world.settle(300);
    world.press({Key::Up, Key::Up}); // past the apps row to the bar
    world.press({Key::Right, Key::Right, Key::Right, Key::Right}); // Search is left of the gear
    world.press(Key::Ok);
    world.reply("quick:stats", statsWith(24));
    world.settle(600);
    check(world.screen() == "quick", "The gear opens the drawer from the bar");
    world.press(Key::Back);
    check(world.screen() == "quick" && world.snapshot()["closing"].boolean(), "Back starts the slide out");
    world.press(Key::Back);
    world.settle(900);
    check(world.screen() == "home" && world.snapshot()["navigation"].boolean() &&
              world.snapshot()["nav"].number() == 4 && world.snapshot()["row"].number() == 0,
          "A second Back while it slides changes nothing: Home as it was, with the gear focused");
    world.press(Key::Down);
    check(!world.snapshot()["navigation"].boolean(), "Home answers its keys again");
    // Back while the first read is still out.
    World early;
    early.app.activate("tool:1");
    early.press(Key::Back);
    early.settle(900);
    check(early.screen() == "home", "Back before the first read closes the drawer");
    // A route away (the Home key) leaves at once.
    Quick away;
    away.app.open("home");
    check(away.screen() == "home", "A route to Home leaves the drawer");
    Quick click;
    click.app.click(300, 500);
    click.settle(900);
    check(click.screen() == "home", "A click beside the drawer closes it");
    // A change pressed just before the drawer closes still goes: the layer, not the page, owns it.
    Quick closing;
    closing.press(Key::Right);
    closing.press(Key::Right);
    closing.press(Key::Back);
    closing.settle(900);
    check(closing.screen() == "home" && closing.on("w:quick.volume").back().body["value"].number() == 26,
          "The newest volume is sent although the drawer has gone");
}

int main() {
    try {
        loadingChecks();
        openChecks();
        shownAtOnceChecks();
        heldChecks(false, true);
        heldChecks(false, false);
        heldChecks(true, true);
        heldChecks(true, false);
        failureChecks();
        choiceChecks();
        switchChecks();
        actionChecks();
        navigationChecks();
        std::printf("%d native quick-drawer checks passed\n", checkCount());
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
