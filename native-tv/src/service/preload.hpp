#pragma once

#include "core/json.hpp"
#include <string>

namespace nativetv {
namespace service {

const std::string &preloadConfigKey();
bool preloadEssentialApp(const std::string &id);
bool javascriptTruthy(const Json &value);
std::string javascriptString(const Json &value);

Json normalizePreloadState(const Json &state);
bool preloadStateContainsOff(const Json &state, const std::string &id);
bool preloadChange(const Json &state, const Json &currentList,
                   const std::string &id, bool off, Json &updatedState,
                   Json &updatedList, bool &enabled);
Json preloadPolicyRequest(const std::string &id, bool enabled);
Json preloadConfigsRequest(const Json &list);
Json preloadApps(const Json &list, const Json &installedApps,
                 const Json &runningApps, const Json &state);

} // namespace service
} // namespace nativetv
