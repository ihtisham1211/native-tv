#pragma once

#include "core/json.hpp"
#include <cstdint>
#include <string>

namespace nativetv {
namespace service {

// Merge managed Node snapshots with native Home activity while preserving
// explicit row resets from layout/profile changes.
Json mergeHomeRecentHistory(const Json &current, const Json &incoming,
                            const Json &replaceRows);
Json stampHomeRecentHistory(const Json &current, const std::string &row,
                            const std::string &id, int64_t timestamp);

} // namespace service
} // namespace nativetv
