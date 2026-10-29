#include "daemon/remote_buttons.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <fcntl.h>
#include <fstream>
#include <map>
#include <poll.h>
#include <set>
#include <sstream>
#include <stdexcept>
#include <sys/stat.h>
#include <tuple>
#include <unistd.h>
#include <utility>

namespace nativetv {
namespace daemon {
namespace {
const unsigned kEvKey = 1;
const unsigned kHomeCode = 773;
const unsigned kDebounceMilliseconds = 200;

std::string colorForCode(unsigned code) {
    switch (code) {
    case 398:
    case 18874385: return "red";
    case 399:
    case 18874386: return "green";
    case 400:
    case 18874387: return "yellow";
    case 401:
    case 18874388: return "blue";
    default: return std::string();
    }
}

unsigned readU16(const unsigned char *bytes) {
    return static_cast<unsigned>(bytes[0]) |
           (static_cast<unsigned>(bytes[1]) << 8);
}

int readI32(const unsigned char *bytes) {
    const std::uint32_t value = static_cast<std::uint32_t>(bytes[0]) |
        (static_cast<std::uint32_t>(bytes[1]) << 8) |
        (static_cast<std::uint32_t>(bytes[2]) << 16) |
        (static_cast<std::uint32_t>(bytes[3]) << 24);
    return static_cast<std::int32_t>(value);
}

bool isRcuName(const std::string &name) {
    std::string lowered(name);
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](unsigned char value) {
                       return static_cast<char>(std::tolower(value));
                   });
    for (const char *candidate : {"lge rcu", "lge m-rcu", "smart remote rcu input",
                                  "lge simple premium"}) {
        if (lowered.find(candidate) != std::string::npos)
            return true;
    }
    return false;
}

std::string eventPath(const std::string &directory, unsigned index) {
    return directory + (directory.empty() || directory.back() == '/' ? "" : "/") +
           "event" + std::to_string(index);
}
} // namespace

RemoteButtonsParser::RemoteButtonsParser(size_t eventSize) : eventSize_(eventSize) {
    if (eventSize_ != 16 && eventSize_ != 24)
        throw std::invalid_argument("Linux input_event record size must be 16 or 24 bytes");
}

size_t RemoteButtonsParser::platformEventSize() {
    return sizeof(void *) == 8 ? 24 : 16;
}

std::vector<RemoteButtonEvent> RemoteButtonsParser::feed(const unsigned char *bytes,
                                                         size_t size) {
    std::vector<RemoteButtonEvent> events;
    if (!bytes || !size)
        return events;
    pending_.insert(pending_.end(), bytes, bytes + size);
    size_t offset = 0;
    while (pending_.size() - offset >= eventSize_) {
        const unsigned char *record = pending_.data() + offset;
        const size_t typeOffset = eventSize_ - 8;
        const unsigned type = readU16(record + typeOffset);
        const unsigned code = readU16(record + typeOffset + 2);
        const int value = readI32(record + typeOffset + 4);
        offset += eventSize_;
        if (type != kEvKey)
            continue;
        if (code == kHomeCode && (value == 0 || value == 1)) {
            events.push_back({RemoteButtonEventType::HomeKey, "", code, value == 1});
            continue;
        }
        if (value != 1)
            continue;
        const std::string color = colorForCode(code);
        if (!color.empty())
            events.push_back({RemoteButtonEventType::ColorButton, color, code, true});
        else
            events.push_back({RemoteButtonEventType::Key, "", code, true});
    }
    if (offset)
        pending_.erase(pending_.begin(), pending_.begin() + offset);
    return events;
}

std::vector<RemoteButtonEvent> RemoteButtonsParser::parseQtLogLine(
    const std::string &line) const {
    std::vector<RemoteButtonEvent> events;
    if (line.find("pressed") == std::string::npos &&
        line.find("D_KEY_PRESS") == std::string::npos)
        return events;
    for (const unsigned code : {18874385u, 18874386u, 18874387u, 18874388u}) {
        if (line.find(std::to_string(code)) == std::string::npos)
            continue;
        events.push_back({RemoteButtonEventType::ColorButton, colorForCode(code), code, true});
    }
    return events;
}

RemoteButtonsReader::RemoteButtonsReader(Handler handler,
                                         RemoteButtonsReaderOptions options)
    : handler_(std::move(handler)), options_(std::move(options)) {}

RemoteButtonsReader::~RemoteButtonsReader() { stop(); }

bool RemoteButtonsReader::isSupported(const RemoteButtonsReaderOptions &options) {
    if (!options.inputLogPath.empty() && access(options.inputLogPath.c_str(), F_OK) == 0)
        return true;
    if (access(eventPath(options.inputDirectory, 1).c_str(), F_OK) == 0 ||
        access(eventPath(options.inputDirectory, 2).c_str(), F_OK) == 0)
        return true;
    std::ifstream input(options.inputDevicesPath.c_str());
    std::string line;
    while (std::getline(input, line)) {
        if (line.compare(0, 6, "Name=\"") == 0 && isRcuName(line.substr(6)))
            return true;
    }
    return false;
}

bool RemoteButtonsReader::start() {
    std::lock_guard<std::mutex> lock(lifecycleMutex_);
    if (running_.load(std::memory_order_acquire))
        return true;
    if (worker_.joinable() || !handler_ || !isSupported(options_))
        return false;
    int descriptors[2] = {-1, -1};
    if (pipe(descriptors) != 0)
        return false;
    wakeRead_ = descriptors[0];
    wakeWrite_ = descriptors[1];
    (void)fcntl(wakeRead_, F_SETFL, fcntl(wakeRead_, F_GETFL) | O_NONBLOCK);
    (void)fcntl(wakeWrite_, F_SETFL, fcntl(wakeWrite_, F_GETFL) | O_NONBLOCK);
    running_.store(true, std::memory_order_release);
    try {
        worker_ = std::thread(&RemoteButtonsReader::run, this);
    } catch (...) {
        running_.store(false, std::memory_order_release);
        close(wakeRead_);
        close(wakeWrite_);
        wakeRead_ = wakeWrite_ = -1;
        return false;
    }
    return true;
}

void RemoteButtonsReader::stop() {
    std::lock_guard<std::mutex> lock(lifecycleMutex_);
    running_.store(false, std::memory_order_release);
    if (wakeWrite_ >= 0) {
        const unsigned char wake = 1;
        // A failed wake only means the reader notices at its next poll timeout.
        if (write(wakeWrite_, &wake, 1) < 0) {
        }
    }
    if (worker_.joinable() && worker_.get_id() != std::this_thread::get_id())
        worker_.join();
    if (wakeRead_ >= 0)
        close(wakeRead_);
    if (wakeWrite_ >= 0)
        close(wakeWrite_);
    wakeRead_ = wakeWrite_ = -1;
}

bool RemoteButtonsReader::running() const {
    return running_.load(std::memory_order_acquire);
}

void RemoteButtonsReader::run() {
    struct Device {
        int fd;
        std::string path;
        RemoteButtonsParser parser;
        Device(int descriptor, std::string devicePath, size_t eventSize)
            : fd(descriptor), path(std::move(devicePath)), parser(eventSize) {}
    };
    std::map<int, Device> devices;
    int logFd = -1;
    off_t logPosition = 0;
    std::string logPending;
    std::map<std::string, std::chrono::steady_clock::time_point> lastButtonPress;
    const size_t eventSize = RemoteButtonsParser::platformEventSize();
    auto nextScan = std::chrono::steady_clock::now();

    const auto scan = [&] {
        std::set<std::string> openPaths;
        for (const auto &device : devices)
            openPaths.insert(device.second.path);
        for (unsigned index = 0; index < 16; ++index) {
            const std::string path = eventPath(options_.inputDirectory, index);
            if (openPaths.count(path))
                continue;
            const int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
            if (fd >= 0)
                devices.emplace(std::piecewise_construct,
                                std::forward_as_tuple(fd),
                                std::forward_as_tuple(fd, path, eventSize));
        }
        if (logFd < 0 && !options_.inputLogPath.empty()) {
            logFd = open(options_.inputLogPath.c_str(), O_RDONLY | O_CLOEXEC);
            if (logFd >= 0)
                logPosition = lseek(logFd, 0, SEEK_END);
        }
        nextScan = std::chrono::steady_clock::now() +
                   std::chrono::milliseconds(options_.scanIntervalMilliseconds);
    };

    const auto dispatchEvents = [&](const std::vector<RemoteButtonEvent> &events) {
        for (const RemoteButtonEvent &event : events) {
            if (event.type == RemoteButtonEventType::ColorButton) {
                const auto now = std::chrono::steady_clock::now();
                const auto found = lastButtonPress.find(event.button);
                if (found != lastButtonPress.end() &&
                    now - found->second < std::chrono::milliseconds(kDebounceMilliseconds))
                    continue;
                lastButtonPress[event.button] = now;
            }
            try { handler_(event); } catch (...) {}
        }
    };

    scan();
    while (running_.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() >= nextScan)
            scan();
        std::vector<pollfd> descriptors;
        descriptors.reserve(devices.size() + 1);
        descriptors.push_back({wakeRead_, POLLIN, 0});
        for (const auto &device : devices)
            descriptors.push_back({device.first, POLLIN | POLLERR | POLLHUP, 0});
        int ready;
        do {
            ready = poll(descriptors.data(), descriptors.size(),
                         static_cast<int>(options_.pollTimeoutMilliseconds));
        } while (ready < 0 && errno == EINTR);
        if (ready < 0)
            break;
        if (descriptors[0].revents & POLLIN) {
            unsigned char discard[32];
            while (read(wakeRead_, discard, sizeof(discard)) > 0) {}
            if (!running_.load(std::memory_order_acquire))
                break;
        }

        for (size_t index = 1; index < descriptors.size(); ++index) {
            pollfd &polled = descriptors[index];
            auto found = devices.find(polled.fd);
            if (found == devices.end())
                continue;
            if (polled.revents & (POLLERR | POLLHUP | POLLNVAL)) {
                close(found->first);
                devices.erase(found);
                continue;
            }
            if (!(polled.revents & POLLIN))
                continue;
            std::array<unsigned char, 4096> bytes{};
            const ssize_t count = read(found->first, bytes.data(), bytes.size());
            if (count <= 0) {
                if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
                    continue;
                close(found->first);
                devices.erase(found);
                continue;
            }
            dispatchEvents(found->second.parser.feed(bytes.data(),
                                                    static_cast<size_t>(count)));
        }
        if (logFd >= 0) {
            struct stat status;
            if (fstat(logFd, &status) == 0) {
                if (status.st_size < logPosition) {
                    logPosition = 0;
                    logPending.clear();
                    (void)lseek(logFd, 0, SEEK_SET);
                }
                if (status.st_size > logPosition) {
                    std::array<char, 4096> bytes{};
                    while (logPosition < status.st_size) {
                        const ssize_t count = pread(logFd, bytes.data(),
                            std::min<size_t>(bytes.size(),
                                static_cast<size_t>(status.st_size - logPosition)),
                            logPosition);
                        if (count <= 0)
                            break;
                        logPosition += count;
                        logPending.append(bytes.data(), static_cast<size_t>(count));
                        size_t newline;
                        while ((newline = logPending.find('\n')) != std::string::npos) {
                            const std::string line = logPending.substr(0, newline);
                            logPending.erase(0, newline + 1);
                            dispatchEvents(RemoteButtonsParser(eventSize).parseQtLogLine(line));
                        }
                        if (logPending.size() > 16384)
                            logPending.erase(0, logPending.size() - 16384);
                    }
                }
            }
        }
    }

    for (const auto &device : devices)
        close(device.first);
    if (logFd >= 0)
        close(logFd);
    running_.store(false, std::memory_order_release);
}

RemoteHomeHoldTimer::RemoteHomeHoldTimer(
    EventLoop &loop, Handler handler, EventLoop::Milliseconds holdDuration,
    ShortPressHandler shortPressHandler)
    : loop_(loop), handler_(std::move(handler)),
      shortPressHandler_(std::move(shortPressHandler)),
      holdDuration_(holdDuration) {
    if (!handler_ || holdDuration_.count() <= 0)
        throw std::invalid_argument("Home hold timer needs a handler and positive duration");
}

RemoteHomeHoldTimer::~RemoteHomeHoldTimer() { stop(); }

void RemoteHomeHoldTimer::onKeyEvent(bool pressed) {
    ShortPressHandler shortPressHandler;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopped_ || pressed_ == pressed)
            return;
        pressed_ = pressed;
        ++generation_;
        if (!pressed) {
            const bool shortPress = timer_ != 0 && !holdFired_;
            if (timer_)
                (void)loop_.cancelTimer(timer_);
            timer_ = 0;
            if (shortPress) shortPressHandler = shortPressHandler_;
        } else {
            holdFired_ = false;
            const unsigned long long generation = generation_;
            timer_ = loop_.scheduleAfter(holdDuration_, [this, generation]() {
                Handler handler;
                {
                    std::lock_guard<std::mutex> callbackLock(mutex_);
                    if (stopped_ || !pressed_ || generation_ != generation)
                        return;
                    timer_ = 0;
                    holdFired_ = true;
                    handler = handler_;
                }
                // Keep lifecycle work outside the timer lock; the handler
                // only submits it to the bounded worker pool.
                handler();
            });
            if (!timer_)
                pressed_ = false;
        }
    }
    if (shortPressHandler)
        shortPressHandler();
}

void RemoteHomeHoldTimer::stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopped_)
        return;
    stopped_ = true;
    pressed_ = false;
    holdFired_ = false;
    ++generation_;
    if (timer_)
        (void)loop_.cancelTimer(timer_);
    timer_ = 0;
}

} // namespace daemon
} // namespace nativetv
