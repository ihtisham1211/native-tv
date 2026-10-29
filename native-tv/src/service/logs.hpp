#pragma once

#include "core/json.hpp"
#include <string>
#include <vector>

namespace nativetv {
namespace service {
std::string detectLogLevel(const std::string &text, const std::string &hint = "");
Json parseSystemLogs(const std::string &raw, double bootTimeMs, double nowMs = 0);
Json parseNativeTvLogs(const std::string &raw, double bootTimeMs, double uptimeSeconds);
Json parseKernelLogs(const std::string &raw, double bootTimeMs);
Json parseLogCursor(const std::string &cursor);
std::string redactLogText(const std::string &text);
Json redactLogEntry(const Json &entry);

struct LogsQuery {
    std::string root = "/";
    std::string nativeTvPath;
    std::vector<std::string> sources;
    std::string limit;
    std::string filter;
    std::string since;
    bool redact = false;
    std::string kernelText;
    std::string kernelError;
    double nowMs = 0;
};

Json collectLogs(const LogsQuery &query);
} // namespace service
} // namespace nativetv
