#include "daemon/logger.hpp"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <iostream>

namespace nativetv {
namespace daemon {
namespace {
const size_t kMaximumMessageBytes = 16 * 1024;
const size_t kDefaultLimit = 100;
const size_t kMaximumLimit = 1000;

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c);
    });
    return value;
}

const char *tag(LogLevel level) {
    switch (level) {
    case LogLevel::Debug: return "DBG";
    case LogLevel::Info: return "INFO";
    case LogLevel::Warning: return "WARN";
    case LogLevel::Error: return "ERR";
    }
    return "INFO";
}

const char *name(LogLevel level) {
    switch (level) {
    case LogLevel::Debug: return "debug";
    case LogLevel::Info: return "info";
    case LogLevel::Warning: return "warning";
    case LogLevel::Error: return "error";
    }
    return "info";
}

std::string timestamp(std::chrono::system_clock::time_point now) {
    const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
    std::tm utc;
    if (!gmtime_r(&seconds, &utc))
        return "1970-01-01T00:00:00.000Z";
    char buffer[24];
    if (std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S", &utc) == 0)
        return "1970-01-01T00:00:00.000Z";
    long millis = static_cast<long>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000);
    if (millis < 0)
        millis += 1000;
    std::string fractional = std::to_string(millis);
    while (fractional.size() < 3)
        fractional.insert(fractional.begin(), '0');
    return std::string(buffer) + "." + fractional + "Z";
}

std::string boundedMessage(const std::string &message) {
    if (message.size() <= kMaximumMessageBytes)
        return message;
    return message.substr(0, kMaximumMessageBytes - 14) + " [truncated]";
}

void defaultSink(LogLevel level, const std::string &line, bool flush) {
    std::ostream &stream = level == LogLevel::Warning || level == LogLevel::Error
                               ? std::cerr : std::cout;
    stream << line << '\n';
    if (flush)
        stream.flush();
}
} // namespace

Json LogRecord::toJson() const {
    Json result = Json::object();
    result.set("ts", Json::text(timestamp));
    result.set("mono", Json::numeric(monotonicSeconds));
    result.set("source", Json::text("nativetv"));
    result.set("proc", Json::text(process));
    result.set("level", Json::text(level));
    result.set("msg", Json::text(message));
    result.set("raw", Json::text(raw));
    return result;
}

Logger::Logger(LoggerOptions options) : options_(std::move(options)) {
    if (options_.ringCapacity == 0)
        options_.ringCapacity = 1;
    if (options_.process.empty())
        options_.process = "nativetv-daemon";
    std::string environmentLevel = options_.environmentLevel;
    if (environmentLevel.empty()) {
        const char *environment = std::getenv("TVWEB_LOG_LEVEL");
        if (environment)
            environmentLevel = environment;
    }
    activeLevel_ = resolveLevel(environmentLevel, options_.configLevel);
}

std::string Logger::resolveLevel(const std::string &environmentLevel,
                                 const std::string &configLevel) {
    const std::string selected = environmentLevel.empty() ? configLevel : environmentLevel;
    return selected.empty() ? "info" : lower(selected);
}

bool Logger::enabled(LogLevel level) const {
    if (level == LogLevel::Error || level == LogLevel::Warning)
        return true;
    if (activeLevel_ == "quiet" || activeLevel_ == "error" || activeLevel_ == "warn")
        return false;
    if (level == LogLevel::Debug)
        return activeLevel_ == "debug";
    return true;
}

void Logger::write(LogLevel level, const std::string &message) {
    if (!enabled(level))
        return;

    const auto now = std::chrono::system_clock::now();
    const auto monotonicNow = std::chrono::steady_clock::now();
    LogRecord record;
    record.timestamp = timestamp(now);
    record.monotonicSeconds = std::chrono::duration<double>(monotonicNow.time_since_epoch()).count();
    record.process = options_.process;
    record.level = name(level);
    record.message = boundedMessage(message);
    record.raw = record.timestamp + " [" + std::to_string(record.monotonicSeconds) + "] [" +
                 tag(level) + "] " + record.message;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        records_.push_back(record);
        if (records_.size() > options_.ringCapacity)
            records_.pop_front();
    }
    std::lock_guard<std::mutex> sinkLock(sinkMutex_);
    if (options_.sink)
        options_.sink(level, record.raw, level == LogLevel::Debug);
    else
        defaultSink(level, record.raw, level == LogLevel::Debug);
}

void Logger::debug(const std::string &message) { write(LogLevel::Debug, message); }
void Logger::info(const std::string &message) { write(LogLevel::Info, message); }
void Logger::warning(const std::string &message) { write(LogLevel::Warning, message); }
void Logger::error(const std::string &message) { write(LogLevel::Error, message); }

std::vector<LogRecord> Logger::snapshot(size_t limit, const std::string &filter) const {
    if (limit == 0)
        limit = kDefaultLimit;
    limit = std::min(limit, kMaximumLimit);
    const std::string needle = lower(filter);
    std::vector<LogRecord> result;
    std::lock_guard<std::mutex> lock(mutex_);
    for (std::deque<LogRecord>::const_reverse_iterator it = records_.rbegin();
         it != records_.rend(); ++it) {
        if (!needle.empty() && lower(it->raw).find(needle) == std::string::npos &&
            lower(it->process).find(needle) == std::string::npos)
            continue;
        result.push_back(*it);
        if (result.size() == limit)
            break;
    }
    std::reverse(result.begin(), result.end());
    return result;
}

Json Logger::snapshotJson(size_t limit, const std::string &filter) const {
    Json result = Json::array();
    const std::vector<LogRecord> entries = snapshot(limit, filter);
    for (const LogRecord &entry : entries)
        result.add(entry.toJson());
    return result;
}

size_t Logger::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return records_.size();
}

const std::string &Logger::activeLevel() const { return activeLevel_; }
} // namespace daemon
} // namespace nativetv
