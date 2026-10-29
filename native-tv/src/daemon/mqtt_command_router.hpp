#pragma once

#include "core/json.hpp"
#include <string>

namespace nativetv {
namespace daemon {

struct MqttControlCommand {
    bool valid = false;
    bool refreshTelemetry = false;
    std::string action;
    Json payload = Json::object();
};

// Translate the legacy MQTT command topic and text payload into the control
// request accepted by /api/control. Invalid or unknown LG-setting topics are
// ignored, matching the legacy MQTT handler.
MqttControlCommand routeMqttControlTopic(const std::string &topic,
                                        const std::string &commandPrefix,
                                        const std::string &textPayload,
                                        const Json &lgSettingsRows = Json::array());

} // namespace daemon
} // namespace nativetv
