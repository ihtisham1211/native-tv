#pragma once
#include "ui/screen.hpp"
#include <memory>
#include <string>

// The TV's own pages for what the dashboard showed: Live (the readings), Settings (the ten
// dashboard sections as a list and a panel) and Catalog (installing apps). Each answers null for
// a route it does not own.
namespace nativetv {
std::unique_ptr<ScreenView> makeLiveScreen(const std::string &route);
std::unique_ptr<ScreenView> makeSettingsScreen(const std::string &route);
std::unique_ptr<ScreenView> makeCatalogScreen(const std::string &route);
// The one line screen_registry.cpp needs.
void registerDashboardScreens(ScreenRegistry &registry);
} // namespace nativetv
