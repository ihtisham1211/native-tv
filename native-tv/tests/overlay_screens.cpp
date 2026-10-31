// Profiles, title details and the search and profile editors, driven through the Application as
// the remote drives them: keys in, requests and replies at the Services seam, the scene and the
// snapshot read back. (The Quick drawer's own tests are in quick_drawer.cpp.)
#include "overlay_harness.hpp"
#include "ui/profile_names.hpp"

using namespace overlay_test;

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
        panel.loadingMs = 300;
        kit::statusPanel({loading, glyphs}, panel);
        check(loading.nodes.size() == plain.nodes.size() + 12, "A loading panel turns the circular spinner");
    }
    {
        Scene early, later;
        kit::spinner({early, glyphs}, {100, 100, 40, 40}, 0, {1, 1, 1, 1});
        kit::spinner({later, glyphs}, {100, 100, 40, 40}, 400, {1, 1, 1, 1});
        check(early.nodes.size() == 12 && later.nodes.size() == 12,
              "The circular progress indicator is twelve dots");
        float headEarly = 0, headLater = 0;
        size_t at = 0, moved = 0;
        for (size_t i = 0; i < 12; ++i) {
            if (early.nodes[i].color.a > headEarly) {
                headEarly = early.nodes[i].color.a;
                at = i;
            }
            if (later.nodes[i].color.a > headLater) {
                headLater = later.nodes[i].color.a;
                moved = i;
            }
        }
        check(headEarly > .95f && headLater > .8f && at != moved,
              "One dot is the bright head, and it has moved on a moment later");
        Scene none;
        kit::spinner({none, glyphs}, {0, 0, 40, 40}, 0, {1, 1, 1, 1}, 0);
        check(none.nodes.empty(), "A spinner at no opacity draws nothing");
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
    check(world.snapshot()["contentState"].string() == "ready" && world.snapshot()["focus"].string() == "Trailer" &&
              world.app.shows("A journey across the water.") && world.app.shows("Cast: Ada, Ben") &&
              world.app.shows("More like this"),
          "A reply fills the page, with Trailer focused (the only deep links are Netflix and YouTube) and the similar titles below");
    check(!world.app.shows("Open"), "There is no Open button: a title opens on Netflix or as a trailer");
    world.reply("title:netflix", R"({"ok":true,"netflixId":"80057281"})");
    check(world.snapshot()["focus"].string() == "Open on Netflix",
          "A late Netflix link is the first action, and has the focus while nothing has moved it");
    world.press(Key::Ok);
    check(world.on("w:details.open").size() == 1 && world.on("w:details.open").back().body["kind"].string() == "netflix" &&
              !world.on("w:details.open").back().body.has("netflixId") &&
              world.on("w:details.open").back().body["id"].string() == "7",
          "Open on Netflix posts the title and leaves the mapping to the server");
    world.press(Key::Ok);
    check(world.on("w:details.open").size() == 1, "A second OK while it is opening is ignored");
    world.answer(world.on("w:details.open").back(), false, "Refused");
    check(world.toasted("That did not open: Refused"), "A refused open says so, with the daemon's reason");
    world.press(Key::Right);
    check(world.snapshot()["focus"].string() == "Trailer", "Trailer is reachable");
    world.press(Key::Ok);
    check(world.on("w:details.open").size() == 2 && world.on("w:details.open").back().body["kind"].string() == "youtube" &&
              world.on("w:details.open").back().body["id"].string() == "dQw4w9WgXcQ",
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











// ---- Profiles ---------------------------------------------------------------------------------

void profileNameChecks() {
    check(tvShortName("OLED65B3PUA.DUSQLJR") == "B3" && tvShortName("OLED55C4PUA") == "C4" &&
              tvShortName("OLED55CXPUA") == "CX" && tvShortName("OLED77G3PUA") == "G3",
          "An OLED model gives its series");
    check(tvShortName("55UQ7570PUB") == "LGTV" && tvShortName("") == "LGTV" && tvShortName("OLED") == "LGTV",
          "Anything else is LGTV");
    Json main = Json::parse(R"({"id":"main","name":"Main"})"), renamed = Json::parse(R"({"id":"main","name":"Dad"})"),
         other = Json::parse(R"({"id":"guest","name":"Main"})");
    check(profileName(main, "OLED65B3PUA.DUSQLJR") == "B3" && profileName(renamed, "OLED65B3PUA") == "Dad" &&
              profileName(other, "OLED65B3PUA") == "Main",
          "Only the untouched first profile takes the TV's name");
}

void profilesChecks() {
    World world;
    world.openProfiles();
    check(world.screen() == "profiles" && world.snapshot()["selected"].number() == 0 &&
              world.snapshot()["active"].string() == "main",
          "The avatar opens the profile page on the profile in use");
    check(world.app.shows("LGTV") && !world.app.shows("Main"),
          "The first profile is named after the TV (LGTV until the model is known)");
    world.reply("profiles:stats", R"({"ok":true,"device":{"model":"OLED65B3PUA.DUSQLJR"}})");
    world.settle(100);
    check(world.app.shows("B3"), "and after the model B3");

    // Switching: the chosen profile is the one in use at once, before any reply.
    world.press(Key::Right);
    world.press(Key::Ok);
    check(world.snapshot()["active"].string() == "guest" && world.snapshot()["busy"].boolean(),
          "OK on a profile makes it the one in use at once, pending");
    check(world.on("w:profiles.active").size() == 1 &&
              world.on("w:profiles.active")[0].operation == Operation::SetProfile &&
              world.on("w:profiles.active")[0].body["action"].string() == "switch" &&
              world.on("w:profiles.active")[0].body["id"].string() == "guest",
          "The write is a switch to it");
    world.settle(300);
    check(world.snapshot()["active"].string() == "guest" && world.screen() == "profiles",
          "With no reply the page waits and keeps it");
    // A change of mind: the newest wins, the older reply says nothing.
    world.press(Key::Left);
    world.press(Key::Ok);
    check(world.snapshot()["active"].string() == "main", "A second choice replaces it at once");
    world.settle(200);
    check(world.on("w:profiles.active").size() == 2 &&
              world.on("w:profiles.active")[1].body["id"].string() == "main",
          "Only the newest is sent after the interval");
    world.answer(world.on("w:profiles.active")[0]);
    check(world.screen() == "profiles" && world.snapshot()["active"].string() == "main",
          "The reply to the older switch changes nothing");
    world.answer(world.on("w:profiles.active")[1]);
    world.settle(300);
    check(world.screen() == "home", "The newest reply starts Home over");
}

void profilesFailureChecks() {
    World world;
    world.openProfiles();
    world.press(Key::Right);
    world.press(Key::Ok);
    world.answer(world.on("w:profiles.active")[0], false, "Not allowed");
    world.settle(40);
    check(world.screen() == "profiles" && world.snapshot()["active"].string() == "main" &&
              !world.snapshot()["busy"].boolean(),
          "A refused switch gives the profile back");
    check(world.toasted("That did not work: Not allowed"), "and says why");
    // Silence is a failure too.
    World silent;
    silent.openProfiles();
    silent.press(Key::Right);
    silent.press(Key::Ok);
    silent.settle(8300);
    check(silent.snapshot()["active"].string() == "main" && silent.screen() == "profiles",
          "Eight seconds without an answer give it back");
    // Leaving before the reply: the reply no longer opens Home.
    World leaving;
    leaving.openProfiles();
    leaving.press(Key::Right);
    leaving.press(Key::Ok);
    leaving.press(Key::Back);
    check(leaving.screen() == "home", "Back leaves at once");
    leaving.answer(leaving.on("w:profiles.active")[0]);
    leaving.settle(200);
    check(leaving.screen() == "home", "and a late reply stays out of the way");
    // Add profile.
    World adding;
    adding.openProfiles();
    adding.press({Key::Right, Key::Right});
    adding.press(Key::Ok);
    check(adding.on("w:profiles.add").size() == 1 &&
              adding.on("w:profiles.add")[0].body["action"].string() == "add" &&
              adding.on("w:profiles.add")[0].body["switch"].boolean() && adding.snapshot()["busy"].boolean(),
          "Add profile posts an add that switches to it, pending");
    adding.press(Key::Ok);
    check(adding.on("w:profiles.add").size() == 1, "A second press while it is on its way is dropped");
    adding.answer(adding.on("w:profiles.add")[0]);
    check(adding.toasted("Profile added") && adding.screen() == "home", "A new profile is announced and Home starts over");
    World same;
    same.openProfiles();
    same.press(Key::Ok);
    check(same.screen() == "home" && same.on("w:profiles.active").empty(),
          "OK on the profile in use only closes the page");
    World back;
    back.openProfiles();
    back.press(Key::Back);
    check(back.screen() == "home", "Back returns Home");
    // The colour of the profile in use: Down from the avatars, Left and Right between the swatches, OK picks.
    World colour;
    colour.openProfiles();
    colour.press(Key::Down);
    check(colour.snapshot()["onColours"].boolean() && colour.snapshot()["colour"].string() == "#a8c7fa",
          "Down from the avatars goes to the colours, on the first swatch");
    colour.press(Key::Right);
    check(colour.snapshot()["colour"].string() == "#f2b8b5", "Right moves to the next swatch");
    colour.press(Key::Ok);
    check(colour.on("w:profile.colour.main").size() == 1 &&
              colour.on("w:profile.colour.main")[0].operation == Operation::SetProfile &&
              colour.on("w:profile.colour.main")[0].body["action"].string() == "rename" &&
              colour.on("w:profile.colour.main")[0].body["id"].string() == "main" &&
              colour.on("w:profile.colour.main")[0].body["color"].string() == "#f2b8b5" &&
              colour.screen() == "profiles",
          "OK saves it on the profile route and stays on the page");
    colour.answer(colour.on("w:profile.colour.main")[0], false, "Busy");
    check(colour.toasted("The colour could not be saved: Busy"), "A refusal says why");
    colour.press(Key::Up);
    check(!colour.snapshot()["onColours"].boolean(), "Up goes back to the avatars");
    colour.press({Key::Down, Key::Down});
    check(colour.snapshot()["onButton"].boolean(), "Down twice reaches Rename profile");
    World rename;
    rename.openProfiles();
    rename.press(Key::Down);
    rename.press(Key::Down);
    rename.press(Key::Ok);
    check(rename.screen() == "rename", "Rename profile opens the editor");
    rename.press(Key::Back);
    check(rename.screen() == "profiles", "Back from the editor returns to the profiles");
}

void renameChecks() {
    World world;
    world.openProfiles();
    world.press(Key::Down);
    world.press(Key::Down);
    world.press(Key::Ok);
    check(world.screen() == "rename" && world.snapshot()["query"].string() == "LGTV",
          "Rename starts with the name on screen");
    world.app.activate("editor:clear");
    check(world.snapshot()["contentState"].string() == "empty" && world.app.shows("Enter a name"),
          "An empty name is explained");
    world.app.activate("editor:kb:4:4");
    check(world.on("w:profile.name.main").empty() && world.screen() == "rename", "An empty name is not sent");
    world.app.input("Sasha");
    world.app.activate("editor:kb:4:4");
    check(world.screen() == "profiles", "Save returns to the profiles at once");
    check(world.app.shows("Sasha") && !world.app.shows("LGTV"), "with the new name on them before any reply");
    check(world.on("w:profile.name.main").size() == 1 &&
              world.on("w:profile.name.main")[0].operation == Operation::SetProfile &&
              world.on("w:profile.name.main")[0].body["action"].string() == "rename" &&
              world.on("w:profile.name.main")[0].body["id"].string() == "main" &&
              world.on("w:profile.name.main")[0].body["name"].string() == "Sasha",
          "The write is a rename on the profile route");
    world.settle(300);
    check(world.app.shows("Sasha"), "The name stays while the server has not answered");
    world.answer(world.on("w:profile.name.main")[0], false, "Profile service is starting");
    check(world.app.shows("LGTV") && !world.app.shows("Sasha"), "A refusal gives the name back");
    check(world.toasted("The name could not be saved. Try again.: Profile service is starting"),
          "and says why");
    // An accepted rename is held until the catalogue says it too.
    World ok;
    ok.openProfiles();
    ok.press(Key::Down);
    ok.press(Key::Down);
    ok.press(Key::Ok);
    ok.app.activate("editor:clear");
    ok.app.input("Ana");
    ok.app.activate("editor:kb:4:4");
    ok.answer(ok.on("w:profile.name.main")[0]);
    ok.settle(100);
    check(ok.app.shows("Ana"), "An accepted name stays");
    size_t catalogues = ok.transport.count("catalogue");
    ok.settle(800);
    check(ok.transport.count("catalogue") == catalogues + 1, "The catalogue is read once to settle it");
    ok.reply("catalogue", std::string(catalogueJson).replace(std::string(catalogueJson).find("\"name\":\"Main\""), 13, "\"name\":\"Ana\""));
    ok.settle(100);
    check(ok.app.shows("Ana"), "and the server's name is the one that shows");
    World limit;
    limit.openProfiles();
    limit.press(Key::Down);
    limit.press(Key::Down);
    limit.press(Key::Ok);
    limit.app.input("aaaaaaaaaaaaaaaaaaaaaaaaa");
    check(limit.snapshot()["query"].string() == "LGTV", "A name over twenty characters is not taken whole");
    limit.app.input("aaaaaaaaaaaaaaaa");
    limit.app.input("b");
    check(limit.snapshot()["query"].string() == "LGTVaaaaaaaaaaaaaaaa", "A name is limited to twenty characters");
}

int main() {
    try {
        kitChecks();
        detailsChecks();
        detailsFailureChecks();
        detailsOverviewChecks();
        detailsDepthChecks();
        detailsFocusReturnChecks();
        profileNameChecks();
        profilesChecks();
        profilesFailureChecks();
        searchChecks();
        searchStateChecks();
        searchGridChecks();
        keyboardChecks();
        renameChecks();
        std::printf("%d native overlay-screen checks passed\n", checkCount());
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
