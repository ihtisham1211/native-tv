#pragma once

#include "core/json.hpp"
#include <string>

namespace nativetv {
namespace service {

struct OledCareSources {
    std::string systemRoot = "/";
    bool isOled = false;
    bool serviceControls = false;
    bool writable = false;
    Json systemProperties;
    Json panelController;
    Json displayStatus;
    Json liveProtections;
    Json pictureSettings;
};

bool oledModel(const std::string &model);
bool hasOledPanelTypeMarker(const std::string &systemRoot);
bool hasOledPanelRecords(const std::string &systemRoot);
Json oledCareResponse(const OledCareSources &sources);
// The panel's wear figures as /api/stats carries them under `oled` (the care
// route shows a subset): hours, compensation and refresher cycles, protection
// states, the picture settings that decide how hard the panel is worked.
Json oledStatsObject(const OledCareSources &sources);

} // namespace service
} // namespace nativetv
