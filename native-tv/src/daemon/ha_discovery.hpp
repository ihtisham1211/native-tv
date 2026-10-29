#pragma once

#include "core/json.hpp"
#include <functional>
#include <string>
#include <vector>

namespace nativetv {
namespace daemon {

struct HaDiscoveryOptions {
    std::string topicPrefix = "tv";
    std::string discoveryPrefix = "homeassistant";
    std::string deviceId = "lg_tv";
    Json device;
    Json capabilities;
    Json installedApps;
    Json pictureModes;
    Json soundOutputs;
    Json lgSettings;
    std::string iconBaseUrl;
    bool applyCapabilities = false;
    bool allowPower = true;
    bool piccap = true;
};

struct HaDiscoveryMessage {
    std::string topic;
    std::string payload;
};

struct HaDiscoveryBatch {
    std::vector<HaDiscoveryMessage> publish;
    std::vector<std::string> clear;
};

Json haDiscoveryTemplates();
HaDiscoveryBatch buildHaDiscoveryBatch(const HaDiscoveryOptions &options);
bool publishHaDiscoveryBatch(
    const HaDiscoveryBatch &batch,
    const std::function<bool(const std::string &, const std::string &, bool)> &publish);
std::string haDefaultEntityId(const std::string &type, const std::string &deviceId,
                              const std::string &entityId);

} // namespace daemon
} // namespace nativetv
