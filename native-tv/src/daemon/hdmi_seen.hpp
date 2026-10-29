#pragma once

#include "core/json.hpp"
#include <string>

namespace nativetv {
namespace daemon {

// Merge non-null diagnostic fields from an hdmi_diag observation into the
// legacy sorted, newline-separated marker file.
bool recordHdmiSeen(const std::string &path, const Json &diagnostics,
                    std::string *error = nullptr);
// Read the legacy marker as a capability map. Invalid or missing markers are
// reported through error and return an empty object.
Json readHdmiSeen(const std::string &path, std::string *error = nullptr);

} // namespace daemon
} // namespace nativetv
