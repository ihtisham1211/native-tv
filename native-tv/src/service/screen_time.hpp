#pragma once

#include "core/json.hpp"
#include <cstdint>
#include <string>

namespace nativetv {
namespace service {

struct ScreenTimeDue {
    bool due = false;
    std::string id;
    std::string action;
    std::string text;
};

struct ScreenTimeBedtime {
    bool configured = false;
    bool inside = false;
    int64_t untilStartSeconds = 0;
};

// Validate and copy a partial screen-time settings document using the same
// accepted fields and error strings as the legacy API.
bool validateScreenTimePatch(const Json &patch, Json &value, std::string &error);
Json screenTimeSettings(const Json &configDocument, bool allowPower, bool hasHomeApp);

// Evaluate the policy rules from local milliseconds since epoch and today's
// recorded seconds by app/input. Enforcement and timers are owned by the
// daemon integration; these functions contain the shared policy decisions.
ScreenTimeBedtime screenTimeBedtime(const Json &settings, int64_t localNowMs);
ScreenTimeDue screenTimeTvDue(const Json &settings, int64_t localNowMs,
                              const Json &todaySeconds);
ScreenTimeDue screenTimeAppDue(const Json &settings, const std::string &key,
                               double todaySeconds);

} // namespace service
} // namespace nativetv
