#pragma once
#include "ui/screen.hpp"
#include <memory>
#include <string>

// The Quick drawer, Profiles, Details and the editors: each answers null for a route it does not
// own (screen_registry.cpp adds the four).
namespace nativetv {
std::unique_ptr<ScreenView> makeQuickScreen(const std::string &route);
std::unique_ptr<ScreenView> makeProfilesScreen(const std::string &route);
std::unique_ptr<ScreenView> makeDetailsScreen(const std::string &route);
std::unique_ptr<ScreenView> makeEditorScreen(const std::string &route);

// Where closing a screen goes: back to Home as it was when Home (or one of these screens, which
// were reached from Home) opened it, otherwise to the page it came from.
inline std::string returnRoute(const std::string &from) {
    for (const std::string prefix : {"home", "quick", "profiles", "rename", "search", "details:"})
        if (from.compare(0, prefix.size(), prefix) == 0)
            return "home:resume";
    return from.empty() ? "home" : from;
}
} // namespace nativetv
