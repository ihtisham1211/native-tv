#pragma once

#include "core/json.hpp"
#include <string>
#include <vector>

namespace nativetv {
namespace service {

struct LgSettingWrite {
    bool handled = false;
    bool valid = false;
    std::string id;
    std::string category;
    std::string key;
    Json storedValue;
    std::string error;
    // The dashboard greys the row out while another input is on screen; LG's
    // service stores a write regardless, so the caller checks the description.
    bool greyable = false;
};

// The native control path owns the Game Optimizer settings first. Other LG
// settings keep the Node fallback until their firmware-specific descriptors
// move with the /api/lgsettings route.
LgSettingWrite lgGameSettingWrite(const Json &value);

// Any dashboard or Game Optimizer row: the value the TV takes for the
// {id, on|value} a control request carries, validated against the row.
LgSettingWrite lgSettingWrite(const Json &value);

// Convert a backup value into LG's stored value for a setting currently
// reported by the TV. The current dashboard row supplies firmware-specific
// choices, ranges and availability.
LgSettingWrite lgSettingsRestoreWrite(const std::string &id,
                                     const Json &value,
                                     const Json &currentRow);

// The Game Optimizer endpoint reads each LG category whole, then asks for
// descriptions only for settings the TV actually reports.
std::vector<std::string> lgGameSettingCategories();
std::vector<std::string> lgGameSettingKeys(const std::string &category);
std::vector<std::string> lgSettingsCategories(
    const std::vector<std::string> &sections);
std::vector<std::string> lgSettingsKeys(
    const std::string &category, const std::vector<std::string> &sections);

// Convert LG's settings, descriptions and dimensions into the legacy
// /api/game response shape. The arguments are objects keyed by category;
// description values are each category's getSystemSettingDesc results array.
Json lgGameResponse(const Json &settingsByCategory,
                    const Json &descriptionsByCategory,
                    const Json &dimensionsByCategory,
                    const Json &pictureSettings);
Json lgSettingsResponse(const std::vector<std::string> &sections,
                        const Json &settingsByCategory,
                        const Json &descriptionsByCategory,
                        const Json &dimensionsByCategory,
                        const Json &inputs);
Json lgSettingsInputNames(const Json &devices);

} // namespace service
} // namespace nativetv
