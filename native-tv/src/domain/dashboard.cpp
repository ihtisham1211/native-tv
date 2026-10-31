#include "domain/models.hpp"
#include "domain/say.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <strings.h>

// The ten sections of the TV dashboard as rows, from the same daemon routes the dashboard page
// read. The words are the page's own (the same keys, so one text in both), and every row that
// changes something carries the command that does it.
namespace nativetv {
namespace {
using Rows = std::vector<MenuRow>;
const char *const kDash = "\xe2\x80\x94"; // an em dash
Json vars(std::initializer_list<std::pair<const char *, std::string>> values) {
    Json result = Json::object();
    for (const auto &value : values)
        result.set(value.first, Json::text(value.second));
    return result;
}
// A subsection of a section: the Settings screen shows each as a tab and lists only its rows. A
// section with no pages is one list. The marker row has no label.
void page(Rows &rows, const std::string &title) {
    MenuRow marker;
    marker.heading = true;
    marker.page = title;
    rows.push_back(marker);
}
void head(Rows &rows, const std::string &title) {
    MenuRow row;
    row.label = title;
    row.heading = true;
    rows.push_back(row);
}
// `action` is what the panel says OK does where "Turn off" and "Change" would not say it;
// `state` is 0 or 1 for a row that is a switch whose value is not On or Off.
// The row is returned so a caller can add what only some rows have (`choices`, `reverse`).
MenuRow &row(Rows &rows, const std::string &label, const std::string &value = "",
             const Command &command = Command(), const std::string &hint = "",
             const std::string &action = "", int state = -1) {
    MenuRow item;
    item.label = label;
    item.value = value;
    // The panel has a line of its own for "Press OK to ...".
    item.hint = hint.substr(0, hint.find(" Press OK"));
    item.command = command;
    item.action = action;
    item.state = state;
    rows.push_back(item);
    return rows.back();
}
std::string on(bool value) {
    return value ? say::msg("common.on", "On") : say::msg("common.off", "Off");
}
std::string grouped(long long value) {
    std::string digits = std::to_string(value < 0 ? -value : value), result;
    for (size_t i = 0; i < digits.size(); ++i) {
        if (i && (digits.size() - i) % 3 == 0)
            result += ',';
        result += digits[i];
    }
    return value < 0 ? "-" + result : result;
}
std::string fixed(double value, int decimals) {
    char text[48];
    std::snprintf(text, sizeof(text), "%.*f", decimals, value);
    return text;
}
// A number the server sent with or without its unit: the unit is not doubled, and a percent
// sits against its number.
std::string withUnit(const Json &value, const std::string &unit) {
    if (!value.valid() || value.string().empty())
        return kDash;
    std::string text = value.string();
    if (text.size() >= unit.size() &&
        strcasecmp(text.c_str() + text.size() - unit.size(), unit.c_str()) == 0) {
        text.erase(text.size() - unit.size());
        while (!text.empty() && text.back() == ' ')
            text.pop_back();
    }
    return text + (unit == "%" ? "" : " ") + unit;
}
std::string kilobytes(double kb) {
    return kb >= 1048576 ? fixed(kb / 1048576, 1) + " GB"
                         : std::to_string(std::lround(kb / 1024)) + " MB";
}
// "Press OK to schedule" next to the key cap that already says OK: just what it does.
std::string pressAction(std::string text) {
    for (const char *lead : {"Press OK, then again, to ", "Press OK to "}) {
        std::string prefix = lead;
        if (text.compare(0, prefix.size(), prefix) == 0) {
            text.erase(0, prefix.size());
            break;
        }
    }
    if (!text.empty())
        text[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(text[0])));
    return text;
}

using Choices = std::vector<std::pair<std::string, Command>>;

// ---- the LG settings the TV exposes (Game, Sound, HDMI, Devices, ads) --------------------------

// A choice steps to its next value on OK, and a number rises by one and wraps; a wide range,
// such as balance's 101 steps, is shown and left to a browser. `section` limits the rows to one
// of the server's sections; empty takes them all.
void lgRows(Rows &rows, const Json &values, bool writable = true, const std::string &section = "") {
    for (const auto &setting : values.items()) {
        if (!section.empty() && setting["section"].string() != section)
            continue;
        std::string value = setting["value"].string(), type = setting["type"].string();
        std::string hint = setting["desc"].string();
        Command command;
        Choices options;
        int state = -1;
        bool active = writable && setting["active"].boolean(true);
        if (type == "choice" && setting["choices"].size()) {
            auto choices = setting["choices"].items();
            size_t index = 0;
            for (size_t i = 0; i < choices.size(); ++i)
                if (choices[i]["value"].dump() == setting["value"].dump())
                    index = i;
            value = choices[index]["label"].string();
            if (active)
                for (const auto &choice : choices)
                    options.push_back({choice["label"].string(),
                                       control("lgSetting", fields({{"id", setting["id"]},
                                                                    {"value", choice["value"]}}))});
            if (active)
                command =
                    control("lgSetting",
                            fields({{"id", setting["id"]},
                                    {"value", choices[(index + 1) % choices.size()]["value"]}}));
        } else if (type == "number") {
            if (active && setting["max"].number() - setting["min"].number() <= 30) {
                double next = setting["value"].number() >= setting["max"].number()
                                  ? setting["min"].number()
                                  : setting["value"].number() + 1;
                command = control("lgSetting",
                                  fields({{"id", setting["id"]}, {"value", Json::numeric(next)}}));
                for (double v = setting["min"].number(); v <= setting["max"].number(); ++v)
                    options.push_back(
                        {compactNumber(v),
                         control("lgSetting",
                                 fields({{"id", setting["id"]}, {"value", Json::numeric(v)}}))});
            }
        } else {
            value = on(setting["on"].boolean());
            state = setting["on"].boolean() ? 1 : 0;
            if (active)
                command = control(
                    "lgSetting",
                    fields({{"id", setting["id"]}, {"on", Json::flag(!setting["on"].boolean())}}));
        }
        // A setting the TV has greyed out, such as Deep Colour off an HDMI input.
        if (setting["active"].valid() && !setting["active"].boolean())
            hint += " " + say::msg("lgs.inactive",
                                   "Not available with the TV’s current input or sound output.");
        row(rows, setting["title"].string(), value, command, hint, "", state).choices = options;
    }
}
bool anyLg(const Json &values, const std::string &section) {
    for (const auto &setting : values.items())
        if (setting["section"].string() == section)
            return true;
    return false;
}

// Game Optimizer's settings by what they are for: the picture, the signal from the console, and
// the sound. A setting this build does not know goes under More.
void gameRows(Rows &rows, const Json &values, bool writable) {
    struct Group {
        std::string title;
        std::vector<std::string> ids;
        Json rows = Json::array();
    };
    std::vector<Group> groups = {
        {say::msg("tv.settings.page.gamePicture", "Picture"),
         {"gameGenre", "darkMode", "blackStabilizer", "whiteStabilizer"}},
        {say::msg("tv.settings.page.gameSignal", "Signal"),
         {"inputOptimization", "gameOptimization", "freesync", "enableALLM"}},
        {say::msg("tv.settings.page.gameSound", "Sound"), {"aigamesound"}},
        {say::msg("tv.settings.page.more", "More"), {}}};
    for (const auto &setting : values.items()) {
        Group *target = &groups.back();
        for (auto &group : groups)
            if (std::find(group.ids.begin(), group.ids.end(), setting["id"].string()) !=
                group.ids.end())
                target = &group;
        target->rows.add(setting);
    }
    for (const auto &group : groups)
        if (group.rows.size()) {
            page(rows, group.title);
            lgRows(rows, group.rows, writable);
        }
}

// ---- Metrics ---------------------------------------------------------------------------------

void metrics(Rows &rows, const Json &data) {
    page(rows, say::msg("tv.system.head.load", "Load"));
    head(rows, say::msg("tv.system.head.load", "Load"));
    if (data["temp"].valid() &&
        !(data["capabilities"]["thermal"].valid() && !data["capabilities"]["thermal"].boolean()))
        row(rows, say::msg("metrics.temperature", "Temperature"),
            withUnit(data["temp"], "\xc2\xb0"
                                   "C"),
            Command(),
            say::msg("tv.system.temperature.hint",
                     "The main processor (SoC) sensor, not the panel. It climbs with video "
                     "decoding and CPU workload."));
    if (data["load"].valid())
        row(rows, say::msg("tv.system.cpuLoad", "CPU load"), withUnit(data["load"], "%"), Command(),
            say::msg("tv.system.cpuLoad.hint",
                     "Across all cores. Brief peaks while an app opens are normal; a sustained "
                     "high figure with nothing playing is not."));
    if (data["mhz"].number(0) > 0)
        row(rows, say::msg("tv.system.cpuFrequency", "CPU frequency"), withUnit(data["mhz"], "MHz"),
            Command(),
            say::msg("tv.system.cpuFrequency.hint",
                     "The current clock. It drops when the TV is idle and rises under load."));
    if (data["gpuMhz"].number(0) > 0)
        row(rows, say::msg("tv.system.gpuClock", "GPU clock"), withUnit(data["gpuMhz"], "MHz"),
            Command(),
            say::msg("tv.system.gpuClock.hint", "Drives the interface and video scaling."));
    if (data["cores"].size()) {
        std::string values;
        for (const auto &core : data["cores"].items())
            values += (values.empty() ? "" : "  ") + core.string() + "%";
        row(rows, say::msg("tv.system.perCoreLoad", "Per-core load"), values, Command(),
            say::msg("tv.system.perCoreLoad.hint",
                     "One figure per core. An even spread is healthy; one core pinned while the "
                     "rest idle points at a stuck process."));
    }
    page(rows, say::msg("tv.system.head.memoryAndStorage", "Memory and storage"));
    head(rows, say::msg("tv.system.head.memoryAndStorage", "Memory and storage"));
    if (data["mem"].valid()) {
        Json v = vars({{"free", kilobytes(data["mem"]["avail"].number())},
                       {"total", kilobytes(data["mem"]["total"].number())}});
        row(rows, say::msg("tv.system.memoryFree", "Memory free"),
            say::msg("tv.system.freeOf", "{free} of {total}", v), Command(),
            say::msg("tv.system.memoryFree.hint",
                     "Free memory is expected to be low: webOS keeps apps loaded so they reopen "
                     "quickly, and releases them under pressure."));
    }
    if (data["swap"]["total"].number() > 0) {
        Json v = vars({{"free", kilobytes(data["swap"]["free"].number())},
                       {"total", kilobytes(data["swap"]["total"].number())}});
        row(rows, say::msg("tv.system.swapFree", "Swap free"),
            say::msg("tv.system.freeOf", "{free} of {total}", v), Command(),
            say::msg("tv.system.swapFree.hint",
                     "Compressed memory held in RAM rather than on disk, so heavy use costs speed "
                     "but does not wear the storage."));
    }
    std::string health = data["emmc"]["health"].string();
    if (!health.empty() && health != "unknown" && data["capabilities"]["emmcWear"].boolean(true))
        row(rows, say::msg("tv.system.emmcHealth", "eMMC health"), health, Command(),
            say::msg("tv.system.emmcHealth.hint",
                     "Life left in the built-in storage, as the chip itself reports it. It falls "
                     "very slowly and cannot be reset."));
    page(rows, say::msg("tv.system.head.thisTv", "This TV"));
    head(rows, say::msg("tv.system.head.thisTv", "This TV"));
    if (data["device"]["model"].valid())
        row(rows, say::msg("tv.system.model", "Model"), data["device"]["model"].string());
    if (data["system"]["firmware"].valid())
        row(rows, say::msg("metrics.firmware", "Firmware"), data["system"]["firmware"].string());
    if (data["system"]["webos"].valid())
        row(rows, "webOS", data["system"]["webos"].string());
    if (data["hardware"]["soc_arch"].valid())
        row(rows, "SoC", data["hardware"]["soc_arch"].string());
    if (data["hardware"]["ram"].valid())
        row(rows, "RAM", data["hardware"]["ram"].string());
    if (data["hardware"]["refresh_rate"].valid())
        row(rows, say::msg("tv.system.panelRefresh", "Panel refresh"),
            withUnit(data["hardware"]["refresh_rate"], "Hz"));
    if (data["remote"]["battery"].valid())
        row(rows, say::msg("tv.system.remoteBattery", "Remote battery"),
            withUnit(data["remote"]["battery"], "%"));
    if (data["ssid"].valid())
        row(rows, say::msg("tv.system.wiFi", "Wi-Fi"), data["ssid"].string());
}

// ---- Apps ------------------------------------------------------------------------------------

std::string uninstallHint(const Json &app) {
    Json v = vars({{"version", app["version"].string()}, {"id", app["id"].string()}});
    return app["version"].string().empty()
               ? say::msg("tv.apps.uninstall.hint",
                          "Store or user-installed application ({id}). Press OK to uninstall from "
                          "the TV.",
                          v)
               : say::msg("tv.apps.uninstall.hintVersion",
                          "Store or user-installed application (v{version}, {id}). Press OK to "
                          "uninstall from the TV.",
                          v);
}
void apps(Rows &rows, const Json &data) {
    bool writable = data["writable"].boolean(true);
    if (writable || data["installed"].size())
        page(rows, say::msg("tv.settings.page.installed", "Installed"));
    if (writable) {
        head(rows, say::msg("tv.apps.head.installHomebrewApps", "Install Homebrew Applications"));
        row(rows, say::msg("apps.install", "Install homebrew apps"),
            say::msg("tv.apps.install.browse", "Browse"), navigate("catalog"),
            say::msg("tv.apps.install.entry.hint", "Apps from the Homebrew Channel catalog."),
            say::msg("tv.settings.open", "Open"));
    }
    if (data["installed"].size())
        head(rows, say::msg("tv.apps.head.uninstallApplications", "Uninstall Applications"));
    for (const auto &app : data["installed"].items())
        row(rows, app["title"].string(app["id"].string()),
            app["size"].number(0) > 0 ? fixed(app["size"].number() / 1048576, 1) + " MB" : "",
            writable ? post(Operation::UninstallApp, fields({{"id", app["id"]}}), true) : Command(),
            uninstallHint(app), say::msg("apps.uninstall.button", "Uninstall"));
    Json hs = data["homeScreen"];
    bool locked = hs["experimental"].valid() && !hs["experimental"]["allowed"].boolean();
    if (hs["parts"].size()) {
        page(rows, say::msg("tv.settings.page.homeScreen", "LG launcher"));
        head(rows, say::msg("apps.home", "LG launcher layout"));
        if (data["nativeTvHome"].boolean())
            row(rows, say::msg("tv.apps.nativeTvHome", "Native Tv Home is on"), "", Command(),
                say::msg("apps.nativeTvHomeNote",
                         "Native Tv Home is switched on, so this applies only to LG’s Home, seen "
                         "from its tile, by holding Home, or while Native Tv starts.") +
                    " " +
                    say::msg("tv.apps.nativeTvHomeWhere",
                             "Native Tv Home is arranged in its own Home section."));
        if (locked)
            row(rows, say::msg("apps.home.exp.title", "Experimental on this TV"),
                say::msg("common.off", "Off"), Command(),
                say::msg("tv.apps.home.exp.hint", "Switch it on from the dashboard in a browser, "
                                                  "under Apps, where its warning is."));
    }
    for (const auto &input : hs["inputs"].items())
        row(rows, input["title"].string(),
            input["onHome"].boolean() ? say::msg("apps.home.onHome", "On Home")
                                      : say::msg("apps.home.notOnHome", "Not on Home"),
            writable && !locked ? post(Operation::SetLgHomeLayout,
                                       fields({{"input", input["id"]},
                                               {"onHome", Json::flag(!input["onHome"].boolean())}}))
                                : Command(),
            say::msg("tv.apps.home.input.hint",
                     "A tile among the apps on LG’s launcher that switches to this input."),
            "", input["onHome"].boolean() ? 1 : 0);
    for (const auto &part : hs["parts"].items())
        row(rows, part["title"].string(),
            part["shown"].boolean() ? say::msg("apps.home.shown", "Shown")
                                    : say::msg("apps.home.hidden", "Hidden"),
            writable && !locked ? post(Operation::SetLgHomeLayout,
                                       fields({{"part", part["id"]},
                                               {"shown", Json::flag(!part["shown"].boolean())}}))
                                : Command(),
            part["desc"].string() + " " +
                say::msg("tv.apps.home.restart",
                         "LG’s launcher starts again, which takes a few seconds."),
            "", part["shown"].boolean() ? 1 : 0);
    if (hs["parts"].size() && writable && !locked)
        row(rows, say::msg("apps.home.restore", "Put back LG’s launcher"),
            say::msg("tv.apps.home.restore.value", "Put back"),
            post(Operation::SetLgHomeLayout, fields({{"restore", Json::flag(true)}}), true),
            say::msg("tv.apps.home.restore.hint",
                     "Every part shown again, and the input tiles added here taken off."),
            say::msg("tv.apps.home.restore.value", "Put back"));
    if (data["quickAccess"]["keys"].size()) {
        page(rows, say::msg("apps.quickAccess", "Quick Access"));
        head(rows, say::msg("apps.quickAccess", "Quick Access"));
    }
    for (const auto &key : data["quickAccess"]["keys"].items())
        row(rows, key["key"].string(),
            key["id"].valid() ? key["title"].string()
                              : say::msg("apps.quickAccess.none", "Nothing set"),
            Command(),
            say::msg("apps.quickAccess.desc",
                     "Holding a number key on the remote opens the app or input set for it. A "
                     "change here applies on the TV straight away.") +
                " " +
                say::msg("tv.apps.quickAccess.browser",
                         "Change them from the dashboard in a browser."));
    if (data["systemTiles"].size()) {
        page(rows, say::msg("tv.settings.page.systemApps", "System apps"));
        head(rows, say::msg("tv.apps.head.hideHomeScreenSystem", "Hide Home Screen System Apps"));
        if (data["nativeTvHome"].boolean())
            row(rows, say::msg("tv.apps.nativeTvHome", "Native Tv Home is on"), "", Command(),
                say::msg(
                    "tv.apps.nativeTvHomeTilesNote",
                    "Native Tv Home is switched on. Apps hidden here are hidden on it too; its "
                    "Home section hides others on Native Tv Home only."));
        bool enabled = data["tileHidingEnabled"].boolean(), held = data["tileHidingHeld"].boolean();
        std::string note =
            data["tileHidingOverridden"].boolean()
                ? say::msg("common.heldOverridden",
                           "Turned back on with allowOnWebos10 in config.json. On webOS 10 and "
                           "later custom screen savers and tile hiding have left the picture, the "
                           "sound and HDMI control off until the TV was unplugged.")
            : held ? say::msg("apps.tiles.held",
                              "Hiding system apps is turned off on this TV for now. On webOS 10 "
                              "and later it can leave the picture, the sound and HDMI control off "
                              "until the TV is unplugged. Apps already hidden come back after the "
                              "TV is next fully restarted.")
            : enabled
                ? say::msg(
                      "tv.apps.tiles.on.hint",
                      "When enabled: On each cold boot of your TV, this can cause a brief 1-second "
                      "blank screen or input refresh before returning to your active HDMI source.")
                : say::msg("tv.apps.tiles.off.hint",
                           "Stock behavior active. Press OK to enable system app hiding.");
        row(rows, say::msg("tv.apps.systemAppHiding", "System App Hiding"),
            enabled ? say::msg("mqtt.cat.enabled", "Enabled")
                    : say::msg("mqtt.cat.disabled", "Disabled"),
            writable && (!held || enabled)
                ? post(Operation::SetTileHiding, fields({{"enabled", Json::flag(!enabled)}}))
                : Command(),
            note, "", enabled ? 1 : 0);
        if (enabled && !held)
            for (const auto &tile : data["systemTiles"].items())
                row(rows, tile["title"].string(tile["id"].string()),
                    tile["hidden"].boolean() ? say::msg("apps.tile.hidden", "Hidden")
                                             : say::msg("apps.tile.visible", "Visible"),
                    writable ? post(tile["hidden"].boolean() ? Operation::SetAppVisible
                                                             : Operation::SetAppHidden,
                                    fields({{"id", tile["id"]}}))
                             : Command(),
                    tile["hidden"].boolean() ? say::msg("tv.apps.tile.hidden.hint",
                                                        "Currently hidden from the TV home ribbon. "
                                                        "Press OK to make this tile visible again.")
                                             : say::msg("tv.apps.tile.visible.hint",
                                                        "Currently visible on the TV home ribbon. "
                                                        "Press OK to hide this tile."),
                    "", tile["hidden"].boolean() ? 0 : 1)
                    .reverse = post(tile["hidden"].boolean() ? Operation::SetAppHidden
                                                             : Operation::SetAppVisible,
                                    fields({{"id", tile["id"]}}));
    }
    if (data["preload"].size()) {
        page(rows, say::msg("tv.settings.page.keptReady", "Kept ready"));
        head(rows, say::msg("apps.preload", "Apps kept ready in the background"));
    }
    for (const auto &app : data["preload"].items()) {
        std::string hint = say::msg(
            "apps.preload.desc",
            "LG keeps these apps loaded in the background so they open faster, including ones that "
            "are never used, and each takes up memory. An app switched off frees its memory once "
            "it is next closed or the TV restarts, and takes a few seconds longer to open.");
        if (app["memoryMb"].number(0) > 0) {
            Json v = vars({{"mb", compactNumber(app["memoryMb"].number())}});
            hint += " " + say::msg("tv.apps.preload.memory",
                                   "This one takes about {mb} MB when loaded.", v);
        }
        if (app["loaded"].boolean())
            hint += " " + say::msg("tv.apps.preload.loadedNow", "It is loaded now.");
        row(rows, app["title"].string(app["id"].string()),
            app["off"].boolean() ? say::msg("apps.preload.off", "Switched off")
                                 : say::msg("apps.preload.kept", "Kept ready"),
            writable ? post(Operation::SetPreload,
                            fields({{"id", app["id"]}, {"off", Json::flag(!app["off"].boolean())}}))
                     : Command(),
            hint, "", app["off"].boolean() ? 0 : 1);
    }
    if (data["services"].size()) {
        page(rows, say::msg("tv.settings.page.services", "LG bloatware"));
        head(rows,
             say::msg("tv.apps.head.turnOffBackgroundServices", "Turn off LG’s bloatware services"));
    }
    for (const auto &service : data["services"].items()) {
        bool disabled = service["disabled"].boolean(),
             still = disabled && service["running"].boolean();
        std::string hint =
            (service["badge"].valid() ? "[" + service["badge"].string() + "] " : "") +
            service["desc"].string();
        if (still)
            hint += " " + say::msg("apps.service.restarted",
                                   "Switched off, but something on the TV has started it again.");
        row(rows, service["title"].string(service["id"].string()),
            !disabled ? say::msg("metrics.hdmi.active", "Active")
            : still   ? say::msg("apps.service.stillRunning", "Disabled, still running")
                      : say::msg("mqtt.cat.disabled", "Disabled"),
            writable ? post(Operation::SetBackgroundService,
                            fields({{"id", service["id"]}, {"disabled", Json::flag(!disabled)}}))
                     : Command(),
            hint, "", disabled ? 0 : 1);
    }
}

// ---- System ----------------------------------------------------------------------------------

void switchRow(Rows &rows, const Json &source, const std::string &key, const std::string &label,
               const std::string &hint, const std::string &action = "",
               const std::string &onWord = "", const std::string &offWord = "") {
    if (!source.has(key))
        return;
    bool state = source[key].boolean();
    row(rows, label,
        state ? (onWord.empty() ? say::msg("common.on", "On") : onWord)
              : (offWord.empty() ? say::msg("common.off", "Off") : offWord),
        control(action.empty() ? key : action, Json::flag(!state)), hint, "", state ? 1 : 0);
}
std::string sleepWord(const std::string &step) {
    return step == "off"   ? say::msg("common.off", "Off")
           : step == "10"  ? say::msg("ctl.sleep.10", "10 min")
           : step == "30"  ? say::msg("ctl.sleep.30", "30 min")
           : step == "60"  ? say::msg("ctl.sleep.60", "60 min")
           : step == "90"  ? say::msg("ctl.sleep.90", "90 min")
           : step == "120" ? say::msg("ctl.sleep.120", "120 min")
                           : step;
}
std::string timerWhen(const Json &timer) {
    std::string time = timer["time"].string();
    std::vector<std::string> names = {
        say::msg("adv.timer.sun", "Sun"), say::msg("adv.timer.mon", "Mon"),
        say::msg("adv.timer.tue", "Tue"), say::msg("adv.timer.wed", "Wed"),
        say::msg("adv.timer.thu", "Thu"), say::msg("adv.timer.fri", "Fri"),
        say::msg("adv.timer.sat", "Sat")};
    std::string days;
    // Monday first, as LG's menu lists them; LG counts from Sunday.
    for (int day : {1, 2, 3, 4, 5, 6, 0})
        for (const auto &set : timer["days"].items())
            if (static_cast<int>(set.number()) == day)
                days += (days.empty() ? "" : " ") + names[static_cast<size_t>(day)];
    Json v = vars({{"time", time}, {"days", days}});
    return days.empty() ? say::msg("tv.options.timer.once", "{time}, once", v)
                        : say::msg("tv.options.timer.repeats", "{time}, repeats {days}", v);
}
void options(Rows &rows, const Json &data, const Json &caps) {
    Json lights = data["lights"], timers = data["powerTimers"], lg = data["lgSettings"];
    if (data.has("quickBoot") || data.has("wakeOnLan") || data.has("alwaysReady") ||
        data.has("alwaysReadyScreen")) {
        page(rows, say::msg("adv.powerStandby", "Power & standby"));
        head(rows, say::msg("adv.powerStandby", "Power & standby"));
        switchRow(rows, data, "quickBoot", say::msg("adv.quickBoot", "Quick Boot"),
                  say::msg("adv.quickBoot.desc",
                           "Switching off puts the TV into a light sleep rather than shutting it "
                           "down, so it comes back on faster. LG calls this Quick Start+."));
        switchRow(rows, data, "alwaysReady",
                  say::msg("adv.stayConnected", "Stay connected when off"),
                  say::msg("tv.options.stayConnected.hint",
                           "Keeps the TV on the network while it is switched off, apart from five "
                           "hours each night, so Native Tv and Home Assistant keep working. The "
                           "screen stays dark. Uses about 12 W on an LG C2."));
        switchRow(
            rows, data, "alwaysReadyScreen", say::msg("adv.alwaysReady", "Always Ready"),
            say::msg("tv.options.alwaysReady.hint",
                     "Switched off with the remote, the TV shows LG's Always Ready screen, such as "
                     "a clock or artwork, instead of going dark, from the next time it is switched "
                     "off. Uses about 30 W on an LG C2, close to what it uses when on. Home turns "
                     "the TV back on from the clock; the power button switches it fully off."));
        // Only while it is on: with it off, the TV sleeps every night anyway.
        if (data["alwaysReady"].boolean() && data["alwaysReadyOff"].valid()) {
            int hour = std::atoi(data["alwaysReadyOff"]["start"].string().c_str());
            row(rows, say::msg("adv.nightlyOff", "Nightly power-down"),
                data["alwaysReadyOff"]["start"].string() + "\xe2\x80\x93" +
                    data["alwaysReadyOff"]["end"].string(),
                control("alwaysReadyOffStart", Json::numeric((hour + 1) % 24)),
                say::msg("tv.options.nightlyPowerDown.hint",
                         "For five hours every night, LG lets a switched-off TV sleep fully. It is "
                         "offline then, so Native Tv and Home Assistant cannot reach it. LG fixes "
                         "the length; choose hours when the TV is not used."),
                pressAction(say::msg("tv.options.nightlyPowerDown.press",
                                     "Press OK to move it an hour later")))
                .choices = [&] {
                // The five hours LG lets the TV sleep, from each hour of the day: OK steps on by
                // one.
                Choices options;
                for (int start = 0; start < 24; ++start) {
                    char window[32];
                    std::snprintf(window, sizeof(window), "%02d:00\xe2\x80\x93%02d:00", start,
                                  (start + 5) % 24);
                    options.push_back(
                        {window, control("alwaysReadyOffStart", Json::numeric(start))});
                }
                return options;
            }();
        }
        switchRow(rows, data, "wakeOnLan", say::msg("adv.wol", "Wake-on-LAN"),
                  say::msg("tv.options.wol.hint",
                           "Lets Home Assistant, a phone or another device on the network switch "
                           "the TV on from standby. Requires Quick Boot (Quick Start+) to keep the "
                           "network connection responsive while off. LG calls this Mobile TV On."));
    }
    if (data.has("sleepTimer") || timers["on"].valid() || timers["off"].valid() ||
        data["idlePowerOff"].valid()) {
        page(rows, say::msg("adv.timers", "Timers"));
        head(rows, say::msg("adv.timers", "Timers"));
        if (data.has("sleepTimer")) {
            const std::vector<std::string> steps = {"off", "10", "30", "60", "90", "120"};
            std::string current = data["sleepTimer"].string();
            auto at = std::find(steps.begin(), steps.end(), current);
            size_t next = at == steps.end()
                              ? 0
                              : (static_cast<size_t>(at - steps.begin()) + 1) % steps.size();
            row(rows, say::msg("ctl.sleep", "Sleep timer"), sleepWord(current),
                control("sleepTimer", Json::text(steps[next])),
                say::msg("tv.options.sleep.hint", "Switches the TV off once this time has passed."),
                say::msg("tv.settings.change", "Change"))
                .choices = [&] {
                Choices options;
                for (const auto &step : steps)
                    options.push_back({sleepWord(step), control("sleepTimer", Json::text(step))});
                return options;
            }();
        }
        Json idle = data["idlePowerOff"];
        if (idle.valid()) {
            // The times offered come from the server: Off, 15 and 30 minutes, then every hour up to eight.
            auto word = [](int minutes) {
                Json n = vars({{"n", std::to_string(minutes < 60 ? minutes : minutes / 60)}});
                return minutes == 0   ? say::msg("common.off", "Off")
                       : minutes < 60 ? say::msg("adv.idle.minutes", "{n} minutes", n)
                       : minutes == 60 ? say::msg("adv.idle.hour", "1 hour")
                                       : say::msg("adv.idle.hours", "{n} hours", n);
            };
            const bool idleWritable = idle["writable"].boolean();
            const int now = static_cast<int>(idle["minutes"].number());
            Choices options;
            size_t at = 0;
            for (const auto &choice : idle["choices"].items()) {
                int minutes = static_cast<int>(choice.number());
                if (minutes == now)
                    at = options.size();
                options.push_back({word(minutes), idleWritable ? post(Operation::SetScreenTime,
                                                                       fields({{"idleMinutes", choice}}))
                                                                : Command()});
            }
            if (!options.empty())
                row(rows, say::msg("adv.idle", "Switch off when idle"), word(now),
                    idleWritable ? options[(at + 1) % options.size()].second : Command(),
                    say::msg("tv.options.idle.hint",
                             "Switches the TV off when no key has been pressed and no app, input "
                             "or volume has changed for this long, and nothing is playing. A "
                             "warning shows a minute before; any key keeps it on. An HDMI input is "
                             "never switched off."),
                    say::msg("tv.settings.change", "Change"))
                    .choices = options;
        }
        for (const auto &key : {"on", "off"}) {
            Json timer = timers[key];
            if (!timer.valid())
                continue;
            bool enabled = timer["enabled"].boolean();
            std::string when = timerWhen(timer);
            Json v = vars({{"when", when}});
            row(rows,
                std::string(key) == "on" ? say::msg("adv.onTimer", "On Timer")
                                         : say::msg("adv.offTimer", "Off Timer"),
                enabled ? when : say::msg("tv.options.timer.off", "Off ({when})", v),
                control("powerTimer",
                        fields({{"timer", Json::text(key)}, {"enabled", Json::flag(!enabled)}})),
                (std::string(key) == "on"
                     ? say::msg("adv.onTimer.desc",
                                "Switches the TV on at this time, once or on the days picked.")
                     : say::msg("adv.offTimer.desc",
                                "Switches the TV off at this time, once or on the days picked.")) +
                    " " +
                    say::msg(
                        "tv.options.timer.hint",
                        "The time and days are set from Native Tv in a browser, under Advanced."),
                "", enabled ? 1 : 0);
        }
    }
    if (anyLg(lg, "sound")) {
        page(rows, say::msg("tv.settings.page.sound", "Sound and HDMI"));
        head(rows, say::msg("adv.sound", "Sound"));
        lgRows(rows, lg, true, "sound");
    }
    if (anyLg(lg, "hdmi")) {
        if (!anyLg(lg, "sound"))
            page(rows, say::msg("tv.settings.page.sound", "Sound and HDMI"));
        head(rows, say::msg("adv.hdmi", "HDMI inputs"));
        lgRows(rows, lg, true, "hdmi");
    }
    if (data.has("lgLogo")) {
        page(rows, say::msg("tv.settings.page.devices", "Display and devices"));
        head(rows, say::msg("adv.display", "Display"));
        switchRow(
            rows, data, "lgLogo", say::msg("adv.lgLogo", "LG logo"),
            say::msg(
                "adv.lgLogo.desc",
                "Shows the LG logo as the TV switches on and off. LG calls this LG Logo Display."),
            "", say::msg("tv.options.lgLogo.shown", "Shown"),
            say::msg("apps.tile.hidden", "Hidden"));
    }
    if (data.has("deviceDetection") || anyLg(lg, "devices")) {
        if (!data.has("lgLogo"))
            page(rows, say::msg("tv.settings.page.devices", "Display and devices"));
        head(rows, say::msg("adv.devices", "Devices"));
        lgRows(rows, lg, true, "devices");
        switchRow(
            rows, data, "deviceDetection", say::msg("adv.deviceDetection", "Auto device detection"),
            say::msg("tv.options.deviceDetection.hint",
                     "Finds devices to show on the Home Dashboard and control: a set-top box, and "
                     "smart lights, plugs and switches. Newer TVs look up every address on your "
                     "home network each time they switch on to do it. Switched off, the scans stop "
                     "and the TV no longer finds devices by itself."));
    }
    if (lights.has("standby") || lights["hasLogo"].boolean()) {
        if (!data.has("lgLogo") && !data.has("deviceDetection") && !anyLg(lg, "devices"))
            page(rows, say::msg("tv.settings.page.devices", "Display and devices"));
        head(rows, say::msg("adv.frontLights", "Front lights"));
        switchRow(
            rows, lights, "standby", say::msg("adv.standbyLight", "Standby light"),
            say::msg("adv.standbyLight.desc",
                     "The small light on the front of the TV that shows while it is switched off."),
            "standbyLight");
        if (lights["hasLogo"].boolean())
            switchRow(
                rows, lights, "logo", say::msg("adv.logoLight", "Logo light"),
                say::msg("tv.options.logoLight.hint", "The lit LG logo on the front of the TV."),
                "logoLight");
    }
    // Last, away from the row the page opens on, so a stray OK lands nowhere near them.
    if (caps["allowPower"].boolean()) {
        page(rows, say::msg("adv.power", "Power"));
        head(rows, say::msg("adv.power", "Power"));
        row(rows, say::msg("ctl.powerOff", "Power off"), "", control("powerOff", Json(), true),
            say::msg("tv.options.powerOff.hint", "The same as the power button on the remote."),
            say::msg("ctl.powerOff", "Power off"));
        row(rows, say::msg("ctl.reboot", "Reboot"), "", control("reboot", Json(), true),
            say::msg("tv.options.reboot.hint", "A full restart of the TV, which LG’s menus do not "
                                               "offer. It is unavailable for about a minute."),
            say::msg("ctl.reboot", "Reboot"));
    }
    if (rows.empty())
        row(rows, say::msg("tv.options.nothingToSetOn", "Nothing to set on this TV"));
}

// ---- OLED Care -------------------------------------------------------------------------------

void reading(Rows &rows, const Json &source, const std::string &key, const std::string &label,
             const std::string &value, const std::string &hint) {
    if (source[key].valid())
        row(rows, label, value, Command(), hint);
}
void oled(Rows &rows, const Json &data) {
    if (!data["isOled"].boolean()) {
        row(rows, say::msg("tv.oled.noPanel", "This TV has no OLED panel"));
        return;
    }
    page(rows, say::msg("oled.panelLife", "Panel life"));
    head(rows, say::msg("oled.panelLife", "Panel life"));
    reading(rows, data, "panelHours", say::msg("tv.oled.totalPowerOnHours", "Total power-on hours"),
            grouped(std::lround(data["panelHours"].number())),
            say::msg("tv.oled.totalPowerOnHours.hint",
                     "Hours the panel has been lit since new. The figure the maintenance cycles "
                     "below are scheduled against."));
    reading(rows, data, "hoursUntilComp", say::msg("oled.compIn", "Panel maintenance in"),
            withUnit(Json::text(std::to_string(std::lround(data["hoursUntilComp"].number()))), "h"),
            say::msg("tv.oled.panelMaintenanceIn.hint",
                     "The short compensation pass, run roughly every four hours of use. It happens "
                     "by itself after the TV is switched off."));
    reading(rows, data, "hoursUntilRefresher", say::msg("oled.refresherIn", "Pixel refresher in"),
            withUnit(Json::text(std::to_string(std::lround(data["hoursUntilRefresher"].number()))),
                     "h"),
            say::msg("tv.oled.pixelRefresherIn.hint",
                     "The long calibration, due about every 2,000 hours. It takes around an hour "
                     "and runs once the TV is off."));
    reading(rows, data, "compCycles", say::msg("oled.cycles.comp", "Short cycles completed"),
            grouped(std::lround(data["compCycles"].number())),
            say::msg("tv.oled.shortCyclesCompleted.hint",
                     "How many short compensation passes the panel has run."));
    reading(rows, data, "refresherCycles",
            say::msg("oled.cycles.refresher", "Pixel refresher cycles"),
            grouped(std::lround(data["refresherCycles"].number())),
            say::msg("tv.oled.pixelRefresherCycles.hint",
                     "How many long calibrations the panel has run. These are the ones that matter "
                     "for wear."));
    reading(rows, data, "gsrStressCount", say::msg("oled.cycles.gsr", "GSR stress events"),
            grouped(std::lround(data["gsrStressCount"].number())),
            say::msg("tv.oled.gsrStressEvents.hint",
                     "How often the panel has detected static image stress conditions. A rising "
                     "count means static content is being shown for long stretches, even if GSR "
                     "dimming is switched off."));
    page(rows, say::msg("oled.protections", "Protections"));
    head(rows, say::msg("oled.protections", "Protections"));
    bool writable = data["writable"].boolean();
    for (const auto &key : {"gsr", "tpc"})
        if (data[key].valid()) {
            bool enabled = data[key].boolean();
            bool gsr = std::string(key) == "gsr";
            row(rows,
                gsr ? say::msg("tv.oled.globalStickyReduction", "Global sticky reduction")
                    : say::msg("tv.oled.temporalPeakControl", "Temporal peak control"),
                on(enabled),
                writable && data["serviceControls"].boolean()
                    ? control("oledProtection",
                              fields({{"key", Json::text(key)}, {"enabled", Json::flag(!enabled)}}),
                              enabled)
                    : Command(),
                gsr ? say::msg("tv.oled.globalStickyReduction.hint",
                               "Dims static content - channel logos, scoreboards, menu bars - so "
                               "it does not burn in. Switching it off makes permanent image "
                               "retention more likely.")
                    : say::msg("tv.oled.temporalPeakControl.hint",
                               "Limits how long the panel holds peak brightness, which protects it "
                               "and keeps heat down. Switching it off makes burn-in more likely."),
                "", enabled ? 1 : 0);
        }
    if (data["screenShift"].valid()) {
        std::string value = data["screenShift"].string();
        bool enabled = value.find("on") != std::string::npos;
        row(rows, say::msg("tv.oled.screenShift", "Screen shift"), value,
            writable ? control("screenShift", Json::text(enabled ? "off" : "on")) : Command(),
            say::msg("tv.oled.screenShift.hint",
                     "Nudges the whole image by a pixel now and then so a fixed edge never sits on "
                     "the same pixels. Invisible in use."),
            "", enabled ? 1 : 0)
            .reverse = control("screenShift", Json::text(enabled ? "on" : "off"));
    }
    if (data["logoDimming"].valid()) {
        std::string value = data["logoDimming"].string();
        row(rows, say::msg("tv.oled.logoDimming", "Logo dimming"), value,
            writable ? control("logoDimming", Json::text(value == "off"     ? "light"
                                                         : value == "light" ? "strong"
                                                                            : "off"))
                     : Command(),
            say::msg("tv.oled.logoDimming.hint",
                     "Spots a station logo that has not moved and fades it slightly. Higher "
                     "settings dim more of the picture around it."),
            say::msg("tv.settings.change", "Change"))
            .choices = [&] {
            Choices options;
            for (const char *level : {"off", "light", "strong"})
                options.push_back({level, control("logoDimming", Json::text(level))});
            return options;
        }();
    }
    if (writable) {
        page(rows, say::msg("tv.oled.head.runNow", "Run now"));
        head(rows, say::msg("tv.oled.head.runNow", "Run now"));
        row(rows, say::msg("tv.oled.scheduleAPixelRefresher", "Schedule a pixel refresher"), "",
            control("refresherSchedule"),
            say::msg("tv.oled.scheduleAPixelRefresher.hint",
                     "Queues the long calibration for the next time the TV is switched off. It "
                     "cannot run while the screen is on."),
            pressAction(say::msg("tv.oled.scheduleAPixelRefresher.press", "Press OK to schedule")));
        row(rows, say::msg("tv.oled.cancelAScheduledRefresher", "Cancel a scheduled refresher"), "",
            control("refresherCancel"),
            say::msg("tv.oled.cancelAScheduledRefresher.hint",
                     "Clears a queued calibration so the TV does not spend an hour on it after the "
                     "next switch-off."),
            pressAction(say::msg("tv.oled.cancelAScheduledRefresher.press", "Press OK to cancel")));
    }
}

// ---- Screen Saver ----------------------------------------------------------------------------

void saver(Rows &rows, const Json &data) {
    page(rows, say::msg("tv.saver.head.screenSaver", "Screen saver"));
    head(rows, say::msg("tv.saver.head.screenSaver", "Screen saver"));
    bool switching = data["switching"].boolean();
    bool writable = data["writable"].boolean() && !switching;
    if (switching)
        row(rows, say::msg("tv.saver.switchingScreenSavers", "Switching screen savers"), "",
            Command(),
            say::msg(
                "tv.saver.switchingScreenSavers.hint",
                "The screen may stay dark for up to a minute, then it comes back on its own."));
    if (data["held"].boolean() || data["heldOverridden"].boolean()) {
        bool allowed = data["heldOverridden"].boolean();
        row(rows, say::msg("tv.saver.held", "Custom screen savers"), on(allowed),
            data["writable"].boolean()
                ? control("setWebos10ScreensaversAllowed", Json::flag(!allowed), !allowed)
                : Command(),
            say::msg("ss.exp.desc",
                     "They have not been tried on this webOS version. Before Native Tv 0.98.1, on "
                     "webOS 10, using one sometimes left the picture, the sound and HDMI control "
                     "off until the TV was unplugged."),
            "", allowed ? 1 : 0);
    }
    if (data["external"]["active"].boolean())
        row(rows, say::msg("tv.saver.external", "Homebrew screen saver"),
            say::msg("apps.service.active", "Active"), Command(),
            say::msg("ss.external.active",
                     "A third-party homebrew screen saver is currently active on the TV. Selecting "
                     "a Native Tv screen saver will replace it."));
    for (const auto &mode : data["modes"].items())
        if (mode["available"].boolean() || mode["active"].boolean()) {
            bool stock = mode["id"].string() == "stock";
            // Where LG's screen saver is built differently from ours, moving to or from it makes
            // the TV go dark for about a minute.
            bool slow = data["slowSwitch"].boolean() &&
                        (data["current"].string() == "stock" ? !stock : stock);
            row(rows, mode["label"].string(),
                mode["active"].boolean() ? say::msg("ss.inUse", "In use") : "",
                writable ? control("screensaverMode", mode["id"]) : Command(),
                slow ? say::msg("tv.saver.slowSwitch", "Switching to or from LG's screen saver may "
                                                       "leave the screen dark for about a minute.")
                : stock ? say::msg("tv.saver.stock.hint", "LG’s own screen saver.")
                : mode["id"].string() == "black" ? mode["description"].string()
                        : say::msg("tv.saver.ours.hint",
                                   "Replaces LG’s screen saver. Everything on it drifts slowly so "
                                   "no pixel holds the same image."),
                pressAction(say::msg("tv.saver.use", "Press OK to use this one")));
        }
    if (writable) {
        head(rows, say::msg("ss.showNow", "Show it now"));
        row(rows, say::msg("ss.start", "Start screen saver"), "", control("screensaver"),
            say::msg("tv.saver.startScreenSaver.hint",
                     "Shows the screen saver in use straight away, to see how it looks. Any button "
                     "on the remote ends it."),
            pressAction(say::msg("tv.saver.startScreenSaver.press", "Press OK to start it")));
    }
    // The black screen has nothing to dim.
    bool brightness = writable && data["current"].string() != "stock" &&
                      data["current"].string() != "black" && !data["current"].string().empty();
    bool starts = false;
    for (const auto &setting : data["lgSettings"].items())
        starts = starts || setting["section"].string() == "screensaver";
    if (brightness || starts)
        page(rows, say::msg("tv.settings.page.saverOptions", "Options"));
    if (starts) {
        head(rows, say::msg("tv.saver.head.whenItStarts", "When it starts"));
        lgRows(rows, data["lgSettings"], writable, "screensaver");
    }
    if (brightness) {
        bool bright = data["level"].string() == "bright";
        head(rows, say::msg("tv.saver.head.howItLooks", "How it looks"));
        row(rows, say::msg("ss.brightness", "Brightness"),
            bright ? say::msg("ss.bright", "Bright") : say::msg("ss.dim", "Dim"),
            control("screensaverMode", fields({{"mode", data["current"]},
                                               {"level", Json::text(bright ? "dim" : "bright")}})),
            say::msg("tv.saver.brightness.hint",
                     "Dim suits a dark room and is kinder to the panel; bright suits a lit one."),
            say::msg("tv.settings.change", "Change"))
            .choices = {
            {say::msg("ss.bright", "Bright"),
             control("screensaverMode",
                     fields({{"mode", data["current"]}, {"level", Json::text("bright")}}))},
            {say::msg("ss.dim", "Dim"),
             control("screensaverMode",
                     fields({{"mode", data["current"]}, {"level", Json::text("dim")}}))}};
    }
}

// ---- Privacy ---------------------------------------------------------------------------------

std::string adBlockWord(const std::string &mode) {
    return mode == "ads"    ? say::msg("pv.adblock.ads", "Ads & telemetry")
           : mode == "full" ? say::msg("pv.adblock.full", "Everything")
                            : say::msg("common.off", "Off");
}
void privacy(Rows &rows, const Json &data, const std::string &sub) {
    bool consentWritable = data["consentWritable"].boolean(true);
    if (sub == "consents") {
        // Each consent, under the group the server puts it in. Flags with no name and no
        // agreement behind them are one row, not one each.
        std::vector<Json> flags = data["consent"]["known"].items();
        for (const auto &flag : data["consent"]["other"].items())
            flags.push_back(flag);
        std::vector<Json> groups = data["consentGroups"].items();
        if (groups.empty())
            groups.push_back(Json::parse(R"(["unknown",""])"));
        for (const auto &group : groups) {
            std::string id = group[static_cast<size_t>(0)].string();
            std::vector<Json> named;
            long unnamed = 0, unnamedOn = 0;
            for (const auto &flag : flags) {
                if (flag["group"].string("unknown") != id)
                    continue;
                if (id == "unknown" && !flag["settable"].boolean() &&
                    !flag["documents"].boolean()) {
                    ++unnamed;
                    unnamedOn += flag["enabled"].boolean();
                } else
                    named.push_back(flag);
            }
            if (named.empty() && !unnamed)
                continue;
            head(rows, group[static_cast<size_t>(1)].string().empty()
                           ? say::msg("tv.privacy.otherGroup", "Other")
                           : group[static_cast<size_t>(1)].string());
            for (const auto &flag : named) {
                bool can = consentWritable && flag["settable"].boolean();
                std::string why = flag["detail"].string();
                if (!why.empty() && std::string(".!?").find(why.back()) == std::string::npos)
                    why += ".";
                if (flag["agreements"].size()) {
                    std::string names;
                    for (const auto &name : flag["agreements"].items())
                        names += (names.empty() ? "" : ", ") + name.string();
                    Json v = vars({{"names", names}});
                    why += " " + say::msg("tv.privacy.partOf", "Part of: {names}.", v);
                }
                if (flag["sharesWith"].size()) {
                    std::string names;
                    for (const auto &name : flag["sharesWith"].items())
                        names += (names.empty() ? "" : ", ") + name.string();
                    Json v = vars({{"names", names}});
                    why += " " + say::msg("tv.privacy.sharesWith",
                                          "Switching this off also switches off {names}.", v);
                }
                if (!can && !consentWritable)
                    why += " " + say::msg("tv.privacy.controlsOff",
                                          "Controls are switched off in this dashboard’s config.");
                row(rows, flag["label"].string(flag["key"].string()), on(flag["enabled"].boolean()),
                    can ? control("consent",
                                  fields({{"key", flag["key"]},
                                          {"enabled", Json::flag(!flag["enabled"].boolean())}}))
                        : Command(),
                    why, "", flag["enabled"].boolean() ? 1 : 0);
            }
            if (unnamed) {
                Json v = vars({{"n", std::to_string(unnamed)}});
                Json w = vars({{"n", std::to_string(unnamedOn)}});
                row(rows,
                    unnamed == 1 ? say::msg("pv.anon.one", "1 unidentified flag")
                                 : say::msg("pv.anon", "{n} unidentified flags", v),
                    unnamedOn ? say::msg("pv.areaOn", "{n} on", w)
                              : say::msg("pv.allOffShort", "All off"),
                    Command(),
                    say::msg("tv.privacy.anon.hint", "Recorded by the TV, tied to no agreement on "
                                                     "this firmware, and not writable."));
            }
        }
        return;
    }
    if (sub == "services") {
        head(rows, say::msg("tv.service.head.lgDataServices", "LG data services"));
        bool any = false;
        for (const auto &service : data["daemons"].items()) {
            if (service["onDemand"].boolean())
                continue; // an on-demand one starts whenever anything asks it a question
            any = true;
            std::string detail = service["detail"].string();
            if (!detail.empty() && std::string(".!?").find(detail.back()) == std::string::npos)
                detail += ".";
            row(rows, service["label"].string(service["name"].string()),
                service["running"].boolean() ? say::msg("pv.running", "Running")
                                             : say::msg("pv.stopped", "Stopped"),
                Command(),
                (detail.empty() ? "" : detail + " ") +
                    say::msg("tv.service.whereOff",
                             "Switched off in the Apps tab, under Turn off LG’s bloatware services."));
        }
        if (!any)
            row(rows, say::msg("tv.service.noneOnThisTv", "None on this TV"));
        return;
    }
    Json simple = data["simple"];
    if (simple.valid()) {
        page(rows, say::msg("tv.settings.page.overview", "Overview"));
        head(rows, say::msg("tv.privacy.head.trackingAndAdsOverview", "Tracking and ads overview"));
        double total = simple["total"].number();
        Json v = vars({{"n", compactNumber(total)}});
        row(rows,
            total ? (total == 1 ? say::msg("pv.stillOn.one", "1 thing is still on")
                                : say::msg("pv.stillOn", "{n} things are still on", v))
                  : say::msg("pv.allOff", "Tracking and ads are off"),
            total ? (consentWritable ? say::msg("pv.switchAllOff", "Switch it all off")
                                     : say::msg("pv.areaOn", "{n} on", v))
                  : say::msg("pv.allOffShort", "All off"),
            total && consentWritable ? control("privacyAllOff") : Command(),
            total ? say::msg("tv.privacy.allOff.hint",
                             "Includes blocking LG’s ad and tracking servers. If an app stops "
                             "working afterwards, switch blocking off further down.")
                  : say::msg("pv.allOff.watch",
                             "If an LG update switches anything back on, it shows here."),
            pressAction(say::msg("tv.privacy.allOff.press", "Press OK to switch it all off")));
        for (const auto &area : simple["areas"].items()) {
            std::string names;
            for (const auto &item : area["items"].items())
                names += (names.empty() ? "" : ", ") + item["label"].string();
            Json n = vars({{"n", std::to_string(area["items"].size())}});
            Json listed = vars({{"names", names}});
            row(rows, area["name"].string(),
                area["items"].size() ? say::msg("tv.privacy.area.stillOn", "{n} still on", n)
                                     : say::msg("tv.privacy.area.nothingOn", "Nothing on"),
                Command(),
                area["detail"].string() + (names.empty()
                                               ? ""
                                               : " " + say::msg("tv.privacy.area.stillOnNames",
                                                                "Still on: {names}.", listed)));
        }
        if (simple["kept"].size()) {
            std::string names;
            for (const auto &name : simple["kept"].items())
                names += (names.empty() ? "" : ", ") + name.string();
            Json n = vars({{"n", std::to_string(simple["kept"].size())}});
            Json listed = vars({{"names", names}});
            row(rows, say::msg("tv.privacy.leftOn", "Left on"),
                say::msg("tv.privacy.leftOn.count", "{n} kept", n), Command(),
                say::msg("tv.privacy.leftOn.hint",
                         "These run features, so switching it all off leaves them: {names}. They "
                         "can be switched off further down.",
                         listed));
        }
    }
    if (data["adblock"].valid()) {
        std::string mode =
            data["adblock"]["enabled"].boolean() ? data["adblock"]["mode"].string("full") : "off";
        page(rows, say::msg("tv.settings.page.blocking", "Blocking"));
        head(rows, say::msg("tv.privacy.head.adAndTrackerBlocking", "Ad and tracker blocking"));
        std::string detail =
            mode == "ads" ? say::msg("tv.privacy.adsTelemetry.hint",
                                     "Blocks advertising, tracking and diagnostics addresses. LG’s "
                                     "app store and software updates still work.")
            : mode == "full"
                ? say::msg("tv.privacy.everything.hint",
                           "Also blocks LG’s platform services. The LG ThinQ app can no longer "
                           "control the TV, and the app store, software updates and LG Channels "
                           "may stop working.")
                : say::msg("pv.adblock.off.detail", "Nothing is blocked. LG’s advertising and "
                                                    "telemetry addresses work as normal.");
        if (mode != "off")
            detail += " " + say::msg("tv.privacy.blocking.troubleshoot",
                                     "If an app stops working, switch blocking off to check "
                                     "whether it is the cause.");
        row(rows, say::msg("tv.privacy.blocking", "Blocking"), adBlockWord(mode),
            control("setAdBlock",
                    Json::text(mode == "off"   ? "ads"
                               : mode == "ads" ? "full"
                                               : "off"),
                    mode == "ads"),
            detail + " " +
                say::msg("tv.privacy.blocking.streaming", "Streaming apps are untouched."),
            say::msg("tv.settings.change", "Change"))
            .choices = [&] {
            // Everything asks to be confirmed when it is moved to from Ads and telemetry.
            Choices options;
            for (const char *level : {"off", "ads", "full"})
                options.push_back(
                    {adBlockWord(level), control("setAdBlock", Json::text(level),
                                                 std::string(level) == "full" && mode != "full")});
            return options;
        }();
        // The owner's own list, a hosts file sent from the browser dashboard; the row is there with none, to say so.
        Json custom = data["adblock"]["custom"];
        Json v = vars({{"n", compactNumber(custom["count"].number())}});
        row(rows, say::msg("pv.adblock.custom", "Custom ad blocking"),
            !custom.valid() ? say::msg("tv.privacy.custom.none", "No list")
            : !custom["enabled"].boolean()
                ? say::msg("tv.privacy.custom.off", "Off, {n} names kept", v)
            : custom["mode"].string() == "add"
                ? say::msg("tv.privacy.custom.add", "{n} names, with Native Tv’s", v)
                : say::msg("tv.privacy.custom.replace", "{n} names, instead of Native Tv’s", v),
            Command(),
            say::msg("tv.privacy.custom.hint",
                     "Your own list of names to block, sent from the browser dashboard (Privacy, "
                     "under the blocker) and changed or removed there."));
    }
    if (data["tvUpdates"]["blocked"].isBoolean()) {
        bool blocked = data["tvUpdates"]["blocked"].boolean();
        if (!data["adblock"].valid())
            page(rows, say::msg("tv.settings.page.blocking", "Blocking"));
        head(rows, say::msg("tv.privacy.head.systemUpdates", "System updates"));
        row(rows, say::msg("server.tvUpdates.block", "Block TV software updates"),
            blocked ? say::msg("server.tvUpdates.blocked", "Blocked")
                    : say::msg("server.tvUpdates.allowed", "Allowed"),
            control("blockTvUpdates", Json::flag(!blocked)),
            say::msg(
                "tv.privacy.blockTvUpdates.hint",
                "Stops LG installing its firmware updates, which may remove root access. Native Tv "
                "releases are installed through server deployment or the Homebrew Channel."),
            "", blocked ? 1 : 0);
    }
    bool promotions = anyLg(data["lgSettings"], "promotions");
    if (promotions) {
        page(rows, say::msg("tv.settings.page.lgAds", "LG ads"));
        head(rows, say::msg("pv.promotions", "LG ads and promotions"));
        bool anyOn = false;
        for (const auto &setting : data["lgSettings"].items())
            anyOn = anyOn || (setting["section"].string() == "promotions" && setting["on"].boolean());
        row(rows, say::msg("pv.promotions.all", "All LG ads and promotions"), on(anyOn),
            control("lgAds", Json::flag(!anyOn)),
            say::msg("tv.privacy.allAds.hint",
                     "Switches every ad and promotion below on or off together. Each can still be "
                     "set by itself."),
            "", anyOn ? 1 : 0);
        lgRows(rows, data["lgSettings"], true, "promotions");
    }
    if (data["acr"].valid()) {
        page(rows, say::msg("tv.settings.page.tracking", "Tracking"));
        head(rows,
             say::msg("tv.privacy.head.screenContentRecognition", "Screen content recognition"));
        bool enabled = data["acr"]["consentOn"].boolean();
        row(rows,
            data["acr"]["label"].string(say::msg("tv.privacy.acr", "Screen content recognition")),
            on(enabled), consentWritable ? control("acr", Json::flag(!enabled)) : Command(),
            say::msg("tv.privacy.acr.hint",
                     "LG’s ACR captures what is on screen, from apps and HDMI alike, to work out "
                     "what you watch and target ads at you."),
            "", enabled ? 1 : 0);
        row(rows, say::msg("pv.acr.service", "Recognition service"),
            data["acr"]["active"].boolean()    ? say::msg("pv.acr.recognizing", "Recognizing")
            : data["acr"]["running"].boolean() ? say::msg("pv.acr.idle", "Running, idle")
                                               : say::msg("pv.notRunning", "Not running"),
            Command(),
            say::msg("tv.privacy.acrService.hint",
                     "acr2, LG’s recognition program. The TV starts it at boot, and anything that "
                     "asks it a question starts it again."));
        if (data["acr"]["captureState"].valid())
            row(rows, say::msg("tv.privacy.capture", "Capture"),
                data["acr"]["capturing"].boolean() ? say::msg("pv.capturing", "Capturing")
                                                   : say::msg("pv.stopped", "Stopped"),
                Command(),
                say::msg("tv.privacy.capture.hint", "Whether it is sampling the screen right now, "
                                                    "as opposed to merely being switched on."));
    }
    Json ad = data["advertisingId"];
    if (ad["available"].boolean() || ad["lmtKnown"].boolean()) {
        if (!data["acr"].valid())
            page(rows, say::msg("tv.settings.page.tracking", "Tracking"));
        head(rows, say::msg("pv.adid", "Advertising identifier"));
        bool limited = ad["limitTracking"].boolean();
        row(rows, ad["limitTrackingLabel"].string(say::msg("pv.lmt", "Limit ad tracking")),
            on(limited),
            consentWritable ? control("limitAdTracking", Json::flag(!limited)) : Command(),
            say::msg("tv.privacy.lmt.hint",
                     "When on, apps are asked not to use the TV’s advertising ID to profile the "
                     "viewer. It is a request, not an enforcement."),
            "", limited ? 1 : 0);
        // With the ad service stopped there is no ID, and a reset would start it.
        if (ad["available"].boolean())
            row(rows, say::msg("tv.privacy.resetTheAdvertisingId", "Reset the advertising ID"), "",
                control("resetAdId"),
                say::msg("tv.privacy.resetTheAdvertisingId.hint",
                         "Issues a fresh identifier, which breaks the link to whatever has been "
                         "collected against the old one. Harmless to repeat."),
                pressAction(
                    say::msg("tv.privacy.resetTheAdvertisingId.press", "Press OK to reset")));
    }
    if (data["consent"]["known"].valid()) {
        long flags =
            static_cast<long>(data["consent"]["known"].size() + data["consent"]["other"].size());
        long enabled = 0;
        for (const auto &flag : data["consent"]["known"].items())
            enabled += flag["enabled"].boolean();
        for (const auto &flag : data["consent"]["other"].items())
            enabled += flag["enabled"].boolean();
        Json v = vars({{"n", std::to_string(enabled)}, {"total", std::to_string(flags)}});
        page(rows, say::msg("tv.settings.page.data", "Data"));
        head(rows, say::msg("tv.privacy.head.dataConsents", "Data consents"));
        row(rows, say::msg("tv.privacy.consentsSwitchedOn", "Consents switched on"),
            say::msg("tv.privacy.nOfTotal", "{n} of {total}", v), navigate("sub:consents"),
            say::msg("tv.privacy.consentsSwitchedOn.hint",
                     "The agreements the TV records against this account, each with what it lets "
                     "LG do."),
            say::msg("tv.settings.open", "Open"));
    }
    long supervised = 0, running = 0;
    for (const auto &service : data["daemons"].items())
        if (!service["onDemand"].boolean()) {
            ++supervised;
            running += service["running"].boolean();
        }
    if (supervised) {
        if (!data["consent"]["known"].valid())
            page(rows, say::msg("tv.settings.page.data", "Data"));
        Json v = vars({{"n", std::to_string(running)}, {"total", std::to_string(supervised)}});
        row(rows, say::msg("tv.privacy.lgDataServicesRunning", "LG data services running"),
            say::msg("tv.privacy.nOfTotal", "{n} of {total}", v), navigate("sub:services"),
            say::msg("tv.privacy.lgDataServicesRunning.hint",
                     "Background services that collect and send usage data. They are switched off "
                     "in the Apps tab."),
            say::msg("tv.settings.open", "Open"));
    }
}

// ---- Home ------------------------------------------------------------------------------------

void homeSettings(Rows &rows, const Json &data, const std::string &sub) {
    bool writable = data["writable"].boolean(true);
    Json layout = data["layout"];
    if (sub == "channels") {
        head(rows, say::msg("home.ch.addTitle", "Add channels"));
        Json ids = layout["channels"];
        for (const auto &channel : data["channels"].items()) {
            bool added = false;
            for (const auto &id : ids.items())
                if (id.string() == channel["id"].string())
                    added = true;
            if (added)
                continue;
            Json next = Json::array();
            for (const auto &id : ids.items())
                next.add(id);
            next.add(channel["id"]);
            row(rows, channel["number"].string() + " " + channel["name"].string(), "",
                writable ? post(Operation::SetHomeLayout, fields({{"channels", next}})) : Command(),
                say::msg("tv.home.ch.hint", "From the TV’s own channel list. Each one added goes "
                                            "on the end of the Channels row."),
                pressAction(say::msg("tv.home.ch.add", "Press OK to add it")));
        }
        if (rows.size() == 1)
            row(rows, say::msg("home.ch.allAdded", "Every channel is on Home."));
        return;
    }
    if (sub == "kinds") {
        // The kinds in their order; OK on one moves it up past the one above.
        head(rows, say::msg("home.kinds", "Kinds"));
        Json kinds = data["catalogue"]["kinds"];
        std::vector<std::string> keys;
        for (size_t i = 0; i < kinds.size(); ++i)
            keys.push_back(kinds[i]["key"].string());
        for (size_t i = 0; i < kinds.size(); ++i) {
            std::vector<std::string> moved = keys;
            if (i > 0)
                std::swap(moved[i - 1], moved[i]);
            Json next = Json::array();
            for (const std::string &key : moved)
                next.add(Json::text(key));
            row(rows, kinds[i]["label"].string(), "",
                writable && i > 0 ? post(Operation::SetHomeLayout, fields({{"kinds", next}}))
                                  : Command(),
                say::msg("home.kindsDesc", "The rows of the Apps screen, top to bottom. On the Apps "
                                           "screen itself, holding OK on a row moves it."),
                i > 0 ? pressAction(say::msg("tv.home.kind.up", "Press OK to move it up one")) : "");
        }
        return;
    }
    page(rows, say::msg("home.title", "Native Tv Home"));
    head(rows, say::msg("home.title", "Native Tv Home"));
    if (data["supported"].valid() && !data["supported"]["ok"].boolean() &&
        !data["enabled"].boolean()) {
        row(rows, say::msg("home.unavailable", "Not available on this TV"), "", Command(),
            data["supported"]["reason"].string());
        return;
    }
    bool enabled = data["enabled"].boolean();
    row(rows, say::msg("home.title", "Native Tv Home"), on(enabled),
        writable
            ? post(enabled ? Operation::DisableHome : Operation::EnableHome, Json::object(), true)
            : Command(),
        enabled
            ? say::msg("home.isOnDesc", "It opens in LG’s Home’s place: with the Home and Back "
                                        "buttons, and when the TV wakes on Home.")
            : say::msg("home.isOffDesc", "Nothing of LG’s is changed: Native Tv Home is laid over "
                                         "LG’s Home while on, and switching off takes it away."),
        "", enabled ? 1 : 0)
        .reverse = post(enabled ? Operation::EnableHome : Operation::DisableHome, Json::object());
    if (data["hideLgHomeAtStartAvailable"].boolean(true))
        row(rows, say::msg("home.hideLg", "Hide LG’s Home while the TV starts"),
            on(data["hideLgHomeAtStart"].boolean()),
            writable ? post(Operation::SetHomeStartLayout,
                            fields({{"hideLgHomeAtStart",
                                     Json::flag(!data["hideLgHomeAtStart"].boolean())}}),
                            !data["hideLgHomeAtStart"].boolean())
                     : Command(),
            say::msg("home.hideLgDesc",
                     "For about 25 seconds after a cold start, until Native Tv Home takes its "
                     "place, LG’s Home opens as a black screen instead of its own. If Native Tv "
                     "stops running, for example after a firmware update removes root, LG’s Home "
                     "stays black until it is put back over SSH."),
            "", data["hideLgHomeAtStart"].boolean() ? 1 : 0);
    row(rows, say::msg("home.startup", "Power-on screen"),
        data["openOnPowerOn"].boolean() ? say::msg("home.startupNativeTv", "Native Tv Home")
                                        : say::msg("home.startupLg", "Follow LG setting"),
        writable && enabled
            ? post(Operation::SetHomeStartup,
                   fields({{"openOnPowerOn", Json::flag(!data["openOnPowerOn"].boolean())}}))
            : Command(),
        say::msg("home.startupDesc",
                 "Choose Native Tv Home to skip LG Home and open it when ready, or follow LG's "
                 "power-on setting. Full shutdown still needs time to start. Choosing another app "
                 "or input cancels the automatic opening."),
        "", data["openOnPowerOn"].boolean() ? 1 : 0);
    page(rows, say::msg("tv.appearance.head.appearance", "Appearance"));
    head(rows, say::msg("tv.appearance.head.appearance", "Appearance"));
    struct Switch {
        const char *key;
        std::string label, hint;
        bool fallback;
    };
    const std::vector<Switch> switches = {
        {"appNames", say::msg("home.appNames", "Show app names"),
         say::msg("home.appNamesDesc", "Show names below app icons. Switching off keeps icons "
                                       "only; input and channel names stay visible."),
         true},
        {"showInputs", say::msg("home.showInputs", "Show inputs"),
         say::msg("home.showInputsDesc", "Show the Inputs row. Switching off leaves the whole row "
                                         "out; the inputs themselves stay on the TV."),
         true},
        {"showLive", say::msg("home.showLive", "Show Live"),
         say::msg("home.showLiveDesc",
                  "Show the Live tab, with the channels added to Native Tv Home. Off by default. "
                  "Switching off leaves the whole tab out; the added channels are kept."),
         false},
        {"showLgHome", say::msg("home.showLgHome", "Show the LG Home tile"),
         say::msg("home.showLgHomeDesc",
                  "Show the tile that opens LG’s Home at the end of the Apps row. Holding the Home "
                  "button still opens LG’s Home."),
         true},
        {"picture", say::msg("home.picture", "Picture details"),
         say::msg(
             "home.pictureDesc",
             "In the top right of Native Tv Home: the resolution, refresh rate and HDR of what "
             "was last on, and the picture mode and brightness it gets."),
         false}};
    for (const auto &setting : switches) {
        bool shown = layout[setting.key].boolean(setting.fallback);
        row(rows, setting.label, on(shown),
            writable ? post(Operation::SetHomeLayout, fields({{setting.key, Json::flag(!shown)}}))
                     : Command(),
            setting.hint, "", shown ? 1 : 0);
    }
    bool twelve = layout["clockFormat"].string() == "12h";
    row(rows, say::msg("home.clockFormat", "Clock format"),
        twelve ? say::msg("home.clock12", "12-hour") : say::msg("home.clock24", "24-hour"),
        writable ? post(Operation::SetHomeLayout,
                        fields({{"clockFormat", Json::text(twelve ? "24h" : "12h")}}))
                 : Command(),
        say::msg("home.clockFormatDesc",
                 "Choose 12-hour time with AM/PM or 24-hour time on Native Tv Home."),
        say::msg("tv.settings.change", "Change"))
        .choices = {{say::msg("home.clock12", "12-hour"),
                     post(Operation::SetHomeLayout, fields({{"clockFormat", Json::text("12h")}}))},
                    {say::msg("home.clock24", "24-hour"),
                     post(Operation::SetHomeLayout, fields({{"clockFormat", Json::text("24h")}}))}};
    row(rows, say::msg("home.profile", "Profile"), "", navigate("profiles"),
        say::msg("home.profile.tvHint",
                 "Each profile has its own layout, which the rows and settings here change. Names, "
                 "colours and removing are on the browser dashboard’s Home tab."),
        say::msg("tv.settings.open", "Open"));
    row(rows, say::msg("home.reset", "Reset the layout"), "",
        writable ? post(Operation::SetHomeLayout, fields({{"reset", Json::flag(true)}}), true)
                 : Command(),
        say::msg("home.resetDesc", "The rows, apps and inputs go back to their first order, with "
                                   "LG’s promotions hidden again. Channels stay as they are."),
        pressAction(say::msg("tv.home.reset.press", "Press OK, then again, to reset")));
    Json catalogue = data["catalogue"];
    bool channelsListed = false;
    for (const auto &kind : {"app", "dev", "ch"}) {
        const char *items = std::string(kind) == "app"   ? "apps"
                            : std::string(kind) == "dev" ? "devices"
                                                         : "channels";
        if (!catalogue[items].size())
            continue;
        std::string rowTitle = std::string(kind) == "app" ? say::msg("home.row.apps", "Apps")
                               : std::string(kind) == "dev"
                                   ? say::msg("home.row.inputs", "Inputs")
                                   : say::msg("home.row.channels", "Channels");
        page(rows, rowTitle);
        head(rows, rowTitle);
        if (std::string(kind) == "ch")
            channelsListed = true;
        const bool apps = std::string(kind) == "app";
        const std::string sortValue = layout["sort"][kind].string();
        const bool recent = sortValue == "recent", byKind = apps && sortValue == "kind";
        // OK steps Your order, Last opened and, for the apps, By kind round.
        const std::string next = recent ? (apps ? "kind" : "custom") : byKind ? "custom" : "recent";
        auto sortTo = [&](const char *value) {
            return post(Operation::SetHomeLayout,
                        fields({{"sort", fields({{kind, Json::text(value)}})}}));
        };
        MenuRow &order = row(
            rows, say::msg("home.sort", "Order"),
            recent   ? say::msg("home.sortRecent", "Last opened")
            : byKind ? say::msg("home.sortKind", "By kind")
                     : say::msg("home.sortCustom", "Your order"),
            writable ? sortTo(next.c_str()) : Command(),
            recent ? say::msg("home.sortRecentDesc",
                              "The tile opened last comes first, whether it was opened from Native "
                              "Tv Home or not. Tiles not yet opened follow in this row’s own order.")
            : byKind ? say::msg("home.sortKindDesc",
                                "Rows by kind on the Apps screen, in the order under Kinds. Tiles "
                                "keep their order within a kind.")
                     : say::msg("tv.home.sort.hint", "As arranged on the browser dashboard’s Home tab."),
            say::msg("tv.settings.change", "Change"));
        order.choices = {{say::msg("home.sortCustom", "Your order"), sortTo("custom")},
                         {say::msg("home.sortRecent", "Last opened"), sortTo("recent")}};
        if (apps)
            order.choices.push_back({say::msg("home.sortKind", "By kind"), sortTo("kind")});
        if (byKind)
            row(rows, say::msg("home.kinds", "Kinds"), "", navigate("sub:kinds"),
                say::msg("home.kindsDesc", "The rows of the Apps screen, top to bottom. On the Apps "
                                           "screen itself, holding OK on a row moves it."),
                say::msg("tv.settings.open", "Open"));
        // One Shown/Hidden row per tile. The apps have two lists: the row on Native Tv Home and the
        // Apps tab, each with its own hidden apps.
        auto tiles = [&](const std::string &list) {
            const Json current = list == "appsTab" && !layout["hidden"]["appsTab"].isArray()
                                     ? layout["hidden"]["app"]
                                     : layout["hidden"][list];
            for (const auto &item : catalogue[items].items()) {
                std::string id = item["id"].string();
                if (id == "lg-home")
                    continue;
                Json hidden = Json::array();
                bool wasHidden = false;
                for (const auto &value : current.items()) {
                    if (value.string() == id)
                        wasHidden = true;
                    else
                        hidden.add(value);
                }
                if (!wasHidden)
                    hidden.add(item["id"]);
                row(rows, item["title"].string(item["name"].string(id)),
                    wasHidden ? say::msg("home.hidden", "Hidden") : say::msg("home.shown", "Shown"),
                    writable ? post(Operation::SetHomeLayout,
                                    fields({{"hidden", fields({{list, hidden}})}}))
                             : Command(),
                    list == "app" ? say::msg("tv.home.app.hint",
                                             "Hold OK on an app on Native Tv Home to move or hide it.")
                    : list == "appsTab"
                        ? say::msg("tv.home.appsTab.hint",
                                   "Hold OK on an app on the Apps tab to move or hide it there.")
                        : say::msg("tv.home.tile.hint",
                                   "Tiles are moved on the browser dashboard’s Home tab."),
                    "", wasHidden ? 0 : 1)
                    // The list as it is now sets it back.
                    .reverse = post(Operation::SetHomeLayout,
                                    fields({{"hidden", fields({{list, current}})}}));
            }
        };
        if (apps) {
            head(rows, say::msg("home.onHome", "On Native Tv Home"));
            tiles("app");
            head(rows, say::msg("home.onAppsTab", "On the Apps tab"));
            tiles("appsTab");
        } else {
            tiles(kind);
        }
    }
    if (!channelsListed) {
        page(rows, say::msg("home.row.channels", "Channels"));
        head(rows, say::msg("home.row.channels", "Channels"));
    }
    row(rows, say::msg("home.ch.addTitle", "Add channels"),
        say::msg("tv.apps.install.browse", "Browse"), navigate("sub:channels"),
        say::msg(
            "tv.home.ch.hint",
            "From the TV’s own channel list. Each one added goes on the end of the Channels row."),
        say::msg("tv.settings.open", "Open"));
}

// ---- Service Menu and Settings -----------------------------------------------------------------

void service(Rows &rows, const Json &data) {
    head(rows, say::msg("tab.serviceMenu", "Service menu"));
    if (data["lockable"].boolean()) {
        bool locked = data["locked"].boolean();
        row(rows, say::msg("tv.svc.lock", "Lock"),
            locked ? say::msg("svc.locked", "Locked") : say::msg("svc.unlocked", "Unlocked"),
            data["writable"].boolean()
                ? control("serviceMenuLock", fields({{"locked", Json::flag(!locked)}}))
                : Command(),
            say::msg("tv.svc.lock.hint", "Locked keeps the engineering menus shut even to someone "
                                         "who knows the button sequence."),
            "", locked ? 1 : 0);
    }
    if (data["app"].boolean() && data["writable"].boolean()) {
        row(rows, say::msg("tv.svc.openEzAdjust", "Open EZ Adjust"), "",
            control("serviceMenuOpen", fields({{"menu", Json::text("ezAdjust")}})),
            say::msg("tv.svc.openEzAdjust.hint",
                     "LG’s engineering menu for hardware configuration, Tool Options, and white "
                     "balance calibration. The TV asks for a four-digit PIN — usually 0413, though "
                     "it varies by model and region."),
            pressAction(say::msg("tv.svc.openEzAdjust.press", "Press OK to open")));
        row(rows, say::msg("tv.svc.openInStart", "Open In Start"), "",
            control("serviceMenuOpen", fields({{"menu", Json::text("inStart")}})),
            say::msg("tv.svc.openInStart.hint",
                     "LG’s factory diagnostic menu for system status, total power-on hours (UTT), "
                     "error logs, and OLED protections (TPC and GSR). The TV asks for a four-digit "
                     "PIN — usually 0413, though it varies by model and region."),
            pressAction(say::msg("tv.svc.openInStart.press", "Press OK to open")));
    }
    if (rows.size() == 1)
        row(rows, say::msg("tv.svc.notAvailableOnThis", "Not available on this TV"));
}
void settings(Rows &rows, const Json &data) {
    head(rows, say::msg("tv.settings.head.thisDashboard", "This dashboard"));
    bool network = data["network"].boolean();
    std::string web =
        say::msg("tv.settings.web.hint", "A fuller version of this dashboard, with more controls, "
                                         "for a browser on a phone or computer.") +
        " ";
    row(rows, say::msg("tv.settings.webDashboard", "Web dashboard"), on(network),
        data["writable"].boolean()
            ? post(Operation::Setup,
                   fields({{"action", Json::text("network")}, {"open", Json::flag(!network)}}))
            : Command(),
        web + (network
                   ? (data["token"].boolean()
                          ? say::msg("tv.settings.web.token",
                                     "It is on for your home network, and needs the dashboard’s "
                                     "access token, which the code under Address includes.")
                          : say::msg("tv.settings.web.open",
                                     "It is on for your home network, with no password, so anyone "
                                     "on that network can use it."))
                   : say::msg("tv.settings.web.off",
                              "It is off, so only this TV can reach the dashboard.")),
        "", network ? 1 : 0);
    if (network && data["address"].valid())
        row(rows, say::msg("tv.settings.address", "Address"), data["address"].string(),
            navigate("qr:address"),
            say::msg("tv.settings.address.hint", "Where the web dashboard is, for a browser on a "
                                                 "phone or computer on your home network.") +
                " " + say::msg("tv.settings.address.scan", "Scan to open it on your phone."),
            say::msg("tv.settings.open", "Open"));
    Json ha = data["homeAssistant"];
    std::string state = ha["state"].string();
    row(rows, say::msg("tv.settings.homeAssistant", "Home Assistant"),
        !ha["configured"].boolean() ? say::msg("tv.settings.ha.notSetUp", "Not set up")
        : state == "connected"      ? say::msg("mqtt.state.connected", "Connected")
        : state == "error"          ? say::msg("tv.settings.ha.notConnecting", "Not connecting")
                                    : say::msg("mqtt.state.connecting", "Connecting"),
        data["writable"].boolean() ? navigate("setup:handoff") : Command(),
        state == "connected"
            ? say::msg("tv.settings.ha.connected",
                       "Connected to Home Assistant through your MQTT broker. The TV reports its "
                       "state and takes commands, so it can join your automations.")
        : ha["configured"].boolean()
            ? say::msg("tv.settings.ha.failing",
                       "The MQTT broker’s details are saved, but the TV cannot connect to it yet. "
                       "Set it up again to check them.")
            : say::msg("tv.settings.ha.none",
                       "Connects this TV to Home Assistant through an MQTT broker, such as Home "
                       "Assistant’s Mosquitto add-on, so it can join your automations. The "
                       "broker’s details are entered on your phone."),
        say::msg("tv.settings.open", "Open"));
    row(rows, say::msg("theme", "Theme"), say::msg("theme.dark", "Dark"), navigate("theme"),
        say::msg("tv.appearance.theme.hint", "Switches the dashboard interface between Dark (true "
                                             "black for OLED) and Light theme."));
    row(rows, say::msg("home.title", "Native Tv Home"), say::msg("tv.settings.open", "Open"),
        navigate("home"), say::msg("tv.settings.openHome", "Open Native Tv Home on the TV."),
        say::msg("tv.settings.open", "Open"));
}
} // namespace

std::vector<Tab> dashboardTabs() {
    return {{"system", "Metrics", Operation::Metrics},
            {"apps", "Apps", Operation::Apps},
            {"oled", "OLED Care", Operation::OledCare},
            {"game", "Game", Operation::Game},
            {"saver", "Screen Saver", Operation::ScreenSaver},
            {"home", "Home", Operation::HomeStatus},
            {"privacy", "Privacy", Operation::Privacy},
            {"service", "Service Menu", Operation::ServiceMenu},
            {"options", "System", Operation::SystemSettings},
            {"settings", "Settings", Operation::Setup}};
}
std::vector<MenuRow> dashboardRows(const std::string &tab, const Json &data, const Json &caps,
                                   const std::string &sub) {
    Rows rows;
    if (!data.valid()) {
        row(rows, say::msg("tv.loading", "Loading…"));
        return rows;
    }
    if (data["ok"].valid() && !data["ok"].boolean()) {
        row(rows, say::msg("tv.native.couldNotLoad", "Could not load this screen"), "",
            navigate("refresh"),
            data["error"].string(say::msg("tv.native.pressToRetry", "Press OK to retry.")),
            say::msg("tv.native.retry", "Try again"));
        return rows;
    }
    if (tab == "system")
        metrics(rows, data);
    else if (tab == "apps")
        apps(rows, data);
    else if (tab == "options")
        options(rows, data, caps);
    else if (tab == "oled")
        oled(rows, data);
    else if (tab == "game") {
        if (data["available"].valid() && !data["available"].boolean())
            row(rows, say::msg("tv.game.none", "This TV has no Game Optimizer"));
        else {
            gameRows(rows, data["rows"], data["writable"].boolean(true));
        }
    } else if (tab == "saver")
        saver(rows, data);
    else if (tab == "privacy")
        privacy(rows, data, sub);
    else if (tab == "home")
        homeSettings(rows, data, sub);
    else if (tab == "service")
        service(rows, data);
    else if (tab == "settings")
        settings(rows, data);
    if (rows.empty())
        row(rows, say::msg("tv.msg.nothing", "Nothing to show here"));
    return rows;
}
} // namespace nativetv
