#include "service/prometheus.hpp"
#include "domain/names.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace nativetv {
namespace service {
namespace {
const double kUserHz = 100.0;
const double kPanelUnitSeconds = 600.0;
const double kMebibyte = 1024.0 * 1024.0;

struct Sample {
    std::vector<std::pair<std::string, Json>> labels;
    double value;
};

Json path(const Json &value, const std::vector<std::string> &keys) {
    Json result = value;
    for (const std::string &key : keys)
        result = result[key];
    return result;
}

bool number(const Json &value, double &result) {
    if (!value.isNumber())
        return false;
    result = value.number();
    return std::isfinite(result);
}

bool positive(const Json &value, double &result) {
    return number(value, result) && result > 0;
}

Json scaled(const Json &value, double by) {
    double numeric = 0;
    return number(value, numeric) ? Json::numeric(numeric * by) : Json();
}

bool boolean(const Json &value, double &result) {
    if (!value.isBoolean())
        return false;
    result = value.boolean() ? 1.0 : 0.0;
    return true;
}

std::string jsString(const Json &value) {
    if (value.isString()) return value.string();
    if (value.isBoolean()) return value.boolean() ? "true" : "false";
    if (value.isNumber()) return value.dump();
    return "";
}

std::string numberText(double value) {
    if (value == 0) return "0";
    char raw[64] = {};
    for (int precision = 1; precision <= std::numeric_limits<double>::max_digits10;
         ++precision) {
        std::snprintf(raw, sizeof(raw), "%.*g", precision, value);
        char *end = nullptr;
        const double parsed = std::strtod(raw, &end);
        if (end && *end == '\0' && parsed == value) break;
    }
    std::string shortest(raw);
    const size_t exponentMark = shortest.find_first_of("eE");
    int exponent = 0;
    if (exponentMark != std::string::npos)
        exponent = std::atoi(shortest.c_str() + exponentMark + 1);
    std::string mantissa = shortest.substr(0, exponentMark);
    const bool negative = !mantissa.empty() && mantissa[0] == '-';
    if (negative) mantissa.erase(mantissa.begin());
    const size_t dot = mantissa.find('.');
    int decimalPosition = static_cast<int>(dot == std::string::npos ? mantissa.size() : dot) + exponent;
    std::string digits;
    for (char character : mantissa)
        if (character >= '0' && character <= '9') digits += character;
    while (digits.size() > 1 && digits.front() == '0') {
        digits.erase(digits.begin());
        --decimalPosition;
    }
    while (digits.size() > 1 && digits.back() == '0') digits.pop_back();
    std::string result = negative ? "-" : "";
    const int normalizedExponent = decimalPosition - 1;
    if (normalizedExponent >= -6 && normalizedExponent < 21) {
        if (decimalPosition <= 0) {
            result += "0." + std::string(static_cast<size_t>(-decimalPosition), '0') + digits;
        } else if (decimalPosition >= static_cast<int>(digits.size())) {
            result += digits + std::string(static_cast<size_t>(decimalPosition) - digits.size(), '0');
        } else {
            result += digits.substr(0, static_cast<size_t>(decimalPosition)) + "." +
                      digits.substr(static_cast<size_t>(decimalPosition));
        }
    } else {
        result += digits.substr(0, 1);
        if (digits.size() > 1) result += "." + digits.substr(1);
        result += "e" + std::string(normalizedExponent >= 0 ? "+" : "") +
                  std::to_string(normalizedExponent);
    }
    return result;
}

std::string escape(const std::string &value) {
    std::string result;
    result.reserve(value.size());
    for (char character : value) {
        if (character == '\\') result += "\\\\";
        else if (character == '"') result += "\\\"";
        else if (character == '\n') result += "\\n";
        else result += character;
    }
    return result;
}

std::string labelsText(const std::vector<std::pair<std::string, Json>> &labels) {
    if (labels.empty()) return "";
    std::string result = "{";
    for (size_t i = 0; i < labels.size(); ++i) {
        if (i) result += ",";
        result += labels[i].first + "=\"" + escape(jsString(labels[i].second)) + "\"";
    }
    return result + "}";
}

Sample sample(double value,
              std::vector<std::pair<std::string, Json>> labels =
                  std::vector<std::pair<std::string, Json>>()) {
    return {std::move(labels), value};
}

std::vector<Sample> one(const Json &value,
                        std::vector<std::pair<std::string, Json>> labels =
                            std::vector<std::pair<std::string, Json>>()) {
    double numeric = 0;
    if (!number(value, numeric)) return {};
    std::vector<Sample> result;
    result.push_back(sample(numeric, std::move(labels)));
    return result;
}

std::vector<Sample> one(double value,
                        std::vector<std::pair<std::string, Json>> labels =
                            std::vector<std::pair<std::string, Json>>()) {
    return std::isfinite(value) ? std::vector<Sample>{sample(value, std::move(labels))}
                                : std::vector<Sample>();
}

std::vector<std::pair<std::string, Json>> label(const std::string &key,
                                                const Json &value) {
    return {{key, value}};
}

std::vector<std::pair<std::string, Json>> label(const std::string &key,
                                                const std::string &value) {
    return {{key, Json::text(value)}};
}

double dateSeconds(const Json &value) {
    if (!value.isString()) return std::numeric_limits<double>::quiet_NaN();
    const std::string text = value.string();
    if (text.size() < 19) return std::numeric_limits<double>::quiet_NaN();
    std::tm parts = {};
    char *parsed = strptime(text.c_str(), "%Y-%m-%dT%H:%M:%S", &parts);
    if (!parsed) return std::numeric_limits<double>::quiet_NaN();
    const double seconds = static_cast<double>(timegm(&parts));
    double milliseconds = 0;
    if (*parsed == '.') {
        ++parsed;
        unsigned digits = 0;
        while (*parsed >= '0' && *parsed <= '9') {
            if (digits < 3) milliseconds = milliseconds * 10 + (*parsed - '0');
            ++digits;
            ++parsed;
        }
        if (digits == 1) milliseconds *= 100;
        else if (digits == 2) milliseconds *= 10;
    }
    return seconds + milliseconds / 1000.0;
}

bool eolStates(const std::string &display, std::vector<Sample> &result) {
    const Json states = names::emmcEolStates();
    bool known = false;
    for (const std::string &code : states.keys())
        if (states[code]["display"].string() == display) known = true;
    if (!known) return false;
    for (const std::string &code : states.keys()) {
        const Json state = states[code];
        result.push_back(sample(state["display"].string() == display ? 1 : 0,
                                label("state", state["label"])));
    }
    return true;
}

std::vector<Sample> cpuTimes(const Json &stats) {
    std::vector<Sample> result;
    const Json cpus = stats["cpuTimes"];
    std::vector<std::string> ids = cpus.keys();
    std::sort(ids.begin(), ids.end(), [](const std::string &a, const std::string &b) {
        return std::strtoul(a.c_str(), nullptr, 10) < std::strtoul(b.c_str(), nullptr, 10);
    });
    for (const std::string &cpu : ids) {
        const Json modes = cpus[cpu];
        for (const std::string &mode : modes.keys()) {
            double ticks = 0;
            if (number(modes[mode], ticks))
                result.push_back(sample(ticks / kUserHz,
                                        {{"cpu", Json::text(cpu)},
                                         {"mode", Json::text(mode)}}));
        }
    }
    return result;
}
} // namespace

std::string escapePrometheusLabel(const std::string &value) { return escape(value); }

std::string renderPrometheus(const Json &stats, const std::string &version,
                             const Json &syslog, const Json &ntp,
                             const Json &watch) {
    const Json s = stats.isObject() ? stats : Json::object();
    std::string output;
    auto emit = [&output](const std::string &name, const std::string &type,
                          const std::string &help, const std::vector<Sample> &samples) {
        if (samples.empty()) return;
        output += "# HELP " + name + " " + help + "\n# TYPE " + name + " " + type + "\n";
        for (const Sample &entry : samples)
            output += name + labelsText(entry.labels) + " " + numberText(entry.value) + "\n";
    };
    std::vector<Sample> samples;

    samples = {sample(1, {{"model", path(s, {"device", "model"})},
                         {"firmware", path(s, {"system", "firmware"})},
                         {"webos", path(s, {"system", "webos"})},
                         {"version", Json::text(version)}})};
    emit("nativetv_info", "gauge", "Always 1, labelled with the TV model, its firmware and webOS versions, and the nativeTv version.", samples);

    double value = 0;
    if (positive(s["btime"], value)) samples = one(value);
    else {
        value = dateSeconds(s["bootTime"]);
        samples = std::isfinite(value) ? one(value) : std::vector<Sample>();
    }
    emit("nativetv_boot_time_seconds", "gauge", "Time the TV's system last booted, in seconds since the Unix epoch.", samples);

    samples.clear();
    if (boolean(path(s, {"powerState", "systemOn"}), value)) samples = one(value);
    emit("nativetv_system_on", "gauge", "1 when the TV is on, including with the screen off; 0 in standby or off.", samples);
    samples.clear();
    if (boolean(path(s, {"powerState", "screenOn"}), value)) samples = one(value);
    emit("nativetv_screen_on", "gauge", "1 when the screen is on, including a screen saver; 0 otherwise.", samples);

    samples.clear();
    {
        const Json reason = path(s, {"powerState", "onReason"});
        if (reason.isString() && !reason.string().empty()) {
            const Json mapped = names::powerOnReason(reason.string());
            samples = one(1, label("reason", mapped["label"]));
        }
    }
    emit("nativetv_power_on_info", "gauge", "Always 1, labelled with the reason the TV's power service gives for the TV last powering on, such as remote_key, wake_on_wifi, or reboot_by_on_regular.", samples);

    samples.clear();
    {
        double ontime = 0, timestamp = 0;
        if (positive(path(s, {"powerState", "onTime", "ontime"}), ontime) &&
            number(s["time"], timestamp))
            samples = one(std::round(timestamp - ontime * 1000.0) / 1000.0);
    }
    emit("nativetv_power_on_time_seconds", "gauge", "Time the TV last powered on, in seconds since the Unix epoch, from the time on its power service reports. No sample in standby.", samples);

    samples.clear();
    {
        double precise = 0;
        if (positive(s["tempMillidegrees"], precise)) samples = one(precise / 1000.0);
        else samples = one(s["temp"]);
    }
    emit("nativetv_soc_temperature_celsius", "gauge", "Temperature of the SoC in degrees Celsius.", samples);
    emit("nativetv_cpu_seconds_total", "counter", "Time each online core has spent in each mode since boot, in seconds, from the native system snapshot.", cpuTimes(s));

    samples.clear();
    if (positive(s["cpuHz"], value)) samples = one(value);
    else if (number(s["mhz"], value)) samples = one(value * 1e6);
    emit("nativetv_cpu_frequency_hertz", "gauge", "Processor clock frequency in hertz, as the TV's power manager reports it.", samples);
    samples.clear();
    if (positive(s["gpuHz"], value)) samples = one(value);
    else if (number(s["gpuMhz"], value)) samples = one(value * 1e6);
    emit("nativetv_gpu_frequency_hertz", "gauge", "GPU clock frequency in hertz.", samples);

    const Json mem = s["mem"];
    samples.clear();
    if (positive(mem["total"], value)) samples = one(value * 1024.0);
    emit("nativetv_memory_total_bytes", "gauge", "Memory usable by the system in bytes, MemTotal in /proc/meminfo.", samples);
    samples.clear();
    const bool memoryRead = positive(mem["total"], value);
    if (memoryRead) samples = one(Json::numeric(mem["avail"].number() * 1024.0));
    emit("nativetv_memory_available_bytes", "gauge", "Memory available for new work without swapping in bytes, MemAvailable in /proc/meminfo.", samples);
    const Json swap = s["swap"];
    samples.clear();
    if (memoryRead) samples = one(Json::numeric(swap["total"].number() * 1024.0));
    emit("nativetv_swap_total_bytes", "gauge", "Swap space in bytes, SwapTotal in /proc/meminfo.", samples);
    samples.clear();
    if (memoryRead) samples = one(Json::numeric(swap["free"].number() * 1024.0));
    emit("nativetv_swap_free_bytes", "gauge", "Unused swap space in bytes, SwapFree in /proc/meminfo.", samples);

    samples.clear();
    if (number(s["netTotal"]["rx"], value))
        samples = one(value, label("interface", s["netTotal"]["iface"]));
    emit("nativetv_network_receive_bytes_total", "counter", "Bytes received on the network interface in use, labelled with its name.", samples);
    samples.clear();
    if (number(s["netTotal"]["tx"], value))
        samples = one(value, label("interface", s["netTotal"]["iface"]));
    emit("nativetv_network_transmit_bytes_total", "counter", "Bytes sent on the network interface in use, labelled with its name.", samples);
    samples.clear();
    if (number(path(s, {"wifi", "level"}), value)) samples = one(value);
    emit("nativetv_wifi_signal_dbm", "gauge", "Wi-Fi signal level in dBm, from the native system snapshot.", samples);
    samples.clear();
    if (number(path(s, {"wifi", "link"}), value)) samples = one(value);
    emit("nativetv_wifi_link_quality", "gauge", "Wi-Fi link quality from the native system snapshot, on a scale the Wi-Fi driver sets.", samples);

    samples.clear();
    eolStates(s["emmc"]["eol"].string(), samples);
    emit("nativetv_emmc_pre_eol_state", "gauge", "eMMC pre-end-of-life state from its PRE_EOL_INFO register, 1 for the current one: normal, warning (80% of reserved blocks used) or urgent (90%).", samples);
    samples.clear();
    for (const std::string type : {"a", "b"}) {
        if (number(s["emmc"]["life_est_" + type], value) && value >= 1 && value <= 11)
            samples.push_back(sample((value - 1) / 10.0, label("type", type)));
    }
    emit("nativetv_emmc_life_used_ratio", "gauge", "Share of the eMMC's estimated life used, from its DEVICE_LIFE_TIME_EST registers, in steps of 0.1: 0 for under a tenth, 1 for all of it or more. type is a or b, the two kinds of memory the card estimates for.", samples);

    samples.clear();
    if (positive(s["appStorage"]["totalKb"], value)) samples = one(value * 1024.0);
    else if (positive(s["appStorage"]["totalMb"], value)) samples = one(value * kMebibyte);
    emit("nativetv_app_storage_size_bytes", "gauge", "Size of the app storage filesystem in bytes.", samples);
    samples.clear();
    if (positive(s["appStorage"]["totalKb"], value))
        samples = one(Json::numeric(s["appStorage"]["availKb"].number() * 1024.0));
    else if (positive(s["appStorage"]["totalMb"], value) &&
             positive(s["appStorage"]["freeMb"], value))
        samples = one(value * kMebibyte);
    emit("nativetv_app_storage_available_bytes", "gauge", "Space on the app storage filesystem available for new apps in bytes.", samples);

    samples.clear();
    if (number(s["remote"]["battery"], value)) samples = one(value / 100.0);
    emit("nativetv_remote_battery_ratio", "gauge", "Battery charge of the paired Magic Remote, from 0 to 1.", samples);
    samples.clear();
    if (boolean(path(s, {"privacy", "adblock", "enabled"}), value)) samples = one(value);
    emit("nativetv_adblock_enabled", "gauge", "1 when the ad-blocking hosts table is in place over /etc/hosts, 0 otherwise.", samples);

    samples.clear();
    if (positive(path(s, {"oled", "panel_usage_units"}), value)) samples = one(value * kPanelUnitSeconds);
    else if (positive(path(s, {"oled", "panel_hours_exact"}), value)) samples = one(value * 3600.0);
    emit("nativetv_oled_panel_usage_seconds_total", "counter", "Time the OLED panel has been in use, in seconds, counted in steps of 10 minutes.", samples);
    samples.clear();
    if (positive(path(s, {"oled", "last_compensation_units"}), value)) samples = one(value * kPanelUnitSeconds);
    else if (positive(path(s, {"oled", "last_compensation_hours"}), value)) samples = one(value * 3600.0);
    emit("nativetv_oled_last_compensation_usage_seconds", "gauge", "Panel usage time at the last short compensation cycle, in seconds.", samples);
    samples.clear();
    {
        double units = 0, hours = 0;
        const bool hasUnits = positive(path(s, {"oled", "comp_interval_units"}), units);
        const bool hasHours = positive(path(s, {"oled", "comp_interval_hours"}), hours);
        if (hasUnits && hasHours && std::abs(units / 6.0 - hours) < 0.1)
            samples = one(units * kPanelUnitSeconds);
        else samples = one(scaled(path(s, {"oled", "comp_interval_hours"}), 3600.0));
    }
    emit("nativetv_oled_compensation_interval_seconds", "gauge", "Panel usage time between short compensation cycles, in seconds.", samples);

    samples.clear();
    {
        const Json status = path(s, {"oled", "comp_status"});
        if (status.isString())
            samples = one(status.string() == names::compensationStatus(true)["display"].string()
                              ? 1 : 0);
    }
    emit("nativetv_oled_compensation_running", "gauge", "1 while a short compensation cycle is running, 0 otherwise.", samples);
    samples.clear();
    if (number(path(s, {"oled", "comp_cycles"}), value)) samples = one(value);
    emit("nativetv_oled_compensation_runs_total", "counter", "Short compensation cycles the panel has completed.", samples);
    samples.clear();
    if (positive(path(s, {"oled", "last_refresher_hours"}), value)) samples = one(value * 3600.0);
    emit("nativetv_oled_last_refresher_usage_seconds", "gauge", "Panel usage time at the last Pixel Refresher run, in seconds, to the nearest hour.", samples);
    samples.clear();
    if (positive(path(s, {"oled", "refresher_interval_hours"}), value)) samples = one(value * 3600.0);
    emit("nativetv_oled_refresher_interval_seconds", "gauge", "Panel usage time between automatic Pixel Refresher runs, in seconds.", samples);
    samples.clear();
    if (number(path(s, {"oled", "refresher_cycles"}), value)) samples = one(value);
    emit("nativetv_oled_refresher_runs_total", "counter", "Pixel Refresher runs the panel has completed.", samples);
    samples.clear();
    {
        const Json status = path(s, {"oled", "refresher_status"});
        if (status.isString())
            samples = one(status.string() == names::refresherStatuses()["processing"].string()
                              ? 1 : 0);
    }
    emit("nativetv_oled_refresher_running", "gauge", "1 while a Pixel Refresher run is in progress, 0 otherwise.", samples);
    samples.clear();
    {
        const Json status = path(s, {"oled", "refresher_status"});
        if (status.isString())
            samples = one(status.string() == names::refresherStatuses()["cancel_schedule"].string()
                              ? 1 : 0);
    }
    emit("nativetv_oled_refresher_scheduled", "gauge", "1 while a Pixel Refresher run is queued for the next standby, 0 otherwise.", samples);
    samples.clear();
    if (number(path(s, {"oled", "failure_alerts"}), value)) samples = one(value);
    emit("nativetv_oled_failure_alerts_total", "counter", "Panel maintenance failure alerts the TV has recorded.", samples);
    samples.clear();
    if (number(path(s, {"oled", "gsr_stress_count"}), value)) samples = one(value);
    emit("nativetv_oled_gsr_stress_events_total", "counter", "Stress events Global Stress Reduction has counted on the panel, as the TV's panel service reports them.", samples);
    samples.clear();
    for (const auto &entry : {std::pair<const char *, const char *>{"asbl", "tpc_enabled"},
                              {"gsr", "gsr_enabled"}}) {
        double enabled = 0;
        if (boolean(path(s, {"oled", entry.second}), enabled))
            samples.push_back(sample(enabled, label("protection", entry.first)));
    }
    emit("nativetv_oled_protection_enabled", "gauge", "1 when the panel protection is on, 0 when off: asbl is the Automatic Static Brightness Limiter, gsr Global Stress Reduction.", samples);

    samples.clear();
    if (number(path(s, {"picture", "backlight_raw"}), value)) samples = one(value / 100.0);
    emit("nativetv_picture_backlight_ratio", "gauge", "Backlight setting of the current picture mode from 0 to 1, which on an OLED is OLED light, the pixel brightness.", samples);

    samples.clear();
    {
        const std::string output = path(s, {"sound", "output_raw"}).string();
        const std::string mode = path(s, {"sound", "mode"}).string();
        const std::string outputLabel = output.empty() ? "" : names::soundOutput(output)["label"].string();
        const std::string modeLabel = mode.empty() ? "" : names::snakeCase(mode);
        if (!outputLabel.empty() || !modeLabel.empty())
            samples = one(1, {{"output", Json::text(outputLabel)},
                              {"mode", Json::text(modeLabel)}});
    }
    emit("nativetv_sound_info", "gauge", "Always 1, labelled with the sound output and the sound mode the TV's settings are set to, such as tv_speaker, external_arc, or bt_soundbar, and standard, ai_sound_plus, or movie.", samples);
    samples.clear();
    {
        const std::string codec = path(s, {"sound", "input", "codec_raw"}).string();
        if (!codec.empty()) {
            const Json immersive = names::audioImmersive(path(s, {"sound", "input", "immersive_raw"}));
            const std::string immersiveLabel = immersive.isObject()
                ? immersive["label"].string() : "none";
            samples = one(1, {{"codec", names::audioCodec(Json::text(codec))["label"]},
                              {"immersive", Json::text(immersiveLabel)}});
        }
    }
    emit("nativetv_sound_input_info", "gauge", "Always 1 while something plays, labelled with the format of the sound the app or input sends, such as pcm, opus, or eac3, and its immersive layer, such as dolby_atmos, or none. Absent while nothing plays.", samples);
    samples.clear();
    if (s["volume_control"].string() == "level")
        samples = one(Json::numeric(s["volume"].number() / 100.0));
    emit("nativetv_volume_ratio", "gauge", "Volume of the sound output from 0 to 1, while the TV sets the level rather than only stepping it or not controlling it.", samples);
    samples.clear();
    if (boolean(s["muted"], value)) samples = one(value);
    emit("nativetv_muted", "gauge", "1 while the sound is muted, 0 otherwise.", samples);

    samples.clear();
    {
        const Json id = s["app_id"];
        if (id.isString() && !id.string().empty())
            samples = one(1, {{"app_id", id}, {"app_name", s["app_name"]}});
    }
    emit("nativetv_foreground_app_info", "gauge", "Always 1 for the app in the foreground, whether or not nativetv_screen_on reports the screen lit, labelled with its id and its name: the name given an input in the TV's settings, an app's title, or the short id.", samples);

    samples.clear();
    {
        const Json raw = path(s, {"picture", "dynamicRange_raw"});
        const Json mode = path(s, {"picture", "mode_raw"});
        const Json dynamic = raw.isString() && !raw.string().empty()
            ? names::dynamicRange(raw.string()) : Json();
        const std::string dynamicLabel = dynamic.isObject() ? dynamic["label"].string() : "";
        const std::string modeLabel = mode.isString() && !mode.string().empty()
            ? names::pictureMode(mode.string())["label"].string() : "";
        if (!dynamicLabel.empty() || !modeLabel.empty())
            samples = one(1, {{"dynamic_range", Json::text(dynamicLabel)},
                              {"picture_mode", Json::text(modeLabel)}});
    }
    emit("nativetv_signal_info", "gauge", "Always 1, labelled with the dynamic range (sdr, hdr, dolby_vision, technicolor) and the picture mode the picture settings are using.", samples);
    samples.clear();
    {
        const Json raw = path(s, {"picture", "dynamicRange_raw"});
        const Json dynamic = raw.isString() && !raw.string().empty()
            ? names::dynamicRange(raw.string()) : Json();
        if (dynamic.isObject() && dynamic["lowLatency"].isBoolean())
            samples = one(dynamic["lowLatency"].boolean() ? 1 : 0);
    }
    emit("nativetv_signal_low_latency", "gauge", "1 while the TV's picture settings are in their low-latency mode, which an HDMI source requests with the ALLM flag in any format, 0 otherwise.", samples);
    samples.clear();
    if (boolean(path(s, {"hdmi_diag", "vrr"}), value)) samples = one(value);
    emit("nativetv_signal_vrr", "gauge", "1 while the HDMI source is using variable refresh rate (VRR, FreeSync, or HDMI Forum VRR), 0 otherwise.", samples);
    samples.clear();
    if (positive(path(s, {"signal_timing", "width"}), value)) samples = one(value);
    emit("nativetv_signal_width_pixels", "gauge", "Width of the HDMI source's picture in pixels.", samples);
    samples.clear();
    if (positive(path(s, {"signal_timing", "height"}), value)) samples = one(value);
    emit("nativetv_signal_height_pixels", "gauge", "Height of the HDMI source's picture in pixels.", samples);
    samples.clear();
    if (positive(path(s, {"signal_timing", "refresh_hz"}), value)) samples = one(value);
    emit("nativetv_signal_refresh_hertz", "gauge", "Refresh rate of the HDMI signal in hertz, as the source sends it rather than the content's frame rate.", samples);
    samples.clear();
    {
        const double hz = positive(path(s, {"source_frame_rate", "hz"}), value)
            ? value : std::numeric_limits<double>::quiet_NaN();
        if (std::isfinite(hz)) {
            const Json vrr = names::vrrType(path(s, {"source_frame_rate", "vrr_type"}));
            samples = one(hz, label("vrr_type", vrr["label"]));
        }
    }
    emit("nativetv_signal_frame_rate_hertz", "gauge", "Frame rate in hertz: the rate the game is running at while VRR is in use, and the same as the refresh rate otherwise. vrr_type is the kind of VRR, such as gsync, or off.", samples);

    samples.clear();
    {
        const Json format = s["signal_format"];
        if (format.isObject()) {
            const Json colorimetry = format["colorimetry"].isString()
                ? names::signalColorimetry(format["colorimetry"].string()) : Json();
            const Json encoding = format["encoding"].isString()
                ? names::signalEncoding(format["encoding"].string()) : Json();
            samples = one(1, {{"type", Json::text(format["type"].isString() ? format["type"].string() : "")},
                              {"eotf", Json::text(format["eotf"].isString() ? format["eotf"].string() : "")},
                              {"colorimetry", colorimetry.isObject() ? colorimetry["label"] : Json::text("")},
                              {"encoding", encoding.isObject() ? encoding["label"] : Json::text("")}});
        }
    }
    emit("nativetv_signal_format_info", "gauge", "Always 1 while a video sink is connected, labelled with the source's HDR type, such as sdr or hdr10, the EOTF its HDR metadata names (sdr, hdr, pq, or hlg), its colorimetry, and its pixel encoding (rgb_444, ycbcr_444, ycbcr_422, or ycbcr_420).", samples);
    samples.clear();
    if (number(path(s, {"signal_format", "max_luminance"}), value)) samples = one(value);
    emit("nativetv_signal_hdr_mastering_luminance_max_nits", "gauge", "Peak luminance of the display the content was mastered on, in nits, from the source's HDR10 static metadata.", samples);
    samples.clear();
    if (number(path(s, {"signal_format", "min_luminance"}), value)) samples = one(value);
    emit("nativetv_signal_hdr_mastering_luminance_min_nits", "gauge", "Black level of the display the content was mastered on, in nits, from the source's HDR10 static metadata.", samples);
    samples.clear();
    if (positive(path(s, {"signal_format", "max_cll"}), value)) samples = one(value);
    emit("nativetv_signal_hdr_content_light_level_max_nits", "gauge", "Brightest pixel in the content (MaxCLL) in nits, from the source's HDR10 static metadata, while the source gives one.", samples);
    samples.clear();
    if (positive(path(s, {"signal_format", "max_fall"}), value)) samples = one(value);
    emit("nativetv_signal_hdr_frame_average_light_level_max_nits", "gauge", "Brightest frame average in the content (MaxFALL) in nits, from the source's HDR10 static metadata, while the source gives one.", samples);
    samples.clear();
    if (boolean(path(s, {"signal_format", "game_mode"}), value)) samples = one(value);
    emit("nativetv_signal_game_mode", "gauge", "1 while the video output service has the connected source in game mode, 0 otherwise.", samples);

    samples.clear();
    {
        const Json inputs = s["inputs"];
        std::vector<std::string> keys = inputs.keys();
        std::sort(keys.begin(), keys.end());
        for (const std::string &key : keys) {
            if (!names::isHdmiKey(key)) continue;
            samples.push_back(sample(1, {{"input", Json::text(key)},
                                         {"app_id", Json::text(names::inputAppId(key))},
                                         {"name", inputs[key]}}));
        }
    }
    emit("nativetv_hdmi_input_info", "gauge", "Always 1 for each HDMI input, labelled with the input, its app id, and the name given it in the TV's settings.", samples);

    const Json links = s["hdmi_links"];
    samples.clear();
    if (links.isArray()) {
        for (const Json &linkValue : links.items()) {
            double input = 0;
            if (!number(linkValue["input"], input)) continue;
            const std::string inputKey = names::hdmiKey(static_cast<unsigned>(input));
            const Json phy = names::hdmiPhyMode(linkValue["phy_mode"].string(),
                                                linkValue["tmds_clock_khz"]);
            const Json chroma = linkValue["chroma"].isString() && !linkValue["chroma"].string().empty()
                ? names::hdmiChroma(linkValue["chroma"].string()) : Json();
            const Json hdcp = linkValue["hdcp"].isString() && !linkValue["hdcp"].string().empty()
                ? names::hdmiHdcp(linkValue["hdcp"].string()) : Json();
            samples.push_back(sample(1, {{"input", Json::text(inputKey)},
                                         {"phy_mode", phy["label"]},
                                         {"chroma", chroma.isObject() ? chroma["label"] : Json::text("")},
                                         {"hdcp", hdcp.isObject() ? hdcp["label"] : Json::text("")}}));
        }
    }
    emit("nativetv_hdmi_link_info", "gauge", "Always 1 for each HDMI input with a link, labelled with the input, its PHY mode (frl_48 to frl_9, tmds_6g, or tmds_3g), its chroma format, and its HDCP version.", samples);
    samples.clear();
    if (links.isArray()) {
        for (const Json &linkValue : links.items()) {
            double input = 0;
            if (!number(linkValue["input"], input)) continue;
            const Json phy = names::hdmiPhyMode(linkValue["phy_mode"].string(),
                                                linkValue["tmds_clock_khz"]);
            const std::vector<Sample> rate = one(
                phy["bitsPerSecond"],
                label("input", names::hdmiKey(static_cast<unsigned>(input))));
            samples.insert(samples.end(), rate.begin(), rate.end());
        }
    }
    emit("nativetv_hdmi_link_bits_per_second", "gauge", "Rate of each HDMI input's link in bits per second: the lane rate times the lanes on FRL, and ten bits a character on each of three channels on TMDS.", samples);
    samples.clear();
    if (links.isArray()) {
        for (const Json &linkValue : links.items()) {
            double input = 0, qms = 0;
            if (!number(linkValue["input"], input) || !boolean(linkValue["qms"], qms)) continue;
            samples.push_back(sample(qms, label("input", names::hdmiKey(static_cast<unsigned>(input)))));
        }
    }
    emit("nativetv_hdmi_qms", "gauge", "1 while Quick Media Switching is active on the HDMI input's link, 0 otherwise.", samples);

    samples.clear();
    {
        const Json sources = s["hdmi_sources"];
        if (sources.isArray()) {
            for (const Json &source : sources.items()) {
                double input = 0;
                if (!number(source["input"], input)) continue;
                samples.push_back(sample(source["powered"].boolean() ? 1 : 0,
                    label("input", names::hdmiKey(static_cast<unsigned>(input)))));
            }
        }
    }
    emit("nativetv_hdmi_source_powered", "gauge", "1 while a powered source is on the HDMI input's cable, whether or not its link is up, 0 otherwise.", samples);

    samples.clear();
    if (syslog.isObject() && syslog["messages"].isObject()) {
        for (const std::string &source : syslog["messages"].keys()) {
            double messages = 0;
            if (number(syslog["messages"][source], messages))
                samples.push_back(sample(messages, label("source", source)));
        }
    }
    emit("nativetv_syslog_messages_total", "counter", "Log lines sent to the syslog server since the server started, by source: system, nativetv, or kernel.", samples);
    samples = syslog.isObject() ? one(syslog["errors"]) : std::vector<Sample>();
    emit("nativetv_syslog_errors_total", "counter", "Log lines that could not be sent to the syslog server since the server started. They are dropped, not sent again.", samples);
    samples = ntp.isObject() ? one(ntp["sets"]) : std::vector<Sample>();
    emit("nativetv_ntp_clock_sets_total", "counter", "Times the TV's clock was set from the NTP server since the server started.", samples);
    samples = ntp.isObject() ? one(ntp["errors"]) : std::vector<Sample>();
    emit("nativetv_ntp_errors_total", "counter", "NTP syncs since the server started that set no clock: no answer, an answer not used, or the time service refusing the time.", samples);

    const Json last = ntp["last"];
    samples.clear();
    if (ntp.isObject() && last.isObject() && number(last["offsetMs"], value)) samples = one(value / 1000.0);
    emit("nativetv_ntp_offset_seconds", "gauge", "How far the NTP server's time was ahead of the TV's clock at the last answer, negative when behind it.", samples);
    samples.clear();
    if (ntp.isObject() && last.isObject() && number(last["roundTripMs"], value)) samples = one(value / 1000.0);
    emit("nativetv_ntp_round_trip_seconds", "gauge", "Round trip to the NTP server for the last answer, less the server's own time to answer.", samples);
    samples.clear();
    if (ntp.isObject() && last.isObject() && number(last["stratum"], value)) samples = one(value);
    emit("nativetv_ntp_stratum", "gauge", "The NTP server's stratum in the last answer.", samples);
    samples.clear();
    if (ntp.isObject() && last.isObject() && number(last["at"], value)) samples = one(value / 1000.0);
    emit("nativetv_ntp_last_answer_timestamp_seconds", "gauge", "When the last answer came from the NTP server, as a Unix time.", samples);

    samples.clear();
    if (watch.isObject()) {
        std::vector<std::string> apps = watch.keys();
        std::sort(apps.begin(), apps.end());
        for (const std::string &app : apps) {
            double seconds = 0;
            if (number(watch[app], seconds))
                samples.push_back(sample(seconds, label("app", app)));
        }
    }
    emit("nativetv_watch_seconds_total", "counter", "Time the TV has been on, per app or input in front, in seconds, while watch time is on. screen_off is the TV on with its panel off, screen_saver with LG's screen saver showing. Clearing the history resets it.", samples);
    return output;
}

} // namespace service
} // namespace nativetv
