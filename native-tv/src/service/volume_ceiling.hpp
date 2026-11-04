#pragma once

#include "core/json.hpp"
#include <cstdint>
#include <string>

namespace nativetv {
namespace service {

// The "volumeCeiling" settings: enabled; maxVolume and nightMaxVolume, 0 to 100
// (100 is no limit); nightStart and nightEnd, "HH:MM" or "" for no night window,
// which may run past midnight; startupVolume, -1 to leave the volume alone or 0 to
// 100 to set when the TV comes on.
bool validateVolumeCeilingPatch(const Json &patch, Json &value, std::string &error);
// Whether one setting holds a valid value (what a settings backup checks).
bool volumeCeilingValidField(const std::string &key, const Json &value);
// Every setting, the stored valid ones over the defaults.
Json volumeCeilingSettings(const Json &configDocument);

// The loudest the TV may be at a local time, in milliseconds since midnight. A
// negative time (the TV's clock is not set) leaves the night window out.
int volumeCeilingLimit(const Json &settings, int64_t localNowMs);
// The volume to set when the TV comes on, held to the ceiling, or -1 for none.
int volumeCeilingStartup(const Json &settings, int64_t localNowMs);

} // namespace service
} // namespace nativetv
