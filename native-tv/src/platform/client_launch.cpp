#include "platform/client_launch.hpp"

#include "core/json.hpp"

#include <algorithm>
#include <iterator>

namespace nativetv {
namespace {
const char *const kScreens[] = {
    "splash", "home", "dashboard", "dashboard:system", "dashboard:apps",
    "dashboard:oled", "dashboard:game", "dashboard:saver",
    "dashboard:home", "dashboard:privacy", "dashboard:service",
    "dashboard:options", "dashboard:settings", "setup:welcome",
    "setup:ha", "setup:summary", "setup:standby"};
}

bool applyClientLaunchParameters(const std::string &argument,
                                 std::string &initialScreen) {
    if (argument.empty() || argument.size() > 4096 || argument[0] != '{')
        return false;
    const Json parameters = Json::parse(argument);
    const Json screen = parameters["screen"];
    if (!parameters.isObject()) return false;
    // The dashboard's tile is launched from LG's launcher, which sends no
    // parameters, so the app's own id (the jailer's "nid") names its screen.
    if (!screen.isString()) {
        const Json app = parameters["nid"];
        if (!app.isString() || app.string() != "com.tvweb.dashboard") return false;
        initialScreen = "dashboard";
        return true;
    }
    const std::string requested = screen.string();
    if (std::find(std::begin(kScreens), std::end(kScreens), requested) ==
        std::end(kScreens))
        return false;
    initialScreen = requested;
    return true;
}

} // namespace nativetv
