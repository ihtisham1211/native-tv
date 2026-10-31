#pragma once

#include "core/json.hpp"
#include <cstdint>
#include <string>
#include <vector>

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

// The times the idle power-off offers, in minutes (0 is off): 15 and 30 minutes, then every hour up to eight.
const std::vector<int> &screenTimeIdleChoices();

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
