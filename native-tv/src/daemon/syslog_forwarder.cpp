#include "daemon/syslog_forwarder.hpp"
#include "daemon/logger.hpp"
#include "service/logs.hpp"
#include "service/syslog.hpp"
#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <netdb.h>
#include <regex>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace nativetv {
namespace daemon {
namespace {
const double clockFloorMs = 1767225600000.0;
const size_t readChunk = 64 * 1024;

double wallClockMs() {
    return static_cast<double>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::system_clock::now().time_since_epoch())
                                   .count());
}

double readUptime(const std::string &procRoot) {
    std::string path = procRoot.empty() || procRoot == "/"
                           ? "/proc/uptime"
                           : procRoot + (procRoot.back() == '/' ? "uptime" : "/uptime");
    std::ifstream input(path.c_str());
    std::string value;
    if (!(input >> value))
        return 0;
    try {
        const double seconds = std::stod(value);
        return std::isfinite(seconds) && seconds >= 0 ? seconds : 0;
    } catch (...) {
        return 0;
    }
}

bool fileEnd(const std::string &path, uint64_t &end, std::string &inode) {
    struct stat info;
    if (stat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode)) {
        end = 0;
        inode.clear();
        return false;
    }
    end = static_cast<uint64_t>(std::max<off_t>(0, info.st_size));
    inode = std::to_string(static_cast<unsigned long long>(info.st_ino));
    return true;
}

std::string target(const service::SyslogSettings &settings) {
    const bool ipv6Literal = settings.server.find(':') != std::string::npos &&
                             settings.server.find(']') == std::string::npos;
    return (ipv6Literal ? "[" + settings.server + "]" : settings.server) + ":" +
           std::to_string(settings.port);
}

std::string findDmesgExecutable() {
    const char *pathValue = std::getenv("PATH");
    if (pathValue) {
        std::string path(pathValue);
        size_t start = 0;
        while (start <= path.size()) {
            const size_t end = path.find(':', start);
            const std::string directory = path.substr(
                start, (end == std::string::npos ? path.size() : end) - start);
            const std::string candidate = (directory.empty() ? "." : directory) + "/dmesg";
            if (access(candidate.c_str(), X_OK) == 0)
                return candidate;
            if (end == std::string::npos)
                break;
            start = end + 1;
        }
    }
    for (const char *candidate : {"/bin/dmesg", "/usr/bin/dmesg", "/sbin/dmesg",
                                  "/usr/sbin/dmesg"}) {
        if (access(candidate, X_OK) == 0)
            return candidate;
    }
    return "";
}

bool sourceEnabled(const service::SyslogSettings &settings, const std::string &source) {
    return std::find(settings.sources.begin(), settings.sources.end(), source) !=
           settings.sources.end();
}

Json dateUnstampedNativeTv(const Json &entries, double nowMs, double uptimeSeconds) {
    static const std::regex stamp(R"(^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(?:\.\d+)?Z)");
    const std::string now = service::syslogIsoTimestamp(nowMs);
    bool beforeFirstTimestamp = true;
    Json adjusted = Json::array();
    for (const Json &source : entries.items()) {
        Json entry = source;
        if (beforeFirstTimestamp && std::regex_search(entry["raw"].string(), stamp))
            beforeFirstTimestamp = false;
        if (beforeFirstTimestamp) {
            entry.set("ts", Json::text(now));
            entry.set("mono", Json::numeric(uptimeSeconds));
        }
        adjusted.add(entry);
    }
    return adjusted;
}

} // namespace

SyslogForwarder::SyslogForwarder(Json config, ChildProcessRunner &children, Logger *logger,
                                 SyslogForwarderOptions options)
    : options_(std::move(options)), children_(children), logger_(logger) {
    (void)service::readSyslogSettings(config, settings_);
    const Json device = config["device"];
    deviceName_ = device["name"].string();
    if (options_.nativeTvLogPath.empty()) {
        const char *configured = std::getenv("TVWEB_LOG");
        options_.nativeTvLogPath = configured && *configured
                                         ? configured
                                         : "/var/lib/tvweb/tvweb.log";
    }
    systemLogPath_ = options_.systemLogPath;
    nativeTvLogPath_ = options_.nativeTvLogPath;
    if (!options_.wallClockMs)
        options_.wallClockMs = wallClockMs;
    if (!options_.uptimeSeconds) {
        const std::string procRoot = options_.procRoot;
        options_.uptimeSeconds = [procRoot]() { return readUptime(procRoot); };
    }
}

SyslogForwarder::~SyslogForwarder() { stop(); }

bool SyslogForwarder::start() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (settings_.server.empty())
            return false;
        if (active_)
            return true;
        stopping_ = false;
        flushRequested_ = false;
        needsLookup_ = true;
        announced_ = false;
        messages_.clear();
        for (const std::string &source : settings_.sources)
            messages_[source] = 0;
        errors_ = 0;
        error_.clear();
        active_ = true;
    }

    uint64_t end = 0;
    std::string inode;
    const bool boot = access(options_.bootMarkerPath.c_str(), F_OK) != 0;
    bootPending_ = boot;
    if (boot) {
        systemCursor_.end = 0;
        systemCursor_.inode.clear();
        systemCursor_.initialized = true;
        kernelCursorInitialized_ = true;
        kernelCursor_ = -1;
    } else {
        (void)fileEnd(systemLogPath_, end, inode);
        systemCursor_.end = end;
        systemCursor_.inode = inode;
        systemCursor_.initialized = true;
        kernelCursorInitialized_ = false;
    }
    (void)fileEnd(nativeTvLogPath_, end, inode);
    nativeTvCursor_.end = end;
    nativeTvCursor_.inode = inode;
    nativeTvCursor_.initialized = true;
    hasKernelRead_ = false;
    worker_ = std::thread(&SyslogForwarder::run, this);
    publishStatus();
    return true;
}

void SyslogForwarder::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!active_ && !worker_.joinable())
            return;
        stopping_ = true;
    }
    condition_.notify_all();
    if (worker_.joinable())
        worker_.join();
    closeSocket();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        active_ = false;
        stopping_ = false;
    }
    publishStatus();
}

void SyslogForwarder::flush() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!active_)
            return;
        flushRequested_ = true;
    }
    condition_.notify_one();
}

void SyslogForwarder::setHostname(const std::string &name) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        deviceName_ = name;
        if (active_)
            flushRequested_ = true;
    }
    condition_.notify_one();
}

Json SyslogForwarder::countersLocked() const {
    if (!active_)
        return Json();
    Json messages = Json::object();
    for (const auto &source : messages_)
        messages.set(source.first, Json::numeric(source.second));
    Json result = Json::object();
    result.set("messages", messages);
    result.set("errors", Json::numeric(errors_));
    return result;
}

Json SyslogForwarder::statusLocked() const {
    if (!active_)
        return Json();
    Json result = Json::object();
    result.set("server", Json::text(settings_.server));
    result.set("port", Json::numeric(settings_.port));
    result.set("hostname", Json::text(settings_.hostname));
    Json sources = Json::array();
    for (const std::string &source : settings_.sources)
        sources.add(Json::text(source));
    result.set("sources", sources);
    result.set("redact", Json::flag(settings_.redact));
    result.set("messages", countersLocked()["messages"]);
    result.set("errors", Json::numeric(errors_));
    result.set("error", error_.empty() ? Json() : Json::text(error_));
    return result;
}

Json SyslogForwarder::counters() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return countersLocked();
}

Json SyslogForwarder::status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return statusLocked();
}

void SyslogForwarder::publishStatus() const {
    if (options_.statusChanged)
        options_.statusChanged(status());
}

void SyslogForwarder::run() {
    for (;;) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_)
                break;
        }
        pollOnce();
        std::unique_lock<std::mutex> lock(mutex_);
        if (stopping_)
            break;
        const auto interval = std::chrono::milliseconds(options_.pollIntervalMs);
        condition_.wait_for(lock, interval, [this]() { return stopping_ || flushRequested_; });
        flushRequested_ = false;
    }
}

bool SyslogForwarder::resolve() {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    const std::string port = std::to_string(settings_.port);
    addrinfo *addresses = nullptr;
    const int lookup = getaddrinfo(settings_.server.c_str(), port.c_str(), &hints, &addresses);
    if (lookup != 0) {
        setError("could not resolve " + settings_.server + ": " + gai_strerror(lookup));
        return false;
    }
    bool opened = false;
    for (addrinfo *candidate = addresses; candidate; candidate = candidate->ai_next) {
        if ((candidate->ai_family != AF_INET && candidate->ai_family != AF_INET6) ||
            candidate->ai_addrlen > sizeof(addressStorage_))
            continue;
        const int descriptor = socket(candidate->ai_family, SOCK_DGRAM, IPPROTO_UDP);
        if (descriptor < 0)
            continue;
        closeSocket();
        socket_ = descriptor;
        addressFamily_ = candidate->ai_family;
        addressLength_ = static_cast<socklen_t>(candidate->ai_addrlen);
        std::memcpy(&addressStorage_, candidate->ai_addr, candidate->ai_addrlen);
        char shown[NI_MAXHOST];
        const int nameResult = getnameinfo(candidate->ai_addr,
                                           static_cast<socklen_t>(candidate->ai_addrlen),
                                           shown, sizeof(shown), nullptr, 0,
                                           NI_NUMERICHOST);
        address_ = nameResult == 0 ? shown : settings_.server;
        opened = true;
        break;
    }
    freeaddrinfo(addresses);
    if (!opened) {
        setError("could not create UDP socket for " + target(settings_));
        return false;
    }
    needsLookup_ = false;
    if (!announced_) {
        announced_ = true;
        if (logger_)
            logger_->info("syslog: forwarding " + [&]() {
                std::string list;
                for (const std::string &source : settings_.sources) {
                    if (!list.empty())
                        list += ", ";
                    list += source;
                }
                return list;
            }() + " to " + target(settings_) +
                          (address_ != settings_.server ? " (" + address_ + ")" : ""));
    }
    return true;
}

void SyslogForwarder::closeSocket() {
    if (socket_ >= 0)
        close(socket_);
    socket_ = -1;
    addressFamily_ = 0;
    addressLength_ = 0;
    address_.clear();
}

void SyslogForwarder::setError(const std::string &error) {
    std::string previous;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (error_ == error)
            return;
        previous = error_;
        error_ = error;
    }
    if (!logger_)
        return;
    if (!error.empty())
        logger_->error("syslog: " + error);
    else if (!previous.empty())
        logger_->info("syslog: sending to " + target(settings_) + " again");
}

bool SyslogForwarder::followFile(const std::string &source, const std::string &path,
                                const std::string &kind, double bootTimeMs,
                                double nowMs, double uptimeSeconds) {
    Cursor &cursor = source == "system" ? systemCursor_ : nativeTvCursor_;
    struct stat info;
    if (stat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode))
        return true;
    const uint64_t size = static_cast<uint64_t>(std::max<off_t>(0, info.st_size));
    const std::string inode = std::to_string(static_cast<unsigned long long>(info.st_ino));
    const bool sameFile = cursor.initialized && (cursor.inode.empty() || cursor.inode == inode);
    const bool whole = !sameFile || cursor.end > size;
    const uint64_t start = whole ? 0 : cursor.end;
    const size_t toRead = static_cast<size_t>(std::min<uint64_t>(size - start, readChunk));
    std::string bytes;
    if (toRead) {
        const int descriptor = open(path.c_str(), O_RDONLY);
        if (descriptor < 0)
            return true;
        bytes.resize(toRead);
        size_t received = 0;
        while (received < toRead) {
            const ssize_t count = pread(descriptor, &bytes[received], toRead - received,
                                        static_cast<off_t>(start + received));
            if (count < 0 && errno == EINTR)
                continue;
            if (count <= 0)
                break;
            received += static_cast<size_t>(count);
        }
        close(descriptor);
        bytes.resize(received);
    }

    size_t last = bytes.find_last_of('\n');
    if (last == std::string::npos && bytes.size() == readChunk)
        last = bytes.size() - 1;
    std::string text;
    uint64_t next = start;
    if (last != std::string::npos) {
        text = bytes.substr(0, last + 1);
        next += last + 1;
    }
    cursor = Cursor{next, inode, true};

    if (!text.empty()) {
        Json entries = kind == "system"
                           ? service::parseSystemLogs(text, bootTimeMs, nowMs)
                           : service::parseNativeTvLogs(text, bootTimeMs, uptimeSeconds);
        if (kind == "nativetv")
            entries = dateUnstampedNativeTv(entries, nowMs, uptimeSeconds);
        sendEntries(entries, bootTimeMs);
    }
    return start + bytes.size() >= size;
}

void SyslogForwarder::sendEntries(const Json &entries, double bootTimeMs) {
    for (const Json &entry : entries.items())
        (void)sendMessage(entry, bootTimeMs);
}

bool SyslogForwarder::sendMessage(const Json &entry, double bootTimeMs) {
    if (socket_ < 0 || needsLookup_) {
        std::lock_guard<std::mutex> lock(mutex_);
        ++errors_;
        return false;
    }
    std::string hostname = settings_.hostname;
    if (hostname.empty()) {
        std::lock_guard<std::mutex> lock(mutex_);
        hostname = deviceName_;
    }
    service::SyslogFormatOptions formatOptions;
    formatOptions.hostname = hostname;
    formatOptions.bootTimeMs = bootTimeMs;
    formatOptions.redact = settings_.redact;
    formatOptions.nowMs = options_.wallClockMs();
    const std::string message = service::formatSyslogMessage(entry, formatOptions);
    const ssize_t sent = sendto(socket_, message.data(), message.size(), 0,
                                reinterpret_cast<const sockaddr *>(&addressStorage_),
                                addressLength_);
    if (sent < 0 || static_cast<size_t>(sent) != message.size()) {
        const int failure = errno;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++errors_;
        }
        closeSocket();
        needsLookup_ = true;
        setError("could not send to " + target(settings_) + ": " + std::strerror(failure));
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++messages_[entry["source"].string()];
    }
    setError("");
    return true;
}

void SyslogForwarder::pollOnce() {
    if (needsLookup_ && !resolve()) {
        publishStatus();
        return;
    }
    std::string hostname = settings_.hostname;
    if (hostname.empty()) {
        std::lock_guard<std::mutex> lock(mutex_);
        hostname = deviceName_;
    }
    const double wall = options_.wallClockMs();
    if (hostname.empty() || !addressLength_ || needsLookup_ || wall < clockFloorMs) {
        publishStatus();
        return;
    }
    const double uptime = options_.uptimeSeconds();
    const double bootTime = wall - std::floor(uptime * 1000 + 0.5);
    bool systemCaughtUp = true;
    if (sourceEnabled(settings_, "system"))
        systemCaughtUp = followFile("system", systemLogPath_, "system", bootTime, wall, uptime);
    if (sourceEnabled(settings_, "nativetv"))
        (void)followFile("nativetv", nativeTvLogPath_, "nativetv", bootTime, wall, uptime);

    if (!sourceEnabled(settings_, "kernel")) {
        if (bootPending_ && systemCaughtUp) {
            const int marker = open(options_.bootMarkerPath.c_str(), O_WRONLY | O_CREAT, 0644);
            if (marker >= 0)
                close(marker);
            bootPending_ = false;
        }
        publishStatus();
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (hasKernelRead_ &&
        now - kernelReadAt_ < std::chrono::milliseconds(options_.kernelPollIntervalMs)) {
        if (bootPending_ && systemCaughtUp) {
            const int marker = open(options_.bootMarkerPath.c_str(), O_WRONLY | O_CREAT, 0644);
            if (marker >= 0)
                close(marker);
            bootPending_ = false;
        }
        publishStatus();
        return;
    }
    kernelReadAt_ = now;
    hasKernelRead_ = true;
    std::string kernelText;
    std::string kernelError;
    if (options_.kernelReader) {
        kernelText = options_.kernelReader(kernelError);
    } else {
        const std::string executable = findDmesgExecutable();
        if (executable.empty()) {
            kernelError = "dmesg executable not found";
        } else {
            ChildOptions child;
            child.arguments = {executable, "-r"};
            child.timeout = std::chrono::milliseconds(3500);
            child.maximumOutputBytes = 2 * 1024 * 1024;
            const ChildResult result = children_.run(child);
            if (result.succeeded())
                kernelText = result.standardOutput;
            else if (!result.error.empty())
                kernelError = result.error;
            else if (result.timedOut)
                kernelError = "dmesg timed out";
            else
                kernelError = "dmesg exited with status " + std::to_string(result.exitCode);
        }
    }
    if (!kernelError.empty()) {
        setError("could not run dmesg: " + kernelError);
    } else {
        const Json entries = service::parseKernelLogs(kernelText, bootTime);
        double last = kernelCursorInitialized_ ? kernelCursor_ : -1;
        Json fresh = Json::array();
        for (const Json &entry : entries.items()) {
            const double monotonic = entry["mono"].number();
            if ((!kernelCursorInitialized_ || monotonic > kernelCursor_) && monotonic > -1)
                fresh.add(entry);
            if (monotonic > last)
                last = monotonic;
        }
        if (last >= 0) {
            kernelCursor_ = last;
            kernelCursorInitialized_ = true;
        }
        sendEntries(fresh, bootTime);
    }
    if (kernelError.empty() && bootPending_ && systemCaughtUp) {
        const int marker = open(options_.bootMarkerPath.c_str(), O_WRONLY | O_CREAT, 0644);
        if (marker >= 0)
            close(marker);
        bootPending_ = false;
    }
    publishStatus();
}

} // namespace daemon
} // namespace nativetv
