#pragma once

#include "core/json.hpp"
#include "daemon/child_process.hpp"
#include "service/syslog.hpp"
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <sys/socket.h>

namespace nativetv {
namespace daemon {
class Logger;

struct SyslogForwarderOptions {
    std::string systemLogPath = "/var/log/messages";
    std::string nativeTvLogPath;
    std::string bootMarkerPath = "/var/run/tvweb.syslog-boot-sent";
    std::string procRoot = "/proc";
    unsigned pollIntervalMs = 5000;
    unsigned kernelPollIntervalMs = 30000;
    std::function<double()> wallClockMs;
    std::function<double()> uptimeSeconds;
    std::function<std::string(std::string &)> kernelReader;
    std::function<void(const Json &)> statusChanged;
};

// A dedicated bounded-rate worker keeps DNS, file reads, dmesg and UDP away
// from the daemon event loop. It reads the existing logs and data formats.
class SyslogForwarder {
  public:
    SyslogForwarder(Json config, ChildProcessRunner &children, Logger *logger = nullptr,
                    SyslogForwarderOptions options = SyslogForwarderOptions());
    ~SyslogForwarder();
    SyslogForwarder(const SyslogForwarder &) = delete;
    SyslogForwarder &operator=(const SyslogForwarder &) = delete;

    bool start();
    void stop();
    void flush();
    void setHostname(const std::string &name);
    Json counters() const;
    Json status() const;

  private:
    struct Cursor {
        uint64_t end = 0;
        std::string inode;
        bool initialized = false;
    };
    struct FileRead {
        std::string text;
        uint64_t end = 0;
        std::string inode;
        bool atEnd = false;
    };

    void run();
    void pollOnce();
    bool followFile(const std::string &source, const std::string &path,
                    const std::string &kind, double bootTimeMs,
                    double nowMs, double uptimeSeconds);
    void sendEntries(const Json &entries, double bootTimeMs);
    bool sendMessage(const Json &entry, double bootTimeMs);
    bool resolve();
    void closeSocket();
    void setError(const std::string &error);
    void publishStatus() const;
    Json statusLocked() const;
    Json countersLocked() const;

    service::SyslogSettings settings_;
    SyslogForwarderOptions options_;
    ChildProcessRunner &children_;
    Logger *logger_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::thread worker_;
    std::string deviceName_;
    std::string error_;
    std::unordered_map<std::string, uint64_t> messages_;
    uint64_t errors_ = 0;
    bool active_ = false;
    bool stopping_ = false;
    bool flushRequested_ = false;
    bool bootPending_ = false;
    bool needsLookup_ = true;
    bool announced_ = false;
    int socket_ = -1;
    int addressFamily_ = 0;
    std::string address_;
    sockaddr_storage addressStorage_{};
    socklen_t addressLength_ = 0;
    std::string systemLogPath_;
    std::string nativeTvLogPath_;
    Cursor systemCursor_;
    Cursor nativeTvCursor_;
    bool kernelCursorInitialized_ = false;
    double kernelCursor_ = -1;
    std::chrono::steady_clock::time_point kernelReadAt_;
    bool hasKernelRead_ = false;
};
} // namespace daemon
} // namespace nativetv
