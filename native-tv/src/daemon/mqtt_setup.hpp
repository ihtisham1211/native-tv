#pragma once

#include "core/json.hpp"
#include <string>

namespace nativetv {
namespace daemon {

// Builds the same canonical MQTT/device patch as the phone setup form.
bool buildPhoneMqttSettings(const Json &payload, const Json &config,
                            const Json &identity, Json &mqtt, Json &device,
                            std::string &error);

} // namespace daemon
} // namespace nativetv
