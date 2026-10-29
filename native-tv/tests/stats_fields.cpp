#include "service/stats_fields.hpp"
#include <algorithm>
#include <cstdio>
#include <functional>
#include <fstream>
#include <sstream>
#include <stdexcept>

using namespace nativetv;
using namespace nativetv::service;

namespace {
void check(bool value, const std::string &message) {
    if (!value) throw std::runtime_error(message);
}

void same(const Json &actual, const Json &expected, const std::string &what) {
    // Compare as JSON text with keys in a fixed order; numbers as the reader sees them.
    const Json a = Json::parse(actual.valid() ? actual.dump() : "null");
    const Json b = Json::parse(expected.valid() ? expected.dump() : "null");
    check([&]() {
        // Key order differs between writers; compare the sorted forms.
        std::function<std::string(const Json &)> canon = [&](const Json &v) -> std::string {
            if (v.isObject()) {
                std::vector<std::string> keys = v.keys();
                std::sort(keys.begin(), keys.end());
                std::string text = "{";
                for (const std::string &key : keys) text += "\"" + key + "\":" + canon(v[key]) + ",";
                return text + "}";
            }
            if (v.isArray()) {
                std::string text = "[";
                for (const Json &item : v.items()) text += canon(item) + ",";
                return text + "]";
            }
            if (v.isNumber()) {
                char text[64];
                snprintf(text, sizeof(text), "%.9g", v.number());
                return text;
            }
            return v.valid() ? v.dump() : "null";
        };
        return canon(a) == canon(b);
    }(), what + ": got " + a.dump() + " expected " + b.dump());
}
} // namespace

int main(int argc, char **argv) {
    try {
        check(argc > 1, "fixture path expected");
        std::ifstream file(argv[1]);
        std::stringstream buffer;
        buffer << file.rdbuf();
        const Json cases = Json::parse(buffer.str());
        check(cases.isObject(), "fixture is not an object");

        size_t n = 0;
        for (const Json &c : cases["signalFormat"].items())
            same(statsSignalFormat(c["input"]), c["expected"], "signalFormat #" + std::to_string(n++));
        n = 0;
        for (const Json &c : cases["volumeControl"].items())
            check(statsVolumeControl(c["vs"], c["sound"]) == c["expected"].string(),
                  "volumeControl #" + std::to_string(n++));
        n = 0;
        for (const Json &c : cases["offeredSoundOutputs"].items()) {
            Json got = Json::array();
            for (const std::string &id : statsOfferedSoundOutputs(c["values"], c["cfg"], c["current"].string()))
                got.add(Json::text(id));
            same(got, c["expected"], "offeredSoundOutputs #" + std::to_string(n++));
        }
        n = 0;
        for (const Json &c : cases["timers"].items())
            same(statsTimersFromSettings(c["settings"]), c["expected"], "timers #" + std::to_string(n++));
        n = 0;
        for (const Json &c : cases["appList"].items()) {
            Json titles = Json::object();
            same(statsAppList(c["raw"], titles), c["expected"], "appList #" + std::to_string(n));
            same(titles, c["titles"], "appList titles #" + std::to_string(n++));
        }
        check(statsClockTime(Json::text("1"), Json::text("0")) == "01:00", "clock time pads");
        check(statsClockTime(Json::text("x"), Json()) == "00:00", "clock time falls back");

        // Where the sound goes, from the two services.
        const Json vs = Json::parse("{\"soundOutput\":\"external_arc\"}");
        const Json sound = Json::parse("{\"scenario\":\"mastervolume_tv_speaker\"}");
        check(statsSoundRoute(vs, sound) == "external_arc", "route prefers the newer service");
        check(statsSoundRoute(Json(), sound) == "mastervolume_tv_speaker", "route falls back");
        check(statsSoundRoute(Json(), Json()).empty(), "no route");
        check(statsAudioOutput(Json(), sound) != "Internal", "scenario names an output");
        check(statsAudioOutput(Json(), Json()) == "Internal", "no output is Internal");
    } catch (const std::exception &error) {
        fprintf(stderr, "stats fields test failed: %s\n", error.what());
        return 1;
    }
    return 0;
}
