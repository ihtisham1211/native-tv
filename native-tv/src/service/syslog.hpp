#pragma once

#include "core/json.hpp"
#include <cstddef>
#include <string>
#include <vector>

namespace nativetv {
namespace service {

struct SyslogFormatOptions {
    std::string hostname;
    double bootTimeMs = 0;
    bool redact = true;
    double nowMs = 0;
};

struct SyslogSettings {
    std::string server;
    unsigned short port = 514;
    std::string hostname;
    std::vector<std::string> sources;
    bool redact = true;
};

// Reads config.syslog using the legacy defaults and accepted value shapes.
// Returns false when no non-empty server is configured.
bool readSyslogSettings(const Json &config, SyslogSettings &settings);

constexpr size_t syslogMaximumDatagramSize() { return 2048; }
std::string syslogIsoTimestamp(double milliseconds);

// Format one structured log entry as an RFC 5424 UTF-8 datagram.
std::string formatSyslogMessage(const Json &entry,
                                const SyslogFormatOptions &options);

} // namespace service
} // namespace nativetv
