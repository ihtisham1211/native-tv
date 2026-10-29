#include "daemon/mqtt_publish_payload.hpp"
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <functional>
#include <sstream>
#include <stdexcept>
#include <vector>

using namespace nativetv;
using namespace nativetv::daemon;

namespace {
// Key order differs between writers; compare the sorted forms, numbers as the reader sees them.
std::string canon(const Json &v) {
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
}
} // namespace

int main(int argc, char **argv) {
    try {
        if (argc < 2) throw std::runtime_error("fixture path expected");
        std::ifstream file(argv[1]);
        std::stringstream contents;
        contents << file.rdbuf();
        const Json fixture = Json::parse(contents.str());
        if (!fixture["cases"].isArray() || fixture["cases"].size() == 0)
            throw std::runtime_error("no cases");
        for (const Json &c : fixture["cases"].items()) {
            const Json &o = c["options"];
            MqttPublishOptions options;
            options.lgRows = o["lgRows"];
            options.tvOff = o["tvOff"].boolean();
            options.watch = o["watch"];
            options.piccapState = o["piccapState"];
            options.lastApp = o["lastApp"].string();
            options.lastAppId = o["lastAppId"].string();
            const Json actual = mqttPublishPayload(c["stats"], options);
            if (canon(actual) != canon(c["expected"]))
                throw std::runtime_error(c["name"].string() + ": got " + actual.dump() +
                                         " expected " + c["expected"].dump());
        }
        std::printf("mqtt publish payload matches Node on %zu cases\n", fixture["cases"].size());
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
