#include "service/control_commands.hpp"
#include "core/json.hpp"
#include "domain/names.hpp"
#include <algorithm>
#include <cerrno>
#include <cctype>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace nativetv {
namespace service {
namespace {
std::string legacyString(const Json &value) {
    if (!value.valid())
        return "undefined";
    if (value.isString())
        return value.string();
    if (value.isNumber()) {
        std::string number = value.dump();
        const size_t exponent = number.find_first_of("eE");
        const size_t end = exponent == std::string::npos ? number.size() : exponent;
        const size_t decimal = number.find('.');
        if (decimal != std::string::npos && decimal < end) {
            size_t trimmedEnd = end;
            while (trimmedEnd > decimal + 1 && number[trimmedEnd - 1] == '0')
                --trimmedEnd;
            if (trimmedEnd == decimal + 1)
                --trimmedEnd;
            number.erase(trimmedEnd, end - trimmedEnd);
        }
        return number;
    }
    if (value.isBoolean())
        return value.boolean() ? "true" : "false";
    if (value.isArray()) {
        std::string result;
        const std::vector<Json> values = value.items();
        for (size_t index = 0; index < values.size(); ++index) {
            if (index)
                result.push_back(',');
            if (!values[index].valid())
                continue;
            result += legacyString(values[index]);
        }
        return result;
    }
    if (value.isObject())
        return "[object Object]";
    return "null";
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

std::string trim(std::string value) {
    size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin])))
        ++begin;
    size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])))
        --end;
    return value.substr(begin, end - begin);
}

std::string truthyString(const Json &value) {
    if (!value.valid() || (value.isBoolean() && !value.boolean()) ||
        (value.isNumber() && value.number() == 0) ||
        (value.isString() && value.string().empty()))
        return "";
    return legacyString(value);
}

long parseIntegerOr(const Json &value, long fallback) {
    const std::string text = legacyString(value);
    const char *start = text.c_str();
    while (*start && std::isspace(static_cast<unsigned char>(*start)))
        ++start;
    errno = 0;
    char *end = nullptr;
    const long parsed = std::strtol(start, &end, 10);
    if (end == start)
        return fallback;
    if (errno == ERANGE)
        return *start == '-' ? LONG_MIN : LONG_MAX;
    return parsed;
}

std::string utf16Prefix(const std::string &text, size_t maximumUnits) {
    size_t byteOffset = 0;
    size_t units = 0;
    while (byteOffset < text.size()) {
        const unsigned char first = static_cast<unsigned char>(text[byteOffset]);
        size_t byteCount = 1;
        uint32_t codepoint = first;
        if ((first & 0xe0) == 0xc0 && byteOffset + 1 < text.size()) {
            byteCount = 2;
            codepoint = ((first & 0x1f) << 6) |
                (static_cast<unsigned char>(text[byteOffset + 1]) & 0x3f);
        } else if ((first & 0xf0) == 0xe0 && byteOffset + 2 < text.size()) {
            byteCount = 3;
            codepoint = ((first & 0x0f) << 12) |
                ((static_cast<unsigned char>(text[byteOffset + 1]) & 0x3f) << 6) |
                (static_cast<unsigned char>(text[byteOffset + 2]) & 0x3f);
        } else if ((first & 0xf8) == 0xf0 && byteOffset + 3 < text.size()) {
            byteCount = 4;
            codepoint = ((first & 0x07) << 18) |
                ((static_cast<unsigned char>(text[byteOffset + 1]) & 0x3f) << 12) |
                ((static_cast<unsigned char>(text[byteOffset + 2]) & 0x3f) << 6) |
                (static_cast<unsigned char>(text[byteOffset + 3]) & 0x3f);
        }
        const size_t codeUnits = codepoint > 0xffff ? 2 : 1;
        if (units + codeUnits > maximumUnits)
            break;
        units += codeUnits;
        byteOffset += byteCount;
    }
    return text.substr(0, byteOffset);
}
} // namespace

int controlVolumeTarget(const Json &value) {
    const long parsed = parseIntegerOr(value, 10);
    return static_cast<int>(std::max(0L, std::min(100L, parsed)));
}

int controlVolumeStep(const Json &value) {
    const long parsed = parseIntegerOr(value, 1);
    return static_cast<int>(std::max(-100L, std::min(100L, parsed)));
}

bool controlMuteTarget(const Json &value) {
    if (value.isBoolean())
        return value.boolean();
    if (value.isNumber())
        return value.number() == 1;
    if (!value.isString())
        return false;
    return value.string() == "true" || value.string() == "ON" || value.string() == "1";
}

std::string controlToastMessage(const Json &value) {
    const bool falsy = !value.valid() ||
        (value.isBoolean() && !value.boolean()) ||
        (value.isNumber() && value.number() == 0) ||
        (value.isString() && value.string().empty());
    return utf16Prefix(falsy ? std::string("hello") : legacyString(value), 120);
}

std::string controlServiceMenuTarget(const Json &value) {
    const std::string requested = truthyString(value["menu"]);
    return requested == "inStart" ? "inStart" : "ezAdjust";
}

int controlRemoteKeyCode(const Json &value) {
    if (!value.valid() || (value.isBoolean() && !value.boolean()) ||
        (value.isNumber() && value.number() == 0) ||
        (value.isString() && value.string().empty()))
        return 0;
    const std::string key = lower(trim(legacyString(value)));
    if (key == "up") return 103;
    if (key == "down") return 108;
    if (key == "left") return 105;
    if (key == "right") return 106;
    if (key == "ok") return 28;
    if (key == "back") return 412;
    if (key == "settings") return 139;
    return 0;
}

bool controlRemoteHome(const Json &value) {
    return lower(trim(legacyString(value))) == "home";
}

int controlMediaKeyCode(const Json &value, const std::string &playState,
                        const std::string &playerType) {
    if (!value.isString())
        return 0;
    const std::string &key = value.string();
    if (key == "play") return 207;
    if (key == "pause") return 119;
    if (key == "stop") return 128;
    if (key == "fastForward" || key == "fastforward") return 208;
    if (key == "rewind") return 168;
    if (key == "playPause" || key == "playpause")
        return playState == "playing" && playerType != "external input" ? 119 : 207;
    return 0;
}

std::string controlInputAppId(const Json &value) {
    const std::string key = legacyString(value);
    const Json inputs = names::inputs();
    return inputs.has(key) ? names::inputAppId(key) : std::string();
}

std::string controlPictureModeTarget(const Json &value) {
    return trim(truthyString(value));
}

bool controlEnergySavingTarget(const Json &value, std::string &target) {
    target = lower(trim(truthyString(value)));
    return target == "auto" || target == "off" || target == "min" || target == "med" ||
           target == "max" || target == "screen_off";
}

bool controlBacklightTarget(const Json &value, int &target) {
    const std::string text = legacyString(value);
    const char *start = text.c_str();
    while (*start && std::isspace(static_cast<unsigned char>(*start)))
        ++start;
    const char *numberStart = start;
    if (*numberStart == '+' || *numberStart == '-')
        ++numberStart;
    if (std::string(numberStart).compare(0, 8, "Infinity") == 0)
        return false;

    const char *numberEnd = numberStart;
    bool hasDigits = false;
    while (*numberEnd >= '0' && *numberEnd <= '9') {
        hasDigits = true;
        ++numberEnd;
    }
    if (*numberEnd == '.') {
        ++numberEnd;
        while (*numberEnd >= '0' && *numberEnd <= '9') {
            hasDigits = true;
            ++numberEnd;
        }
    }
    if (!hasDigits)
        return false;
    if (*numberEnd == 'e' || *numberEnd == 'E') {
        const char *exponentEnd = numberEnd + 1;
        if (*exponentEnd == '+' || *exponentEnd == '-')
            ++exponentEnd;
        const char *exponentDigits = exponentEnd;
        while (*exponentEnd >= '0' && *exponentEnd <= '9')
            ++exponentEnd;
        if (exponentEnd != exponentDigits)
            numberEnd = exponentEnd;
    }
    std::string prefix(start, static_cast<size_t>(numberEnd - start));
    char *end = nullptr;
    const double parsed = std::strtod(prefix.c_str(), &end);
    if (end == prefix.c_str() || !std::isfinite(parsed))
        return false;
    const double rounded = std::floor(parsed + 0.5);
    if (rounded < 0 || rounded > 100)
        return false;
    target = static_cast<int>(rounded);
    return true;
}

bool controlIndicatorLightTarget(const Json &value) {
    if (value.isBoolean())
        return value.boolean();
    if (!value.isString())
        return false;
    return value.string() == "on" || value.string() == "ON" ||
           value.string() == "true";
}

bool controlLogoDimmingTarget(const Json &value, std::string &target) {
    target = lower(trim(truthyString(value)));
    return names::logoDimming().has(target);
}

bool controlAlwaysReadyOffStartHour(const Json &value, int &hour) {
    std::string text = legacyString(value);
    const size_t colon = text.find(':');
    if (colon != std::string::npos)
        text.erase(colon);
    const long parsed = parseIntegerOr(Json::text(text), -1);
    if (parsed < 0 || parsed > 23)
        return false;
    hour = static_cast<int>(parsed);
    return true;
}

bool controlSleepTimerTarget(const Json &value, std::string &target) {
    target = value.valid() ? trim(legacyString(value)) : "off";
    return target == "off" || target == "10" || target == "30" ||
           target == "60" || target == "90" || target == "120";
}

bool controlPowerTimerChange(const Json &value, PowerTimerChange &change,
                             std::string &error) {
    change = PowerTimerChange();
    change.settings = Json::object();
    error.clear();
    const Json timerValue = value["timer"];
    if (!value.isObject() || !timerValue.isString() ||
        (timerValue.string() != "on" && timerValue.string() != "off")) {
        error = "timer must be on or off";
        return false;
    }

    change.timer = timerValue.string();
    const std::string prefix = change.timer + "Timer";
    change.enableKey = prefix + "Enable";
    if (value.has("time")) {
        const Json timeValue = value["time"];
        const std::string text = timeValue.valid() ? legacyString(timeValue) : "null";
        const size_t colon = text.find(':');
        const size_t hourLength = colon;
        bool shape = colon != std::string::npos && hourLength >= 1 && hourLength <= 2 &&
                     text.size() == colon + 3;
        for (size_t i = 0; shape && i < text.size(); ++i) {
            if (i == colon)
                continue;
            if (text[i] < '0' || text[i] > '9')
                shape = false;
        }
        const int hour = shape ? std::atoi(text.substr(0, colon).c_str()) : -1;
        const int minute = shape ? std::atoi(text.substr(colon + 1, 2).c_str()) : -1;
        if (!shape || hour < 0 || hour > 23 || minute < 0 || minute > 59) {
            error = "the time must be HH:MM, from 00:00 to 23:59";
            return false;
        }
        change.settings.set(prefix + "Hour", Json::text(std::to_string(hour)));
        change.settings.set(prefix + "Minute", Json::text(std::to_string(minute)));
    }
    if (value.has("days")) {
        const Json days = value["days"];
        if (!days.isArray()) {
            error = "days must be a list of 0 (Sunday) to 6 (Saturday)";
            return false;
        }
        int mask = 0;
        for (const Json &day : days.items()) {
            if (!day.isNumber() || day.number() < 0 || day.number() > 6 ||
                std::floor(day.number()) != day.number()) {
                error = "days must be a list of 0 (Sunday) to 6 (Saturday)";
                return false;
            }
            mask |= 1 << static_cast<int>(day.number());
        }
        change.settings.set(prefix + "Weekday", Json::text(std::to_string(mask)));
    }

    change.enabledProvided = value.has("enabled");
    const Json enabled = value["enabled"];
    change.enable = enabled.isBoolean() && enabled.boolean();
    change.disable = enabled.isBoolean() && !enabled.boolean();
    if (change.settings.keys().empty() && !change.enabledProvided) {
        error = "nothing to change";
        return false;
    }
    return true;
}

std::string controlCloseAppId(const Json &value) {
    return trim(truthyString(value));
}

std::string controlSoundOutputTarget(const Json &value) {
    return trim(truthyString(value));
}

std::string controlSoundOutputAlias(const std::string &target) {
    if (target == "optical") return "external_optical";
    if (target == "external_optical") return "optical";
    if (target == "tv_speaker") return "internal";
    if (target == "internal") return "tv_speaker";
    return std::string();
}

bool controlLaunchUrlTarget(const Json &value, std::string &target) {
    target = trim(truthyString(value));
    const std::string normalized = lower(target);
    const bool http = normalized.compare(0, 7, "http://") == 0;
    const bool https = normalized.compare(0, 8, "https://") == 0;
    if ((!http && !https) || target.size() <= (https ? 8U : 7U))
        return false;
    for (unsigned char character : target)
        if (std::isspace(character))
            return false;
    return true;
}

bool volumeScenarioUnsupported(const Json &reply) {
    const std::string error = lower(reply["errorText"].string());
    return error.find("current scenario doesn't support") != std::string::npos;
}

} // namespace service
} // namespace nativetv
