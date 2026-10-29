#pragma once

#include "core/json.hpp"
#include <string>

namespace nativetv {
namespace daemon {

struct MqttPublishOptions {
    Json lgRows;       // LG's settings rows, as lgSettingsResponse makes them
    bool tvOff = false;
    Json watch;        // history summary, or invalid for none
    Json piccapState;  // {isRunning}, or invalid for none
    std::string lastApp;
    std::string lastAppId;
};

// What goes to the telemetry topic from one stats read (legacy lib/mqtt-state.js
// publishPayload): the stats with LG's settings by row id, the switched-off flag,
// the watch summary and PicCap's power added, the last app and input standing in
// while the TV is off, and without the installed apps, the temperature history and
// the CPU times. The stats are copied, as the collector hands out its cached object.
Json mqttPublishPayload(const Json &stats, const MqttPublishOptions &options);

} // namespace daemon
} // namespace nativetv
