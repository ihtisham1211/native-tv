// What the tests of the Quick drawer, Profiles, title details and the editors share: an
// Application over a fake Services whose replies the test sends (late, out of order, refused), the
// frame clock, and Home's data.
#pragma once
#include "ui/application.hpp"
#include "ui/kit/overlay.hpp"
#include <cmath>
#include <cstdio>
#include <deque>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

using namespace nativetv;
namespace overlay_test {
inline int &checkCount() {
    static int checks = 0;
    return checks;
}
inline void check(bool condition, const char *message) {
    ++checkCount();
    if (!condition)
        throw std::runtime_error(message);
}
struct FakeTransport final : Services {
    struct Request {
        std::string channel;
        Operation operation;
        Json body;
        bool write;
        unsigned generation;
    };
    std::vector<Request> requests;
    std::map<std::string, unsigned> revisions;
    bool refuse = false; // a full queue: the request is not taken
    unsigned read(const std::string &channel, Operation operation, const Json &arguments = Json()) override {
        if (refuse)
            return 0;
        requests.push_back({channel, operation, arguments, false, ++revisions[channel]});
        return revisions[channel];
    }
    unsigned write(const std::string &channel, Operation operation, const Json &body) override {
        if (refuse)
            return 0;
        requests.push_back({channel, operation, body, true, ++revisions[channel]});
        return revisions[channel];
    }
    void cancel(const std::string &channel) override {
        ++revisions[channel];
    }
    size_t count(const std::string &channel) const {
        size_t total = 0;
        for (const auto &request : requests)
            if (request.channel == channel)
                ++total;
        return total;
    }
    const Request &last(const std::string &channel) const {
        for (size_t i = requests.size(); i-- > 0;)
            if (requests[i].channel == channel)
                return requests[i];
        throw std::runtime_error("no request on " + channel);
    }
};
inline uint32_t run(Application &app, uint32_t from, uint32_t ms) {
    uint32_t at = from;
    for (uint32_t step = 0; step <= ms * 60 / 1000; ++step) {
        at = from + step * 1000 / 60;
        app.update(at);
    }
    return at;
}
inline bool hasHit(const Scene &scene, const std::string &id) {
    for (const auto &hit : scene.hits)
        if (hit.id == id)
            return true;
    return false;
}

static const char *const catalogueJson =
    R"({"ok":true,"apps":[{"id":"a","title":"Alpha"}],"devices":[{"id":"hdmi1","name":"Console","port":1},{"id":"hdmi2","name":"Stick","port":2,"unused":true}],"channels":[],"order":{"app":["a"],"dev":["hdmi1","hdmi2"]},"layout":{"rows":["app","dev"],"hidden":{},"showInputs":true,"clockFormat":"12h"},"profiles":[{"id":"main","name":"Main","active":true},{"id":"guest","name":"Guest","active":false}]})";
static const char *const feedJson =
    R"({"ok":true,"enabled":true,"titlesEnabled":true,"rows":[{"key":"trending","items":[{"id":7,"type":"movie","title":"Coast","overview":"A journey."},{"id":8,"type":"tv","title":"Harbor"},{"id":9,"type":"movie","title":"Orbit"}]}]})";
static const char *const statsJson =
    R"({"ok":true,"volume":24,"muted":false,"oled":{"care":true},"sleepTimer":"off","picture":{"mode":"Cinema","mode_raw":"cinema","backlight":70,"modes":[{"value":"vivid","label":"Vivid"},{"value":"cinema","label":"Cinema"},{"value":"game","label":"Game"}]},"sound":{"output":"TV Speaker","output_raw":"tv_speaker","outputs":[{"id":"tv_speaker","name":"TV Speaker"},{"id":"arc","name":"HDMI ARC"}]}})";
static const char *const detailsJson =
    R"({"ok":true,"enabled":true,"netflixEnabled":true,"item":{"id":7,"type":"movie","title":"Coast","overview":"A journey across the water.","runtime":121,"trailer":"dQw4w9WgXcQ","cast":[{"name":"Ada"},{"name":"Ben"}],"similar":[{"id":20,"type":"movie","title":"Horizon"},{"id":21,"type":"tv","title":"Tide"}]}})";

// An application with Home's data in, its entrance played and the clock at a known time.
struct World {
    FakeTransport transport;
    Application app;
    uint32_t clock = 0;
    explicit World(const char *catalogue = catalogueJson) : app(transport) {
        app.update(0);
        reply("catalogue", catalogue, 0);
        reply("discover", feedJson, 0);
        clock = run(app, 16, 1500);
    }
    // The answer to the latest request on a channel (generation 0 asks for none in particular).
    void reply(const std::string &channel, const std::string &json, long status = 200,
               int generation = -1) {
        Response response;
        response.channel = channel;
        response.status = status ? status : 200;
        response.bytes = json;
        response.generation = generation >= 0 ? static_cast<unsigned>(generation)
                                              : transport.revisions[channel];
        app.response(response);
    }
    void settle(uint32_t ms) {
        clock = run(app, clock, ms);
    }
    void press(Key key, uint32_t after = 0) {
        app.key(key);
        if (after)
            settle(after);
    }
    void press(std::initializer_list<Key> keys) {
        for (Key key : keys)
            press(key);
    }
    Json snapshot() {
        return app.snapshot();
    }
    std::string screen() {
        return app.screenName();
    }
    double motion(const std::string &id) {
        return app.animation().value(id);
    }
    // The toast is a tween: it shows after a few frames.
    bool toasted(const std::string &text) {
        settle(120);
        return app.shows(text);
    }
    void openQuick() {
        check(app.activate("tool:1"), "The gear is on the bar");
        reply("quick:stats", statsJson);
        reply("quick:now", R"({"ok":true,"kind":"input","port":1})");
        settle(900);
    }
    void openProfiles() {
        check(app.activate("tool:0"), "The avatar is on the bar");
        settle(500);
    }
    void openSearch() {
        check(app.activate("tool:2"), "The search button is on the bar");
        settle(500);
    }
    void openDetails(const char *answer = detailsJson) {
        press(Key::Ok);
        reply("title:details", answer);
        settle(500);
    }
    // Every request taken on a channel, oldest first.
    std::vector<FakeTransport::Request> on(const std::string &channel) const {
        std::vector<FakeTransport::Request> found;
        for (const auto &request : transport.requests)
            if (request.channel == channel)
                found.push_back(request);
        return found;
    }
    // The reply to one request, whenever the test says (the generation is the request's own).
    void answer(const FakeTransport::Request &request, bool ok = true, const std::string &error = "") {
        reply(request.channel,
              ok ? R"({"ok":true})" : std::string(R"({"ok":false,"error":")") + error + "\"}",
              ok ? 200 : 503, static_cast<int>(request.generation));
    }
};
} // namespace overlay_test

