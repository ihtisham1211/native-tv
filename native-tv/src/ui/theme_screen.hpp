#pragma once
#include "ui/screen.hpp"
#include <memory>
#include <string>

namespace nativetv {
// The TV theme picker ("tvtheme") and the one-time offer of OLED Care ("tvtheme:suggest"); null for
// any other route.
std::unique_ptr<ScreenView> makeThemeScreen(const std::string &route);
} // namespace nativetv
