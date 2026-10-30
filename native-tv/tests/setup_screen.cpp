// First-run setup, the keyboard pad, the QR layout and the splash, driven the way a person drives
// them: keys in, the server's answers handed back, the screen read afterwards.
#include "domain/setup_pages.hpp"
#include "ui/application.hpp"
#include "ui/keyboard_pad.hpp"
#include "ui/qr_code.hpp"
#include "ui/setup_screen.hpp"
#include "ui/splash.hpp"
#include "qrcodegen.hpp"
#include <cmath>
#include <cstdio>
#include <deque>
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
struct Transport final : Services {
    struct Request {
        std::string channel;
        Operation operation;
        Json body;
        bool write;
    };
    std::vector<Request> requests;
    std::map<std::string, unsigned> revisions;
    unsigned read(const std::string &channel, Operation operation, const Json &arguments) override {
        requests.push_back({channel, operation, arguments, false});
        return ++revisions[channel];
    }
    unsigned write(const std::string &channel, Operation operation, const Json &body) override {
        requests.push_back({channel, operation, body, true});
        return ++revisions[channel];
    }
    void cancel(const std::string &channel) override {
        ++revisions[channel];
    }
    size_t count(const std::string &channel) const {
        size_t n = 0;
        for (const auto &request : requests)
            if (request.channel == channel)
                ++n;
        return n;
    }
    const Request *last(const std::string &channel) const {
        for (auto it = requests.rbegin(); it != requests.rend(); ++it)
            if (it->channel == channel)
                return &*it;
        return nullptr;
    }
};
void reply(Application &app, const std::string &channel, const std::string &json, long status = 200) {
    Response response;
    response.channel = channel;
    response.status = status;
    response.bytes = json;
    app.response(response);
}
// The answer to a write the optimistic layer sent: it ignores any but the newest.
void answerWrite(Transport &transport, Application &app, const std::string &key, const std::string &json,
                 long status = 200) {
    Response response;
    response.channel = "w:" + key;
    response.generation = transport.revisions["w:" + key];
    response.status = status;
    response.bytes = json;
    app.response(response);
}
void fail(Application &app, const std::string &channel) {
    Response response;
    response.channel = channel;
    response.error = "Could not connect";
    app.response(response);
}
uint32_t run(Application &app, uint32_t from, uint32_t ms) {
    uint32_t at = from;
    for (uint32_t step = 0; step <= ms * 60 / 1000; ++step) {
        at = from + step * 1000 / 60;
        app.update(at);
    }
    return at;
}

// What /api/setup says. `standby`: the TV has LG's Always-on.
std::string setupState(bool open = false, bool standby = true, const char *haState = "unknown",
                       const char *handoff = "", bool demo = false, bool always = false) {
    std::string text = std::string("{\"ok\":true,\"needed\":true,\"model\":\"OLED65B3PUA\",\"network\":") +
                       (open ? "true" : "false") + ",\"token\":false,\"homeAssistant\":{\"state\":\"" +
                       haState + "\",\"configured\":" + (std::string(haState) == "connected" ? "true" : "false") +
                       "},\"handoff\":" + (*handoff ? std::string("\"") + handoff + "\"" : "null");
    if (standby)
        text += std::string(",\"alwaysReady\":") + (always ? "true" : "false");
    if (demo)
        text += ",\"demo\":true";
    return text + "}";
}
const char *kLink = "http://192.168.1.42:3000/?c=0f3a9c0f3a9c0f3a9c0f3a9c0f3a9cab";
const char *kHandoffReply =
    R"({"ok":true,"url":"http://192.168.1.42:3000/?c=0f3a9c0f3a9c0f3a9c0f3a9c0f3a9cab"})";
const char *kSettings =
    R"({"ok":true,"writable":true,"iconServer":false,"mqtt":{"enabled":false,"host":"","port":null,"tls":false,"tlsRejectUnauthorized":true,"username":"","passwordSet":false,"topicPrefix":"lgtv","discoveryPrefix":"homeassistant","telemetryIntervalMs":30000,"entities":{"controls":true,"oled":true,"video":true,"system":true,"diagnostics":true,"disabled":[]}},"device":{"id":"lg_tv","name":"LG TV"}})";

// A TV at the welcome page, the state answered.
struct Fixture {
    Transport transport;
    Application app;
    uint32_t clock = 16;
    explicit Fixture(const std::string &state = setupState(), const char *route = "setup:welcome")
        : app(transport, route) {
        reply(app, "setup:state", state);
        advance(600);
    }
    void advance(uint32_t ms) {
        clock = run(app, clock, ms);
    }
    void press(Key key, uint32_t settle = 450) {
        app.key(key);
        advance(settle);
    }
    std::string screen() {
        return app.screenName();
    }
    std::string step() {
        return app.snapshot()["step"].string();
    }
    // The pad starts on "q": Down three rows reaches the first key of the last row, four Rights
    // the Done key.
    void pressDone() {
        for (int i = 0; i < 3; ++i)
            press(Key::Down, 40);
        for (int i = 0; i < 4; ++i)
            press(Key::Right, 40);
        press(Key::Ok, 40);
    }
    void typeField(const std::string &text) {
        if (!text.empty())
            app.input(text);
        pressDone();
    }
    // Welcome, then the name as it stands: the first question.
    void toNetwork() {
        press(Key::Ok);
        pressDone();
        advance(450);
    }
    void answerWrite(const std::string &key, const std::string &json) {
        ::answerWrite(transport, app, key, json);
        advance(100);
    }
    // The server's reply to every request of a channel not yet answered, as `json`.
    void answer(const std::string &channel, const std::string &json) {
        reply(app, channel, json);
        advance(100);
    }
    // Waits out the page's own timers until `channel` has been asked for again.
    bool waitFor(const std::string &channel, size_t before, uint32_t limitMs) {
        for (uint32_t waited = 0; waited < limitMs; waited += 100) {
            if (transport.count(channel) > before)
                return true;
            advance(100);
        }
        return transport.count(channel) > before;
    }
};

const Node *imageNodeWith(const Scene &scene, const std::string &prefix) {
    for (const auto &node : scene.nodes)
        if (node.kind == Node::Kind::Image && node.value.compare(0, prefix.size(), prefix) == 0)
            return &node;
    return nullptr;
}
bool hasHit(const Scene &scene, const std::string &id) {
    for (const auto &hit : scene.hits)
        if (hit.id == id)
            return true;
    return false;
}
// Moves a pad's focus to `id` by the arrow keys, by the shortest way (tried on copies).
void focusKey(KeyboardPad &pad, Motion &motion, const std::string &id) {
    std::deque<std::pair<KeyboardPad, std::vector<Key>>> queue;
    std::set<std::string> seen;
    queue.push_back({pad, {}});
    while (!queue.empty()) {
        auto step = queue.front();
        queue.pop_front();
        if (step.first.focusedKey() == id) {
            for (Key key : step.second)
                pad.key(motion, 0, key);
            return;
        }
        if (!seen.insert(step.first.focusedKey()).second)
            continue;
        for (Key key : {Key::Up, Key::Down, Key::Left, Key::Right}) {
            KeyboardPad next = step.first;
            Motion scratch;
            if (next.key(scratch, 0, key) == KeyboardPad::Result::Moved) {
                auto path = step.second;
                path.push_back(key);
                queue.push_back({next, path});
            }
        }
    }
}
} // namespace

int main() {
    try {
        // ---- The pad: layers, shift, caret, limits ---------------------------------------------
        {
            Motion motion;
            MotionClock clock([] { return 0.0; });
            KeyboardPad pad;
            KeyboardPad::Options options;
            options.maximum = 6;
            pad.begin(motion, "ab", options);
            check(pad.value() == "ab" && pad.cursor() == 2 && pad.focusedKey() == "q",
                  "The pad starts with the caret at the end and focus on q");
            check(pad.key(motion, 10, Key::Right) == KeyboardPad::Result::Moved &&
                      pad.focusedKey() == "w",
                  "Right moves along the row");
            check(pad.key(motion, 20, Key::Ok) == KeyboardPad::Result::Edited && pad.value() == "abw",
                  "OK types the focused key at the caret");
            check(pad.key(motion, 30, Key::Left) == KeyboardPad::Result::Moved &&
                      pad.key(motion, 30, Key::Up) == KeyboardPad::Result::Moved &&
                      pad.focusedKey() == "1",
                  "Up reaches the digits");
            check(pad.key(motion, 40, Key::Up) == KeyboardPad::Result::None,
                  "Up from the top row is left to the page");
            focusKey(pad, motion, "left");
            check(pad.focusedKey() == "left", "The caret keys are on the last row");
            pad.key(motion, 50, Key::Ok);
            pad.key(motion, 50, Key::Ok);
            check(pad.cursor() == 1, "The caret key moves the caret by a character");
            check(pad.type(60, "Z") == KeyboardPad::Result::Edited && pad.value() == "aZbw",
                  "Hardware text goes in at the caret");
            check(pad.key(motion, 70, Key::Delete) == KeyboardPad::Result::Edited && pad.value() == "abw",
                  "Delete erases before the caret");
            // Capitals stay on until shift is pressed again, as the TV keyboard has always done.
            focusKey(pad, motion, "shift");
            check(pad.focusedKey() == "shift", "Shift is on the fourth row");
            pad.key(motion, 80, Key::Ok);
            check(pad.model().upper(), "Shift turns capitals on");
            pad.key(motion, 80, Key::Up);
            pad.key(motion, 80, Key::Ok);
            pad.key(motion, 80, Key::Ok);
            check(pad.value() == "aAAbw", "Capitals stay on for every key until shift is pressed again");
            focusKey(pad, motion, "shift");
            pad.key(motion, 90, Key::Ok);
            check(!pad.model().upper(), "Shift again turns them off");
            check(pad.type(100, "xyz") == KeyboardPad::Result::None && pad.value() == "aAAbw",
                  "The limit of 6 characters holds (5 typed, 3 more refused)");
            // The symbols layer.
            focusKey(pad, motion, "symbols");
            pad.key(motion, 110, Key::Ok);
            check(pad.model().symbols() && pad.model().padRows()[1][0] == "!" &&
                      pad.model().padRows()[3].back() == "delete",
                  "The symbols key swaps the layer and keeps delete");
            check(pad.focusedKey() == "symbols", "Focus stays on a key that exists after the swap");
            // Done and the pointer.
            Keyboard names;
            names.reset("", 10);
            check(names.padRows()[2].back() == "'", "A name's letters end with an apostrophe");
            names.reset("", 10, true);
            check(names.padRows()[2].back() == ".", "An address's letters end with a full stop");
            KeyboardPad::Options done;
            done.id = "kbx";
            pad.begin(motion, "", done);
            check(pad.click(motion, 0, "kbx:4:4") == KeyboardPad::Result::Done,
                  "A click on Done presses it");
            check(pad.click(motion, 0, "kbx:1:2") == KeyboardPad::Result::Edited && pad.value() == "e",
                  "A click on a key moves focus there and types it");
            check(pad.click(motion, 0, "other:1:2") == KeyboardPad::Result::None,
                  "A hit of another component is ignored");
            pad.activate(motion, false);
            pad.setValue("host", 500);
            check(pad.value() == "host" && pad.cursor() == 4 && pad.caretLit(500) && !pad.caretLit(1100),
                  "The page can replace the text, and the caret restarts");
            // A key press dips and springs back.
            motion.update(clock);
            check(motion.value("kbx:p:1:2") > .9, "The pressed key is at the bottom of its dip");
            double now = 0;
            MotionClock stepping([&now] { return now; });
            for (int i = 0; i < 20; ++i) {
                now += 16;
                stepping.tick();
                motion.update(stepping);
            }
            check(motion.value("kbx:p:1:2") < .05, "The key has sprung back within 320 ms");
            check(kit::caretLit(0, 0) && !kit::caretLit(700, 0) && kit::caretLit(1150, 0) &&
                      kit::caretLit(100, 0),
                  "The caret is lit for 550 ms of each 1100 ms and restarts at an edit");
            // Pad geometry: every row is as wide as the digits, the space key takes the rest.
            Scene scene;
            kit::LegacyGlyphs glyphs;
            pad.begin(motion, "", options);
            pad.paint(kit::Paint{scene, glyphs}, motion);
            float digitsRight = 0, lastRight = 0;
            for (const Hit &hit : scene.hits) {
                if (hit.id == "kb:0:9")
                    digitsRight = hit.rect.x + hit.rect.w;
                if (hit.id == "kb:4:4")
                    lastRight = hit.rect.x + hit.rect.w;
            }
            check(std::fabs(digitsRight - lastRight) < 8 && scene.hits.size() == 10 + 10 + 10 + 9 + 5,
                  "Forty-four keys, the last row ending where the digits end");
        }

        // ---- The QR layout ----------------------------------------------------------------------
        {
            const Rect tile = {80, 490, 264, 264};
            for (const char *text :
                 {kLink, "http://192.168.1.42:3000/?c=abcdef", "http://10.0.0.5:8081/?c=0123456789abcdef0123456789abcdef"}) {
                QrLayout layout = qrLayout(text, tile);
                int modules =
                    qrcodegen::QrCode::encodeText(text, qrcodegen::QrCode::Ecc::MEDIUM).getSize();
                check(layout.fits && layout.modules == modules, "The code fits the tile");
                check(layout.scale >= kQrMinScale, "A module is at least three pixels");
                check(layout.image.w == (modules + 8) * layout.scale && layout.image.w == layout.image.h,
                      "The texture (code plus four quiet modules a side) maps to whole pixels");
                check(layout.image.x == std::floor(layout.image.x) && layout.image.y == std::floor(layout.image.y) &&
                          layout.image.x >= tile.x && layout.image.x + layout.image.w <= tile.x + tile.w &&
                          layout.image.y >= tile.y && layout.image.y + layout.image.h <= tile.y + tile.h,
                      "On a whole pixel, inside the tile");
                check(std::fabs((layout.image.x - tile.x) - (tile.x + tile.w - layout.image.x - layout.image.w)) <= 1,
                      "Centred");
                check(layout.value == std::string("qr:") + text, "The renderer is told what to encode");
            }
            check(!qrLayout(std::string(900, 'x'), tile).fits,
                  "A text too long to read at a readable module size does not draw");
        }

        // ---- The setup pages' words -------------------------------------------------------------
        {
            Json state = Json::parse(setupState());
            SetupPage network = setupPage(SetupStep::Network, state, false, false);
            check(network.step == "Step 2 of 4" && network.choices.size() == 2 && network.selected == 0 &&
                      network.dots == 3,
                  "The network page is step 2 of 4 on a TV with Always-on: the name comes first");
            SetupPage name = setupPage(SetupStep::Name, state, false, false);
            check(name.step == "Step 1 of 4" && name.title == "Name this TV" && name.dots == 2 &&
                      setupDefaultName(state) == "B3" && setupDefaultName(Json::object()) == "LGTV",
                  "The name page is step 1, and the default comes from the TV's model");
            check(nameBody("Den")["action"].string() == "rename" && nameBody("Den")["id"].string() == "main" &&
                      nameBody("Den")["name"].string() == "Den",
                  "The name is written as the profile editor writes a rename");
            check(setupPage(SetupStep::Network, state, true, false).selected == 1,
                  "Once answered, a closed network preselects 'only on this TV'");
            check(setupPage(SetupStep::Network, Json::parse(setupState(false, false)), false, false).step ==
                      "Step 2 of 3",
                  "Without Always-on the flow is three steps with the name");
            check(setupPage(SetupStep::Handoff, state, false, true).dots == 0,
                  "From Settings the page has no dots");
            SetupPage summary = setupPage(SetupStep::Summary, Json::parse(setupState(true, true, "connected", "", false, true)),
                                          true, false);
            check(summary.choices.size() == 4 && summary.choices[1].value == "On" &&
                      summary.choices[2].value == "Connected" && summary.choices[3].on && summary.dots == 6,
                  "The summary lists what was chosen");
            SetupPage named = setupPage(SetupStep::Summary, Json::parse(setupState()), true, false, "Den");
            check(named.choices.size() == 5 && named.choices[0].label == "TV name" &&
                      named.choices[0].value == "Den",
                  "and the name first");
            check(brokerProblem("", "1883") != "" && brokerProblem("host", "0") != "" &&
                      brokerProblem("host", "70000") != "" && brokerProblem("host", "abc") != "" &&
                      brokerProblem("host", "1883").empty(),
                  "The broker's address and port are checked as the phone form checks them");
            Json body = brokerSettings(Json::parse(kSettings), "10.0.0.2", "8883", "me", "pw");
            check(body["mqtt"]["host"].string() == "10.0.0.2" && body["mqtt"]["port"].number() == 8883 &&
                      body["mqtt"]["tls"].boolean() && body["mqtt"]["enabled"].boolean() &&
                      body["mqtt"]["telemetryIntervalMs"].number() == 30000 &&
                      body["device"]["id"].string() == "lg_tv" && !body["mqtt"].has("passwordSet"),
                  "The settings body keeps the server's other values and replaces the broker's");
        }

        // ---- Registered: only setup routes ------------------------------------------------------
        check(makeSetupScreen("home") == nullptr && makeSetupScreen("setup:nonsense") == nullptr &&
                  makeSetupScreen("setup:restarting") == nullptr && makeSetupScreen("setup:ha") != nullptr,
              "The factory answers only for the steps a route can name");

        // ---- Whatever the client is opened on, a fresh install opens setup first -----------------
        for (const char *launched : {"dashboard", "home", "dashboard:settings", "quick"}) {
            Transport transport;
            Application app(transport, launched);
            check(transport.count("caps") == 1, "The client asks the server whether setup is needed");
            reply(app, "caps", R"({"ok":true,"setupNeeded":true})");
            check(app.screenName() == "setup:welcome",
                  "A server that says setup is needed opens it, whichever screen was asked for");
        }
        {
            Transport transport;
            Application app(transport, "dashboard");
            reply(app, "caps", R"({"ok":true,"setupNeeded":false})");
            check(app.screenName() != "setup:welcome" && app.screenName().compare(0, 6, "setup:") != 0,
                  "A server that says setup is done leaves the screen alone");
        }
        {
            // Settings' own entry to the phone link is where the person was sent: it stays.
            Transport transport;
            Application app(transport, "setup:handoff");
            reply(app, "caps", R"({"ok":true,"setupNeeded":true})");
            check(app.screenName() == "setup:handoff", "The Settings entry is not sent back to the welcome page");
        }
        {
            // A server that is not up when the client opens is asked again, so the answer still comes.
            Transport transport;
            Application app(transport, "dashboard");
            fail(app, "caps");
            uint32_t clock = run(app, 16, 1500);
            check(transport.count("caps") == 1, "(nothing yet: it waits)");
            clock = run(app, clock, 1200);
            check(transport.count("caps") == 2, "A caps read that failed is asked again after two seconds");
            reply(app, "caps", R"({"ok":true,"setupNeeded":true})");
            check(app.screenName() == "setup:welcome", "and the late answer opens setup");
            run(app, clock, 4200);
            check(transport.count("caps") == 2, "Once answered it is not asked again");
        }

        // ---- A server that does not answer does not trap the person --------------------------------
        {
            Transport transport;
            Application app(transport, "setup:welcome");
            run(app, 16, 300);
            app.key(Key::Ok);
            run(app, 316, 300);
            check(app.screenName() == "setup:welcome" && !app.snapshot()["loaded"].boolean(),
                  "Before the server answers, nothing is chosen");
            app.key(Key::Back);
            check(app.screenName() == "home", "but Back still leaves");
        }

        // ---- First run end to end, on a TV with Always-on ---------------------------------------
        {
            Transport transport;
            Application app(transport, "home");
            reply(app, "caps", R"({"ok":true,"setupNeeded":true})");
            check(app.screenName() == "setup:welcome", "A TV that needs setup opens it");
            uint32_t clock = run(app, 16, 50);
            check(transport.count("setup:state") == 1, "The page asks the server for its state");
            check(!app.shows("Your TV, your rules."), "Nothing is drawn before the state is known");
            reply(app, "setup:state", setupState());
            clock = run(app, clock, 600);
            check(app.shows("Your TV, your rules.") && app.shows("Get started") &&
                      app.shows("Four quick choices first"),
                  "The welcome page names the four choices on a TV with Always-on");
            check(!app.frame().nodes.empty(), "The welcome page draws");
            auto doneKey = [&] {
                for (int i = 0; i < 3; ++i) {
                    app.key(Key::Down);
                    clock = run(app, clock, 40);
                }
                for (int i = 0; i < 4; ++i) {
                    app.key(Key::Right);
                    clock = run(app, clock, 40);
                }
                app.key(Key::Ok);
                clock = run(app, clock, 100);
            };
            app.key(Key::Ok);
            clock = run(app, clock, 500);
            check(app.screenName() == "setup:name" && app.shows("Name this TV") && app.shows("Step 1 of 4") &&
                      app.snapshot()["typed"].string() == "B3",
                  "OK on 'Get started' opens the name, prefilled with the TV's series");
            app.key(Key::Delete);
            app.key(Key::Delete);
            app.input("Den");
            doneKey();
            const auto *renamed = transport.last("w:setup:name");
            check(renamed && renamed->write && renamed->operation == Operation::SetProfile &&
                      renamed->body["action"].string() == "rename" && renamed->body["id"].string() == "main" &&
                      renamed->body["name"].string() == "Den",
                  "Next writes the name as the main profile's");
            clock = run(app, clock, 500);
            check(app.screenName() == "setup:network" && app.shows("Turn on the web dashboard?") &&
                      app.shows("Step 2 of 4"),
                  "and the network question comes at once, step 2 of 4, before the server has answered");
            answerWrite(transport, app, "setup:name",
                        R"({"ok":true,"profiles":[{"id":"main","name":"Den","active":true}]})");
            clock = run(app, clock, 200);
            app.key(Key::Down);
            clock = run(app, clock, 400);
            check(app.snapshot()["selected"].number() == 1 && app.shows("Nothing else on your network"),
                  "Down moves to 'No, only on this TV' and its words replace the first's");
            size_t writes = transport.count("setup:network");
            app.key(Key::Ok);
            clock = run(app, clock, 500);
            check(transport.count("setup:network") == writes && app.screenName() == "setup:ha",
                  "Keeping the dashboard closed on a closed server writes nothing and goes on");
            app.key(Key::Back);
            clock = run(app, clock, 400);
            check(app.screenName() == "setup:network" && app.snapshot()["selected"].number() == 1,
                  "Back returns to the question with the answer still chosen");
            app.key(Key::Ok);
            clock = run(app, clock, 400);
            app.key(Key::Ok);
            clock = run(app, clock, 400);
            check(app.screenName() == "setup:handoff", "'Set it up on my phone' opens the handoff");
            const auto *handoffWrite = transport.last("w:setup:handoff");
            check(handoffWrite && handoffWrite->write && handoffWrite->operation == Operation::Setup &&
                      handoffWrite->body["action"].string() == "handoff",
                  "The link is asked for with the handoff action");
            check(imageNodeWith(app.frame(), "qr:") == nullptr, "No code is drawn before the link answers");
            answerWrite(transport, app, "setup:handoff", kHandoffReply);
            clock = run(app, clock, 700);
            const Node *qr = imageNodeWith(app.frame(), "qr:");
            check(qr && qr->value == std::string("qr:") + kLink,
                  "The link's address is the code the TV draws");
            check(qr->rect.w == std::floor(qr->rect.w) && qr->rect.x == std::floor(qr->rect.x) &&
                      qr->rect.y == std::floor(qr->rect.y),
                  "The code is on whole pixels");
            check(app.shows("192.168.1.42:3000") && app.shows("Waiting for your phone"),
                  "The address and the waiting line are shown");
            check(app.shows("Or enter the broker address here"), "The TV's own entry is offered");
            // Typed on the TV: address, port, user, password.
            app.input("192.168.1.10");
            check(app.snapshot()["typed"].string() == "192.168.1.10", "Typed text lands in the field");
            doneKey();
            check(app.snapshot()["field"].number() == 1 && app.snapshot()["typed"].string() == "1883",
                  "Done on the address moves to the port, which starts at the usual 1883");
            doneKey();
            app.input("me");
            doneKey();
            check(app.snapshot()["field"].number() == 3 && app.snapshot()["typed"].string() == "",
                  "Then the user and the password");
            app.input("pw");
            doneKey();
            const auto *asked = transport.last("setup:settings");
            check(asked && !asked->write && asked->operation == Operation::Settings &&
                      app.snapshot()["link"].string() == "sending",
                  "Connect reads the server's settings first");
            reply(app, "setup:settings", kSettings);
            const auto *saved = transport.last("w:setup:save");
            check(saved && saved->write && saved->operation == Operation::Settings &&
                      saved->body["mqtt"]["host"].string() == "192.168.1.10" &&
                      saved->body["mqtt"]["port"].number() == 1883 &&
                      saved->body["mqtt"]["username"].string() == "me" &&
                      saved->body["mqtt"]["password"].string() == "pw" &&
                      saved->body["mqtt"]["enabled"].boolean() &&
                      saved->body["device"]["id"].string() == "lg_tv",
                  "and writes them back with the broker filled in");
            answerWrite(transport, app, "setup:save", R"({"ok":true,"restarting":true})");
            check(transport.last("setup:stop") == nullptr,
                  "The restart that takes the broker ends the phone link; nothing else is sent");
            clock = run(app, clock, 2200);
            check(transport.count("setup:poll") >= 1, "The page polls for the connection");
            fail(app, "setup:poll"); // the server is restarting
            size_t polls = transport.count("setup:poll");
            for (uint32_t waited = 0; waited < 2500 && transport.count("setup:poll") == polls; waited += 100)
                clock = run(app, clock, 100);
            reply(app, "setup:poll", setupState(false, true, "connected"));
            clock = run(app, clock, 600);
            check(app.shows("Connected"), "It says Connected");
            clock = run(app, clock, 1800);
            check(app.screenName() == "setup:standby" && app.shows("Step 4 of 4"),
                  "and moves on by itself to the standby question");
            // Standby: the choice before is 'No' (Always-on is off); choose 'Yes'.
            check(app.snapshot()["selected"].number() == 1, "The current setting is preselected");
            app.key(Key::Up);
            clock = run(app, clock, 300);
            app.key(Key::Ok);
            const auto *standby = transport.last("w:setup:standby");
            check(standby && standby->body["action"].string() == "alwaysReady" &&
                      standby->body["on"].boolean(),
                  "'Yes, keep it connected' writes Always-on on");
            clock = run(app, clock, 500);
            check(app.screenName() == "setup:summary" && app.shows("Ready to go") &&
                      app.shows("Stays connected when off") && app.shows("Open dashboard") &&
                      app.shows("Den"),
                  "and the summary shows what was chosen, the name too, before the server has answered");
            answerWrite(transport, app, "setup:standby", R"({"ok":true})");
            clock = run(app, clock, 200);
            app.key(Key::Ok);
            const auto *done = transport.last("w:setup:done");
            check(done && done->body["action"].string() == "done" && !done->body.has("demo"),
                  "Open dashboard marks setup done");
            check(app.screenName() == "dashboard:system", "and leaves for the dashboard at once");
            answerWrite(transport, app, "setup:done", R"({"ok":true})");
        }

        // ---- Choices the server refuses are taken back, with a message -----------------------------
        {
            Fixture f(setupState(false, true, "unknown", "", false, false));
            f.press(Key::Ok);
            f.app.key(Key::Delete);
            f.app.key(Key::Delete);
            f.app.input("Kitchen");
            f.pressDone();
            f.advance(300);
            check(f.screen() == "setup:network" && f.transport.last("w:setup:name") != nullptr,
                  "A name is written and setup goes on");
            f.answerWrite("setup:name", R"({"ok":false,"error":"that name is refused"})");
            f.advance(200);
            check(f.app.shows("that name is refused"), "The refusal is said");
            f.press(Key::Down);
            f.press(Key::Ok); // the dashboard stays closed
            f.press(Key::Down);
            f.press(Key::Ok); // not now
            f.press(Key::Up);
            f.press(Key::Ok); // keep it connected
            f.advance(300);
            check(f.screen() == "setup:summary" && f.app.shows("B3"),
                  "The name on the summary is the TV's own again");
            f.answerWrite("setup:standby", R"({"ok":false,"error":"controls disabled in config"})");
            f.advance(300);
            check(f.app.shows("controls disabled in config"), "A refused standby is said too");
            bool off = false;
            for (const auto &row : f.app.frame().nodes)
                if (row.kind == Node::Kind::Text && row.value == "Off")
                    off = true;
            check(f.app.shows("Stays connected when off") && off, "and the summary goes back to Off");
        }

        // ---- A restart to apply the network choice, then trouble ---------------------------------
        {
            Fixture f;
            f.toNetwork();
            f.press(Key::Ok); // 'Yes, on phones and computers': the server is closed, so it changes
            const auto *network = f.transport.last("setup:network");
            check(network && network->body["action"].string() == "network" &&
                      network->body["open"].boolean() && f.screen() == "setup:restarting",
                  "Opening the dashboard writes it and shows the restart");
            f.press(Key::Down);
            f.press(Key::Ok);
            check(f.screen() == "setup:restarting", "Keys do nothing while the server restarts");
            size_t asked = f.transport.count("setup:state");
            f.answer("setup:network", R"({"ok":true})");
            f.advance(1200);
            check(f.transport.count("setup:state") == asked,
                  "The page gives the server 1.5 s before it looks for it");
            check(f.waitFor("setup:state", asked, 800), "then looks");
            fail(f.app, "setup:state");
            asked = f.transport.count("setup:state");
            f.advance(800);
            check(f.transport.count("setup:state") == asked, "and tries again a second later");
            check(f.waitFor("setup:state", asked, 600), "The retry comes");
            f.answer("setup:state", setupState(true));
            f.advance(500);
            check(f.screen() == "setup:ha", "Once the server answers, the next question comes");
        }
        {
            Fixture f;
            f.toNetwork();
            f.press(Key::Ok);
            f.answer("setup:network", R"({"ok":true})");
            size_t handled = f.transport.count("setup:state");
            for (uint32_t waited = 0; waited < 80000 && f.screen() != "setup:trouble"; waited += 100) {
                f.advance(100);
                if (f.transport.count("setup:state") > handled) {
                    handled = f.transport.count("setup:state");
                    fail(f.app, "setup:state");
                }
            }
            f.advance(500);
            check(f.screen() == "setup:trouble" && f.app.shows("Something went wrong") &&
                      f.app.shows("The dashboard stopped answering"),
                  "A server that never comes back ends in the trouble page after 60 tries");
            // The page has waited a minute without a key, so it has dimmed: the first OK wakes it.
            check(f.app.snapshot()["idle"].boolean(), "The trouble page dims like every screen");
            f.press(Key::Ok);
            f.advance(300);
            check(f.screen() == "setup:trouble" && !f.app.snapshot()["idle"].boolean(),
                  "The first OK after the dim only wakes the page");
            f.press(Key::Ok);
            f.advance(200);
            f.answer("setup:state", setupState(true));
            f.advance(500);
            check(f.screen() == "setup:welcome", "OK tries again and starts over once the server is back");
        }

        // ---- The phone link: refused, expired, never connected, connected -------------------------
        {
            Fixture f;
            f.toNetwork();
            f.press(Key::Down);
            f.press(Key::Ok);
            f.press(Key::Ok); // handoff
            f.answerWrite("setup:handoff", R"({"ok":false,"error":"busy"})");
            f.advance(500);
            check(f.app.snapshot()["link"].string() == "failed" &&
                      f.app.shows("The link could not be opened.") && f.app.shows("Try again") &&
                      f.app.shows("Skip for now") && imageNodeWith(f.app.frame(), "qr:") == nullptr,
                  "A refused link says so and offers a new code or a way on");
            size_t asked = f.transport.count("w:setup:handoff");
            f.press(Key::Ok);
            check(f.transport.count("w:setup:handoff") == asked + 1, "Try again asks for a new code");
            f.answerWrite("setup:handoff", kHandoffReply);
            f.advance(600);
            check(f.app.snapshot()["link"].string() == "ready", "The new code is shown");
            // Expired: the server no longer has a link, twice running.
            size_t polls = f.transport.count("setup:poll");
            check(f.waitFor("setup:poll", polls, 2500), "The page polls every two seconds");
            f.answer("setup:poll", setupState());
            polls = f.transport.count("setup:poll");
            check(f.waitFor("setup:poll", polls, 2500), "and again");
            f.answer("setup:poll", setupState());
            f.advance(300);
            check(f.app.snapshot()["link"].string() == "failed" && f.app.shows("The code has expired."),
                  "Two polls without a link mean the code expired");
            f.press(Key::Down);
            f.press(Key::Ok); // Skip for now
            check(f.transport.last("setup:stop") != nullptr && f.screen() == "setup:standby",
                  "Skipping closes the link and goes on to the standby question");
        }
        {
            Fixture f;
            f.toNetwork();
            f.press(Key::Down);
            f.press(Key::Ok);
            f.press(Key::Ok);
            f.answerWrite("setup:handoff", kHandoffReply);
            f.advance(600);
            size_t polls = f.transport.count("setup:poll");
            check(f.waitFor("setup:poll", polls, 2500), "Polling");
            fail(f.app, "setup:poll"); // the phone sent its details and the server is restarting
            for (int i = 0; i < 6; ++i) {
                polls = f.transport.count("setup:poll");
                check(f.waitFor("setup:poll", polls, 2500), "Polling after the restart");
                f.answer("setup:poll", setupState(false, true, "error"));
            }
            f.advance(300);
            check(f.app.snapshot()["link"].string() == "failed" && f.app.shows("It did not connect."),
                  "A broker that refuses six polls in a row is reported");
            f.press(Key::Ok); // Try again
            check(f.screen() == "setup:handoff", "and a new code can be asked for");
        }
        {
            Fixture f;
            f.toNetwork();
            f.press(Key::Down);
            f.press(Key::Ok);
            f.press(Key::Ok);
            f.answerWrite("setup:handoff", kHandoffReply);
            f.advance(600);
            size_t polls = f.transport.count("setup:poll");
            check(f.waitFor("setup:poll", polls, 2500), "Polling");
            fail(f.app, "setup:poll");
            polls = f.transport.count("setup:poll");
            check(f.waitFor("setup:poll", polls, 2500), "Polling after the phone's details arrived");
            f.answer("setup:poll", setupState(false, true, "connected"));
            f.advance(500);
            check(f.app.shows("Connected") && f.screen() == "setup:handoff",
                  "The waiting line turns to Connected");
            f.press(Key::Ok, 100); // keys are ignored for the moment it stays
            f.advance(1600);
            check(f.screen() == "setup:standby", "and the page moves on by itself");
        }

        // ---- Back and Left ------------------------------------------------------------------------
        {
            Fixture f;
            f.toNetwork();
            f.press(Key::Left);
            check(f.screen() == "setup:name", "Left goes back, as it did on the web page");
            f.press(Key::Back);
            check(f.screen() == "setup:welcome", "Back from the name returns to the welcome page");
            f.toNetwork();
            f.press(Key::Down);
            f.press(Key::Ok);
            f.press(Key::Ok);
            f.answerWrite("setup:handoff", kHandoffReply);
            f.advance(500);
            f.press(Key::Back);
            check(f.screen() == "setup:ha" && f.transport.last("setup:stop") != nullptr,
                  "Back from the handoff closes the link and returns to the question");
            f.press(Key::Down);
            f.press(Key::Ok); // Not now
            check(f.screen() == "setup:standby", "'Not now' goes to standby on a TV that has it");
            f.press(Key::Back);
            check(f.screen() == "setup:ha", "Back from standby returns to the question");
            f.press(Key::Ok, 450);
            f.answerWrite("setup:handoff", kHandoffReply);
            f.press(Key::Back);
            f.press(Key::Down);
            f.press(Key::Ok);
            f.press(Key::Ok); // No, let it sleep: already off
            check(f.screen() == "setup:summary", "Choosing what Always-on already is writes nothing");
            f.press(Key::Back);
            check(f.screen() == "setup:standby", "Back from the summary returns to standby");
            f.press(Key::Back);
            f.press(Key::Back);
            f.press(Key::Back);
            f.press(Key::Back);
            check(f.screen() == "setup:welcome", "Back all the way returns to the welcome page");
            f.app.key(Key::Back);
            check(f.screen() == "home", "Back on the welcome page leaves setup");
        }

        // ---- A TV without Always-on ---------------------------------------------------------------
        {
            Fixture f(setupState(false, false));
            check(f.app.shows("Three quick choices first"), "Three choices are promised");
            f.toNetwork();
            check(f.app.shows("Step 2 of 3"), "and counted");
            f.press(Key::Down);
            f.press(Key::Ok);
            f.press(Key::Down);
            f.press(Key::Ok); // Not now
            check(f.screen() == "setup:summary" && !f.app.shows("Stays connected when off") &&
                      f.app.shows("Not set up"),
                  "The summary comes straight after Home Assistant, without the standby line");
        }

        // ---- From Settings: only the phone link ---------------------------------------------------
        {
            Fixture f(setupState(), "setup:handoff");
            check(f.screen() == "setup:handoff" && f.app.snapshot()["fromSettings"].boolean() &&
                      f.transport.last("w:setup:handoff") != nullptr,
                  "Settings opens the phone link directly");
            f.answerWrite("setup:handoff", kHandoffReply);
            f.advance(600);
            check(f.app.shows("Back to Settings") && !f.app.shows("Skip for now"),
                  "The hints say Back returns to Settings");
            f.press(Key::Back);
            check(f.screen() == "dashboard:settings" && f.transport.last("setup:stop") != nullptr,
                  "Back closes the link and returns to Settings");
        }
        {
            Fixture f(setupState(), "setup:handoff");
            f.answerWrite("setup:handoff", kHandoffReply);
            f.advance(600);
            size_t polls = f.transport.count("setup:poll");
            check(f.waitFor("setup:poll", polls, 2500), "Polling");
            fail(f.app, "setup:poll");
            polls = f.transport.count("setup:poll");
            check(f.waitFor("setup:poll", polls, 2500), "Polling");
            f.answer("setup:poll", setupState(false, true, "connected"));
            f.advance(2000);
            check(f.screen() == "dashboard:settings", "Connected from Settings returns to Settings");
        }

        // ---- The dry run ---------------------------------------------------------------------------
        {
            Fixture f(setupState(false, true, "unknown", "", true));
            f.toNetwork();
            f.press(Key::Ok); // open
            const auto *network = f.transport.last("setup:network");
            size_t asked = f.transport.count("setup:state");
            check(network && network->body["demo"].boolean() && network->body["open"].boolean(),
                  "A dry run says so in what it writes");
            f.answer("setup:network", R"({"ok":true,"demo":true})");
            f.advance(500);
            check(f.screen() == "setup:ha" && f.transport.count("setup:state") == asked,
                  "and does not wait for a restart");
            f.press(Key::Down);
            f.press(Key::Ok);
            f.press(Key::Up);
            f.press(Key::Ok);
            check(f.transport.last("w:setup:standby")->body["demo"].boolean(), "Standby too");
            f.answerWrite("setup:standby", R"({"ok":true})");
            f.advance(500);
            f.press(Key::Ok);
            check(f.transport.last("w:setup:done")->body["demo"].boolean(), "and done");
        }

        // ---- Typing on the TV: mistakes, going back, the server's refusal -------------------------
        {
            Fixture f;
            f.toNetwork();
            f.press(Key::Down);
            f.press(Key::Ok);
            f.press(Key::Ok);
            f.answerWrite("setup:handoff", kHandoffReply);
            f.advance(500);
            f.pressDone(); // no address
            f.pressDone();
            f.pressDone();
            f.pressDone();
            check(f.app.snapshot()["field"].number() == 0 && f.app.shows("Enter the broker address.") &&
                      f.transport.last("setup:settings") == nullptr,
                  "Connect with no address says so and returns to the address");
            f.app.input("h");
            f.pressDone();
            f.app.key(Key::Back);
            f.advance(300);
            check(f.app.snapshot()["field"].number() == 0 && f.app.snapshot()["typed"].string() == "h" &&
                      f.screen() == "setup:handoff",
                  "At the port, Back returns to the address with what was typed");
            f.pressDone();
            f.app.input("");
            f.app.key(Key::Delete);
            f.advance(100);
            f.pressDone();
            f.pressDone();
            f.pressDone();
            check(f.transport.last("setup:settings") != nullptr, "A valid address goes to the server");
            f.answer("setup:settings", kSettings);
            f.answerWrite("setup:save", R"({"ok":false,"error":"controls disabled in config"})");
            f.advance(300);
            check(f.app.snapshot()["link"].string() == "ready" &&
                      f.app.shows("controls disabled in config"),
                  "A refusal is shown and the form is back");
        }

        // ---- Pointer and hardware keys --------------------------------------------------------------
        {
            Fixture f;
            f.toNetwork();
            check(hasHit(f.app.frame(), "setup:row:0") && hasHit(f.app.frame(), "setup:row:1"),
                  "Each choice is a hit");
            f.app.activate("setup:row:1");
            f.advance(300);
            check(f.app.snapshot()["selected"].number() == 1, "A first click focuses a choice");
            f.app.activate("setup:row:1");
            f.advance(500);
            check(f.screen() == "setup:ha", "and a second chooses it");
            f.app.activate("setup:row:0");
            f.advance(300);
            f.app.activate("setup:row:0");
            f.advance(500);
            f.answerWrite("setup:handoff", kHandoffReply);
            f.advance(500);
            check(hasHit(f.app.frame(), "setup:kb:1:2"), "Each key is a hit");
            f.app.activate("setup:kb:1:2");
            check(f.app.snapshot()["typed"].string() == "e", "A click on a key types it");
        }

        // ---- The splash ----------------------------------------------------------------------------
        {
            Transport transport;
            Application app(transport, "splash");
            app.update(1000);
            const Scene &scene = app.frame();
            check(app.screenName() == "splash" && scene.nodes.size() <= 8,
                  "The splash is a handful of nodes, not the seventy-two dots it was");
            check(scene.background.r == kit::color::ink.r && scene.background.b == kit::color::ink.b,
                  "on Home's ink");
            check(transport.requests.empty(), "It asks the server for nothing");
        }
    } catch (const std::exception &error) {
        std::fprintf(stderr, "setup_screen: %s\n", error.what());
        return 1;
    }
    std::printf("setup_screen: %d checks passed\n", checks);
    return 0;
}
