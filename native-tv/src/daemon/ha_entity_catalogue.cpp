#include "daemon/ha_entity_catalogue.hpp"
#include "domain/say.hpp"
#include <algorithm>
#include <cstddef>
#include <set>
#include <string>

namespace nativetv { namespace daemon {
namespace {
const Json &catalogueData() {
    static const Json value = Json::parse(R"GHJSON([
  {
    "id": "display_panel",
    "type": "switch",
    "name": "Display Panel",
    "cat": "controls"
  },
  {
    "id": "mute",
    "type": "switch",
    "name": "Mute",
    "cat": "controls"
  },
  {
    "id": "volume",
    "type": "number",
    "name": "Volume",
    "cat": "controls"
  },
  {
    "id": "input_source",
    "type": "select",
    "name": "Input Source",
    "cat": "controls"
  },
  {
    "id": "piccap",
    "type": "switch",
    "name": "PicCap Capture",
    "cat": "controls"
  },
  {
    "id": "screen_notification",
    "type": "text",
    "name": "Screen Notification",
    "cat": "controls"
  },
  {
    "id": "picture_mode",
    "type": "select",
    "name": "Picture Mode (Select)",
    "cat": "controls"
  },
  {
    "id": "energy_saving",
    "type": "select",
    "name": "Energy Saving Step",
    "cat": "controls"
  },
  {
    "id": "sound_output",
    "type": "select",
    "name": "Sound Output",
    "cat": "controls"
  },
  {
    "id": "app",
    "type": "select",
    "name": "Application",
    "cat": "controls"
  },
  {
    "id": "active_app",
    "type": "sensor",
    "name": "Active App",
    "cat": "controls"
  },
  {
    "id": "play_state",
    "type": "sensor",
    "name": "Player State",
    "cat": "controls"
  },
  {
    "id": "sound_format",
    "type": "sensor",
    "name": "Sound Format",
    "cat": "controls"
  },
  {
    "id": "volume_up",
    "type": "button",
    "name": "Volume Up",
    "cat": "controls"
  },
  {
    "id": "volume_down",
    "type": "button",
    "name": "Volume Down",
    "cat": "controls"
  },
  {
    "id": "remote_up",
    "type": "button",
    "name": "Remote Up",
    "cat": "controls"
  },
  {
    "id": "remote_down",
    "type": "button",
    "name": "Remote Down",
    "cat": "controls"
  },
  {
    "id": "remote_left",
    "type": "button",
    "name": "Remote Left",
    "cat": "controls"
  },
  {
    "id": "remote_right",
    "type": "button",
    "name": "Remote Right",
    "cat": "controls"
  },
  {
    "id": "remote_ok",
    "type": "button",
    "name": "Remote OK",
    "cat": "controls"
  },
  {
    "id": "remote_back",
    "type": "button",
    "name": "Remote Back",
    "cat": "controls"
  },
  {
    "id": "remote_home",
    "type": "button",
    "name": "Remote Home",
    "cat": "controls"
  },
  {
    "id": "remote_button",
    "type": "event",
    "name": "Remote Button",
    "cat": "controls"
  },
  {
    "id": "play",
    "type": "button",
    "name": "Play",
    "cat": "controls"
  },
  {
    "id": "pause",
    "type": "button",
    "name": "Pause",
    "cat": "controls"
  },
  {
    "id": "play_pause",
    "type": "button",
    "name": "Play / Pause",
    "cat": "controls"
  },
  {
    "id": "stop",
    "type": "button",
    "name": "Stop",
    "cat": "controls"
  },
  {
    "id": "rewind",
    "type": "button",
    "name": "Rewind",
    "cat": "controls"
  },
  {
    "id": "fast_forward",
    "type": "button",
    "name": "Fast Forward",
    "cat": "controls"
  },
  {
    "id": "screensaver",
    "type": "button",
    "name": "Screen Saver",
    "cat": "controls"
  },
  {
    "id": "screen_saver_active",
    "type": "binary_sensor",
    "name": "Screen Saver Active",
    "cat": "controls"
  },
  {
    "id": "screensaver_mode",
    "type": "select",
    "name": "Screen Saver Mode",
    "cat": "controls"
  },
  {
    "id": "restart",
    "type": "button",
    "name": "Restart",
    "cat": "controls"
  },
  {
    "id": "power_off",
    "type": "button",
    "name": "Power Off",
    "cat": "controls"
  },
  {
    "id": "power_on",
    "type": "button",
    "name": "Power On",
    "cat": "controls"
  },
  {
    "id": "power",
    "type": "binary_sensor",
    "name": "Power",
    "cat": "controls"
  },
  {
    "id": "oled_panel_hours",
    "type": "sensor",
    "name": "OLED Panel Hours",
    "cat": "oled"
  },
  {
    "id": "oled_hours_since_compensation",
    "type": "sensor",
    "name": "Hours Since Compensation",
    "cat": "oled"
  },
  {
    "id": "oled_hours_until_compensation",
    "type": "sensor",
    "name": "Hours Until Compensation",
    "cat": "oled"
  },
  {
    "id": "oled_hours_since_refresher",
    "type": "sensor",
    "name": "Hours Since Pixel Refresher",
    "cat": "oled"
  },
  {
    "id": "oled_hours_until_refresher",
    "type": "sensor",
    "name": "Hours Until Pixel Refresher",
    "cat": "oled"
  },
  {
    "id": "oled_compensation_status",
    "type": "sensor",
    "name": "Compensation Status",
    "cat": "oled"
  },
  {
    "id": "oled_refresher_status",
    "type": "sensor",
    "name": "Pixel Refresher Status",
    "cat": "oled"
  },
  {
    "id": "oled_screen_shift",
    "type": "switch",
    "name": "Screen Shift",
    "cat": "oled"
  },
  {
    "id": "oled_logo_dimming",
    "type": "select",
    "name": "Logo Luminance Adjustment",
    "cat": "oled"
  },
  {
    "id": "oled_short_cycles",
    "type": "sensor",
    "name": "Compensation Cycles Completed",
    "cat": "oled"
  },
  {
    "id": "oled_refresher_cycles",
    "type": "sensor",
    "name": "Pixel Refresher Cycles Completed",
    "cat": "oled"
  },
  {
    "id": "oled_failure_alerts",
    "type": "sensor",
    "name": "Panel Maintenance Alerts",
    "cat": "oled"
  },
  {
    "id": "oled_asbl_dimmer",
    "type": "binary_sensor",
    "name": "ASBL Dimming Active",
    "cat": "oled"
  },
  {
    "id": "pixel_refresher_schedule",
    "type": "switch",
    "name": "Pixel Refresher on Next Standby",
    "cat": "oled"
  },
  {
    "id": "oled_cell_type",
    "type": "sensor",
    "name": "OLED Cell Type",
    "cat": "oled"
  },
  {
    "id": "tcon_firmware",
    "type": "sensor",
    "name": "T-Con Firmware",
    "cat": "oled"
  },
  {
    "id": "dynamic_range",
    "type": "sensor",
    "name": "Dynamic Range",
    "cat": "video"
  },
  {
    "id": "picture_mode",
    "type": "sensor",
    "name": "Picture Mode",
    "cat": "video"
  },
  {
    "id": "oled_light",
    "type": "number",
    "name": "OLED Light",
    "cat": "video"
  },
  {
    "id": "video_signal",
    "type": "sensor",
    "name": "Video Signal",
    "cat": "video"
  },
  {
    "id": "hdmi_link_mode",
    "type": "sensor",
    "name": "HDMI Link Mode",
    "cat": "video"
  },
  {
    "id": "hdmi_chroma",
    "type": "sensor",
    "name": "HDMI Chroma",
    "cat": "video"
  },
  {
    "id": "hdmi_hdcp",
    "type": "sensor",
    "name": "HDMI HDCP Version",
    "cat": "video"
  },
  {
    "id": "hdmi_allm",
    "type": "binary_sensor",
    "name": "HDMI ALLM",
    "cat": "video"
  },
  {
    "id": "hdmi_vrr",
    "type": "binary_sensor",
    "name": "HDMI VRR",
    "cat": "video"
  },
  {
    "id": "video_colorimetry",
    "type": "sensor",
    "name": "Colorimetry",
    "cat": "video"
  },
  {
    "id": "panel_dimming",
    "type": "sensor",
    "name": "Panel Dimming",
    "cat": "video"
  },
  {
    "id": "ambient_light",
    "type": "sensor",
    "name": "Ambient Light",
    "cat": "video"
  },
  {
    "id": "soc_temperature",
    "type": "sensor",
    "name": "SoC Temperature",
    "cat": "system"
  },
  {
    "id": "cpu_load",
    "type": "sensor",
    "name": "CPU Usage",
    "cat": "system"
  },
  {
    "id": "memory_usage",
    "type": "sensor",
    "name": "Memory Usage",
    "cat": "system"
  },
  {
    "id": "swap_usage",
    "type": "sensor",
    "name": "Swap Usage",
    "cat": "system"
  },
  {
    "id": "wifi_signal",
    "type": "sensor",
    "name": "Wi-Fi Signal",
    "cat": "system"
  },
  {
    "id": "download_rate",
    "type": "sensor",
    "name": "Download Rate",
    "cat": "system"
  },
  {
    "id": "upload_rate",
    "type": "sensor",
    "name": "Upload Rate",
    "cat": "system"
  },
  {
    "id": "flash_health",
    "type": "sensor",
    "name": "Flash Storage Health",
    "cat": "system"
  },
  {
    "id": "flash_wear",
    "type": "sensor",
    "name": "Flash Wear Level",
    "cat": "system"
  },
  {
    "id": "soc_architecture",
    "type": "sensor",
    "name": "SoC Architecture",
    "cat": "system"
  },
  {
    "id": "gpu_clock",
    "type": "sensor",
    "name": "GPU Clock",
    "cat": "system"
  },
  {
    "id": "app_storage_free",
    "type": "sensor",
    "name": "App Storage Available",
    "cat": "system"
  },
  {
    "id": "mac_address",
    "type": "sensor",
    "name": "MAC Address",
    "cat": "system"
  },
  {
    "id": "uptime",
    "type": "sensor",
    "name": "Uptime",
    "cat": "system"
  },
  {
    "id": "watch_time_today",
    "type": "sensor",
    "name": "Watch Time Today",
    "cat": "system"
  },
  {
    "id": "watch_time_week",
    "type": "sensor",
    "name": "Watch Time, Last 7 Days",
    "cat": "system"
  },
  {
    "id": "watch_top_app_today",
    "type": "sensor",
    "name": "Most Watched Today",
    "cat": "system"
  },
  {
    "id": "audio_output",
    "type": "sensor",
    "name": "Audio Output",
    "cat": "diagnostics"
  },
  {
    "id": "remote_battery",
    "type": "sensor",
    "name": "Magic Remote Battery",
    "cat": "diagnostics"
  },
  {
    "id": "sleep_timer",
    "type": "select",
    "name": "Sleep Timer",
    "cat": "diagnostics"
  },
  {
    "id": "on_timer",
    "type": "switch",
    "name": "On Timer",
    "cat": "diagnostics"
  },
  {
    "id": "off_timer",
    "type": "switch",
    "name": "Off Timer",
    "cat": "diagnostics"
  },
  {
    "id": "standby_light",
    "type": "switch",
    "name": "Standby Light",
    "cat": "diagnostics"
  },
  {
    "id": "logo_light",
    "type": "switch",
    "name": "Logo Light",
    "cat": "diagnostics"
  },
  {
    "id": "ad_blocker",
    "type": "switch",
    "name": "Ad Blocker",
    "cat": "diagnostics"
  },
  {
    "id": "tvweb_version",
    "type": "sensor",
    "name": "tvweb Version",
    "cat": "diagnostics"
  },
  {
    "id": "sound_mode",
    "type": "select",
    "name": "Sound Mode",
    "cat": "controls"
  },
  {
    "id": "bluetooth_speaker_mode",
    "type": "switch",
    "name": "Bluetooth Speaker Mode",
    "cat": "controls"
  },
  {
    "id": "simplink",
    "type": "switch",
    "name": "SIMPLINK (HDMI-CEC)",
    "cat": "controls"
  },
  {
    "id": "auto_power_sync",
    "type": "switch",
    "name": "Auto Power Sync",
    "cat": "controls"
  },
  {
    "id": "game_genre",
    "type": "select",
    "name": "Game Genre",
    "cat": "video"
  },
  {
    "id": "prevent_input_delay",
    "type": "select",
    "name": "Prevent Input Delay",
    "cat": "video"
  },
  {
    "id": "allm_setting",
    "type": "switch",
    "name": "ALLM",
    "cat": "video"
  },
  {
    "id": "digital_sound_output",
    "type": "select",
    "name": "Digital Sound Output",
    "cat": "controls"
  },
  {
    "id": "balance",
    "type": "number",
    "name": "Balance",
    "cat": "controls"
  },
  {
    "id": "automatic_volume",
    "type": "switch",
    "name": "Automatic Volume",
    "cat": "controls"
  },
  {
    "id": "earc",
    "type": "switch",
    "name": "eARC",
    "cat": "controls"
  },
  {
    "id": "ip_control",
    "type": "switch",
    "name": "IP Control",
    "cat": "controls"
  },
  {
    "id": "vrr_gsync",
    "type": "switch",
    "name": "VRR & G-Sync",
    "cat": "video"
  },
  {
    "id": "freesync_premium",
    "type": "switch",
    "name": "AMD FreeSync Premium",
    "cat": "video"
  },
  {
    "id": "dark_room_mode",
    "type": "select",
    "name": "Dark Room Mode",
    "cat": "video"
  },
  {
    "id": "black_stabilizer",
    "type": "number",
    "name": "Black Stabilizer",
    "cat": "video"
  },
  {
    "id": "white_stabilizer",
    "type": "number",
    "name": "White Stabilizer",
    "cat": "video"
  },
  {
    "id": "ai_game_sound",
    "type": "switch",
    "name": "AI Game Sound",
    "cat": "video"
  },
  {
    "id": "screen_saver_ads",
    "type": "switch",
    "name": "Screen Saver Ads",
    "cat": "diagnostics"
  },
  {
    "id": "home_sponsored_tiles",
    "type": "switch",
    "name": "Sponsored Tiles on Home",
    "cat": "diagnostics"
  },
  {
    "id": "home_recommendations",
    "type": "switch",
    "name": "Recommendations on Home",
    "cat": "diagnostics"
  },
  {
    "id": "ads_while_watching",
    "type": "switch",
    "name": "Ads While Watching",
    "cat": "diagnostics"
  },
  {
    "id": "smart_tips",
    "type": "switch",
    "name": "Smart Tips",
    "cat": "diagnostics"
  },
  {
    "id": "smart_tips_in_settings",
    "type": "switch",
    "name": "Smart Tips in Settings",
    "cat": "diagnostics"
  }
])GHJSON");
    return value;
}
}

Json haEntityCatalogue() { return catalogueData(); }

Json haEntityCategories() {
    Json out = Json::array();
    // Each key beside its English, so scripts/check-strings.py can read the pair.
    struct Category {
        const char *id, *nameKey, *name, *descriptionKey, *description;
    };
    static const Category categories[] = {
        {"controls", "srv.ha.cat.controls", "Controls & Media", "srv.ha.cat.controls.desc",
         "Power, volume, mute, playback buttons, apps, and input sources."},
        {"oled", "srv.ha.cat.oled", "OLED Care", "srv.ha.cat.oled.desc",
         "Panel on-time, pixel refresher countdowns, and burn-in protections."},
        {"video", "srv.ha.cat.video", "Video & HDMI Signal", "srv.ha.cat.video.desc",
         "Active picture mode, dynamic range, refresh rate, VRR, ALLM, and link mode."},
        {"system", "srv.ha.cat.system", "System & Telemetry", "srv.ha.cat.system.desc",
         "CPU, RAM, swap, SoC temperature, network rates, and storage health."},
        {"diagnostics", "srv.ha.cat.diagnostics", "Diagnostics & Settings",
         "srv.ha.cat.diagnostics.desc",
         "Remote battery, audio format, standby LED, sleep timer, and ad blocker."}};
    for (const Category &category : categories) {
        Json item = Json::object();
        item.set("id", Json::text(category.id));
        item.set("name", Json::text(say::msg(category.nameKey, category.name)));
        item.set("desc", Json::text(say::msg(category.descriptionKey, category.description)));
        out.add(item);
    }
    return out;
}

namespace {
bool truthy(const Json &value) {
    if (!value.valid() || value.dump() == "null")
        return false;
    if (value.isBoolean())
        return value.boolean();
    if (value.isNumber())
        return value.number() != 0;
    if (value.isString())
        return !value.string().empty();
    return true;
}

bool explicitlyFalse(const Json &value) {
    return value.isBoolean() && !value.boolean();
}

bool inSet(const std::set<std::string> &values, const std::string &value) {
    return values.find(value) != values.end();
}

bool listed(const Json &array, const std::string &value) {
    if (!array.isArray())
        return false;
    for (const Json &item : array.items())
        if (item.isString() && item.string() == value)
            return true;
    return false;
}
}

HaEntityFilterResult filterHaEntityCatalogue(const Json &catalogue,
                                             const Json &capabilities) {
    static const std::set<std::string> oledOnly = {
        "oled_panel_hours", "oled_hours_since_compensation",
        "oled_hours_until_compensation", "oled_compensation_status",
        "oled_hours_since_refresher", "oled_hours_until_refresher",
        "oled_refresher_status", "oled_short_cycles", "oled_refresher_cycles",
        "oled_failure_alerts", "oled_asbl_dimmer", "oled_cell_type",
        "tcon_firmware", "oled_screen_shift", "oled_logo_dimming",
        "pixel_refresher_schedule"
    };
    static const std::set<std::string> hdmiIds = {
        "hdmi_link_mode", "hdmi_chroma", "hdmi_hdcp", "hdmi_allm", "hdmi_vrr"
    };
    static const std::set<std::string> noPnwash = {
        "oled_short_cycles", "oled_refresher_cycles", "oled_failure_alerts"
    };
    static const std::set<std::string> noCell = {"oled_cell_type", "tcon_firmware"};
    static const std::set<std::string> noWatch = {
        "watch_time_today", "watch_time_week", "watch_top_app_today"
    };

    const Json hdmiSeen = capabilities["hdmiSeen"];
    const Json userEntities = capabilities["userEntities"];
    const Json disabled = userEntities["disabled"];
    HaEntityFilterResult result{Json::array(), Json::array()};

    for (const Json &entity : catalogue.items()) {
        const std::string id = entity["id"].string();
        const std::string type = entity["type"].string();
        const std::string category = entity["cat"].string();
        bool omit = false;

        if (!truthy(capabilities["hasRemoteInfo"]) && id == "remote_battery")
            omit = true;
        if (!truthy(capabilities["hasPnwash"]) && inSet(noPnwash, id))
            omit = true;
        if (!truthy(capabilities["hasCell"]) && inSet(noCell, id))
            omit = true;
        if (!truthy(capabilities["hasHdmiProc"]) && id.compare(0, 5, "hdmi_") == 0)
            omit = true;
        if (!truthy(capabilities["hasMediaState"]) && id == "play_state")
            omit = true;
        if (!truthy(capabilities["hasSoundFormat"]) && id == "sound_format")
            omit = true;
        if (!truthy(capabilities["hasHdrStatus"]) && id == "video_colorimetry")
            omit = true;
        if (!truthy(capabilities["socArch"]) && id == "soc_architecture")
            omit = true;
        if (explicitlyFalse(capabilities["hasLogoLight"]) && id == "logo_light")
            omit = true;
        if (explicitlyFalse(capabilities["hasOnTimer"]) && id == "on_timer")
            omit = true;
        if (explicitlyFalse(capabilities["hasOffTimer"]) && id == "off_timer")
            omit = true;
        if (!truthy(capabilities["thermalPresent"]) && id == "soc_temperature")
            omit = true;
        if (!truthy(capabilities["emmcWearPresent"]) &&
            (id == "flash_health" || id == "flash_wear"))
            omit = true;
        if (!truthy(capabilities["hasLightSensor"]) && id == "ambient_light")
            omit = true;
        if (!truthy(capabilities["hasRemoteButtons"]) && id == "remote_button")
            omit = true;
        if (inSet(hdmiIds, id) && !truthy(hdmiSeen[
                id == "hdmi_link_mode" ? "phy_mode" :
                id == "hdmi_chroma" ? "chroma" :
                id == "hdmi_hdcp" ? "hdcp" :
                id == "hdmi_allm" ? "allm" : "vrr"]))
            omit = true;
        if (!truthy(capabilities["hasGpuClock"]) && id == "gpu_clock")
            omit = true;
        if (!truthy(capabilities["watchHistory"]) && inSet(noWatch, id))
            omit = true;

        const Json isOled = capabilities["isOled"];
        if (isOled.isBoolean() && isOled.boolean() && id == "panel_dimming")
            omit = true;
        if (isOled.isBoolean() && !isOled.boolean() && inSet(oledOnly, id))
            omit = true;

        if (userEntities.isObject() && explicitlyFalse(userEntities[category]))
            omit = true;
        if (listed(disabled, id) || listed(disabled, type + "." + id))
            omit = true;

        (omit ? result.withheld : result.published).add(entity);
    }
    return result;
}

} } // namespace nativetv::daemon
