#pragma once
#include "ui/screen.hpp"
#include <memory>
#include <string>

namespace nativetv {
// First-run setup ("setup:welcome", "setup:name", "setup:pin", "setup:network", "setup:ha",
// "setup:handoff", "setup:standby", "setup:summary"). "setup:handoff" is also what Settings opens to connect Home Assistant later:
// then only the phone link runs and Back returns to Settings. A route this does not own answers
// null.
std::unique_ptr<ScreenView> makeSetupScreen(const std::string &route);
} // namespace nativetv
