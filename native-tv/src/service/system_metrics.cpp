#include "service/system_metrics.hpp"
#include "service/kernel_files.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <sstream>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <utility>
#include <vector>

namespace nativetv {
namespace service {
namespace {
const char *LG_THERMAL = "/proc/lg/pm/temperature";
const char *SYS_THERMAL = "/sys/class/thermal/thermal_zone0/temp";
const char *EMMC = "/sys/block/mmcblk0/device/";
const uint64_t MAX_SAFE_INTEGER = 9007199254740991ULL;
std::string fileText(const std::string &path) {
    std::string text;
    return readKernelFile(path, text) ? text : "";
}
std::string trim(const std::string &text) {
    size_t first = 0, last = text.size();
    while (first < last && std::isspace(static_cast<unsigned char>(text[first])))
        ++first;
    while (last > first && std::isspace(static_cast<unsigned char>(text[last - 1])))
        --last;
    return text.substr(first, last - first);
}
size_t findCaseInsensitive(const std::string &text, const std::string &needle) {
    if (needle.empty() || needle.size() > text.size())
        return std::string::npos;
    for (size_t at = 0; at + needle.size() <= text.size(); ++at) {
        size_t index = 0;
        while (index < needle.size() &&
               std::tolower(static_cast<unsigned char>(text[at + index])) ==
                   std::tolower(static_cast<unsigned char>(needle[index])))
            ++index;
        if (index == needle.size())
            return at;
    }
    return std::string::npos;
}
bool decimalAt(const std::string &text, size_t &at, uint64_t &value) {
    while (at < text.size() && std::isspace(static_cast<unsigned char>(text[at])))
        ++at;
    size_t first = at;
    while (at < text.size() && std::isdigit(static_cast<unsigned char>(text[at])))
        ++at;
    return at > first && parseUnsigned(text.substr(first, at - first), value);
}
bool bracketTextAt(const std::string &text, size_t &at, std::string &value) {
    while (at < text.size() && std::isspace(static_cast<unsigned char>(text[at])))
        ++at;
    if (at >= text.size() || text[at] != '[')
        return false;
    size_t end = text.find(']', at + 1);
    if (end == std::string::npos)
        return false;
    value = trim(text.substr(at + 1, end - at - 1));
    at = end + 1;
    return true;
}
bool bracketNumberAt(const std::string &text, size_t &at, uint64_t &value) {
    std::string raw;
    return bracketTextAt(text, at, raw) && parseUnsigned(raw, value);
}
bool bracketAfter(const std::string &text, const std::string &marker, std::string &value) {
    size_t at = findCaseInsensitive(text, marker);
    if (at == std::string::npos)
        return false;
    at += marker.size();
    at = text.find('[', at);
    return at != std::string::npos && bracketTextAt(text, at, value);
}
bool bracketNumberAfter(const std::string &text, const std::string &marker, uint64_t &value) {
    std::string raw;
    return bracketAfter(text, marker, raw) && parseUnsigned(raw, value);
}
bool decimalAfter(const std::string &text, const std::string &marker, uint64_t &value) {
    size_t at = findCaseInsensitive(text, marker);
    if (at == std::string::npos)
        return false;
    at += marker.size();
    return decimalAt(text, at, value);
}
bool parseParenthesizedNumber(const std::string &text, size_t &at, uint64_t &value) {
    while (at < text.size() && std::isspace(static_cast<unsigned char>(text[at])))
        ++at;
    if (at >= text.size() || text[at] != '(')
        return false;
    ++at;
    bool found = decimalAt(text, at, value);
    while (at < text.size() && std::isspace(static_cast<unsigned char>(text[at])))
        ++at;
    return found && at < text.size() && text[at++] == ')';
}
bool parseSig(const std::string &text, uint64_t &width, uint64_t &height,
              uint64_t &hTotal, uint64_t &vTotal, uint64_t &refresh) {
    size_t at = findCaseInsensitive(text, "Sig:");
    if (at == std::string::npos)
        return false;
    at += 4;
    if (!bracketNumberAt(text, at, width))
        return false;
    parseParenthesizedNumber(text, at, hTotal);
    while (at < text.size() && std::isspace(static_cast<unsigned char>(text[at])))
        ++at;
    if (at >= text.size() || std::tolower(static_cast<unsigned char>(text[at++])) != 'x' ||
        !bracketNumberAt(text, at, height))
        return false;
    parseParenthesizedNumber(text, at, vTotal);
    while (at < text.size() && std::isspace(static_cast<unsigned char>(text[at])))
        ++at;
    if (at >= text.size() || text[at++] != '@' || !bracketNumberAt(text, at, refresh))
        return false;
    while (at < text.size() && std::isspace(static_cast<unsigned char>(text[at])))
        ++at;
    return at + 1 < text.size() && std::tolower(static_cast<unsigned char>(text[at])) == 'h' &&
           std::tolower(static_cast<unsigned char>(text[at + 1])) == 'z';
}
std::string stableSection(const std::string &raw) {
    const std::string marker = "[Stable Sync Info]";
    size_t at = findCaseInsensitive(raw, marker);
    return at == std::string::npos ? raw : raw.substr(at + marker.size());
}
bool startsCaseInsensitive(const std::string &text, const std::string &prefix) {
    if (text.size() < prefix.size())
        return false;
    for (size_t i = 0; i < prefix.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(text[i])) !=
            std::tolower(static_cast<unsigned char>(prefix[i])))
            return false;
    }
    return true;
}
std::string lowercase(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });
    return text;
}
Json socArchitecture(const std::string &prefix) {
    const Json environment = Json::parse(
        fileText(prefix + "/var/luna/preferences/environmentCondition"));
    std::string raw = environment["boardTypeStr"].string();
    if (raw.empty())
        raw = environment["socChip"].string();
    if (raw.empty())
        raw = trim(fileText(prefix + "/proc/lg/base/chip_name"));
    while (!raw.empty() && raw.front() == '_')
        raw.erase(raw.begin());
    while (!raw.empty() && raw.back() == '_')
        raw.pop_back();
    if (raw.empty())
        return Json();
    std::transform(raw.begin(), raw.end(), raw.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    if (raw == "O22")
        return Json::text("Alpha 9 Gen 5 (O22)");
    if (raw == "O20")
        return Json::text("Alpha 9 Gen 3 (O20)");
    if (raw == "O18")
        return Json::text("Alpha 9 Gen 1 (O18)");
    if (raw == "M16P" || raw == "M16PLUS")
        return Json::text("Alpha 7 (M16P)");
    return Json::text(raw);
}
bool vicSize(uint64_t vic, uint64_t &width, uint64_t &height) {
    if (vic == 1) { width = 640; height = 480; }
    else if (vic == 2 || vic == 3 || vic == 17 || vic == 18) {
        width = 720; height = vic < 17 ? 480 : 576;
    } else if (vic == 4 || vic == 19 || vic == 60 || vic == 61 || vic == 62) {
        width = 1280; height = 720;
    } else if (vic == 16 || vic == 31 || vic == 32 || vic == 33 || vic == 34 || vic == 63 || vic == 64) {
        width = 1920; height = 1080;
    } else if (vic == 93 || vic == 94 || vic == 95 || vic == 96 || vic == 97 ||
               vic == 103 || vic == 104 || vic == 105 || vic == 106 || vic == 107 ||
               vic == 117 || vic == 118 || vic == 119 || vic == 120) {
        width = 3840; height = 2160;
    } else if (vic == 98 || vic == 99 || vic == 100 || vic == 101 || vic == 102 ||
               vic == 218 || vic == 219) {
        width = 4096; height = 2160;
    } else {
        return false;
    }
    return true;
}
Json nullableNumber(bool present, double value) {
    return present ? Json::numeric(value) : Json();
}
Json nullableText(bool present, const std::string &value) {
    return present ? Json::text(value) : Json();
}
Json nullableFlag(bool present, bool value) {
    return present ? Json::flag(value) : Json();
}
struct HdmiStatus {
    bool present;
    std::string raw;
};
struct HdmiParsed {
    bool connected;
    bool hasWidth;
    bool hasHeight;
    bool hasRefresh;
    bool hasPixelClock;
    uint64_t width;
    uint64_t height;
    uint64_t refresh;
    double pixelClockKhz;
    bool hasLock;
    bool locked;
    bool hasPhyMode;
    std::string phyMode;
    bool hasChroma;
    std::string chroma;
    bool hasHdcp;
    std::string hdcp;
    bool hasTmdsClock;
    uint64_t tmdsClockKhz;
    bool hasQms;
    bool qms;
    bool diagnosticConnected;
    bool hasAllm;
    bool allm;
    bool hasVrr;
    bool vrr;
    bool hasSourcePower;
    bool sourcePowered;
    bool hasColorDepth;
    std::string colorDepth;
    bool interlaced;
};
HdmiParsed parseHdmiStatus(const std::string &raw) {
    HdmiParsed result{};
    uint64_t width = 0, height = 0, refresh = 0, rawClockKhz = 0;
    double clockKhz = 0;
    uint64_t hTotal = 0, vTotal = 0;
    bool hasWidthField = decimalAfter(raw, "horizontal-active:", width);
    bool hasHeightField = decimalAfter(raw, "vertical-active:", height);
    bool hasTiming = hasWidthField && hasHeightField && width > 0 && height > 0;
    if (hasTiming) {
        result.hasRefresh = decimalAfter(raw, "pixel-clock-V:", refresh) && refresh > 0;
        result.hasPixelClock = decimalAfter(raw, "pixel-clock:", rawClockKhz) && rawClockKhz > 0;
        clockKhz = static_cast<double>(rawClockKhz);
    } else {
        width = 0;
        height = 0;
        std::string stable = stableSection(raw);
        hasTiming = parseSig(stable, width, height, hTotal, vTotal, refresh);
        if (hasTiming) {
            result.hasRefresh = refresh > 0;
            uint64_t vic = 0, correctedWidth = 0, correctedHeight = 0;
            if (bracketNumberAfter(stable, "VIC Code", vic) && vicSize(vic, correctedWidth, correctedHeight) &&
                std::max(width, correctedWidth) - std::min(width, correctedWidth) <= 2 &&
                std::max(height, correctedHeight) - std::min(height, correctedHeight) <= 2) {
                width = correctedWidth;
                height = correctedHeight;
            }
            std::string phyMode;
            if (bracketAfter(raw, "PHY Mode", phyMode) && startsCaseInsensitive(phyMode, "FRL")) {
                if (hTotal && vTotal && refresh)
                    clockKhz = static_cast<double>(hTotal) * static_cast<double>(vTotal) *
                               static_cast<double>(refresh) / 1000.0;
            } else {
                uint64_t pixelClock = 0;
                if (bracketNumberAfter(stable, "Pixel Clk", pixelClock))
                    clockKhz = static_cast<double>(pixelClock);
            }
            result.hasPixelClock = clockKhz > 0;
        }
    }

    std::string lockText;
    result.hasLock = bracketAfter(raw, "PHY Lock", lockText) && !lockText.empty() &&
                     std::isdigit(static_cast<unsigned char>(lockText[0]));
    result.locked = result.hasLock && lockText[0] == '1';
    bool hasTimingValue = hasTiming && width > 0 && height > 0 && (!result.hasLock || result.locked);
    result.connected = result.hasLock ? result.locked :
                       findCaseInsensitive(raw, "connected: on") != std::string::npos ||
                       hasTimingValue;
    result.hasWidth = hasTimingValue;
    result.hasHeight = hasTimingValue;
    result.width = hasTimingValue ? width : 0;
    result.height = hasTimingValue ? height : 0;
    result.refresh = refresh;
    result.hasRefresh = hasTimingValue && result.hasRefresh;
    result.pixelClockKhz = clockKhz;
    result.hasPixelClock = hasTimingValue && result.hasPixelClock;

    std::string field;
    result.hasPhyMode = bracketAfter(raw, "PHY Mode", result.phyMode);
    std::string stable = stableSection(raw);
    result.hasChroma = bracketAfter(stable, "Video Format", result.chroma);
    const std::string hdcpMarker = "Current HDCP Auth Version =>";
    size_t hdcpAt = findCaseInsensitive(raw, hdcpMarker);
    if (hdcpAt != std::string::npos) {
        size_t tokenAt = findCaseInsensitive(raw.substr(hdcpAt + hdcpMarker.size()), "HDCP");
        if (tokenAt != std::string::npos) {
            tokenAt += hdcpAt + hdcpMarker.size();
            size_t end = tokenAt + 4;
            while (end < raw.size() && (std::isalnum(static_cast<unsigned char>(raw[end])) || raw[end] == '_'))
                ++end;
            result.hdcp = raw.substr(tokenAt, end - tokenAt);
            result.hasHdcp = end > tokenAt + 4;
        }
    }
    result.hasTmdsClock = !startsCaseInsensitive(result.phyMode, "FRL") &&
                          bracketNumberAfter(stable, "TMDS Clk", result.tmdsClockKhz);
    uint64_t qms = 0;
    result.hasQms = bracketNumberAfter(raw, "QMSMode", qms);
    result.qms = result.hasQms && qms == 1;
    result.diagnosticConnected = findCaseInsensitive(raw, "connected: on") != std::string::npos ||
                                 result.locked;
    uint64_t fiveVolt = 0;
    result.hasSourcePower = bracketNumberAfter(raw, "is5Vconnected", fiveVolt);
    result.sourcePowered = result.hasSourcePower && fiveVolt == 1;
    result.diagnosticConnected = result.diagnosticConnected || result.sourcePowered;
    uint64_t allm = 0;
    result.hasAllm = bracketNumberAfter(raw, "isAllm", allm);
    result.allm = result.hasAllm && allm == 1;
    uint64_t freeSync = 0, minVrr = 0, maxVrr = 0;
    bool hasFreeSync = bracketNumberAfter(raw, "isFreeSync", freeSync);
    size_t minAt = findCaseInsensitive(raw, "VRR Min");
    bool hasVrrRange = false;
    if (minAt != std::string::npos) {
        minAt += std::string("VRR Min").size();
        uint64_t first = 0;
        if (bracketNumberAt(raw, minAt, first)) {
            size_t slash = raw.find('/', minAt);
            if (slash != std::string::npos) {
                size_t maxAt = findCaseInsensitive(raw.substr(slash + 1), "Max");
                if (maxAt != std::string::npos) {
                    maxAt += slash + 1 + 3;
                    hasVrrRange = bracketNumberAt(raw, maxAt, maxVrr);
                    minVrr = first;
                }
            }
        }
    }
    result.hasVrr = hasFreeSync || hasVrrRange;
    result.vrr = result.hasVrr && ((hasFreeSync && freeSync != 0) ||
                                   (hasVrrRange && (minVrr > 0 || maxVrr > 0)));
    size_t depthAt = findCaseInsensitive(raw, "deep-color-mode:");
    if (depthAt != std::string::npos) {
        depthAt += std::string("deep-color-mode:").size();
        size_t lineEnd = raw.find('\n', depthAt);
        std::istringstream depthStream(raw.substr(depthAt, lineEnd == std::string::npos
                                                              ? std::string::npos
                                                              : lineEnd - depthAt));
        std::string first, second;
        if (depthStream >> first >> second) {
            result.colorDepth = first + " " + second;
            result.hasColorDepth = true;
        }
    }
    if (!result.hasColorDepth && bracketAfter(raw, "DeepColorMode", field)) {
        size_t first = 0;
        while (first < field.size() && (field[first] == '.' || std::isspace(static_cast<unsigned char>(field[first]))))
            ++first;
        result.colorDepth = field.substr(first);
        result.hasColorDepth = !result.colorDepth.empty();
    }
    result.interlaced = findCaseInsensitive(raw, "interlaced: yes") != std::string::npos ||
                        (bracketAfter(raw, "Interlaced", field) && field == "1");
    return result;
}
Json hdmiPortFact(const std::string &raw) {
    HdmiParsed parsed = parseHdmiStatus(raw);
    Json fact = Json::object(), timing = Json::object(), link = Json::object();
    timing.set("connected", Json::flag(parsed.connected));
    timing.set("width", nullableNumber(parsed.hasWidth, static_cast<double>(parsed.width)));
    timing.set("height", nullableNumber(parsed.hasHeight, static_cast<double>(parsed.height)));
    timing.set("refreshHz", nullableNumber(parsed.hasRefresh, static_cast<double>(parsed.refresh)));
    timing.set("pixelClockKhz", nullableNumber(parsed.hasPixelClock, parsed.pixelClockKhz));
    link.set("locked", Json::flag(parsed.locked));
    link.set("phy_mode", nullableText(parsed.hasPhyMode, parsed.phyMode));
    link.set("chroma", nullableText(parsed.hasChroma, parsed.chroma));
    link.set("hdcp", nullableText(parsed.hasHdcp, parsed.hdcp));
    link.set("tmds_clock_khz", nullableNumber(parsed.hasTmdsClock, static_cast<double>(parsed.tmdsClockKhz)));
    link.set("qms", nullableFlag(parsed.hasQms, parsed.qms));
    fact.set("timing", timing);
    fact.set("link", link);
    fact.set("diagnosticConnected", Json::flag(parsed.diagnosticConnected));
    fact.set("allm", nullableFlag(parsed.hasAllm, parsed.allm));
    fact.set("vrr", nullableFlag(parsed.hasVrr, parsed.vrr));
    fact.set("sourcePowered", nullableFlag(parsed.hasSourcePower, parsed.sourcePowered));
    fact.set("colorDepth", nullableText(parsed.hasColorDepth, parsed.colorDepth));
    fact.set("interlaced", Json::flag(parsed.interlaced));
    return fact;
}
std::vector<HdmiStatus> readHdmiStatusFiles(const std::string &prefix) {
    std::vector<HdmiStatus> statuses;
    for (unsigned port = 0; port < 4; ++port) {
        HdmiStatus status{};
        std::string path = prefix + "/proc/lg/hdmi20/port" + std::to_string(port) + "/status";
        status.present = readKernelFile(path, status.raw);
        statuses.push_back(status);
    }
    return statuses;
}
Json hdmiRawSnapshot(const std::vector<HdmiStatus> &statuses) {
    Json result = Json::array();
    for (size_t i = 0; i < statuses.size(); ++i)
        result.add(statuses[i].present ? Json::text(statuses[i].raw) : Json());
    return result;
}
Json hdmiFactsSnapshot(const std::vector<HdmiStatus> &statuses) {
    Json result = Json::array();
    for (size_t i = 0; i < statuses.size(); ++i)
        result.add(statuses[i].present ? hdmiPortFact(statuses[i].raw) : Json());
    return result;
}
bool exists(const std::string &path) {
    struct stat info;
    return stat(path.c_str(), &info) == 0;
}
bool fileNumber(const std::string &path, uint64_t &value, unsigned base = 10) {
    std::istringstream stream(fileText(path));
    std::string token, extra;
    return (stream >> token) && !(stream >> extra) && parseUnsigned(token, value, base);
}
// A kernel range list such as "0-3" or "0,2-3", as the CPU ids it names.
std::vector<unsigned> cpuRange(const std::string &raw) {
    std::vector<unsigned> result;
    std::istringstream parts(raw);
    for (std::string part; std::getline(parts, part, ',');) {
        size_t dash = part.find('-');
        uint64_t low, high;
        if (!parseUnsigned(trim(part.substr(0, dash)), low))
            continue;
        if (dash == std::string::npos)
            high = low;
        else if (!parseUnsigned(trim(part.substr(dash + 1)), high))
            continue;
        if (high < low || high > 4095)
            continue;
        for (uint64_t id = low; id <= high && result.size() < 4096; ++id)
            result.push_back(static_cast<unsigned>(id));
    }
    return result;
}
std::map<std::string, double> memory(const std::string &path) {
    std::map<std::string, double> values;
    std::istringstream lines(fileText(path));
    for (std::string line; std::getline(lines, line);) {
        std::istringstream fields(line);
        std::string key, token, unit;
        uint64_t number;
        if ((fields >> key >> token >> unit) && key.back() == ':' && unit == "kB" &&
            parseUnsigned(token, number) && number <= 9007199254740991ULL)
            values[key.substr(0, key.size() - 1)] = static_cast<double>(number);
    }
    return values;
}
Json swapBacking(const std::string &path) {
    std::istringstream lines(fileText(path));
    std::string line;
    std::getline(lines, line);
    uint64_t largest = 0;
    Json result;
    bool found = false;
    while (std::getline(lines, line)) {
        std::istringstream fields(line);
        std::string name, type, token;
        uint64_t size;
        if (!(fields >> name >> type >> token) || !parseUnsigned(token, size) ||
            (found && size <= largest))
            continue;
        largest = size;
        found = true;
        std::transform(name.begin(), name.end(), name.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        result = Json::text(name.find("zram") != std::string::npos ? "zram"
                            : type == "file"                       ? "file"
                                                                   : "flash");
    }
    return result;
}
Json emmcHealth(const std::string &prefix) {
    Json result = Json::object();
    std::string wear;
    unsigned health = 100;
    std::vector<std::string> ranges;
    std::istringstream tokens(fileText(prefix + "life_time"));
    unsigned index = 0;
    for (std::string token; tokens >> token;) {
        uint64_t value;
        bool valid = parseUnsigned(token, value, 16) && value >= 1 && value <= 11;
        if (index < 2)
            result.set(index == 0 ? "life_est_a" : "life_est_b",
                       valid ? Json::numeric(static_cast<double>(value)) : Json());
        ++index;
        if (!valid)
            continue;
        std::string range =
            value == 11 ? ">100%"
                        : std::to_string((value - 1) * 10) + "-" + std::to_string(value * 10) + "%";
        if (std::find(ranges.begin(), ranges.end(), range) == ranges.end())
            ranges.push_back(range);
        health = std::min(health, value == 11 ? 0 : static_cast<unsigned>(100 - value * 10));
    }
    for (const char *key : {"life_est_a", "life_est_b"})
        if (!result.has(key))
            result.set(key, Json());
    for (const auto &range : ranges)
        wear += (wear.empty() ? "" : " / ") + range;
    result.set("life", Json::text(wear.empty() ? "unknown" : wear));
    result.set("wear", Json::text(wear.empty() ? "unknown" : wear));
    result.set("health", Json::text(wear.empty()   ? "unknown"
                                    : health >= 90 ? ">90% (Healthy)"
                                                   : std::to_string(health) + "% remaining"));
    uint64_t eol;
    bool known = fileNumber(prefix + "pre_eol_info", eol, 16);
    result.set("eol", Json::text(!known     ? "unknown"
                                 : eol == 1 ? "Normal"
                                 : eol == 2 ? "Warning"
                                 : eol == 3 ? "Urgent"
                                            : "unknown"));
    return result;
}
Json appStorage(const std::string &path) {
    struct statvfs info;
    if (statvfs(path.c_str(), &info) != 0 || !info.f_blocks || !info.f_frsize ||
        info.f_bfree > info.f_blocks || info.f_bavail > info.f_blocks)
        return Json();
    double total = std::floor(static_cast<double>(info.f_blocks) * info.f_frsize / 1024);
    double used =
        std::floor(static_cast<double>(info.f_blocks - info.f_bfree) * info.f_frsize / 1024);
    double available = std::floor(static_cast<double>(info.f_bavail) * info.f_frsize / 1024);
    if (total <= 0 || total > 9007199254740991.0)
        return Json();
    Json result = Json::object();
    result.set("totalKb", Json::numeric(total));
    result.set("availKb", Json::numeric(available));
    result.set("totalMb", Json::numeric(std::round(total / 1024)));
    result.set("usedMb", Json::numeric(std::round(used / 1024)));
    result.set("freeMb", Json::numeric(std::round(available / 1024)));
    result.set("pct", Json::numeric(std::round(used / total * 100)));
    return result;
}
/*
 * Each online core's time by mode and the boot time, from /proc/stat. guest
 * and guest_nice are left out: the kernel counts them in user and nice
 * already.
 */
Json cpuTimes(const std::string &path) {
    static const char *MODES[] = {"user", "nice",   "system",  "idle",
                                  "iowait", "irq",  "softirq", "steal"};
    Json cpus = Json::object(), boot;
    std::istringstream lines(fileText(path));
    for (std::string line; std::getline(lines, line);) {
        if (line.compare(0, 3, "cpu") == 0 && line.size() > 3 &&
            std::isdigit(static_cast<unsigned char>(line[3]))) {
            size_t at = 3;
            while (at < line.size() && std::isdigit(static_cast<unsigned char>(line[at])))
                ++at;
            std::istringstream fields(line.substr(at));
            Json modes = Json::object();
            bool any = false;
            for (unsigned mode = 0; mode < 8; ++mode) {
                std::string token;
                uint64_t value;
                if (!(fields >> token))
                    break;
                if (parseUnsigned(token, value) && value <= MAX_SAFE_INTEGER) {
                    modes.set(MODES[mode], Json::numeric(static_cast<double>(value)));
                    any = true;
                }
            }
            if (any)
                cpus.set(line.substr(3, at - 3), modes);
        } else if (line.compare(0, 5, "btime") == 0 &&
                   (line.size() == 5 || std::isspace(static_cast<unsigned char>(line[5])))) {
            uint64_t value;
            if (parseUnsigned(trim(line.substr(5)), value) && value <= MAX_SAFE_INTEGER)
                boot = Json::numeric(static_cast<double>(value));
        }
    }
    Json result = Json::object();
    result.set("cpus", cpus);
    result.set("btime", boot);
    return result;
}
/*
 * CPU times, present/online core ids and LG's instantaneous load slots.
 * Offline cores drop out of /proc/stat, so the load slots are mapped by the
 * online ids a core is named with. coreLoads is only the fallback a consumer
 * shows until it has two tick samples for its own window.
 */
Json cpuReadings(const std::string &prefix) {
    std::string status = fileText(prefix + "/proc/lg/pm/status");
    std::vector<uint64_t> slots;
    unsigned cpuNum = 0;
    bool haveCpuNum = false;
    std::istringstream lines(status);
    for (std::string line; std::getline(lines, line);) {
        size_t colon = line.find(':');
        if (colon == std::string::npos)
            continue;
        std::string name = trim(line.substr(0, colon));
        std::istringstream fields(line.substr(colon + 1));
        if (name == "load") {
            for (std::string token; fields >> token;) {
                uint64_t value;
                if (!parseUnsigned(token, value))
                    break;
                slots.push_back(value);
            }
        } else if (name == "cpu_num") {
            std::string token;
            uint64_t value;
            if ((fields >> token) && parseUnsigned(token, value) && value <= 4096) {
                cpuNum = static_cast<unsigned>(value);
                haveCpuNum = true;
            }
        }
    }
    std::vector<unsigned> present =
        cpuRange(trim(fileText(prefix + "/sys/devices/system/cpu/present")));
    std::vector<unsigned> online =
        cpuRange(trim(fileText(prefix + "/sys/devices/system/cpu/online")));
    if (online.empty() && haveCpuNum)
        for (unsigned id = 0; id < cpuNum; ++id)
            online.push_back(id);
    Json loads = Json::array();
    if (!online.empty()) {
        for (unsigned id : online)
            if (id < slots.size())
                loads.add(Json::numeric(static_cast<double>(slots[id])));
    } else {
        for (uint64_t value : slots)
            loads.add(Json::numeric(static_cast<double>(value)));
    }
    Json result = Json::object();
    result.set("times", cpuTimes(prefix + "/proc/stat"));
    result.set("coresTotal",
               Json::numeric(
                   static_cast<double>(!present.empty() ? present.size() : slots.size())));
    result.set("coreLoads", loads);
    uint64_t current;
    result.set("currentLoad", fileNumber(prefix + "/proc/lg/pm/current_load", current)
                                  ? Json::numeric(static_cast<double>(current))
                                  : Json());
    return result;
}
// up/down outrank an unknown state, which outranks down, as before.
int ifaceRank(const std::string &prefix, const std::string &name) {
    std::string state = trim(fileText(prefix + "/sys/class/net/" + name + "/operstate"));
    if (!state.empty()) {
        if (state == "up")
            return 2;
        if (state == "down")
            return 0;
        return 1;
    }
    std::string carrier = trim(fileText(prefix + "/sys/class/net/" + name + "/carrier"));
    if (carrier.empty())
        return 1;
    return carrier == "1" ? 2 : 0;
}
bool validMac(const std::string &mac) {
    if (mac.size() != 17)
        return false;
    bool nonzero = false;
    for (size_t i = 0; i < mac.size(); ++i) {
        if (i % 3 == 2) {
            if (mac[i] != ':')
                return false;
        } else if (!std::isxdigit(static_cast<unsigned char>(mac[i]))) {
            return false;
        } else if (mac[i] != '0') {
            nonzero = true;
        }
    }
    return nonzero;
}
/*
 * The highest-ranked non-loopback interface's byte counters, and its MAC.
 * Operstate/carrier prefer an interface that is up; equal ranks go to the one
 * with more received bytes.
 */
Json network(const std::string &prefix) {
    std::istringstream lines(fileText(prefix + "/proc/net/dev"));
    bool found = false;
    std::string best;
    int bestRank = -1;
    uint64_t bestRx = 0, bestTx = 0;
    for (std::string line; std::getline(lines, line);) {
        size_t colon = line.find(':');
        if (colon == std::string::npos)
            continue;
        std::string name = trim(line.substr(0, colon));
        if (name.empty() || name == "lo")
            continue;
        std::istringstream fields(line.substr(colon + 1));
        std::vector<std::string> values;
        for (std::string token; fields >> token;)
            values.push_back(token);
        if (values.size() < 9)
            continue;
        uint64_t rx, tx;
        if (!parseUnsigned(values[0], rx) || !parseUnsigned(values[8], tx) ||
            rx > MAX_SAFE_INTEGER || tx > MAX_SAFE_INTEGER)
            continue;
        int rank = ifaceRank(prefix, name);
        if (!found || rank > bestRank || (rank == bestRank && rx > bestRx)) {
            found = true;
            best = name;
            bestRank = rank;
            bestRx = rx;
            bestTx = tx;
        }
    }
    if (!found)
        return Json();
    Json result = Json::object();
    result.set("iface", Json::text(best));
    result.set("rx", Json::numeric(static_cast<double>(bestRx)));
    result.set("tx", Json::numeric(static_cast<double>(bestTx)));
    std::string mac = trim(fileText(prefix + "/sys/class/net/" + best + "/address"));
    std::transform(mac.begin(), mac.end(), mac.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    result.set("mac", validMac(mac) ? Json::text(mac) : Json());
    return result;
}
Json wifi(const std::string &prefix) {
    std::istringstream lines(fileText(prefix + "/proc/net/wireless"));
    for (std::string line; std::getline(lines, line);) {
        if (line.find("wlan0") == std::string::npos)
            continue;
        std::istringstream fields(line);
        std::string face, state;
        double link, level;
        if (!(fields >> face >> state >> link >> level))
            return Json();
        // A driver with no reading reports both as zero.
        if (link == 0 && level == 0)
            return Json();
        if (level > 127)
            level -= 256;
        Json result = Json::object();
        result.set("link", Json::numeric(link));
        result.set("level", Json::numeric(level));
        return result;
    }
    return Json();
}
double uptimeSeconds(const std::string &path) {
    std::istringstream stream(fileText(path));
    double value;
    if (!(stream >> value) || !(value >= 0) || value > static_cast<double>(MAX_SAFE_INTEGER))
        return 0;
    return std::floor(value);
}
Json loadAverage(const std::string &path) {
    std::istringstream stream(fileText(path));
    Json result = Json::array();
    for (unsigned index = 0; index < 3; ++index) {
        std::string token;
        double value;
        if (!(stream >> token))
            break;
        std::istringstream number(token);
        if (!(number >> value) || !number.eof() || !std::isfinite(value))
            break;
        result.add(Json::text(token));
    }
    return result;
}
/*
 * The main picture window's colourimetry and HDR mode from the picture
 * engine's hdr_status. Window [0] is the main picture; an unindexed file is
 * read whole. Unknown colourimetry is kept as the TV spelled it.
 */
Json pictureEngine(const std::string &path) {
    std::string raw = fileText(path);
    if (raw.empty())
        return Json();
    std::string target = raw;
    size_t window = raw.find("[0]{");
    if (window != std::string::npos) {
        size_t close = raw.find('}', window + 4);
        if (close != std::string::npos)
            target = raw.substr(window + 4, close - window - 4);
    }
    Json colorimetry, hdr;
    size_t col = findCaseInsensitive(target, "colorimetry:");
    if (col != std::string::npos) {
        size_t at = col + 12;
        while (at < target.size() && std::isspace(static_cast<unsigned char>(target[at])))
            ++at;
        size_t end = at;
        while (end < target.size() && target[end] != ',' && target[end] != '}' &&
               target[end] != '\n' && target[end] != '\r')
            ++end;
        std::string value = trim(target.substr(at, end - at));
        std::string lower = value;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (lower == "bt709")
            colorimetry = Json::text("BT.709");
        else if (lower == "bt2020")
            colorimetry = Json::text("BT.2020");
        else if (lower == "bt601")
            colorimetry = Json::text("BT.601");
        else if (!value.empty())
            colorimetry = Json::text(value);
    }
    size_t hdrAt = findCaseInsensitive(target, "hdrstatus:");
    if (hdrAt != std::string::npos) {
        size_t at = hdrAt + 10;
        while (at < target.size() && std::isspace(static_cast<unsigned char>(target[at])))
            ++at;
        size_t end = at;
        while (end < target.size() && target[end] != '(' && target[end] != ',' &&
               target[end] != '}' &&
               !std::isspace(static_cast<unsigned char>(target[end])))
            ++end;
        std::string mode = target.substr(at, end - at);
        size_t open = end;
        while (open < target.size() && std::isspace(static_cast<unsigned char>(target[open])))
            ++open;
        if (!mode.empty() && open < target.size() && target[open] == '(') {
            size_t close = target.find(')', open + 1);
            if (close != std::string::npos) {
                std::string inner = trim(target.substr(open + 1, close - open - 1));
                if (!inner.empty())
                    mode = inner;
            }
        }
        if (!mode.empty()) {
            std::transform(mode.begin(), mode.end(), mode.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            hdr = Json::text(mode);
        }
    }
    Json result = Json::object();
    result.set("colorimetry", colorimetry);
    result.set("hdr_mode", hdr);
    return result;
}
/*
 * The Magic Remote's pairing file facts. LG writes the remote model's Mid and
 * Low battery thresholds as "Battery = 52(M) 35(L)", which is not a level; a
 * plain "Battery = N" is. The mrcu path is preferred; btsvc is the fallback.
 */
Json remoteInfo(const std::string &prefix, const std::string &dataRoot) {
    std::string raw = fileText(prefix + "/mnt/lg/cmn_data/mrcu/mrcu1.info");
    if (raw.empty())
        raw = fileText(prefix + "/mnt/lg/cmn_data/btsvc/mrcu1.info");
    if (raw.empty())
        return Json();
    bool haveBattery = false, haveName = false;
    Json battery, model, mac, firmware;
    std::istringstream lines(raw);
    for (std::string line; std::getline(lines, line);) {
        size_t equals = line.find('=');
        if (equals == std::string::npos)
            continue;
        std::string key = trim(line.substr(0, equals));
        std::transform(key.begin(), key.end(), key.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        std::string value = trim(line.substr(equals + 1));
        if (key == "battery") {
            size_t digits = 0;
            while (digits < value.size() &&
                   std::isdigit(static_cast<unsigned char>(value[digits])))
                ++digits;
            uint64_t level;
            if (digits && parseUnsigned(value.substr(0, digits), level)) {
                size_t after = digits;
                while (after < value.size() &&
                       std::isspace(static_cast<unsigned char>(value[after])))
                    ++after;
                if (after >= value.size() || value[after] != '(')
                    battery = Json::numeric(static_cast<double>(level));
                haveBattery = true;
            }
        } else if (key == "name" && !value.empty()) {
            model = Json::text(value);
            haveName = true;
        } else if (key == "bdaddr" && !value.empty()) {
            mac = Json::text(value);
        } else if (key == "fwver" && !value.empty()) {
            firmware = Json::text(value);
        }
    }
    if (!haveBattery && !haveName)
        return Json();
    Json result = Json::object();
    // The pairing file contains model thresholds on newer webOS releases.
    // Prefer the last level reported by the remote when its saved address is
    // compatible with the paired device, matching Node's cache semantics.
    const Json saved = Json::parse(fileText(dataRoot + "/remote-battery.json"));
    const Json savedLevel = saved["level"];
    const std::string pairedMac = mac.string();
    const std::string savedMac = saved["mac"].string();
    const bool addressMatches = pairedMac.empty() || savedMac.empty() ||
        lowercase(pairedMac) == lowercase(savedMac);
    if (savedLevel.isNumber() && savedLevel.number() >= 0 && savedLevel.number() <= 100 &&
        std::floor(savedLevel.number()) == savedLevel.number() && addressMatches)
        battery = Json::numeric(savedLevel.number());
    if (!model.isString())
        model = saved["model"].isString() ? saved["model"] : Json();
    if (!firmware.isString())
        firmware = saved["firmware"].isString() ? saved["firmware"] : Json();
    result.set("battery", battery);
    result.set("model", model);
    result.set("mac", mac);
    result.set("firmware", firmware);
    result.set("paired", Json::flag(true));
    if (saved["connected"].isBoolean())
        result.set("connected", saved["connected"]);
    if (saved["isBle"].isBoolean())
        result.set("bluetooth", Json::flag(!pairedMac.empty() || saved["isBle"].boolean()));
    if (saved["lowStrength"].isBoolean())
        result.set("signal", Json::text(saved["lowStrength"].boolean() ? "weak" : "good"));
    return result;
}
} // namespace
Json hdmiPortFactFromStatus(const std::string &raw) { return hdmiPortFact(raw); }

Json collectSystem(const std::string &root, const std::string &dataRoot) {
    const std::string prefix = root == "/" ? "" : root;
    Json result = Json::object();
    result.set("ok", Json::flag(true));
    auto values = memory(prefix + "/proc/meminfo");
    Json mem = Json::object(), swap = Json::object();
    mem.set("total", Json::numeric(values["MemTotal"]));
    mem.set("avail", Json::numeric(values["MemAvailable"]));
    swap.set("total", Json::numeric(values["SwapTotal"]));
    swap.set("free", Json::numeric(values["SwapFree"]));
    swap.set("backing", swapBacking(prefix + "/proc/swaps"));
    result.set("mem", mem);
    result.set("swap", swap);
    result.set("cpu", cpuReadings(prefix));
    result.set("uptime", Json::numeric(uptimeSeconds(prefix + "/proc/uptime")));
    result.set("loadavg", loadAverage(prefix + "/proc/loadavg"));
    result.set("net", network(prefix));
    result.set("wifi", wifi(prefix));
    result.set("pictureEngine", pictureEngine(prefix + "/proc/lg/pe/hdr_status"));
    result.set("hasHdrStatus", Json::flag(exists(prefix + "/proc/lg/pe/hdr_status")));
    result.set("hasHdmiProc", Json::flag(exists(prefix + "/proc/lg/hdmi20")));
    result.set("socArch", socArchitecture(prefix));
    result.set("hasPnwash", Json::flag(exists(
        prefix + "/mnt/lg/cmn_data/pnwash/completedOffRsCount")));
    std::vector<HdmiStatus> hdmiStatuses = readHdmiStatusFiles(prefix);
    result.set("hdmiPorts", hdmiRawSnapshot(hdmiStatuses));
    result.set("hdmiPortFacts", hdmiFactsSnapshot(hdmiStatuses));
    result.set("remote", remoteInfo(prefix, dataRoot));
    bool lgThermal = exists(prefix + LG_THERMAL);
    bool thermal = lgThermal || exists(prefix + SYS_THERMAL);
    uint64_t temp;
    bool tempValid = thermal && fileNumber(prefix + (lgThermal ? LG_THERMAL : SYS_THERMAL), temp) &&
                     temp > 0 && temp <= (lgThermal ? 1000 : 1000000);
    double millidegrees = tempValid ? static_cast<double>(temp) * (lgThermal ? 1000 : 1) : 0;
    result.set("tempMillidegrees", tempValid ? Json::numeric(millidegrees) : Json());
    result.set("temp", tempValid ? Json::numeric(std::round(millidegrees / 1000)) : Json());
    result.set("thermalPresent", Json::flag(thermal));
    result.set("emmcWearPresent", Json::flag(exists(prefix + EMMC + "life_time")));
    result.set("emmc", emmcHealth(prefix + EMMC));
    uint64_t frequency;
    bool cpuValid = fileNumber(prefix + "/proc/lg/pm/frequency", frequency);
    double hz =
        cpuValid ? static_cast<double>(frequency) * (frequency > 10000 ? 1000 : 1000000) : 0;
    cpuValid = cpuValid && hz >= 100e6 && hz <= 10000e6;
    result.set("cpuHz", cpuValid ? Json::numeric(hz) : Json());
    result.set("mhz", cpuValid ? Json::numeric(std::round(hz / 1000000)) : Json());
    std::istringstream gpuLines(fileText(prefix + "/proc/lg/sys/status"));
    Json gpu;
    for (std::string line; std::getline(gpuLines, line);) {
        std::transform(line.begin(), line.end(), line.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        size_t at = line.find("gpu pll out");
        size_t colon = at == std::string::npos ? at : line.find(':', at + 11);
        if (colon == std::string::npos)
            continue;
        std::istringstream fields(line.substr(colon + 1));
        std::string token;
        uint64_t value;
        if ((fields >> token) && parseUnsigned(token, value) && value <= 9007199254740991ULL)
            gpu = Json::numeric(static_cast<double>(value));
        break;
    }
    result.set("gpuHz", gpu);
    result.set("gpuMhz", gpu.valid() ? Json::numeric(std::round(gpu.number() / 1000000)) : Json());
    result.set("appStorage", appStorage(prefix + "/mnt/lg/appstore"));
    return result;
}
} // namespace service
} // namespace nativetv
