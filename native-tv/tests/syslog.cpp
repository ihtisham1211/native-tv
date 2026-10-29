#include "service/logs.hpp"
#include "service/syslog.hpp"
#include <cmath>
#include <ctime>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace nativetv;
using namespace nativetv::service;

namespace {
unsigned checks = 0;

void check(bool value, const std::string &message) {
    ++checks;
    if (!value)
        throw std::runtime_error(message);
}

double utcMilliseconds(int year, int month, int day, int hour, int minute, int second) {
    std::tm utc{};
    utc.tm_year = year - 1900;
    utc.tm_mon = month - 1;
    utc.tm_mday = day;
    utc.tm_hour = hour;
    utc.tm_min = minute;
    utc.tm_sec = second;
    return static_cast<double>(timegm(&utc)) * 1000;
}

Json first(const Json &array) {
    check(array.isArray() && array.size() > 0, "expected one parsed log entry");
    return array[size_t(0)];
}

bool validUtf8(const std::string &value) {
    for (size_t index = 0; index < value.size();) {
        const unsigned char lead = static_cast<unsigned char>(value[index]);
        size_t count = lead < 0x80 ? 1 : (lead & 0xe0) == 0xc0 ? 2
                                 : (lead & 0xf0) == 0xe0 ? 3
                                 : (lead & 0xf8) == 0xf0 ? 4 : 0;
        if (!count || index + count > value.size())
            return false;
        for (size_t offset = 1; offset < count; ++offset) {
            if ((static_cast<unsigned char>(value[index + offset]) & 0xc0) != 0x80)
                return false;
        }
        index += count;
    }
    return true;
}
} // namespace

int main() {
    try {
        const double boot = utcMilliseconds(2026, 10, 8, 12, 0, 0);
        SyslogFormatOptions options;
        options.hostname = "Living Room TV";
        options.bootTimeMs = boot;
        options.nowMs = boot;

        const Json system = first(parseSystemLogs(
            "2026-10-08T15:47:19.010008Z [13639.010008] user.info sam [] "
            "SAM NL_APP_LAUNCH_BEGIN {\"id\":\"com.webos.app.discovery\"}", boot));
        check(formatSyslogMessage(system, options) ==
                  "<14>1 2026-10-08T15:47:19.010Z Living-Room-TV sam - system - "
                  "SAM NL_APP_LAUNCH_BEGIN {\"id\":\"com.webos.app.discovery\"}",
              "System log formats as RFC 5424 with millisecond timestamp and priority");

        const Json warning = first(parseSystemLogs(
            "2026-10-08T12:00:01.000000Z [1.000000] daemon.warning pulseaudio [] "
            "sink underrun", boot));
        check(formatSyslogMessage(warning, options).compare(0, 5, "<28>1") == 0,
              "System facility and warning severity are preserved");

        const Json early = first(parseSystemLogs(
            "2023-01-01T00:00:12.500000Z [12.500000] user.warning WebAppMgr [] "
            "app memory high", boot));
        check(formatSyslogMessage(early, options) ==
                  "<12>1 2026-10-08T12:00:12.500Z Living-Room-TV WebAppMgr - system - "
                  "app memory high",
              "An unsynchronized clock uses boot time plus monotonic uptime");

        const Json nativetv = first(parseNativeTvLogs(
            "2026-10-08T12:30:00.000Z [1800.000] [WARN] mqtt: connection dropped, retrying",
            boot, 1800));
        check(formatSyslogMessage(nativetv, options) ==
                  "<28>1 2026-10-08T12:30:00.000Z Living-Room-TV mqtt - nativetv - "
                  "mqtt: connection dropped, retrying",
              "nativeTv logs use daemon facility and parsed process tag");

        const Json kernel = first(parseKernelLogs(
            "<3>[   42.250000] usb 1-1: device descriptor read/64, error -71", boot));
        check(formatSyslogMessage(kernel, options) ==
                  "<3>1 2026-10-08T12:00:42.250Z Living-Room-TV kernel - kernel - "
                  "usb 1-1: device descriptor read/64, error -71",
              "Kernel priority, process name and monotonic timestamp are preserved");

        SyslogFormatOptions unnamed = options;
        unnamed.hostname.clear();
        check(formatSyslogMessage(nativetv, unnamed).find("Z - mqtt -") != std::string::npos,
              "An empty hostname uses the nil header value");

        Json redactedEntry = first(parseSystemLogs(
            "2026-10-08T13:00:00.000000Z [3600.000000] user.info pqcontroller [] "
            "NL_PICTURE_PERIODIC_REPORT {\"zip_code\":\"zz99zz\","
            "\"ip\":\"192.168.1.20\"}", boot));
        const std::string redacted = formatSyslogMessage(redactedEntry, options);
        check(redacted.find("<LOCATION>") != std::string::npos &&
                  redacted.find("<IP>") != std::string::npos &&
                  redacted.find("192.168.1.20") == std::string::npos,
              "Log privacy redaction is enabled by default");
        SyslogFormatOptions unredacted = options;
        unredacted.redact = false;
        const std::string cleartext = formatSyslogMessage(redactedEntry, unredacted);
        check(cleartext.find("zz99zz") != std::string::npos &&
                  cleartext.find("192.168.1.20") != std::string::npos,
              "Log privacy redaction can be disabled by configuration");

        Json malformed = Json::parse(
            "{\"source\":\"system\",\"level\":\"info\",\"proc\":\"app\","
            "\"msg\":\"bad time\",\"ts\":\"invalid\"}");
        check(formatSyslogMessage(malformed, options).find(
                  "2026-10-08T12:00:00.000Z") != std::string::npos,
              "Missing timestamps use the injected current time");
        Json earlyWithoutMonotonic = malformed;
        earlyWithoutMonotonic.set("ts", Json::text("2023-01-01T00:00:00.000Z"));
        check(formatSyslogMessage(earlyWithoutMonotonic, options).find(
                  "2023-01-01T00:00:00.000Z") != std::string::npos,
              "An old valid timestamp remains intact when no monotonic time exists");

        Json longEntry = Json::object();
        longEntry.set("source", Json::text("nativetv"));
        longEntry.set("level", Json::text("info"));
        longEntry.set("proc", Json::text("app"));
        longEntry.set("ts", Json::text("2026-10-08T12:00:00.000Z"));
        std::string longMessage;
        for (int index = 0; index < 3000; ++index)
            longMessage += "\xC3\xA9";
        longEntry.set("msg", Json::text(longMessage));
        const std::string capped = formatSyslogMessage(longEntry, options);
        check(capped.size() <= syslogMaximumDatagramSize() &&
                  capped.size() > syslogMaximumDatagramSize() - 2 && validUtf8(capped),
              "Datagram byte limit truncates only at a complete UTF-8 character");

        Json fieldEntry = Json::object();
        fieldEntry.set("source", Json::text("system"));
        fieldEntry.set("level", Json::text("info"));
        fieldEntry.set("proc", Json::text("app"));
        fieldEntry.set("msg", Json::text("header test"));
        fieldEntry.set("ts", Json::text("2026-10-08T12:00:00.000Z"));
        SyslogFormatOptions hostile = options;
        hostile.hostname = "Room TV\nwest";
        check(formatSyslogMessage(fieldEntry, hostile).find(
                  " Room-TV-west app - system - header test") != std::string::npos,
              "Header fields replace whitespace with RFC-safe separators");

        // A config from before the rename names the source "glasshouse".
        SyslogSettings fromOldConfig;
        check(readSyslogSettings(Json::parse("{\"syslog\":{\"server\":\"nas\","
                                             "\"sources\":[\"system\",\"glasshouse\"]}}"),
                                 fromOldConfig) &&
                  fromOldConfig.sources == (std::vector<std::string>{"system", "nativetv"}),
              "The source's name from before the rename is read as nativetv");

        std::cout << "native syslog formatting checks passed (" << checks << ")\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
