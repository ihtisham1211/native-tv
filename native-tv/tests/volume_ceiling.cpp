#include "service/volume_ceiling.hpp"
#include <iostream>
#include <string>

using namespace nativetv;

namespace {
int failures = 0;

void check(bool condition, const std::string &message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

// Milliseconds since local midnight for a clock time.
int64_t at(int hour, int minute) { return (hour * 60 + minute) * 60000LL; }

Json settings(const std::string &stored) {
    return service::volumeCeilingSettings(Json::parse("{\"volumeCeiling\":" + stored + "}"));
}
} // namespace

int main() {
    Json value;
    std::string error;
    check(service::validateVolumeCeilingPatch(
              Json::parse("{\"enabled\":true,\"maxVolume\":40,\"nightMaxVolume\":0,"
                          "\"nightStart\":\"22:00\",\"nightEnd\":\"07:30\",\"startupVolume\":-1}"),
              value, error) &&
              value["maxVolume"].number() == 40 && value["nightMaxVolume"].number() == 0 &&
              value["nightEnd"].string() == "07:30" && error.empty(),
          "valid settings are copied, including 0 and no start-up volume");
    check(service::validateVolumeCeilingPatch(Json::parse("{\"nightStart\":\"\"}"), value, error),
          "an empty night time clears the window");
    for (const char *invalid : {"{\"maxVolume\":101}", "{\"maxVolume\":-1}", "{\"maxVolume\":1.5}",
                                "{\"maxVolume\":\"40\"}", "{\"nightMaxVolume\":null}",
                                "{\"startupVolume\":-2}", "{\"startupVolume\":101}",
                                "{\"enabled\":1}"}) {
        const std::string key = Json::parse(invalid).keys()[0];
        check(!service::validateVolumeCeilingPatch(Json::parse(invalid), value, error) &&
                  error == "invalid " + key,
              std::string(invalid) + " is rejected as invalid " + key);
    }
    for (const char *invalid : {"{\"nightStart\":\"24:00\"}", "{\"nightEnd\":\"7:30\"}",
                                "{\"nightStart\":\"22:60\"}", "{\"nightEnd\":2200}"})
        check(!service::validateVolumeCeilingPatch(Json::parse(invalid), value, error),
              std::string(invalid) + " is rejected");
    check(!service::validateVolumeCeilingPatch(Json::parse("{\"loud\":true}"), value, error) &&
              error == "unknown setting: loud",
          "an unknown setting is named");
    check(!service::validateVolumeCeilingPatch(Json::parse("[]"), value, error) &&
              error == "no settings",
          "a patch that is not an object is refused");

    // Defaults, and a stored value that is no longer valid falling back to its default.
    const Json defaults = settings("{}");
    check(!defaults["enabled"].boolean() && defaults["maxVolume"].number() == 100 &&
              defaults["nightMaxVolume"].number() == 100 && defaults["startupVolume"].number() == -1 &&
              defaults["nightStart"].string().empty() && defaults["nightEnd"].string().empty(),
          "the defaults leave the volume alone");
    check(service::volumeCeilingSettings(Json::parse("{}"))["maxVolume"].number() == 100,
          "a config without the section gives the defaults");
    check(settings("{\"maxVolume\":55,\"nightMaxVolume\":200,\"nightStart\":\"99:99\"}")
                  ["maxVolume"].number() == 55 &&
              settings("{\"maxVolume\":55,\"nightMaxVolume\":200}")["nightMaxVolume"].number() == 100 &&
              settings("{\"nightStart\":\"99:99\"}")["nightStart"].string().empty(),
          "a stored value that is not valid falls back to its default");

    // The ceiling.
    check(service::volumeCeilingLimit(settings("{\"maxVolume\":30}"), at(12, 0)) == 100,
          "nothing is held while the ceiling is off");
    const Json day = settings("{\"enabled\":true,\"maxVolume\":60}");
    check(service::volumeCeilingLimit(day, at(12, 0)) == 60 &&
              service::volumeCeilingLimit(day, -1) == 60,
          "the maximum holds at any time, and with the clock unset");
    // A night window that runs past midnight.
    const Json night = settings("{\"enabled\":true,\"maxVolume\":60,\"nightMaxVolume\":20,"
                                "\"nightStart\":\"22:00\",\"nightEnd\":\"07:00\"}");
    check(service::volumeCeilingLimit(night, at(21, 59)) == 60 &&
              service::volumeCeilingLimit(night, at(22, 0)) == 20 &&
              service::volumeCeilingLimit(night, at(23, 59)) == 20 &&
              service::volumeCeilingLimit(night, at(0, 0)) == 20 &&
              service::volumeCeilingLimit(night, at(6, 59)) == 20 &&
              service::volumeCeilingLimit(night, at(7, 0)) == 60,
          "the night limit holds from the start to the end, across midnight");
    check(service::volumeCeilingLimit(night, -1) == 60,
          "the night window is left out while the clock is unset");
    // A night window inside one day, and a night limit above the maximum.
    const Json nap = settings("{\"enabled\":true,\"maxVolume\":60,\"nightMaxVolume\":10,"
                              "\"nightStart\":\"13:00\",\"nightEnd\":\"15:00\"}");
    check(service::volumeCeilingLimit(nap, at(12, 59)) == 60 &&
              service::volumeCeilingLimit(nap, at(13, 0)) == 10 &&
              service::volumeCeilingLimit(nap, at(15, 0)) == 60,
          "a window within one day holds from its start up to its end");
    check(service::volumeCeilingLimit(
              settings("{\"enabled\":true,\"maxVolume\":30,\"nightMaxVolume\":80,"
                       "\"nightStart\":\"22:00\",\"nightEnd\":\"07:00\"}"), at(23, 0)) == 30,
          "a night limit above the maximum never raises it");
    check(service::volumeCeilingLimit(
              settings("{\"enabled\":true,\"maxVolume\":60,\"nightMaxVolume\":5,"
                       "\"nightStart\":\"22:00\"}"), at(23, 0)) == 60 &&
              service::volumeCeilingLimit(
                  settings("{\"enabled\":true,\"maxVolume\":60,\"nightMaxVolume\":5,"
                           "\"nightStart\":\"08:00\",\"nightEnd\":\"08:00\"}"), at(8, 0)) == 60,
          "half a window, or one of no length, is no window");

    // The start-up volume.
    check(service::volumeCeilingStartup(settings("{\"startupVolume\":15}"), at(12, 0)) == -1 &&
              service::volumeCeilingStartup(day, at(12, 0)) == -1,
          "no start-up volume while off or unset");
    check(service::volumeCeilingStartup(
              settings("{\"enabled\":true,\"startupVolume\":15}"), at(12, 0)) == 15 &&
              service::volumeCeilingStartup(
                  settings("{\"enabled\":true,\"startupVolume\":0}"), at(12, 0)) == 0,
          "the start-up volume is set as chosen, 0 included");
    check(service::volumeCeilingStartup(
              settings("{\"enabled\":true,\"maxVolume\":25,\"startupVolume\":40}"), at(12, 0)) == 25 &&
              service::volumeCeilingStartup(
                  settings("{\"enabled\":true,\"maxVolume\":60,\"nightMaxVolume\":10,"
                           "\"nightStart\":\"22:00\",\"nightEnd\":\"07:00\",\"startupVolume\":40}"),
                  at(23, 0)) == 10,
          "the start-up volume is held to the ceiling, the night one at night");

    if (failures) {
        std::cerr << failures << " volume ceiling check(s) failed\n";
        return 1;
    }
    std::cout << "volume ceiling checks passed\n";
    return 0;
}
