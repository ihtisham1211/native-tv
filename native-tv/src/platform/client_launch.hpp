#pragma once

#include <string>

namespace nativetv {

// Apply a webOS Application Manager launch-parameter JSON argument to the
// initial screen. Other JSON arguments (such as jailer metadata) are ignored,
// except that the dashboard app's id opens the dashboard when no screen is named.
bool applyClientLaunchParameters(const std::string &argument,
                                 std::string &initialScreen);

} // namespace nativetv
