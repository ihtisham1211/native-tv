#include "daemon/mqtt_command_router.hpp"
#include <cstdio>
#include <stdexcept>
#include <string>

using nativetv::Json;
using nativetv::daemon::MqttControlCommand;
using nativetv::daemon::routeMqttControlTopic;

namespace {
void check(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}

MqttControlCommand route(const std::string &action, const std::string &payload,
                         const Json &rows = Json::array()) {
    return routeMqttControlTopic("tv_native/command/" + action,
                                 "tv_native/command/", payload, rows);
}

void testSimpleCommandMappings() {
    MqttControlCommand command = route("piccap/power", " ON ");
    check(command.valid && command.action == "piccap" &&
          command.payload["value"].string() == "ON",
          "PicCap topic did not map to the native PicCap control");

    command = route("rcu", "  ArrowUp  ");
    check(command.valid && command.action == "rcu" &&
          command.payload["value"].string() == "ArrowUp" &&
          !command.refreshTelemetry,
          "remote-key command did not retain its payload or telemetry behavior");

    command = route("screen", " off ");
    check(command.valid && command.action == "screenOff" &&
          !command.payload["value"].valid(),
          "screen OFF did not map to a value-less screenOff action");
    command = route("screen", "standby");
    check(command.valid && command.action == "screenOn",
          "non-OFF screen command did not map to screenOn");

    for (const std::string action : {"reboot", "powerOff", "powerOn"}) {
        command = route(action, "ignored");
        check(command.valid && command.action == action &&
              !command.payload["value"].valid() && !command.refreshTelemetry,
              "power command did not map to a value-less action");
    }

    command = route("mute", " on ");
    check(command.valid && command.payload["value"].isBoolean() &&
          command.payload["value"].boolean() && command.refreshTelemetry,
          "mute command did not use the legacy ON comparison");
    command = route("mute", "true");
    check(command.valid && !command.payload["value"].boolean(),
          "mute command accepted a value other than ON");

    command = route("volume", " 37.9 trailing");
    check(command.valid && command.payload["value"].number() == 37,
          "volume did not match legacy parseInt behavior");
    command = route("volume", "not a number");
    check(command.valid && command.payload["value"].number() == 10,
          "invalid volume did not use the legacy default");

    command = route("input", " HDMI  1 \t ");
    check(command.valid && command.payload["value"].string() == "hdmi1",
          "input command did not lowercase and remove whitespace");
}

void testTimerAndLgSettingsMappings() {
    MqttControlCommand command = route("powerTimer/on", "ON");
    check(command.valid && command.action == "powerTimer" &&
          command.payload["value"]["timer"].string() == "on" &&
          command.payload["value"]["enabled"].boolean(),
          "On Timer topic was not converted to a powerTimer control");
    command = route("powerTimer/off", "true");
    check(command.valid && !command.payload["value"]["enabled"].boolean(),
          "Off Timer accepted a value other than ON");

    Json rows = Json::array();
    rows.add(Json::parse(R"({"id":"enabled","type":"switch","on":false})"));
    rows.add(Json::parse(R"({"id":"level","type":"number","value":4})"));
    rows.add(Json::parse(R"({"id":"mode","type":"choice","value":"standard"})"));

    command = route("lgs/enabled", "ON", rows);
    check(command.valid && command.action == "lgSetting" &&
          command.payload["value"]["id"].string() == "enabled" &&
          command.payload["value"]["on"].boolean(),
          "LG switch topic did not create the expected setting value");
    command = route("lgs/level", "8more", rows);
    check(command.valid && command.payload["value"]["value"].number() == 8,
          "LG number topic did not use parseInt");
    command = route("lgs/level", "invalid", rows);
    check(command.valid && command.payload["value"]["value"].number() == 4,
          "LG number topic did not fall back to its current value");
    command = route("lgs/mode", "cinema", rows);
    check(command.valid && command.payload["value"]["value"].string() == "cinema",
          "LG choice topic did not retain its string value");
    check(!route("lgs/unknown", "anything", rows).valid,
          "unknown LG setting topic was not ignored");
}

void testOtherCommandsAndTopicBounds() {
    MqttControlCommand command = route("toast", "  Hello TV  ");
    check(command.valid && command.payload["value"].string() == "Hello TV" &&
          !command.refreshTelemetry,
          "toast command did not preserve the trimmed text");
    command = route("refresher", " Schedule ");
    check(command.valid && command.action == "refresherSchedule" &&
          !command.payload["value"].valid() && command.refreshTelemetry,
          "refresher schedule topic was not mapped");
    command = route("refresher", "cancel");
    check(command.valid && command.action == "refresherCancel",
          "refresher cancel topic was not mapped");

    command = route("customAction", " raw payload ");
    check(command.valid && command.action == "customAction" &&
          command.payload["value"].string() == "raw payload",
          "unknown legacy action did not pass through to control routing");

    check(!routeMqttControlTopic("other/command/volume", "tv/command/", "20").valid,
          "a command outside the configured topic prefix was accepted");
    check(!routeMqttControlTopic("tv/command/", "tv/command/", "20").valid,
          "an empty command suffix was accepted");
    check(!routeMqttControlTopic("tv/command/volume", "tv/command/",
                                 std::string(16385, '1')).valid,
          "an oversized control payload was accepted");
}
} // namespace

int main() {
    try {
        testSimpleCommandMappings();
        testTimerAndLgSettingsMappings();
        testOtherCommandsAndTopicBounds();
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "native MQTT command router test failed: %s\n", error.what());
        return 1;
    }
}
