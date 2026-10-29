#include "service/logs.hpp"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <ctime>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <regex>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace nativetv {
namespace service {
namespace {
std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::string trim(const std::string &value) {
    size_t first = 0, last = value.size();
    while (first < last && std::isspace(static_cast<unsigned char>(value[first])))
        ++first;
    while (last > first && std::isspace(static_cast<unsigned char>(value[last - 1])))
        --last;
    return value.substr(first, last - first);
}

double decimal(const std::string &value, double fallback = 0) {
    try {
        size_t used = 0;
        const double parsed = std::stod(value, &used);
        return used == value.size() && std::isfinite(parsed) ? parsed : fallback;
    } catch (...) {
        return fallback;
    }
}

bool parseIsoMs(const std::string &value, double &milliseconds) {
    static const std::regex expression(
        "^(\\d{4})-(\\d{2})-(\\d{2})T(\\d{2}):(\\d{2}):(\\d{2})(?:\\.(\\d+))?Z$");
    std::smatch match;
    if (!std::regex_match(value, match, expression))
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
    milliseconds = static_cast<double>(seconds) * 1000 +
                   (fraction.empty() ? 0 : std::stod(fraction));
    return true;
}

std::string isoTimestamp(double milliseconds) {
    const long long rounded = static_cast<long long>(std::llround(milliseconds));
    const std::time_t seconds = static_cast<std::time_t>(rounded / 1000);
    long millis = static_cast<long>(rounded % 1000);
    if (millis < 0)
        millis += 1000;
    std::tm utc{};
    if (!gmtime_r(&seconds, &utc))
        return "1970-01-01T00:00:00.000Z";
    char buffer[24];
    if (std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S", &utc) == 0)
        return "1970-01-01T00:00:00.000Z";
    std::ostringstream output;
    output << buffer << '.' << std::setw(3) << std::setfill('0') << millis << 'Z';
    return output.str();
}

Json entry(const std::string &timestamp, double monotonic, const std::string &source,
           const std::string &level, const std::string &process, const std::string &message,
           const std::string &raw) {
    Json result = Json::object();
    result.set("ts", Json::text(timestamp));
    result.set("mono", Json::numeric(monotonic));
    result.set("source", Json::text(source));
    result.set("level", Json::text(level));
    result.set("proc", Json::text(process));
    result.set("msg", Json::text(message));
    result.set("raw", Json::text(raw));
    return result;
}

std::vector<std::string> lines(const std::string &raw) {
    std::vector<std::string> result;
    std::istringstream input(raw);
    for (std::string line; std::getline(input, line);)
        result.push_back(line);
    return result;
}

std::string normalizeHint(const std::string &hint) {
    const std::string value = lower(hint);
    if (value == "err" || value == "error" || value == "crit" || value == "alert" ||
        value == "emerg")
        return "error";
    if (value == "warn" || value == "warning")
        return "warning";
    if (value == "debug")
        return "debug";
    if (value == "info" || value == "notice")
        return "info";
    return "";
}

std::string replaceRegex(const std::string &input, const std::regex &pattern,
                         const std::string &replacement) {
    try {
        return std::regex_replace(input, pattern, replacement);
    } catch (...) {
        return input;
    }
}

std::string rootPath(const std::string &root, const std::string &path) {
    if (root.empty() || root == "/")
        return path;
    if (root.back() == '/')
        return root.substr(0, root.size() - 1) + path;
    return root + path;
}

bool fileInfo(const std::string &path, uint64_t &size, std::string &inode) {
    struct stat info;
    if (stat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode))
        return false;
    size = static_cast<uint64_t>(info.st_size);
    inode = std::to_string(static_cast<unsigned long long>(info.st_ino));
    return true;
}

struct LogRead {
    std::string text;
    uint64_t end = 0;
    std::string inode;
    bool whole = false;
};

bool readLogLines(const std::string &path, bool hasCursor, uint64_t cursor,
                  const std::string &cursorInode, size_t maximumBytes, LogRead &result) {
    uint64_t size = 0;
    std::string inode;
    if (!fileInfo(path, size, inode))
        return false;
    const bool sameFile = hasCursor && (cursorInode.empty() || cursorInode == inode);
    result.whole = !sameFile || cursor > size || size - std::min(size, cursor) > maximumBytes;
    uint64_t start = result.whole ? (size > maximumBytes ? size - maximumBytes : 0) : cursor;
    const size_t length = static_cast<size_t>(std::min<uint64_t>(size - start, maximumBytes));
    result.inode = inode;
    if (length == 0) {
        result.end = start;
        return true;
    }

    const int descriptor = open(path.c_str(), O_RDONLY);
    if (descriptor < 0)
        return false;
    std::string bytes(length, '\0');
    size_t received = 0;
    while (received < length) {
        const ssize_t count = pread(descriptor, &bytes[received], length - received,
                                    static_cast<off_t>(start + received));
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            break;
        received += static_cast<size_t>(count);
    }
    close(descriptor);
    bytes.resize(received);
    const size_t newline = bytes.find_last_of('\n');
    if (newline == std::string::npos) {
        result.end = start;
        return true;
    }
    const size_t completeLength = newline + 1;
    size_t textStart = 0;
    if (result.whole && start > 0) {
        const size_t firstNewline = bytes.find('\n');
        textStart = firstNewline == std::string::npos ? completeLength : firstNewline + 1;
    }
    result.text = bytes.substr(textStart, completeLength - textStart);
    result.end = start + completeLength;
    return true;
}

double uptimeAtRoot(const std::string &root) {
    std::ifstream input(rootPath(root, "/proc/uptime"));
    std::string uptime;
    return input >> uptime ? decimal(uptime, 0) : 0;
}

double currentTimeMs() {
    return static_cast<double>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::system_clock::now().time_since_epoch())
                                   .count());
}

std::string pathFromEnvironment(const std::string &root) {
    const char *configured = std::getenv("TVWEB_LOG");
    return configured && *configured ? configured : rootPath(root, "/var/lib/tvweb/tvweb.log");
}

Json sourceMetadata(bool available, const std::string &path) {
    Json result = Json::object();
    result.set("available", Json::flag(available));
    result.set("path", Json::text(path));
    return result;
}

uint64_t parsedLimit(const std::string &value) {
    char *end = nullptr;
    const long parsed = std::strtol(value.c_str(), &end, 10);
    if (value.empty() || end == value.c_str() || parsed <= 0)
        return 100;
    return std::min<uint64_t>(static_cast<uint64_t>(parsed), 1000);
}

bool containsSource(const std::vector<std::string> &sources, const std::string &source) {
    return std::find(sources.begin(), sources.end(), source) != sources.end();
}

void appendEntries(Json &destination, const Json &source) {
    for (const Json &item : source.items())
        destination.add(item);
}

void appendReadEntries(Json &allEntries, const LogRead &read, const std::string &source,
                       double bootTimeMs, double nowMs, double uptimeSeconds, bool &incremental,
                       std::vector<std::string> &cursor, uint64_t &count) {
    Json parsed = source == "system"
                      ? parseSystemLogs(read.text, bootTimeMs, nowMs)
                      : parseNativeTvLogs(read.text, bootTimeMs, uptimeSeconds);
    count = parsed.size();
    appendEntries(allEntries, parsed);
    if (read.whole)
        incremental = false;
    cursor.push_back(source + ":" + std::to_string(read.end) + ":" + read.inode);
}
} // namespace

std::string detectLogLevel(const std::string &text, const std::string &hint) {
    const std::string explicitLevel = normalizeHint(hint);
    if (!explicitLevel.empty())
        return explicitLevel;
    const std::string value = lower(text);
    static const std::regex errors("\\b(error|fail|failed|failure|fatal|panic|corrupt|segfault)\\b");
    static const std::regex warnings("\\b(warn|warning)\\b");
    static const std::regex debug("\\bdebug\\b");
    if (std::regex_search(value, errors))
        return "error";
    if (std::regex_search(value, warnings))
        return "warning";
    if (std::regex_search(value, debug))
        return "debug";
    return "info";
}

Json parseSystemLogs(const std::string &raw, double bootTimeMs, double nowMs) {
    Json result = Json::array();
    static const std::regex structured(
        "^(\\d{4}-\\d{2}-\\d{2}T\\d{2}:\\d{2}:\\d{2}(?:\\.\\d+)?Z)\\s+"
        "\\[([0-9.]+)\\]\\s+(\\S+)\\s+(\\S+)\\s+(?:\\[[^\\]]*\\])?\\s*(.*)$");
    static const std::regex timestamped(
        "^(\\d{4}-\\d{2}-\\d{2}T\\d{2}:\\d{2}:\\d{2}(?:\\.\\d+)?Z)\\s+(.*)$");
    for (const std::string &original : lines(raw)) {
        const std::string line = trim(original);
        if (line.empty())
            continue;
        std::smatch match;
        if (std::regex_match(line, match, structured)) {
            const std::string facilityLevel = match[3].str();
            const size_t dot = facilityLevel.find('.');
            const std::string hint = dot == std::string::npos
                                         ? facilityLevel
                                         : facilityLevel.substr(dot + 1);
            result.add(entry(match[1].str(), decimal(match[2].str()), "system",
                             detectLogLevel(match[5].str(), hint), match[4].str(),
                             match[5].str(), line));
            continue;
        }
        std::string timestamp, message;
        double monotonic = 0;
        if (std::regex_match(line, match, timestamped)) {
            timestamp = match[1].str();
            message = match[2].str();
            double absolute = 0;
            monotonic = parseIsoMs(timestamp, absolute) ? (absolute - bootTimeMs) / 1000 : 0;
        } else {
            const double fallbackNow = nowMs > 0 ? nowMs : bootTimeMs;
            timestamp = isoTimestamp(fallbackNow);
            monotonic = std::max(0.0, (fallbackNow - bootTimeMs) / 1000);
            message = line;
        }
        result.add(entry(timestamp, monotonic, "system", detectLogLevel(message), "system",
                         message, line));
    }
    return result;
}

Json parseNativeTvLogs(const std::string &raw, double bootTimeMs, double uptimeSeconds) {
    Json result = Json::array();
    static const std::regex stamped(
        "^(\\d{4}-\\d{2}-\\d{2}T\\d{2}:\\d{2}:\\d{2}(?:\\.\\d+)?Z)"
        "(?:\\s+\\[([0-9.]+)\\])?"
        "(?:\\s+\\[(INFO|WARN|WARNING|ERR|ERROR|DBG|DEBUG)\\])?"
        "(?:\\s+(.*))?$", std::regex::icase);
    static const std::regex processTag("^([a-zA-Z0-9_-]+):\\s*(.*)$");
    double lastMono = -1;
    std::string lastTimestamp;
    for (const std::string &original : lines(raw)) {
        const std::string line = trim(original);
        if (line.empty())
            continue;
        std::smatch match;
        std::string timestamp, message;
        double monotonic = 0;
        std::string explicitLevel;
        if (std::regex_match(line, match, stamped)) {
            timestamp = match[1].str();
            if (match[2].matched) {
                monotonic = decimal(match[2].str());
            } else {
                double absolute = 0;
                monotonic = parseIsoMs(timestamp, absolute) ?
                                (absolute - bootTimeMs) / 1000 : uptimeSeconds;
            }
            if (match[3].matched)
                explicitLevel = normalizeHint(match[3].str());
            message = match[4].matched ? match[4].str() : "";
            lastMono = monotonic;
            lastTimestamp = timestamp;
        } else if (lastMono >= 0) {
            timestamp = lastTimestamp;
            monotonic = lastMono;
            message = line;
        } else {
            timestamp = isoTimestamp(bootTimeMs);
            message = line;
        }
        std::string process = "tvweb";
        std::smatch tagMatch;
        if (std::regex_match(message, tagMatch, processTag)) {
            const std::string candidate = tagMatch[1].str();
            const std::string normalized = lower(candidate);
            if (normalized != "warning" && normalized != "warn" && normalized != "error" &&
                normalized != "info" && normalized != "debug")
                process = candidate;
        }
        result.add(entry(timestamp, monotonic, "nativetv",
                         explicitLevel.empty() ? detectLogLevel(message) : explicitLevel,
                         process, message, line));
    }
    return result;
}

Json parseKernelLogs(const std::string &raw, double bootTimeMs) {
    Json result = Json::array();
    static const std::regex kernelLine("^\\s*(?:<(\\d+)>)?\\[\\s*([0-9.]+)\\s*\\]\\s*(.*)$");
    static const std::regex tagOne("^\\[([a-zA-Z0-9_.-]+)\\]:?\\s*(.*)$");
    static const std::regex tagTwo("^([a-zA-Z0-9_.-]+)(?:\\s+[\\w.:-]+)?:\\s*(.*)$");
    for (const std::string &original : lines(raw)) {
        const std::string line = trim(original);
        if (line.empty())
            continue;
        std::smatch match;
        if (!std::regex_match(line, match, kernelLine))
            continue;
        const double monotonic = decimal(match[2].str());
        const std::string message = match[3].str();
        std::string process = "kernel";
        std::smatch tagMatch;
        if (std::regex_match(message, tagMatch, tagOne) ||
            std::regex_match(message, tagMatch, tagTwo)) {
            const std::string candidate = tagMatch[1].str();
            const std::string normalized = lower(candidate);
            if (normalized != "warning" && normalized != "warn" && normalized != "error" &&
                normalized != "info" && normalized != "debug")
                process = candidate;
        }
        Json parsed = entry(isoTimestamp(bootTimeMs + monotonic * 1000), monotonic, "kernel",
                            detectLogLevel(message), process, message, line);
        if (match[1].matched)
            parsed.set("pri", Json::numeric(std::strtoul(match[1].str().c_str(), nullptr, 10)));
        result.add(parsed);
    }
    return result;
}

Json parseLogCursor(const std::string &cursor) {
    Json result = Json::object();
    std::istringstream parts(cursor);
    for (std::string part; std::getline(parts, part, ',');) {
        std::vector<std::string> fields;
        std::istringstream fieldStream(part);
        for (std::string field; std::getline(fieldStream, field, ':');)
            fields.push_back(field);
        if (fields.size() >= 2 && fields[0] == "kernel") {
            const double value = decimal(fields[1], -1);
            if (value >= 0) {
                result.set("kernel", Json::numeric(value));
            }
        } else if (fields.size() >= 2 &&
                   (fields[0] == "system" || fields[0] == "nativetv")) {
            char *end = nullptr;
            const unsigned long long offset = std::strtoull(fields[1].c_str(), &end, 10);
            if (!fields[1].empty() && end && *end == '\0') {
                Json source = Json::object();
                source.set("end", Json::numeric(static_cast<double>(offset)));
                source.set("ino", Json::text(fields.size() > 2 ? fields[2] : ""));
                result.set(fields[0], source);
            }
        }
    }
    return result;
}

std::string redactLogText(const std::string &text) {
    std::string value = text;
    value = replaceRegex(value, std::regex(R"((://[^:]+:)[^@\s]+(@))"), "$1<REDACTED>$2");
    value = replaceRegex(value, std::regex(R"(((\?|&)(?:k|token|key|api_key|auth)=)[^&\s"'`>]+)",
                                             std::regex::icase), "$1<REDACTED>");
    value = replaceRegex(value, std::regex(R"((Bearer\s+)[A-Za-z0-9_.-]+)", std::regex::icase),
                         "$1<REDACTED>");
    value = replaceRegex(value, std::regex(
        R"((["']?(?:password|passwd|secret|client_secret|access_token|refresh_token)["']?\s*[:=]\s*["']?)[^"',\s}]+(["']?))",
        std::regex::icase), "$1<REDACTED>$2");
    value = replaceRegex(value, std::regex(
        R"((["']?(?:serial(?:_?number)?|device_?id|esn)["']?\s*[:=]\s*["']?)[A-Za-z0-9_-]{6,}(["']?))",
        std::regex::icase), "$1<SERIAL>$2");
    value = replaceRegex(value, std::regex(
        R"((["']?(?:zip_?code|post_?code|postal_?code|latitude|longitude)["']?\s*[:=]\s*["']?)[^"',}\r\n]+(["']?))",
        std::regex::icase), "$1<LOCATION>$2");
    value = replaceRegex(value, std::regex(R"((ssid["':=\s]+["'])[^\r\n"']*(["']))",
                                             std::regex::icase), "$1<SSID>$2");
    value = replaceRegex(value, std::regex(R"(\b([0-9A-Fa-f]{2}[:-]){5}([0-9A-Fa-f]{2})\b)"),
                         "<MAC>");
    value = replaceRegex(value, std::regex(
        R"((?:\bfe80:[0-9a-fA-F:]+\b|\b(?:[0-9a-fA-F]{1,4}:){3,7}[0-9a-fA-F]{1,4}\b|\b[0-9a-fA-F]{1,4}::[0-9a-fA-F:]*\b))",
        std::regex::icase), "<IPV6>");
    value = replaceRegex(value, std::regex(
        R"(\b(?!(?:127\.0\.0\.1|0\.0\.0\.0)\b)(?:(?:25[0-5]|2[0-4][0-9]|[01]?[0-9][0-9]?)\.){3}(?:25[0-5]|2[0-4][0-9]|[01]?[0-9][0-9]?)\b)"),
        "<IP>");
    return value;
}

Json redactLogEntry(const Json &entryValue) {
    Json result = Json::object();
    for (const char *key : {"ts", "mono", "source", "level"})
        result.set(key, entryValue[key]);
    result.set("proc", Json::text(redactLogText(entryValue["proc"].string())));
    result.set("msg", Json::text(redactLogText(entryValue["msg"].string())));
    result.set("raw", Json::text(redactLogText(entryValue["raw"].string())));
    return result;
}

Json collectLogs(const LogsQuery &query) {
    const double nowMs = query.nowMs > 0 ? query.nowMs : currentTimeMs();
    const double uptimeSeconds = uptimeAtRoot(query.root);
    const double bootTimeMs = nowMs - std::round(uptimeSeconds * 1000);
    const uint64_t limit = parsedLimit(query.limit);
    const std::vector<std::string> sources =
        query.sources.empty() ? std::vector<std::string>{"system", "nativetv"}
                              : query.sources;
    const bool wantSystem = containsSource(sources, "system");
    const bool wantNativeTv = containsSource(sources, "nativetv");
    const bool wantKernel = containsSource(sources, "kernel");

    const std::string systemPath = rootPath(query.root, "/var/log/messages");
    const std::string nativeTvPath = query.nativeTvPath.empty()
                                           ? pathFromEnvironment(query.root)
                                           : query.nativeTvPath;
    const std::string rotatedPath = nativeTvPath + ".1";
    uint64_t fileSize = 0;
    std::string fileInode;
    const bool systemAvailable = fileInfo(systemPath, fileSize, fileInode);
    const bool nativeTvCurrentAvailable = fileInfo(nativeTvPath, fileSize, fileInode);
    const bool rotatedAvailable = fileInfo(rotatedPath, fileSize, fileInode);

    Json metadata = Json::object();
    metadata.set("system", sourceMetadata(systemAvailable, systemPath));
    Json nativeTvMeta = sourceMetadata(nativeTvCurrentAvailable || rotatedAvailable,
                                         nativeTvPath);
    nativeTvMeta.set("rotatedAvailable", Json::flag(rotatedAvailable));
    metadata.set("nativetv", nativeTvMeta);
    Json kernelMeta = Json::object();
    const bool kernelAvailable = !wantKernel || query.kernelError.empty();
    kernelMeta.set("available", Json::flag(kernelAvailable));
    if (wantKernel && !query.kernelError.empty())
        kernelMeta.set("error", Json::text(query.kernelError));
    metadata.set("kernel", kernelMeta);

    const Json since = query.since.empty() ? Json::object() : parseLogCursor(query.since);
    bool incremental = !query.since.empty();
    std::vector<std::string> cursor;
    Json allEntries = Json::array();
    if (wantSystem && systemAvailable) {
        const Json at = since["system"];
        LogRead read;
        if (readLogLines(systemPath, at.isObject(),
                         static_cast<uint64_t>(std::max(0.0, at["end"].number())),
                         at["ino"].string(), 512 * 1024, read)) {
            uint64_t count = 0;
            appendReadEntries(allEntries, read, "system", bootTimeMs, nowMs,
                              uptimeSeconds, incremental, cursor, count);
        } else if (!query.since.empty()) {
            incremental = false;
        }
    }
    if (wantNativeTv && (nativeTvCurrentAvailable || rotatedAvailable)) {
        const Json at = since["nativetv"];
        LogRead read;
        if (nativeTvCurrentAvailable &&
            readLogLines(nativeTvPath, at.isObject(),
                         static_cast<uint64_t>(std::max(0.0, at["end"].number())),
                         at["ino"].string(), 512 * 1024, read)) {
            uint64_t count = 0;
            appendReadEntries(allEntries, read, "nativetv", bootTimeMs, nowMs,
                              uptimeSeconds, incremental, cursor, count);
            if (read.whole && count < limit && rotatedAvailable) {
                LogRead rotated;
                if (readLogLines(rotatedPath, false, 0, "", 512 * 1024, rotated))
                    appendEntries(allEntries,
                                  parseNativeTvLogs(rotated.text, bootTimeMs, uptimeSeconds));
            }
        } else if (!query.since.empty()) {
            incremental = false;
        }
    }
    if (wantKernel) {
        const Json kernelCursor = since["kernel"];
        const double after = incremental && kernelCursor.isNumber()
                                 ? kernelCursor.number()
                                 : -1;
        double lastMono = after;
        const Json parsed = parseKernelLogs(query.kernelText, bootTimeMs);
        for (const Json &item : parsed.items()) {
            const double monotonic = item["mono"].number();
            if (monotonic > after)
                allEntries.add(item);
            if (monotonic > lastMono)
                lastMono = monotonic;
        }
        if (lastMono >= 0) {
            std::ostringstream value;
            value << std::setprecision(15) << lastMono;
            cursor.push_back("kernel:" + value.str());
        }
    }

    std::vector<Json> entries = allEntries.items();
    std::stable_sort(entries.begin(), entries.end(), [](const Json &left, const Json &right) {
        const double leftMono = left["mono"].number();
        const double rightMono = right["mono"].number();
        if (leftMono != rightMono)
            return leftMono < rightMono;
        return left["ts"].string() < right["ts"].string();
    });
    const std::string filter = lower(trim(query.filter));
    if (!filter.empty()) {
        entries.erase(std::remove_if(entries.begin(), entries.end(), [&filter](const Json &item) {
                          return lower(item["raw"].string()).find(filter) == std::string::npos &&
                                 lower(item["proc"].string()).find(filter) == std::string::npos;
                      }), entries.end());
    }
    if (entries.size() > limit)
        entries.erase(entries.begin(), entries.end() - static_cast<std::ptrdiff_t>(limit));
    Json outputEntries = Json::array();
    for (const Json &item : entries)
        outputEntries.add(query.redact ? redactLogEntry(item) : item);

    Json result = Json::object();
    result.set("ok", Json::flag(true));
    std::string cursorText;
    for (const std::string &item : cursor) {
        if (!cursorText.empty())
            cursorText += ",";
        cursorText += item;
    }
    result.set("cursor", Json::text(cursorText));
    result.set("incremental", Json::flag(incremental));
    result.set("sources", metadata);
    result.set("uptime", Json::numeric(uptimeSeconds));
    result.set("bootTime", Json::numeric(bootTimeMs));
    result.set("limit", Json::numeric(limit));
    result.set("total", Json::numeric(entries.size()));
    result.set("entries", outputEntries);
    return result;
}
} // namespace service
} // namespace nativetv
