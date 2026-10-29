#pragma once

#include "core/json.hpp"
#include <set>
#include <string>

namespace nativetv {
namespace service {

struct AppListState {
    std::set<std::string> hiddenIds;
    bool tileHidingEnabled = false;
    std::string dataRoot;
    std::string systemRoot = "/";
};

// Builds the installed-app, system-tile and saved-page projections returned by
// GET /api/apps from application-manager replies and the existing state files.
Json appListResponse(const Json &appsReply, const Json &launchPointsReply,
                    const AppListState &state, bool writable);

} // namespace service
} // namespace nativetv
