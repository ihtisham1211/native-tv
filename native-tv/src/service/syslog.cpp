#include "service/syslog.hpp"
#include "service/logs.hpp"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <regex>
#include <sstream>
#include <cstdlib>
#include <cerrno>
#include <unordered_map>

namespace nativetv {
namespace service {
namespace {
const double clockFloorMs = 1767225600000.0;

int severity(const std::string &level) {
    if (level == "error")
        return 3;
    if (level == "warning")
        return 4;
    if (level == "debug")
        return 7;
    return 6;
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

int priority(const Json &entry) {
    const std::string source = entry["source"].string();
    const int level = severity(entry["level"].string());
    if (source == "kernel") {
        const Json raw = entry["pri"];
        if (raw.isNumber() && raw.number() >= 0 && raw.number() <= 191)
            return static_cast<int>(raw.number());
        return level;
    }
    if (source == "nativetv")
        return 3 * 8 + level;

    static const std::regex systemPriority(
        R"(^\S+\s+\[[0-9.]+\]\s+([a-z0-9]+)\.([a-z]+)\s)",
        std::regex_constants::icase);
    std::smatch match;
    const std::string raw = entry["raw"].string();
    if (std::regex_search(raw, match, systemPriority)) {
        static const std::unordered_map<std::string, int> facilities = {
            {"kern", 0}, {"user", 1}, {"mail", 2}, {"daemon", 3},
            {"auth", 4}, {"syslog", 5}, {"lpr", 6}, {"news", 7},
            {"uucp", 8}, {"cron", 9}, {"authpriv", 10}, {"ftp", 11},
            {"local0", 16}, {"local1", 17}, {"local2", 18}, {"local3", 19},
            {"local4", 20}, {"local5", 21}, {"local6", 22}, {"local7", 23}};
        static const std::unordered_map<std::string, int> severities = {
            {"emerg", 0}, {"panic", 0}, {"alert", 1}, {"crit", 2},
            {"err", 3}, {"error", 3}, {"warn", 4}, {"warning", 4},
            {"notice", 5}, {"info", 6}, {"debug", 7}};
        const auto facility = facilities.find(lower(match[1].str()));
        const auto sev = severities.find(lower(match[2].str()));
        if (facility != facilities.end() && sev != severities.end())
            return facility->second * 8 + sev->second;
    }
    return 8 + level;
}

std::string headerField(const std::string &value, size_t maxLength) {
    std::string result;
    result.reserve(std::min(value.size(), maxLength));
    bool replacing = false;
    for (unsigned char character : value) {
        const bool valid = character >= 0x21 && character <= 0x7e;
        if (valid) {
            if (result.size() == maxLength)
                break;
            result.push_back(static_cast<char>(character));
            replacing = false;
        } else if (!replacing) {
            if (result.size() == maxLength)
                break;
            result.push_back('-');
            replacing = true;
        }
    }
    return result.empty() ? "-" : result;
}

bool parseTimestamp(const std::string &value, double &milliseconds) {
    static const std::regex iso(
        R"(^([0-9]{4})-([0-9]{2})-([0-9]{2})T([0-9]{2}):([0-9]{2}):([0-9]{2})(?:\.([0-9]+))?Z$)");
    std::smatch match;
    if (!std::regex_match(value, match, iso))
        return false;
    std::tm utc{};
    utc.tm_year = std::stoi(match[1].str()) - 1900;
    utc.tm_mon = std::stoi(match[2].str()) - 1;
    utc.tm_mday = std::stoi(match[3].str());
    utc.tm_hour = std::stoi(match[4].str());
    utc.tm_min = std::stoi(match[5].str());
    utc.tm_sec = std::stoi(match[6].str());
    const std::time_t seconds = timegm(&utc);
    std::string fraction = match[7].str();
    if (fraction.size() > 3)
        fraction.resize(3);
    while (fraction.size() < 3)
        fraction.push_back('0');
    const double fractionalMs = fraction.empty() ? 0 : std::stod(fraction);
    milliseconds = static_cast<double>(seconds) * 1000 + fractionalMs;
    return std::isfinite(milliseconds);
}

std::string isoTimestampImpl(double milliseconds) {
    if (!std::isfinite(milliseconds))
        return "1970-01-01T00:00:00.000Z";
    const long long rounded = static_cast<long long>(std::llround(milliseconds));
    long long seconds = rounded / 1000;
    long millis = static_cast<long>(rounded % 1000);
    if (millis < 0) {
        --seconds;
        millis += 1000;
    }
    const std::time_t time = static_cast<std::time_t>(seconds);
    std::tm utc{};
    if (!gmtime_r(&time, &utc))
        return "1970-01-01T00:00:00.000Z";
    char buffer[24];
    if (std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S", &utc) == 0)
        return "1970-01-01T00:00:00.000Z";
    std::ostringstream output;
    output << buffer << '.' << std::setw(3) << std::setfill('0') << millis << 'Z';
    return output.str();
}

double wallNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string trim(const std::string &value) {
    size_t first = 0;
    size_t last = value.size();
    while (first < last && std::isspace(static_cast<unsigned char>(value[first])))
        ++first;
    while (last > first && std::isspace(static_cast<unsigned char>(value[last - 1])))
        --last;
    return value.substr(first, last - first);
}

std::string legacyString(const Json &value) {
    if (!value.valid())
        return "undefined";
    if (value.isString())
        return value.string();
    if (value.isNumber())
        return value.dump();
    if (value.isBoolean())
        return value.boolean() ? "true" : "false";
    if (value.isObject())
        return "[object Object]";
    if (value.isArray()) {
        std::string result;
        const std::vector<Json> items = value.items();
        for (size_t index = 0; index < items.size(); ++index) {
            if (index)
                result.push_back(',');
            if (items[index].valid() && !items[index].isArray() && !items[index].isObject())
                result += legacyString(items[index]);
        }
        return result;
    }
    return "null";
}
} // namespace

bool readSyslogSettings(const Json &config, SyslogSettings &settings) {
    const Json source = config["syslog"];
    const Json server = source["server"];
    settings = SyslogSettings();
    settings.server = server.isString() ? trim(server.string()) : "";
    if (settings.server.empty())
        return false;

    const std::string portText = legacyString(source["port"]);
    const char *portStart = portText.c_str();
    while (*portStart && std::isspace(static_cast<unsigned char>(*portStart)))
        ++portStart;
    errno = 0;
    char *portEnd = nullptr;
    const long parsedPort = std::strtol(portStart, &portEnd, 10);
    if (portEnd != portStart && errno != ERANGE && parsedPort >= 1 && parsedPort <= 65535)
        settings.port = static_cast<unsigned short>(parsedPort);

    const Json hostname = source["hostname"];
    settings.hostname = hostname.isString() ? trim(hostname.string()) : "";
    const Json configuredSources = source["sources"];
    const std::vector<std::string> allowed = {"system", "nativetv", "kernel"};
    if (configuredSources.isArray()) {
        const std::vector<Json> values = configuredSources.items();
        for (const std::string &name : allowed) {
            for (const Json &value : values) {
                if (value.isString() && value.string() == name) {
                    settings.sources.push_back(name);
                    break;
                }
            }
        }
    } else {
        settings.sources.push_back("system");
        settings.sources.push_back("nativetv");
    }
    const Json redact = source["redact"];
    settings.redact = !(redact.isBoolean() && !redact.boolean());
    return true;
}

std::string syslogIsoTimestamp(double milliseconds) {
    return isoTimestampImpl(milliseconds);
}

std::string formatSyslogMessage(const Json &entry, const SyslogFormatOptions &options) {
    const Json shown = options.redact ? redactLogEntry(entry) : entry;
    double timestamp = 0;
    const bool timestampValid = parseTimestamp(shown["ts"].string(), timestamp);
    const Json monotonic = shown["mono"];
    if ((!timestampValid || timestamp < clockFloorMs) && monotonic.isNumber() &&
        std::isfinite(monotonic.number())) {
        timestamp = options.bootTimeMs + std::floor(monotonic.number() * 1000 + 0.5);
    } else if (!timestampValid) {
        timestamp = options.nowMs > 0 ? options.nowMs : wallNowMs();
    }
    const std::string source = shown["source"].string();
    const std::string app = source == "kernel" ? "kernel" : shown["proc"].string();
    const std::string message = shown["msg"].string();
    std::string result = "<" + std::to_string(priority(shown)) + ">1 " +
                         syslogIsoTimestamp(timestamp) + " " +
                         headerField(options.hostname, 255) + " " +
                         headerField(app, 48) + " - " +
                         headerField(source, 32) + " -";
    if (!message.empty())
        result += " " + message;

    const size_t maximum = syslogMaximumDatagramSize();
    if (result.size() <= maximum)
        return result;
    size_t cut = maximum;
    while (cut > 0 && (static_cast<unsigned char>(result[cut]) & 0xc0) == 0x80)
        --cut;
    result.resize(cut);
    return result;
}

} // namespace service
} // namespace nativetv
