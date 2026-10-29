#pragma once

#include "core/json.hpp"
#include <string>

namespace nativetv {
namespace daemon {

// Persist the legacy {"app": ..., "app_id": ...} last-foreground-app snapshot.
bool validateLastAppSnapshot(const Json &snapshot, std::string *error = nullptr);
bool recordLastApp(const std::string &path, const Json &snapshot,
                   std::string *error = nullptr);

} // namespace daemon
} // namespace nativetv
