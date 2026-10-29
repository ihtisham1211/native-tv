#include "service/logs.hpp"
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

using namespace nativetv;
using namespace nativetv::service;

namespace {
unsigned checks = 0;

void check(bool value, const char *message) {
    ++checks;
    if (!value)
        throw std::runtime_error(message);
}

void writeFile(const std::string &path, const std::string &value, bool append = false) {
    std::ofstream output(path.c_str(), append ? std::ios::app : std::ios::trunc);
    if (!output)
        throw std::runtime_error("could not write log fixture");
    output << value;
}

void makeDirectory(const std::string &path) {
    if (mkdir(path.c_str(), 0700) != 0)
        throw std::runtime_error("could not create log fixture directory");
}
} // namespace

int main() {
    try {
        check(detectLogLevel("everything ok", "crit") == "error" &&
                  detectLogLevel("warning: audio underrun") == "warning" &&
                  detectLogLevel("debug verbose output") == "debug" &&
                  detectLogLevel("server ready") == "info",
              "Log level detection");

        const double boot = 1760000000000.0;
        const Json system = parseSystemLogs(
            "2026-10-07T15:59:28.001403Z [13639.010008] user.info sam [] launch started\n"
            "2026-10-07T15:59:29.100000Z [13640.100000] daemon.warn pulseaudio [1234] sink underrun\n",
            boot);
        check(system.size() == 2 && system[size_t(0)]["source"].string() == "system" &&
                  system[size_t(0)]["mono"].number() == 13639.010008 &&
                  system[size_t(0)]["proc"].string() == "sam" &&
                  system[size_t(1)]["level"].string() == "warning",
              "Structured webOS system logs");
        const Json fallback = parseSystemLogs(
            "2026-10-07T16:00:00.000Z simple message without facility", boot);
        check(fallback.size() == 1 && fallback[size_t(0)]["proc"].string() == "system" &&
                  fallback[size_t(0)]["msg"].string() == "simple message without facility",
              "Unstructured system log fallback");

        const Json nativetv = parseNativeTvLogs(
            "2026-10-07T15:59:30.123Z [13641.123] [INFO] mqtt: connected to 192.168.1.100\n"
            "2026-10-07T15:59:31.000Z [13642.000] [WARN] ha: discovery published\n"
            "    additional stack trace line\n"
            "update: installed v0.75.0\n",
            boot, 13640.0);
        check(nativetv.size() == 4 && nativetv[size_t(0)]["proc"].string() == "mqtt" &&
                  nativetv[size_t(1)]["level"].string() == "warning" &&
                  nativetv[size_t(2)]["mono"].number() == 13642.0 &&
                  nativetv[size_t(2)]["proc"].string() == "tvweb" &&
                  nativetv[size_t(3)]["mono"].number() == 13642.0,
              "nativeTv log tags, continuation lines and legacy entries");

        const Json kernel = parseKernelLogs(
            "<3>[   42.250000] usb 1-1: descriptor read failed\n"
            "[ 43.000000 ] [drm] initialized panel driver\n", boot);
        check(kernel.size() == 2 && kernel[size_t(0)]["pri"].number() == 3 &&
                  kernel[size_t(0)]["proc"].string() == "usb" &&
                  kernel[size_t(0)]["mono"].number() == 42.25 &&
                  kernel[size_t(1)]["proc"].string() == "drm",
              "Kernel log levels, tags and monotonic timestamps");

        const Json cursor = parseLogCursor("system:120:55,nativetv:9:7,kernel:13.5,rubbish");
        check(cursor["system"]["end"].number() == 120 &&
                  cursor["nativetv"]["ino"].string() == "7" &&
                  cursor["kernel"].number() == 13.5 &&
                  !parseLogCursor("system:x")["system"].valid(),
              "Log cursors and malformed-field handling");

        check(redactLogText("connected to 192.168.1.125:1883 and 127.0.0.1") ==
                  "connected to <IP>:1883 and 127.0.0.1",
              "IPv4 redaction preserves loopback");
        check(redactLogText("?k=secret&sources=nativetv") ==
                  "?k=<REDACTED>&sources=nativetv",
              "Query credential redaction");
        check(redactLogText("serialNumber: \"301NDXK0C912\"") ==
                  "serialNumber: \"<SERIAL>\"",
              "Device serial redaction");
        check(redactLogText("wlan0: SSID \"Home_Network\"") ==
                  "wlan0: SSID \"<SSID>\"",
              "Wi-Fi network name redaction");
        check(redactLogText("url=https://alice:secret@example.test") ==
                  "url=https://alice:<REDACTED>@example.test" &&
                  redactLogText("Authorization: Bearer abc.def-123") ==
                      "Authorization: Bearer <REDACTED>" &&
                  redactLogText("password=secret-value") ==
                      "password=<REDACTED>",
              "URL, bearer and key-value credential redaction");
        const std::string redactedAddresses =
            redactLogText("mac=14:49:e0:12:34:56 ipv6=2001:db8::1 loopback=::1");
        check(redactedAddresses.find("<MAC>") != std::string::npos &&
                  redactedAddresses.find("<IPV6>") != std::string::npos &&
                  redactedAddresses.find("loopback=::1") != std::string::npos &&
                  redactLogText("zip_code=bt155at").find("<LOCATION>") !=
                      std::string::npos,
              "MAC, IPv6 and location redaction");
        Json rawEntry = Json::parse(
            "{\"ts\":\"2026-10-07T12:00:00.000Z\",\"mono\":1,"
            "\"source\":\"nativetv\",\"level\":\"info\","
            "\"proc\":\"tvweb(192.168.1.2)\",\"msg\":\"secret ?k=abc123\","
            "\"raw\":\"raw 192.168.1.2\"}");
        const Json redacted = redactLogEntry(rawEntry);
        check(redacted["proc"].string() == "tvweb(<IP>)" &&
                  redacted["msg"].string() == "secret ?k=<REDACTED>" &&
                  redacted["ts"].string() == rawEntry["ts"].string(),
              "Structured log entry redaction preserves metadata");

        char temporary[] = "/tmp/nativetv-logs-XXXXXX";
        const char *created = mkdtemp(temporary);
        if (!created)
            throw std::runtime_error("could not create log fixture root");
        const std::string root(created);
        makeDirectory(root + "/var");
        makeDirectory(root + "/var/log");
        makeDirectory(root + "/var/lib");
        makeDirectory(root + "/var/lib/tvweb");
        makeDirectory(root + "/proc");
        const std::string systemPath = root + "/var/log/messages";
        const std::string nativeTvPath = root + "/var/lib/tvweb/tvweb.log";
        writeFile(root + "/proc/uptime", "100.0 200.0\n");
        writeFile(systemPath,
                  "2026-10-07T12:00:00.000Z [4.0] user.info system [] system ready\n");
        writeFile(nativeTvPath,
                  "2026-10-07T12:00:01.000Z [5.0] [INFO] app: serialNumber: \"301NDXK0C912\" at 192.168.1.12\n"
                  "2026-10-07T12:00:02.000Z [6.0] [WARN] mqtt: SSID \"Home_Wifi\"\n");
        writeFile(nativeTvPath + ".1",
                  "2026-10-07T11:59:59.000Z [3.0] [ERR] old: rotated failure\n");

        LogsQuery logQuery;
        logQuery.root = root;
        logQuery.nowMs = 1791374400000.0;
        Json collected = collectLogs(logQuery);
        check(collected["ok"].boolean() && collected["total"].number() == 4 &&
                  collected["entries"].size() == 4 &&
                  collected["entries"][size_t(0)]["proc"].string() == "old" &&
                  collected["entries"][size_t(1)]["source"].string() == "system" &&
                  collected["sources"]["nativetv"]["rotatedAvailable"].boolean() &&
                  !collected["incremental"].boolean(),
              "Log collection merges current and rotated sources chronologically");
        const std::string firstCursor = collected["cursor"].string();

        logQuery.limit = "2";
        logQuery.redact = true;
        Json limited = collectLogs(logQuery);
        check(limited["total"].number() == 2 &&
                  limited["entries"][size_t(0)]["msg"].string().find("<SERIAL>") !=
                      std::string::npos &&
                  limited["entries"][size_t(0)]["msg"].string().find("<IP>") !=
                      std::string::npos &&
                  limited["entries"][size_t(1)]["msg"].string().find("<SSID>") !=
                      std::string::npos,
              "Log collection applies newest-entry limits and optional redaction");

        logQuery.filter = "MQTT";
        logQuery.limit = "5";
        Json filtered = collectLogs(logQuery);
        check(filtered["total"].number() == 1 &&
                  filtered["entries"][size_t(0)]["proc"].string() == "mqtt",
              "Log collection filters raw lines and process names case-insensitively");
        logQuery.filter.clear();
        logQuery.limit.clear();

        writeFile(nativeTvPath,
                  "2026-10-07T12:00:01.000Z [5.0] [INFO] app: serialNumber: \"301NDXK0C912\" at 192.168.1.12\n"
                  "2026-10-07T12:00:02.000Z [6.0] [WARN] mqtt: SSID \"Home_Wifi\"\n"
                  "2026-10-07T12:00:03.000Z [7.0] [INFO] mqtt: connected\n");
        logQuery.since = firstCursor;
        logQuery.limit.clear();
        logQuery.redact = false;
        Json appended = collectLogs(logQuery);
        check(appended["incremental"].boolean() && appended["total"].number() == 1 &&
                  appended["entries"][size_t(0)]["msg"].string() == "mqtt: connected",
              "Log collection resumes at byte cursors and defers old entries");

        logQuery.sources = {"system", "kernel"};
        logQuery.since = appended["cursor"].string() + ",kernel:9";
        logQuery.kernelText = "[ 9.000000 ] old: previous kernel entry\n"
                              "[ 10.000000 ] new: latest kernel entry\n";
        writeFile(systemPath,
                  "2026-10-07T12:00:04.000Z [8.0] user.info system [] restarted\n");
        Json reset = collectLogs(logQuery);
        unsigned kernelEntries = 0;
        for (const Json &item : reset["entries"].items()) {
            if (item["source"].string() == "kernel")
                ++kernelEntries;
        }
        check(!reset["incremental"].boolean() && kernelEntries == 2,
              "A source reset restarts the kernel ring cursor too");

        std::remove((nativeTvPath + ".1").c_str());
        std::remove(nativeTvPath.c_str());
        std::remove(systemPath.c_str());
        std::remove((root + "/proc/uptime").c_str());
        rmdir((root + "/proc").c_str());
        rmdir((root + "/var/lib/tvweb").c_str());
        rmdir((root + "/var/lib").c_str());
        rmdir((root + "/var/log").c_str());
        rmdir((root + "/var").c_str());
        rmdir(root.c_str());

        std::printf("%u native log parser checks passed\n", checks);
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
