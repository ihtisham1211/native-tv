// First-run setup against a server over HTTP: the real client adapter and application, driven by
// keys, talking to tests/setup_fixture.py over a data root that starts wiped. setup_flow.py runs
// it and then checks what the server wrote.
//
//   native-setup-flow-tests ORIGIN                 the whole first run, welcome to dashboard
//   native-setup-flow-tests ORIGIN SCREEN EXPECTED opens the client on SCREEN as a launch would
//                                                  and checks where it settles: EXPECTED is a
//                                                  screen name, or "!setup:" for any other
//   native-setup-flow-tests ORIGIN real            the first run against the real daemon, as far
//                                                  as the broker's Connect (setup_daemon.py)
//   native-setup-flow-tests ORIGIN finish          the summary's Open dashboard
#include "adapters/http_services.hpp"
#include "ui/application.hpp"
#include <chrono>
#include <cstdio>
#include <functional>
#include <stdexcept>
#include <thread>

using namespace nativetv;
namespace {
using Clock = std::chrono::steady_clock;
struct Driver {
    HttpServices http;
    Application app;
    Clock::time_point start = Clock::now();
    explicit Driver(const std::string &origin, const std::string &screen = "home")
        : http(origin), app(http, screen) {
        app.holdForSetupCheck(); // as the client does
    }
    uint32_t now() const {
        return static_cast<uint32_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count());
    }
    // One frame: the server's answers in, the clock forward.
    void frame() {
        for (Response &reply : http.drain())
            if (reply.channel.compare(0, 6, "image:") != 0)
                app.response(reply);
        app.update(now());
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
    }
    void pause(int ms) {
        auto until = Clock::now() + std::chrono::milliseconds(ms);
        while (Clock::now() < until)
            frame();
    }
    void until(const std::function<bool()> &ready, const char *what, int limitMs = 30000) {
        auto limit = Clock::now() + std::chrono::milliseconds(limitMs);
        while (!ready()) {
            if (Clock::now() > limit)
                throw std::runtime_error(std::string("Timed out waiting for ") + what + "; on " +
                                         app.screenName() + " " + app.snapshot().dump());
            frame();
        }
    }
    void press(Key key) {
        app.key(key);
        pause(60);
    }
    void screen(const char *name) {
        until([&] { return app.screenName() == name && app.snapshot()["loaded"].boolean(); }, name);
        pause(500); // the page has finished arriving
    }
    // Down to the last row, along it to Done, OK: the next field, or Connect on the last.
    void done() {
        for (int i = 0; i < 3; ++i)
            press(Key::Down);
        for (int i = 0; i < 4; ++i)
            press(Key::Right);
        press(Key::Ok);
    }
};

// A launch: where does the client end up, and does it stay there once the cover is gone?
int launch(const std::string &origin, const std::string &screen, const std::string &expected) {
    Driver tv(origin, screen);
    const bool anyOther = expected == "!setup:";
    auto settled = [&] {
        const std::string name = tv.app.screenName();
        return anyOther ? name.compare(0, 6, "setup:") != 0 : name == expected;
    };
    tv.until([&] { return tv.app.snapshot().has("screen"); }, "the first screen");
    tv.pause(3000); // past the 2.5 s the client waits for the server's word on setup
    if (!settled())
        throw std::runtime_error("Launched on " + screen + " the client is on " + tv.app.screenName() +
                                 ", not " + expected);
    std::printf("setup_flow: %s opens %s\n", screen.c_str(), tv.app.screenName().c_str());
    return 0;
}

// The real daemon: the TV is whatever its stand-in luna says (an OLED65B3PUA), nothing restarts
// (the restart is a script that notes it), and Always-on is not asked: the run ends at Connect.
int real(const std::string &origin) {
    Driver tv(origin);
    tv.screen("setup:welcome");
    tv.press(Key::Ok);
    tv.screen("setup:name");
    if (tv.app.snapshot()["typed"].string() != "B3")
        throw std::runtime_error("The name starts as " + tv.app.snapshot()["typed"].string());
    tv.press(Key::Delete);
    tv.press(Key::Delete);
    tv.app.input("Den");
    tv.done();
    tv.screen("setup:network");
    tv.press(Key::Ok); // open: the daemon writes config.json and asks for a restart
    tv.screen("setup:ha");
    tv.press(Key::Ok);
    tv.until([&] { return tv.app.snapshot()["link"].string() == "ready"; }, "the phone link");
    tv.pause(500);
    tv.app.input("192.168.1.10");
    tv.done();
    tv.done();
    tv.app.input("lg");
    tv.done();
    tv.app.input("secret");
    tv.done();
    tv.until([&] { return tv.app.snapshot()["link"].string() == "sending"; }, "the broker being saved");
    tv.pause(5000); // the settings are read, written and the restart asked for
    if (tv.app.snapshot()["link"].string() == "ready")
        throw std::runtime_error("The daemon refused the broker: the form is back");
    std::puts("setup_flow: the broker went to the real daemon");
    return 0;
}
int finish(const std::string &origin) {
    Driver tv(origin, "setup:summary");
    tv.screen("setup:summary");
    tv.press(Key::Ok);
    tv.until([&] { return tv.app.screenName() != "setup:summary"; }, "the dashboard");
    tv.pause(1500); // the done write is answered
    std::puts("setup_flow: setup was marked done");
    return 0;
}
} // namespace

int main(int argc, char **argv) {
    try {
        if (argc == 4)
            return launch(argv[1], argv[2], argv[3]);
        if (argc == 3 && std::string(argv[2]) == "real")
            return real(argv[1]);
        if (argc == 3 && std::string(argv[2]) == "finish")
            return finish(argv[1]);
        if (argc != 2)
            throw std::runtime_error("The fixture's origin, and optionally a screen and where it ends");
        Driver tv(argv[1]);
        tv.until([&] { return tv.app.screenName() == "setup:welcome"; }, "the first-run page");
        tv.screen("setup:welcome");
        tv.press(Key::Ok);
        tv.screen("setup:name"); // "B3": the stand-in TV is an OLED65B3PUA
        if (tv.app.snapshot()["typed"].string() != "B3")
            throw std::runtime_error("The name starts as " + tv.app.snapshot()["typed"].string());
        tv.press(Key::Delete);
        tv.press(Key::Delete);
        tv.app.input("Den");
        tv.done();
        tv.screen("setup:network");
        tv.press(Key::Ok); // yes, on phones and computers at home: the server restarts
        tv.screen("setup:ha");
        tv.press(Key::Ok); // on my phone
        tv.until([&] { return tv.app.snapshot()["link"].string() == "ready"; }, "the phone link");
        tv.pause(500);
        tv.app.input("192.168.1.10");
        tv.done();
        tv.done(); // port 1883
        tv.app.input("lg");
        tv.done();
        tv.app.input("secret");
        tv.done();
        tv.screen("setup:standby");
        tv.press(Key::Up);
        tv.press(Key::Ok); // keep it connected when off
        tv.screen("setup:summary");
        tv.press(Key::Ok); // open the dashboard
        tv.until([&] { return tv.app.screenName() == "dashboard:system"; }, "the dashboard");
        std::puts("setup_flow: the client went from the welcome page to the dashboard");
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "setup_flow: %s\n", error.what());
        return 1;
    }
}
