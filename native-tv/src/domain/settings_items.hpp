#pragma once
#include "domain/models.hpp"
#include <string>
#include <vector>

// The Settings page's words and kinds of row, derived from the rows dashboardRows() builds, so
// the commands (and what they confirm) stay the ones the models test. Nothing here draws.
namespace nativetv {
// One of the ten dashboard sections, listed in the same order and under the same keys as
// dashboardTabs().
struct SettingGroup {
    std::string key, title, description, icon;
};
std::vector<SettingGroup> settingGroups();
// A few words for the group's row, from the data it last loaded ("58 °C", "3 installed").
std::string settingGroupStatus(const std::string &key, const Json &data);

struct SettingItem {
    enum class Kind {
        Heading, // a section label: not focusable
        Info,    // a reading, or a setting this TV or build does not let the remote change
        Toggle,  // two states; OK flips it
        Choice,  // a value that OK steps to its next one
        Action,  // OK does something, once
        Link     // OK opens another page
    };
    Kind kind = Kind::Info;
    std::string icon, title, value;
    std::string body;   // the detail panel's text
    std::string action; // what the panel says OK does; empty when OK does nothing
    std::vector<std::string> chips;
    bool on = false;
    Command command;
    bool focusable() const {
        return kind != Kind::Heading;
    }
};
std::vector<SettingItem> settingItems(const std::string &group, const std::vector<MenuRow> &rows);
// What a row's value is called on and off: true and sets `on` when `value` is one of them.
bool settingToggleValue(const std::string &value, bool &on);
} // namespace nativetv
