#pragma once

#include "daemon/event_loop.hpp"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace nativetv {
namespace daemon {

enum class RemoteButtonEventType { ColorButton, HomeKey, Key };

struct RemoteButtonEvent {
    RemoteButtonEventType type = RemoteButtonEventType::Key;
    std::string button;
    unsigned code = 0;
    bool pressed = false;
};

struct RemoteButtonsReaderOptions {
    std::string inputDirectory = "/dev/input";
    std::string inputDevicesPath = "/proc/bus/input/devices";
    std::string inputLogPath = "/tmp/var/log/inputcommon";
    unsigned scanIntervalMilliseconds = 5000;
    unsigned pollTimeoutMilliseconds = 500;
};

// Incremental decoder for Linux input_event records and the Qt input log.
class RemoteButtonsParser {
  public:
    explicit RemoteButtonsParser(size_t eventSize);
    std::vector<RemoteButtonEvent> feed(const unsigned char *bytes, size_t size);
    std::vector<RemoteButtonEvent> parseQtLogLine(const std::string &line) const;
    static size_t platformEventSize();

  private:
    size_t eventSize_;
    std::vector<unsigned char> pending_;
};

// Reads evdev in a dedicated thread, rescanning devices and watching LG's Qt
// input log without tying up the daemon event loop.
class RemoteButtonsReader {
  public:
    using Handler = std::function<void(const RemoteButtonEvent &)>;

    explicit RemoteButtonsReader(Handler handler,
                                 RemoteButtonsReaderOptions options =
                                     RemoteButtonsReaderOptions());
    ~RemoteButtonsReader();
    RemoteButtonsReader(const RemoteButtonsReader &) = delete;
    RemoteButtonsReader &operator=(const RemoteButtonsReader &) = delete;

    static bool isSupported(const RemoteButtonsReaderOptions &options =
                                RemoteButtonsReaderOptions());
    bool start();
    void stop();
    bool running() const;

  private:
    void run();
    void scanDevices();
    void readQtLog();
    void dispatch(const std::vector<RemoteButtonEvent> &events);

    Handler handler_;
    RemoteButtonsReaderOptions options_;
    std::atomic<bool> running_{false};
    std::mutex lifecycleMutex_;
    std::thread worker_;
    int wakeRead_ = -1;
    int wakeWrite_ = -1;
};

// Keeps Home hold timing beside the native evdev reader. Input arrives on the
// reader thread; the hold callback is delivered on the daemon event loop.
class RemoteHomeHoldTimer {
  public:
    using Handler = std::function<void()>;
    using ShortPressHandler = std::function<void()>;

    RemoteHomeHoldTimer(EventLoop &loop, Handler handler,
                        EventLoop::Milliseconds holdDuration =
                            EventLoop::Milliseconds(1200),
                        ShortPressHandler shortPressHandler =
                            ShortPressHandler());
    ~RemoteHomeHoldTimer();
    RemoteHomeHoldTimer(const RemoteHomeHoldTimer &) = delete;
    RemoteHomeHoldTimer &operator=(const RemoteHomeHoldTimer &) = delete;

    void onKeyEvent(bool pressed);
    void stop();

  private:
    EventLoop &loop_;
    Handler handler_;
    ShortPressHandler shortPressHandler_;
    EventLoop::Milliseconds holdDuration_;
    std::mutex mutex_;
    EventLoop::TimerId timer_ = 0;
    unsigned long long generation_ = 0;
    bool pressed_ = false;
    bool holdFired_ = false;
    bool stopped_ = false;
};

} // namespace daemon
} // namespace nativetv
