#include "daemon/ha_discovery.hpp"
#include "daemon/ha_entity_catalogue.hpp"
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

using nativetv::Json;
using namespace nativetv::daemon;

namespace {
void check(bool condition, const std::string &message) {
    if (condition)
        return;
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
}

Json encodedBatch(const HaDiscoveryBatch &batch) {
    Json out = Json::array();
    for (const HaDiscoveryMessage &message : batch.publish) {
        Json item = Json::object();
        item.set("topic", Json::text(message.topic));
        item.set("payload", Json::text(message.payload));
        out.add(item);
    }
    return out;
}

Json fullCapabilities() {
    return Json::parse(
        "{\"hasRemoteInfo\":true,\"hasPnwash\":true,\"hasCell\":true,"
        "\"hasHdmiProc\":true,\"hasMediaState\":true,\"hasSoundFormat\":true,"
        "\"hasHdrStatus\":true,\"socArch\":\"aarch64\",\"hasLogoLight\":true,"
        "\"hasOnTimer\":true,\"hasOffTimer\":true,\"thermalPresent\":true,"
        "\"emmcWearPresent\":true,\"hasLightSensor\":true,\"hasRemoteButtons\":true,"
        "\"hdmiSeen\":{\"phy_mode\":true,\"chroma\":true,\"hdcp\":true,"
        "\"allm\":true,\"vrr\":true},\"hasGpuClock\":true,"
        "\"watchHistory\":true,\"isOled\":true}");
}
}

int main(int argc, char **argv) {
    HaDiscoveryOptions defaults;
    const HaDiscoveryBatch baseline = buildHaDiscoveryBatch(defaults);
    if (argc == 2 && std::string(argv[1]) == "--dump") {
        std::cout << encodedBatch(baseline).dump() << '\n';
        return 0;
    }
    if (argc == 3 && std::string(argv[1]) == "--dump-dynamic") {
        std::ifstream input(argv[2]);
        if (!input) {
            std::cerr << "could not read dynamic discovery fixture\n";
            return 2;
        }
        const std::string contents((std::istreambuf_iterator<char>(input)),
                                   std::istreambuf_iterator<char>());
        const Json fixture = Json::parse(contents);
        HaDiscoveryOptions dynamic;
        dynamic.installedApps = fixture["installedApps"];
        dynamic.pictureModes = fixture["pictureModes"];
        dynamic.soundOutputs = fixture["soundOutputs"];
        dynamic.lgSettings = fixture["lgRows"];
        dynamic.iconBaseUrl = fixture["iconBaseUrl"].string();
        std::cout << encodedBatch(buildHaDiscoveryBatch(dynamic)).dump() << '\n';
        return 0;
    }

    check(haDiscoveryTemplates().size() == 114,
          "discovery templates do not cover all current entities");
    check(baseline.publish.size() == 114,
          "baseline discovery batch changed the published entity count");
    check(baseline.clear.size() == 6,
          "baseline discovery batch did not clear every retired entity");
    check(baseline.publish.front().topic ==
              "homeassistant/sensor/lg_tv/soc_temperature/config",
          "baseline discovery topic changed");
    check(baseline.publish.front().payload.find(
              "\"default_entity_id\":\"sensor.lg_tv_soc_temperature\"") !=
              std::string::npos,
          "default entity id is missing from the native discovery payload");
    struct Published { std::string topic; std::string payload; bool retained; };
    std::vector<Published> messages;
    check(publishHaDiscoveryBatch(baseline,
              [&messages](const std::string &topic, const std::string &payload,
                          bool retained) {
                  messages.push_back({topic, payload, retained});
                  return true;
              }),
          "valid discovery batch was rejected");
    check(messages.size() == baseline.clear.size() + baseline.publish.size(),
          "discovery publisher dropped a clear or config message");
    check(messages.front().payload.empty() && messages.front().retained,
          "retired discovery topics must be cleared with retained empty payloads");
    check(messages[baseline.clear.size()].payload == baseline.publish.front().payload &&
              messages[baseline.clear.size()].retained,
          "discovery configs must publish retained after cleanup messages");

    HaDiscoveryOptions custom;
    custom.topicPrefix = "livingroom";
    custom.discoveryPrefix = "ha";
    custom.deviceId = "living_room";
    custom.allowPower = false;
    custom.piccap = false;
    custom.applyCapabilities = true;
    custom.capabilities = fullCapabilities();
    custom.device = Json::parse(
        "{\"name\":\"Living Room\",\"model\":\"OLED C2\","
        "\"manufacturer\":\"LG\",\"sw_version\":\"webOS 9\"}");
    const HaDiscoveryBatch filtered = buildHaDiscoveryBatch(custom);
    check(filtered.publish.size() == 109,
          "capability and user configuration filters changed the entity set");
    check(filtered.publish.front().topic == "ha/sensor/living_room/soc_temperature/config",
          "custom discovery prefix or device id was not applied");
    check(filtered.publish.front().payload.find("livingroom/telemetry") !=
              std::string::npos,
          "custom state topic prefix was not applied");
    check(filtered.publish.front().payload.find("OLED C2") != std::string::npos,
          "custom device metadata was not applied");
    check(filtered.clear.size() == 11,
          "withheld and retired discovery topics were not scheduled for cleanup");
    check(haDefaultEntityId("sensor", " Living Room!TV ", "cpu_load") ==
              "sensor.living_room_tv_cpu_load",
          "default entity id slugging changed");
    std::cout << "native Home Assistant discovery: " << baseline.publish.size()
              << " baseline configs, " << filtered.publish.size()
              << " filtered configs\n";
    return 0;
}
