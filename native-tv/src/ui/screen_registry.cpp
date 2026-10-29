#include "ui/dashboard_screens.hpp"
#include "ui/overlay_routes.hpp"
#include "ui/screen.hpp"
#include "ui/setup_screen.hpp"

namespace nativetv {
// Each task adds its screens here, with the file that defines them in CMake's
// nativetv_presentation list:
//
//   registry.add([](const std::string &route) -> std::unique_ptr<ScreenView> {
//       return route == "live" ? std::unique_ptr<ScreenView>(new LiveScreen()) : nullptr;
//   });
//
// Until a screen is registered its route is handled by the older views in application.cpp.
void registerScreens(ScreenRegistry &registry) {
    registry.add(makeSetupScreen);
    registerDashboardScreens(registry);
    registry.add(makeQuickScreen);
    registry.add(makeProfilesScreen);
    registry.add(makeDetailsScreen);
    registry.add(makeEditorScreen);
}
} // namespace nativetv
