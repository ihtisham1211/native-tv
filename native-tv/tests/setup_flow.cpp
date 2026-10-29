// First-run setup against a server over HTTP: the real client adapter and application, driven by
// keys, talking to tests/setup_fixture.py over a data root that starts wiped. setup_flow.py runs
// it and then checks what the server wrote.
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
    explicit Driver(const std::string &origin) : http(origin), app(http, "home") {}
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
} // namespace

int main(int argc, char **argv) {
    try {
        if (argc != 2)
            throw std::runtime_error("The fixture's origin is the one argument");
        Driver tv(argv[1]);
        tv.until([&] { return tv.app.screenName() == "setup:welcome"; }, "the first-run page");
        tv.screen("setup:welcome");
        tv.press(Key::Ok);
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
