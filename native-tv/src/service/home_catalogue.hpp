#pragma once

#include "core/json.hpp"
#include <string>

namespace nativetv {
namespace service {

// Project the Home launch-point, input-device and layout sources into the
// catalogue consumed by the native Home client.
Json homeCatalogueApps(const Json &launchPointsReply,
                       const Json &sidebarInfoReplies);
Json homeCatalogueDevices(const Json &deviceReply);
Json homeCatalogue(const Json &document, const Json &apps, const Json &devices,
                   const Json &channels, bool channelsRequested,
                   const Json &recent, const Json &historyTotals,
                   bool historyEnabled);
Json readHomeRecent(const std::string &dataRoot);

} // namespace service
} // namespace nativetv
