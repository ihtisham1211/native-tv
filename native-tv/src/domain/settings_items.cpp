#include "domain/settings_items.hpp"
#include "domain/say.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>

namespace nativetv {
namespace {
Json vars(std::initializer_list<std::pair<const char *, std::string>> values) {
    Json result = Json::object();
    for (const auto &value : values)
        result.set(value.first, Json::text(value.second));
    return result;
}
std::string groupedNumber(long long value) {
    std::string digits = std::to_string(value < 0 ? -value : value), result;
    for (size_t i = 0; i < digits.size(); ++i) {
        if (i && (digits.size() - i) % 3 == 0)
            result += ',';
        result += digits[i];
    }
    return value < 0 ? "-" + result : result;
}
// `value` with its first true-or-false turned over: what a switch's command says, said the other
// way.
Json flippedOnce(const Json &value, bool &done) {
    if (value.isBoolean() && !done) {
        done = true;
        return Json::flag(!value.boolean());
    }
    if (value.isObject()) {
        Json copy = Json::object();
        for (const std::string &key : value.keys())
            copy.set(key, flippedOnce(value[key], done));
        return copy;
    }
    if (value.isArray()) {
        Json copy = Json::array();
        for (const Json &item : value.items())
            copy.add(flippedOnce(item, done));
        return copy;
    }
    return value;
}
Command flipped(const Command &command) {
    Command other = command;
    other.confirm = false;
    bool done = false;
    other.body = flippedOnce(command.body, done);
    return other;
}
std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}
// Whether the label has one of the words, at the start of a word: "ram" is not in "program".
bool mentions(const std::string &label, std::initializer_list<const char *> words) {
    for (const char *word : words)
        for (size_t at = label.find(word); at != std::string::npos; at = label.find(word, at + 1))
            if (at == 0 || !std::isalpha(static_cast<unsigned char>(label[at - 1])))
                return true;
    return false;
}
// The glyph of a row, from what its label says; the group's own glyph when nothing matches.
std::string iconOf(const std::string &group, const std::string &title) {
    std::string label = lower(title);
    if (mentions(label, {"temperature"}))
        return "temp";
    if (mentions(label, {"panel"}))
        return "display";
    if (mentions(label, {"cpu", "processor", "per-core", "gpu"}))
        return "cpu";
    if (mentions(label, {"memory", "swap", "ram"}))
        return "memory";
    if (mentions(label, {"emmc", "storage", "space"}))
        return "disk";
    if (mentions(label, {"wi-fi", "wifi"}))
        return "wifi";
    if (mentions(label, {"remote", "battery"}))
        return "remote";
    if (mentions(label, {"timer", "sleep"}))
        return "timer";
    if (mentions(label, {"power", "reboot", "boot", "stay connected", "always ready", "wake"}))
        return "power";
    if (mentions(label, {"sound", "volume", "audio", "speaker", "earc", "balance"}))
        return "volume";
    if (mentions(label, {"light"}))
        return "bulb";
    if (mentions(label, {"hdmi", "input"}))
        return "hdmi";
    if (mentions(label, {"home assistant"}))
        return "ha";
    if (mentions(label, {"address", "web dashboard"}))
        return "globe";
    if (mentions(label, {"lock"}))
        return "lock";
    if (mentions(label, {"profile"}))
        return "access";
    if (mentions(label, {"colour", "theme"}))
        return "palette";
    if (mentions(label, {"clock"}))
        return "clock";
    if (mentions(label, {"reset", "refresh", "restore", "put back"}))
        return "refresh";
    if (mentions(label, {"install", "uninstall", "apps", "tile"}))
        return "apps";
    if (mentions(label, {"consent", "tracking", "ads", "advertising", "acr", "recognition",
                         "privacy", "block", "data"}))
        return "shield";
    if (mentions(label, {"protect", "sticky", "peak"}))
        return "shield";
    if (mentions(label, {"shift", "dimming", "pixel", "brightness", "panel", "screen"}))
        return "display";
    if (mentions(label, {"game", "latency", "vrr", "frame"}))
        return "game";
    if (mentions(label, {"model", "firmware", "webos", "soc", "info"}))
        return "info";
    for (const auto &entry : settingGroups())
        if (entry.key == group)
            return entry.icon;
    return "settings";
}
} // namespace

std::vector<SettingGroup> settingGroups() {
    return {
        {"system", say::msg("tv.settings.group.metrics", "Metrics"),
         say::msg("tv.settings.group.metrics.desc",
                  "Readings from the TV: processor, memory, storage and what it is."),
         "speed"},
        {"apps", say::msg("tv.settings.group.apps", "Apps"),
         say::msg("tv.settings.group.apps.desc",
                  "Install and remove apps, and choose what LG's Home screen shows."),
         "apps"},
        {"oled", say::msg("tv.settings.group.oled", "OLED Care"),
         say::msg("tv.settings.group.oled.desc",
                  "Panel life, burn-in protections and pixel refreshes."),
         "display"},
        {"game", say::msg("tv.settings.group.game", "Game"),
         say::msg("tv.settings.group.game.desc", "LG's Game Optimizer settings."), "game"},
        {"saver", say::msg("tv.settings.group.saver", "Screen Saver"),
         say::msg("tv.settings.group.saver.desc", "Choose and try the screen saver."), "eye"},
        {"home", say::msg("tv.settings.group.home", "Home"),
         say::msg("tv.settings.group.home.desc",
                  "Native Tv Home: whether it replaces LG's, and how it looks."),
         "home"},
        {"privacy", say::msg("tv.settings.group.privacy", "Privacy"),
         say::msg("tv.settings.group.privacy.desc", "Ads, trackers and the data the TV shares."),
         "shield"},
        {"service", say::msg("tv.settings.group.service", "Service Menu"),
         say::msg("tv.settings.group.service.desc", "LG's engineering menus."), "tune"},
        {"options", say::msg("tv.settings.group.system", "System"),
         say::msg("tv.settings.group.system.desc", "Power, timers, sound and devices."),
         "settings"},
        {"settings", say::msg("tv.settings.group.settings", "Settings"),
         say::msg("tv.settings.group.settings.desc",
                  "The web dashboard and Home Assistant, changed only on the TV."),
         "globe"}};
}

std::string settingGroupStatus(const std::string &key, const Json &data) {
    if (!data.valid() || (data["ok"].valid() && !data["ok"].boolean()))
        return "";
    if (key == "system" && data["temp"].isNumber())
        return std::to_string(std::lround(data["temp"].number())) + " \xc2\xb0"
                                                                    "C";
    if (key == "apps" && data["installed"].isArray())
        return say::msg("tv.settings.status.installed", "{n} installed",
                        vars({{"n", std::to_string(data["installed"].size())}}));
    if (key == "oled") {
        if (data["available"].valid() && !data["available"].boolean())
            return say::msg("tv.settings.status.notOnTv", "Not on this TV");
        if (data["panelHours"].isNumber())
            return say::msg("tv.settings.status.hours", "{n} h",
                            vars({{"n", groupedNumber(std::lround(data["panelHours"].number()))}}));
    }
    if (key == "game" && data["available"].valid() && !data["available"].boolean())
        return say::msg("tv.settings.status.notOnTv", "Not on this TV");
    if (key == "saver")
        for (const auto &mode : data["modes"].items())
            if (mode["active"].boolean())
                return mode["label"].string();
    if (key == "home" && data["enabled"].valid())
        return data["enabled"].boolean() ? say::msg("common.on", "On")
                                         : say::msg("common.off", "Off");
    if (key == "privacy" && data["simple"].valid()) {
        double total = data["simple"]["total"].number();
        return total ? say::msg("tv.settings.status.stillOn", "{n} still on",
                                vars({{"n", compactNumber(total)}}))
                     : say::msg("pv.allOffShort", "All off");
    }
    if (key == "service" && data["lockable"].boolean())
        return data["locked"].boolean() ? say::msg("svc.locked", "Locked")
                                        : say::msg("svc.unlocked", "Unlocked");
    if (key == "settings" && data["homeAssistant"].valid())
        return data["homeAssistant"]["configured"].boolean()
                   ? data["homeAssistant"]["state"].string() == "connected"
                         ? say::msg("mqtt.state.connected", "Connected")
                         : say::msg("mqtt.state.connecting", "Connecting")
                   : say::msg("tv.settings.ha.notSetUp", "Not set up");
    return "";
}

bool settingToggleValue(const std::string &value, bool &on) {
    static const std::map<std::string, bool> words = {
        {"On", true},        {"Off", false},       {"Shown", true},         {"Hidden", false},
        {"Visible", true},   {"Kept ready", true}, {"Switched off", false}, {"Active", true},
        {"Disabled", false}, {"Enabled", true},    {"On Home", true},       {"Not on Home", false},
        {"Locked", true},    {"Unlocked", false}};
    auto found = words.find(value);
    if (found == words.end())
        return false;
    on = found->second;
    return true;
}

std::vector<SettingPage> settingPages(const std::string &group, const std::vector<MenuRow> &rows) {
    std::vector<SettingPage> pages(1);
    std::map<std::string, int> seen; // how many rows of a title came before: the id of a repeat
    for (const MenuRow &row : rows) {
        if (row.heading && row.label.empty() && !row.page.empty()) {
            // A page marker; nothing before it means the first page is this one.
            if (pages.back().items.empty() && pages.back().title.empty())
                pages.back().title = row.page;
            else {
                pages.push_back(SettingPage());
                pages.back().title = row.page;
            }
            continue;
        }
        SettingItem item;
        item.title = row.label;
        item.value = row.value;
        item.body = row.hint;
        item.command = row.command;
        item.id =
            row.label + "#" + std::to_string(seen[(row.heading ? "head:" : "") + row.label]++);
        if (row.heading) {
            // The page's own title already says it.
            if (pages.back().items.empty() && row.label == pages.back().title)
                continue;
            item.kind = SettingItem::Kind::Heading;
            pages.back().items.push_back(item);
            continue;
        }
        item.icon = iconOf(group, row.label);
        bool on = false;
        if (row.command.empty())
            item.kind = SettingItem::Kind::Info;
        else if (!row.command.route.empty())
            item.kind = SettingItem::Kind::Link;
        else if (row.state >= 0) {
            item.kind = SettingItem::Kind::Toggle;
            item.on = row.state == 1;
        } else if (!row.choices.empty())
            item.kind = SettingItem::Kind::Choice;
        else if (!row.action.empty())
            item.kind = SettingItem::Kind::Action;
        else if (settingToggleValue(row.value, on)) {
            item.kind = SettingItem::Kind::Toggle;
            item.on = on;
        } else
            item.kind = row.value.empty() ? SettingItem::Kind::Action : SettingItem::Kind::Choice;
        item.choices = row.choices;
        item.reverse = row.reverse.empty() ? flipped(row.command) : row.reverse;
        switch (item.kind) {
        case SettingItem::Kind::Toggle:
            item.action = item.on ? say::msg("tv.settings.turnOff", "Turn off")
                                  : say::msg("tv.settings.turnOn", "Turn on");
            break;
        case SettingItem::Kind::Choice:
            item.action = say::msg("tv.settings.change", "Change");
            break;
        case SettingItem::Kind::Action:
            item.action = say::msg("tv.settings.run", "Run it");
            break;
        case SettingItem::Kind::Link:
            item.action = say::msg("tv.settings.open", "Open");
            break;
        default:
            break;
        }
        if (!row.action.empty() && item.kind != SettingItem::Kind::Toggle &&
            item.kind != SettingItem::Kind::Info)
            item.action = row.action;
        // A switch shows its state itself; its words matter only where they say more than that.
        bool plainWord = false;
        if (!row.value.empty() &&
            !(item.kind == SettingItem::Kind::Toggle && settingToggleValue(row.value, plainWord)))
            item.chips.push_back(row.value);
        if (row.command.confirm)
            item.chips.push_back(say::msg("tv.settings.asksFirst", "Asks to confirm"));
        pages.back().items.push_back(item);
    }
    // A page that ended up with nothing (a marker the model did not fill) is left out.
    pages.erase(std::remove_if(pages.begin(), pages.end(),
                               [](const SettingPage &page) { return page.items.empty(); }),
                pages.end());
    if (pages.empty())
        pages.push_back(SettingPage());
    return pages;
}

std::vector<SettingItem> settingItems(const std::string &group, const std::vector<MenuRow> &rows) {
    std::vector<SettingItem> items;
    for (const SettingPage &page : settingPages(group, rows))
        items.insert(items.end(), page.items.begin(), page.items.end());
    return items;
}
} // namespace nativetv
