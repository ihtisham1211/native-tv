#include "service/config_settings.hpp"
#include <cassert>
#include <iostream>
#include <string>
#include <vector>

using namespace nativetv;
using namespace nativetv::service;

int main() {
    Json input = Json::parse(R"json({"mqtt":{"enabled":true,"host":"  broker.local  ",
        "port":"8883tail","tls":1,"tlsRejectUnauthorized":false,
        "username":" user ","topicPrefix":" house ",
        "discoveryPrefix":"ha","telemetryIntervalMs":"10000.9ms",
        "entities":{"controls":false,"disabled":["cpu.temp","bad/id",5]}},
        "device":{"id":" tv_room ","name":" Living Room "},
        "iconServer":true,"token":"must-not-write","allowControl":false})json");
    Json patch;
    std::vector<std::string> errors;
    assert(validateDashboardSettings(input, patch, errors));
    assert(errors.empty());
    assert(patch["mqtt"]["enabled"].boolean());
    assert(patch["mqtt"]["host"].string() == "broker.local");
    assert(patch["mqtt"]["port"].number() == 8883);
    assert(patch["mqtt"]["tls"].boolean());
    assert(!patch["mqtt"]["tlsRejectUnauthorized"].boolean());
    assert(patch["mqtt"]["username"].string() == "user");
    assert(patch["mqtt"]["topicPrefix"].string() == "house");
    assert(patch["mqtt"]["telemetryIntervalMs"].number() == 10000);
    assert(!patch["mqtt"]["entities"]["controls"].boolean());
    assert(patch["mqtt"]["entities"]["disabled"].size() == 1);
    assert(patch["mqtt"]["entities"]["disabled"][0].string() == "cpu.temp");
    assert(patch["device"]["id"].string() == "tv_room");
    assert(patch["device"]["name"].string() == "Living Room");
    assert(patch["iconServer"].boolean());
    assert(!patch.has("token") && !patch.has("allowControl"));
    assert(!patch["mqtt"].has("password"));

    input = Json::parse(R"json({"mqtt":{"port":null,"telemetryIntervalMs":10000,
        "password":""},"device":{"id":"tv_1"}})json");
    assert(validateDashboardSettings(input, patch, errors));
    assert(!patch["mqtt"]["port"].valid());
    assert(patch["mqtt"].has("password") && patch["mqtt"]["password"].string().empty());
    assert(patch["mqtt"]["tlsRejectUnauthorized"].boolean());
    assert(patch["mqtt"]["entities"]["oled"].boolean());
    assert(!patch.has("iconServer"));

    input = Json::parse(R"json({"mqtt":{"enabled":true,"port":65536,
        "topicPrefix":"bad/#","discoveryPrefix":"/bad",
        "telemetryIntervalMs":999},"device":{"id":"Upper Case"}})json");
    assert(!validateDashboardSettings(input, patch, errors));
    assert((errors == std::vector<std::string>{
        "a broker address is required to enable MQTT",
        "port must be between 1 and 65535",
        "topic prefix cannot contain +, # or spaces, or start or end with /",
        "discovery prefix cannot contain +, # or spaces, or start or end with /",
        "telemetry interval must be between 1000 and 600000 ms",
        "device id must be 1-64 characters of a-z, 0-9 or _"}));

    // test/test-routes.js validateSettings: what an empty form owes, and the defaults.
    errors.clear();
    assert(!validateDashboardSettings(Json::parse("{}"), patch, errors));
    assert(errors.size() == 2);
    errors.clear();
    assert(validateDashboardSettings(Json::parse(
        R"json({"mqtt":{"telemetryIntervalMs":10000},"device":{"id":"lg_living_room"}})json"),
        patch, errors));
    assert(errors.empty());
    assert(patch["mqtt"].has("enabled") && !patch["mqtt"]["enabled"].boolean());
    assert(patch["mqtt"]["topicPrefix"].string() == "lgtv");
    assert(patch["mqtt"]["discoveryPrefix"].string() == "homeassistant");
    assert(patch["device"]["id"].string() == "lg_living_room");
    errors.clear();
    assert(!validateDashboardSettings(Json::parse(
        R"json({"mqtt":{"enabled":true,"host":"","telemetryIntervalMs":10000},"device":{"id":"lg_tv"}})json"),
        patch, errors));
    assert(errors.size() >= 1 && errors[0].find("broker address is required") != std::string::npos);
    errors.clear();
    assert(validateDashboardSettings(Json::parse(
        R"json({"mqtt":{"port":1883,"topicPrefix":"valid_prefix","telemetryIntervalMs":10000},
                "device":{"id":"lg_tv"}})json"), patch, errors));
    assert(patch["mqtt"]["port"].number() == 1883 && patch["mqtt"]["topicPrefix"].string() == "valid_prefix");
    errors.clear();
    assert(!validateDashboardSettings(Json::parse(
        R"json({"mqtt":{"topicPrefix":"invalid/prefix/","telemetryIntervalMs":10000},"device":{"id":"lg_tv"}})json"),
        patch, errors));
    errors.clear();
    assert(!validateDashboardSettings(Json::parse(
        R"json({"mqtt":{"telemetryIntervalMs":10000},"device":{"id":"bad ID with spaces!"}})json"),
        patch, errors));
    // The icon server switch is saved only as true or false.
    for (const char *given : {"true", "false"}) {
        errors.clear();
        assert(validateDashboardSettings(Json::parse(std::string(
            R"json({"mqtt":{"telemetryIntervalMs":10000},"device":{"id":"lg_tv"},"iconServer":)json") + given + "}"),
            patch, errors));
        assert(patch.has("iconServer") && patch["iconServer"].boolean() == (std::string(given) == "true"));
    }
    errors.clear();
    assert(validateDashboardSettings(Json::parse(
        R"json({"mqtt":{"telemetryIntervalMs":10000},"device":{"id":"lg_tv"},"iconServer":"yes"})json"),
        patch, errors));
    assert(!patch.has("iconServer"));

    std::cout << "Dashboard settings validation passed.\n";
    return 0;
}
