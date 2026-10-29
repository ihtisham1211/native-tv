#include "platform/client_launch.hpp"

#include <cassert>
#include <string>

int main() {
    std::string screen = "home";
    assert(!nativetv::applyClientLaunchParameters(
        R"({"@system_native_app":true,"nid":"com.tvweb.native"})", screen));
    assert(screen == "home");
    // The dashboard's tile sends no screen; its app id decides, and a named screen still wins.
    assert(nativetv::applyClientLaunchParameters(
        R"({"@system_native_app":true,"nid":"com.tvweb.dashboard"})", screen));
    assert(screen == "dashboard");
    assert(nativetv::applyClientLaunchParameters(
        R"({"nid":"com.tvweb.dashboard","screen":"dashboard:settings"})", screen));
    assert(screen == "dashboard:settings");
    assert(!nativetv::applyClientLaunchParameters(
        R"({"@system_native_app":true,"nid":"com.tvweb.splash"})", screen));
    assert(screen == "dashboard:settings");
    screen = "home";
    assert(nativetv::applyClientLaunchParameters(
        R"({"screen":"splash"})", screen));
    assert(screen == "splash");
    assert(nativetv::applyClientLaunchParameters(
        R"({"screen":"dashboard"})", screen));
    assert(screen == "dashboard");
    assert(nativetv::applyClientLaunchParameters(
        R"({"screen":"dashboard:settings"})", screen));
    assert(screen == "dashboard:settings");
    assert(nativetv::applyClientLaunchParameters(
        R"({"screen":"setup:welcome"})", screen));
    assert(screen == "setup:welcome");

    const char *invalid[] = {
        "not json", "[]", R"({"screen":false})",
        R"({"screen":"dashboard:unknown"})", R"({"screen":"../../home"})"};
    for (const char *argument : invalid) {
        const std::string before = screen;
        assert(!nativetv::applyClientLaunchParameters(argument, screen));
        assert(screen == before);
    }
    return 0;
}
