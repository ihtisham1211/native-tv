#include "domain/names.hpp"
#include "domain/say.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <regex>
#include <sstream>

namespace nativetv {
namespace names {
namespace {
std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

std::string upper(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::toupper(character));
    });
    return value;
}

Json named(const std::map<std::string, std::pair<std::string, std::string>> &table,
           const std::string &raw) {
    const auto found = table.find(raw);
    Json result = Json::object();
    result.set("display", Json::text(found == table.end() ? raw : found->second.first));
    result.set("label", Json::text(found == table.end() ? snakeCase(raw) : found->second.second));
    return result;
}

std::string readable(const std::string &raw) {
    std::string result;
    for (size_t index = 0; index < raw.size(); ++index) {
        const unsigned char current = static_cast<unsigned char>(raw[index]);
        if (index > 0 && std::isupper(current)) {
            const unsigned char previous = static_cast<unsigned char>(raw[index - 1]);
            if (std::islower(previous) || std::isdigit(previous))
                result.push_back(' ');
        }
        result.push_back(static_cast<char>(current));
    }
    const std::string lowered = lower(result);
    for (const std::string &prefix : {std::string("sdr"), std::string("hdr"), std::string("hlg")}) {
        if (lowered.compare(0, prefix.size(), prefix) == 0) {
            result.replace(0, prefix.size(), upper(prefix));
            break;
        }
    }
    if (!result.empty())
        result[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(result[0])));
    return result;
}

std::string spelledHdrType(const std::string &snake) {
    std::string spaced = snake;
    std::replace(spaced.begin(), spaced.end(), '_', ' ');
    std::istringstream words(spaced);
    std::string word, result;
    while (words >> word) {
        if (!result.empty())
            result.push_back(' ');
        if (word == "sdr" || word == "hlg" ||
            std::regex_match(word, std::regex("hdr[0-9]*"))) {
            result += upper(word);
        } else {
            word[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(word[0])));
            result += word;
        }
    }
    return result;
}

std::string spelledPictureMode(const std::string &raw) {
    std::string expanded = raw;
    if (raw.compare(0, 8, "dolbyHdr") == 0 && raw.size() > 8 &&
        std::isupper(static_cast<unsigned char>(raw[8]))) {
        expanded = "Dolby Vision " + raw.substr(8);
    } else if (raw.compare(0, 3, "hdr") == 0 && raw.size() > 3 &&
               std::isupper(static_cast<unsigned char>(raw[3]))) {
        expanded = "HDR " + raw.substr(3);
    }
    std::string result;
    for (size_t index = 0; index < expanded.size(); ++index) {
        const unsigned char current = static_cast<unsigned char>(expanded[index]);
        if (index > 0 && std::isupper(current)) {
            const unsigned char previous = static_cast<unsigned char>(expanded[index - 1]);
            if (std::islower(previous) || std::isdigit(previous))
                result.push_back(' ');
        }
        result.push_back(static_cast<char>(current));
    }
    if (!result.empty())
        result[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(result[0])));
    return result;
}

Json objectWithName(const std::string &display, const std::string &label) {
    Json result = Json::object();
    result.set("display", Json::text(display));
    result.set("label", Json::text(label));
    return result;
}
} // namespace

std::string snakeCase(const std::string &value) {
    std::string result;
    for (size_t index = 0; index < value.size(); ++index) {
        const unsigned char current = static_cast<unsigned char>(value[index]);
        if (std::isupper(current) && index > 0) {
            const unsigned char previous = static_cast<unsigned char>(value[index - 1]);
            if (std::islower(previous) || std::isdigit(previous))
                result.push_back('_');
        }
        result.push_back(static_cast<char>(std::tolower(current)));
    }
    return result;
}

std::string mapped(const Json &table, const std::string &value) {
    return table.has(value) ? table[value].string() : snakeCase(value);
}

Json hdmiPhyMode(const std::string &raw, const Json &tmdsClockKhz) {
    static const std::regex frl("^FRL\\s+([0-9]+)G\\s+([0-9]+)L\\b", std::regex::icase);
    static const std::regex tmds("^(?:TMDS\\s*)?([36])G$", std::regex::icase);
    std::smatch match;
    if (std::regex_search(raw, match, frl)) {
        const unsigned gbps = static_cast<unsigned>(std::stoul(match[1].str())) *
                              static_cast<unsigned>(std::stoul(match[2].str()));
        Json result = objectWithName("FRL " + std::to_string(gbps) + " Gbps",
                                     "frl_" + std::to_string(gbps));
        result.set("bitsPerSecond", Json::numeric(static_cast<double>(gbps) * 1e9));
        return result;
    }
    if (std::regex_match(raw, match, tmds)) {
        const std::string generation = match[1].str();
        const double clock = tmdsClockKhz.number();
        const bool haveClock = tmdsClockKhz.isNumber() && std::isfinite(clock) && clock > 0;
        Json result = objectWithName("TMDS (" + generation + "G)", "tmds_" + generation + "g");
        result.set("bitsPerSecond", haveClock ? Json::numeric(clock * 1000 * 10 * 3) : Json());
        return result;
    }
    Json result = objectWithName(raw, "other");
    result.set("bitsPerSecond", Json());
    return result;
}

Json hdmiChroma(const std::string &raw) {
    static const std::map<std::string, std::pair<std::string, std::string>> table = {
        {"R444", {"RGB 4:4:4", "rgb_444"}},
        {"Y444", {"YCbCr 4:4:4", "ycbcr_444"}},
        {"Y422", {"YCbCr 4:2:2", "ycbcr_422"}},
        {"Y420", {"YCbCr 4:2:0", "ycbcr_420"}}};
    return named(table, raw);
}

Json hdmiHdcp(const std::string &raw) {
    static const std::map<std::string, std::pair<std::string, std::string>> table = {
        {"HDCP23", {"HDCP 2.3", "2_3"}}, {"HDCP22", {"HDCP 2.2", "2_2"}},
        {"HDCP14", {"HDCP 1.4", "1_4"}}, {"HDCP0", {"None", "none"}}};
    return named(table, raw);
}

Json dynamicRange(const std::string &raw) {
    const bool lowLatency = raw.size() >= 4 && raw.compare(raw.size() - 4, 4, "ALLM") == 0;
    const std::string range = lowLatency ? raw.substr(0, raw.size() - 4) : raw;
    static const std::map<std::string, std::pair<std::string, std::string>> table = {
        {"sdr", {"SDR", "sdr"}}, {"hdr", {"HDR", "hdr"}},
        {"dolbyHdr", {"Dolby Vision", "dolby_vision"}},
        {"technicolorHdr", {"Technicolor HDR", "technicolor"}}};
    const auto found = table.find(range);
    const std::string display = found == table.end() ? readable(range) : found->second.first;
    const std::string label = found == table.end() ? snakeCase(range) : found->second.second;
    Json result = Json::object();
    result.set("display", Json::text(display + (lowLatency ? " · Low latency" : "")));
    result.set("label", Json::text(label));
    result.set("lowLatency", Json::flag(lowLatency));
    return result;
}

Json signalHdrType(const std::string &raw) {
    const std::string type = snakeCase(raw);
    static const std::map<std::string, std::string> known = {
        {"none", "SDR"}, {"hdr10", "HDR10"}, {"hlg", "HLG"},
        {"dolby_vision", "Dolby Vision"},
        {"dolby_ll", "Dolby Vision (low latency)"}};
    const auto found = known.find(type);
    Json result = objectWithName(found == known.end() ? spelledHdrType(type) : found->second,
                                 type == "dolby_ll" ? "dolby_vision_low_latency" : type);
    return result;
}

Json signalEotf(unsigned code) {
    static const char *labels[] = {"sdr", "hdr", "pq", "hlg"};
    if (code >= sizeof(labels) / sizeof(labels[0]))
        return Json();
    Json result = Json::object();
    result.set("label", Json::text(labels[code]));
    return result;
}

Json signalColorimetry(const std::string &raw) {
    Json result = Json::object();
    result.set("label", Json::text(raw == "BT2020_RGBORYCbCr" ? "bt2020_rgb_or_ycbcr"
                                                               : snakeCase(raw)));
    return result;
}

Json signalEncoding(const std::string &raw) {
    static const std::map<std::string, std::string> labels = {
        {"RGB", "rgb_444"}, {"YCbCr444", "ycbcr_444"},
        {"YCbCr422", "ycbcr_422"}, {"YCbCr420", "ycbcr_420"}};
    const auto found = labels.find(raw);
    Json result = Json::object();
    result.set("label", Json::text(found == labels.end() ? snakeCase(raw) : found->second));
    return result;
}

Json powerOnReason(const std::string &raw) {
    Json result = Json::object();
    result.set("label", Json::text(raw == "wakeOnWiFi" ? "wake_on_wifi" : snakeCase(raw)));
    return result;
}

Json inputs() {
    Json result = Json::object();
    result.set("hdmi1", Json::text("HDMI 1"));
    result.set("hdmi2", Json::text("HDMI 2"));
    result.set("hdmi3", Json::text("HDMI 3"));
    result.set("hdmi4", Json::text("HDMI 4"));
    result.set("livetv", Json::text("Live TV"));
    return result;
}

std::string hdmiKey(unsigned port) { return "hdmi" + std::to_string(port); }

Json hdmiKeyForPortId(const std::string &portId) {
    static const std::regex expression("^HDMI_?([1-4])$", std::regex::icase);
    std::smatch match;
    if (!std::regex_match(portId, match, expression))
        return Json();
    return Json::text(hdmiKey(static_cast<unsigned>(std::stoul(match[1].str()))));
}

Json hdmiPort(const std::string &key) {
    static const std::regex expression("^hdmi([1-4])$");
    std::smatch match;
    return std::regex_match(key, match, expression)
               ? Json::numeric(std::stod(match[1].str()))
               : Json();
}

bool isHdmiKey(const std::string &key) { return hdmiPort(key).valid(); }

std::string shortAppId(const std::string &appId, const Json &aliases) {
    static const std::string prefix = "com.webos.app.";
    std::string id = appId;
    if (id.compare(0, prefix.size(), prefix) == 0)
        id.erase(0, prefix.size());
    return aliases.has(id) ? aliases[id].string() : id;
}

std::string inputAppId(const std::string &key) { return "com.webos.app." + key; }

Json input(const std::string &id) {
    Json result = Json::object();
    const Json table = inputs();
    result.set("display", Json::text(table.has(id) ? table[id].string() : id));
    result.set("label", Json::text(id));
    return result;
}

std::string inputTitle(const std::string &id, const Json &name) {
    const std::string display = name.string();
    return !display.empty() && display != id ? display + " (" + upper(id) + ")" : id;
}

Json powerStates() {
    static const Json table = [] {
        Json values = Json::object();
        const auto add = [&values](const std::string &key, const std::string &display,
                                   bool systemOn, bool screenOn) {
            Json state = Json::object();
            state.set("display", Json::text(display));
            state.set("systemOn", Json::flag(systemOn));
            state.set("screenOn", Json::flag(screenOn));
            values.set(key, state);
        };
        add("active", say::msg("srv.power.on", "On"), true, true);
        add("on", say::msg("srv.power.on", "On"), true, true);
        add("screenoff", say::msg("srv.power.screenOff", "Screen off"), true, false);
        add("screensaver", say::msg("srv.power.screenSaver", "Screen Saver"), true, true);
        add("alwaysready", say::msg("srv.power.alwaysReady", "Always Ready"), false, false);
        add("activestandby", say::msg("srv.power.standby", "Standby"), false, false);
        add("standby", say::msg("srv.power.standby", "Standby"), false, false);
        add("suspend", say::msg("srv.power.standby", "Standby"), false, false);
        add("preparesuspend", say::msg("srv.power.standby", "Standby"), false, false);
        add("requestpoweroff", say::msg("srv.power.off", "Off"), false, false);
        add("poweroff", say::msg("srv.power.off", "Off"), false, false);
        add("off", say::msg("srv.power.off", "Off"), false, false);
        add("prepared", say::msg("srv.power.starting", "Starting up"), true, false);
        add("processing", say::msg("srv.power.standby", "Standby"), false, false);
        return values;
    }();
    return table;
}

Json powerState(const Json &raw) {
    const std::string original = raw.string();
    std::string key = lower(original);
    key.erase(std::remove_if(key.begin(), key.end(), [](unsigned char c) {
                  return std::isspace(c) || c == '_' || c == '-';
              }), key.end());
    const Json table = powerStates();
    if (table.has(key))
        return table[key];
    Json result = Json::object();
    result.set("display", Json::text(original.empty() ? "Unknown" : original));
    result.set("systemOn", Json::flag(false));
    result.set("screenOn", Json::flag(false));
    return result;
}

Json pictureModes() {
    static const Json table = [] {
        Json values = Json::object();
        const auto add = [&values](const std::string &id, const std::string &display,
                                   const std::string &label, bool haveDisplay = true,
                                   bool haveLabel = true) {
            Json item = Json::object();
            if (haveDisplay)
                item.set("display", Json::text(display));
            if (haveLabel)
                item.set("label", Json::text(label));
            values.set(id, item);
        };
        add("personalized", "Personalized", "personalized");
        add("hdrPersonalized", "HDR Personalized", "personalized");
        add("dolbyHdrPersonalized", "", "personalized", false, true);
        add("vivid", "Vivid", "vivid");
        add("hdrVivid", "HDR Vivid", "vivid");
        add("dolbyHdrVivid", "Dolby Vision Vivid", "vivid");
        add("standard", "Standard", "", true, false);
        add("normal", "Standard", "standard");
        add("hdrStandard", "HDR Standard", "standard");
        add("dolbyHdrStandard", "Dolby Vision Standard", "standard");
        add("hdrExternal", "", "standard", false, true);
        add("eco", "Eco", "eco");
        add("hdrEco", "", "eco", false, true);
        add("cinema", "Cinema", "cinema");
        add("hdrCinema", "HDR Cinema", "cinema");
        add("dolbyHdrCinema", "Dolby Vision Cinema", "cinema");
        add("hdrCinemaBright", "HDR Cinema Bright", "cinema_bright");
        add("dolbyHdrCinemaBright", "Dolby Vision Cinema Bright", "cinema_bright");
        add("dolbyHdrDarkAmazon", "", "cinema_bright", false, true);
        add("hdrCinemaHome", "HDR Cinema Home", "", true, false);
        add("dolbyHdrCinemaHome", "Dolby Vision Cinema Home", "", true, false);
        add("sports", "", "sports", false, true);
        add("game", "Game", "game");
        add("hdrGame", "HDR Game", "game");
        add("dolbyHdrGame", "Dolby Vision Game", "game");
        add("photo", "", "photo", false, true);
        add("filmMaker", "Filmmaker", "filmmaker");
        add("hdrFilmMaker", "HDR Filmmaker", "filmmaker");
        add("dolbyHdrFilmMaker", "Dolby Vision Filmmaker", "", true, false);
        add("expert1", "ISF Expert (Bright)", "expert_bright");
        add("expert2", "ISF Expert (Dark)", "expert_dark");
        add("technicolor", "Technicolor", "", true, false);
        add("technicolorHdr", "Technicolor HDR", "", true, false);
        add("hdrEffect", "HDR Effect", "hdr_effect");
        add("gallery", "Gallery", "gallery");
        return values;
    }();
    return table;
}

Json pictureMode(const std::string &raw) {
    const Json table = pictureModes();
    const Json known = table[raw];
    const std::string display = known["display"].string();
    const std::string label = known["label"].string();
    return objectWithName(display.empty() ? spelledPictureMode(raw) : display,
                          label.empty() ? snakeCase(raw) : label);
}

Json soundOutputs() {
    static const Json table = [] {
        Json values = Json::object();
        const std::pair<const char *, const char *> entries[] = {
            {"tv_speaker", "TV Speaker"}, {"external_arc", "HDMI ARC"},
            {"optical", "Optical"}, {"external_optical", "Optical"},
            {"ext_speaker_optical", "Optical"}, {"ext_speaker_builtin_lg_optical", "Optical"},
            {"ext_speaker_arc", "HDMI ARC"}, {"headphone", "Headphone / AUX"},
            {"bt_soundbar", "Bluetooth"}, {"tv_external_speaker", "TV Speaker + Optical"},
            {"tv_speaker_bluetooth", "TV Speaker + Bluetooth"},
            {"tv_speaker_bt_surround", "TV Speaker + Bluetooth"},
            {"tv_speaker_external_arc", "TV Speaker + HDMI ARC"}, {"wow_cast", "LG WOWCAST"},
            {"mobile_phone", "Mobile Phone"}, {"wisa_speaker", "WiSA Speakers"},
            {"external_speaker", "External Speaker"}, {"lineout", "Line Out"},
            {"soundbar", "LG Sound Sync"}, {"tv_speaker_headphone", "TV Speaker + Headphone"},
            {"internal", "TV Speaker"}};
        for (const auto &entry : entries)
            values.set(entry.first, Json::text(entry.second));
        return values;
    }();
    return table;
}

Json soundOutput(const std::string &raw) {
    const Json table = soundOutputs();
    std::string display = table[raw].string();
    if (display.empty()) {
        display = raw;
        std::replace(display.begin(), display.end(), '_', ' ');
        bool capitalize = true;
        for (char &character : display) {
            if (character == ' ') {
                capitalize = true;
            } else if (capitalize) {
                character = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
                capitalize = false;
            }
        }
    }
    return objectWithName(display, snakeCase(raw));
}

Json audioCodec(const Json &raw) {
    std::string codec = lower(raw.string());
    static const std::map<std::string, std::string> table = {
        {"pcm", "PCM"}, {"lpcm", "PCM"}, {"aac", "AAC"}, {"heaac", "HE-AAC"},
        {"ac3", "Dolby Digital"}, {"eac3", "Dolby Digital Plus"}, {"ac4", "Dolby AC-4"},
        {"truehd", "Dolby TrueHD"}, {"dts", "DTS"}, {"dtshd", "DTS-HD"},
        {"dtsx", "DTS:X"}, {"mpeg", "MPEG audio"}, {"mpegh", "MPEG-H"},
        {"mp3", "MP3"}, {"opus", "Opus"}, {"vorbis", "Vorbis"}, {"flac", "FLAC"}};
    const auto found = table.find(codec);
    return objectWithName(found == table.end() ? upper(codec) : found->second, snakeCase(codec));
}

Json audioImmersive(const Json &raw) {
    const std::string original = raw.string();
    std::string key = lower(original);
    key.erase(std::remove_if(key.begin(), key.end(), [](unsigned char c) {
                  return std::isspace(c) || c == '_' || c == ':' || c == '-';
              }), key.end());
    if (key.empty() || key == "none")
        return Json();
    std::string display = original;
    std::string label = snakeCase(original);
    if (key == "atmos" || key == "dolbyatmos") {
        display = "Dolby Atmos";
        label = "dolby_atmos";
    } else if (key == "dtsx") {
        display = "DTS:X";
        label = "dts_x";
    } else if (key == "mpegh") {
        display = "MPEG-H";
        label = "mpeg_h";
    }
    return objectWithName(display, label);
}

Json logoDimming() {
    Json result = Json::object();
    result.set("off", Json::text("Off"));
    result.set("light", Json::text("Light"));
    result.set("strong", Json::text("High"));
    return result;
}

Json energySavingSteps() {
    Json result = Json::array();
    for (const char *step : {"auto", "off", "min", "med", "max", "screen_off"})
        result.add(Json::text(step));
    return result;
}

Json refresherStatuses() {
    Json result = Json::object();
    result.set("cancel_schedule", Json::text("Scheduled"));
    result.set("processing", Json::text("Running"));
    return result;
}

std::string refresherStatus(const std::string &raw) {
    const Json table = refresherStatuses();
    return table.has(raw) ? table[raw].string() : "Idle";
}

Json compensationStatuses() {
    Json result = Json::object();
    Json running = Json::object();
    running.set("display", Json::text("Running"));
    running.set("detail", Json::text("Completing Panel Maintenance (Short Cycle)"));
    Json idle = Json::object();
    idle.set("display", Json::text("Idle"));
    idle.set("detail", Json::text("Idle"));
    result.set("running", running);
    result.set("idle", idle);
    return result;
}

Json compensationStatus(bool running) { return compensationStatuses()[running ? "running" : "idle"]; }

Json emmcEolStates() {
    Json result = Json::object();
    result.set("1", objectWithName("Normal", "normal"));
    result.set("2", objectWithName("Warning", "warning"));
    result.set("3", objectWithName("Urgent", "urgent"));
    return result;
}

Json emmcEol(unsigned code) {
    return code >= 1 && code <= 3 ? emmcEolStates()[std::to_string(code)] : Json();
}

Json vrrType(const Json &raw) {
    const std::string type = raw.string();
    Json result = Json::object();
    result.set("label", Json::text(raw.isString() && !type.empty() ? snakeCase(type) : "off"));
    return result;
}

std::string tvDefaultName(const std::string &model) {
    const std::string unknown = "LGTV"; // the owner's name for a TV whose model is not known
    // The leading run of letters and digits, upper case: "OLED55C4PUA.DUSQLJR" -> "OLED55C4PUA",
    // "55UH850T-UA" -> "55UH850T". The brand is dropped.
    std::string text;
    for (char c : model)
        text.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    size_t start = text.find_first_not_of(" \t");
    if (start == std::string::npos)
        return unknown;
    text.erase(0, start);
    if (text.compare(0, 3, "LG ") == 0 || text.compare(0, 3, "LG-") == 0)
        text.erase(0, 3);
    size_t end = 0;
    while (end < text.size() && std::isalnum(static_cast<unsigned char>(text[end])))
        ++end;
    text.resize(end);
    auto letter = [&](size_t at) { return at < text.size() && std::isalpha(static_cast<unsigned char>(text[at])); };
    auto digit = [&](size_t at) { return at < text.size() && std::isdigit(static_cast<unsigned char>(text[at])); };
    size_t at = 0;
    const bool oled = text.compare(0, 4, "OLED") == 0;
    if (oled)
        at = 4;
    // The size in inches comes first, or after OLED.
    size_t digits = at;
    while (digit(digits))
        ++digits;
    if (digits - at >= 2 && digits - at <= 3)
        at = digits;
    if (oled) {
        // OLED series are a letter and the year's digit (B3, C2, G4), or the 2020 pair's X (CX).
        // What follows is the region and the panel's variant, so "C24LA" is the C2.
        if (letter(at) && (digit(at + 1) || (at + 1 < text.size() && text[at + 1] == 'X')))
            return text.substr(at, 2);
        return unknown;
    }
    // Other series keep their letters and the whole number: UQ8000, NANO856, QNED85, UH850.
    size_t lettersEnd = at;
    while (letter(lettersEnd))
        ++lettersEnd;
    size_t numberEnd = lettersEnd;
    while (digit(numberEnd))
        ++numberEnd;
    if (lettersEnd == at || numberEnd == lettersEnd)
        return unknown;
    return text.substr(at, numberEnd - at);
}
} // namespace names
} // namespace nativetv
