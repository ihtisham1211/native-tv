#pragma once

#include "core/json.hpp"
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace nativetv {
namespace daemon {
enum class LogLevel { Debug, Info, Warning, Error };

struct LogRecord {
    std::string timestamp;
    double monotonicSeconds = 0;
    std::string process;
    std::string level;
    std::string message;
    std::string raw;

    Json toJson() const;
};

struct LoggerOptions {
    std::string environmentLevel;
    std::string configLevel = "info";
    std::string process = "nativetv-daemon";
    size_t ringCapacity = 512;
    // The sink receives one unprefixed line and whether it should be flushed.
    // It can enqueue output when synchronous console I/O is unsuitable.
    std::function<void(LogLevel, const std::string &, bool)> sink;
};

class Logger {
  public:
    explicit Logger(LoggerOptions options = LoggerOptions());

    bool enabled(LogLevel level) const;
    void write(LogLevel level, const std::string &message);
    void debug(const std::string &message);
    void info(const std::string &message);
    void warning(const std::string &message);
    void error(const std::string &message);

    // Returns the newest matching entries, in chronological order. The ring
    // is bounded by both record count and message length.
    std::vector<LogRecord> snapshot(size_t limit = 100,
                                    const std::string &filter = "") const;
    Json snapshotJson(size_t limit = 100, const std::string &filter = "") const;
    size_t size() const;
    const std::string &activeLevel() const;

    static std::string resolveLevel(const std::string &environmentLevel,
                                    const std::string &configLevel);

  private:
    LoggerOptions options_;
    std::string activeLevel_;
    mutable std::mutex mutex_;
    mutable std::mutex sinkMutex_;
    std::deque<LogRecord> records_;
};
} // namespace daemon
} // namespace nativetv
