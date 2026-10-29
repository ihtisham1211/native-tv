#pragma once

#include "daemon/event_loop.hpp"
#include "core/json.hpp"
#include <memory>
#include <string>

namespace nativetv {
class JsonFile;
namespace daemon {
class Logger;
class LunaClient;
class LunaSubscription;

struct RemoteBatteryTimings {
    EventLoop::Milliseconds check{600000};
    EventLoop::Milliseconds settle{3000};
    EventLoop::Milliseconds discover{2000};
    EventLoop::Milliseconds retry{300000};
    EventLoop::Milliseconds success{3600000};
};

// Owns the live Magic Remote battery poll and the existing shared cache file.
// All state transitions and Luna callbacks run on the daemon event loop.
class RemoteBattery {
  public:
    RemoteBattery(EventLoop &loop, LunaClient &luna, std::string dataRoot,
                  std::string systemRoot, Logger *logger = nullptr,
                  RemoteBatteryTimings timings = RemoteBatteryTimings());
    ~RemoteBattery();
    RemoteBattery(const RemoteBattery &) = delete;
    RemoteBattery &operator=(const RemoteBattery &) = delete;

    bool start();
    void stop();
    void setPowerOn(bool on);

  private:
    void readOnLoop();
    void onConnection(const Json &status);
    void onDeviceInfo(const Json &info, bool seedBattery);
    void readGatt();
    void finishRead(bool tried, bool hasLevel, int level);
    void completeRead(bool tried, bool hasLevel, int level);
    void scheduleDue(EventLoop::Milliseconds delay);
    void persist();
    void note(const std::string &message);

    EventLoop &loop_;
    LunaClient &luna_;
    std::string dataRoot_;
    std::string systemRoot_;
    Logger *logger_;
    RemoteBatteryTimings timings_;
    std::unique_ptr<JsonFile> file_;
    std::shared_ptr<LunaSubscription> connectionSubscription_;
    EventLoop::TimerId checkTimer_ = 0;
    EventLoop::TimerId settleTimer_ = 0;
    EventLoop::TimerId discoverTimer_ = 0;
    EventLoop::TimerId dueTimer_ = 0;
    bool started_ = false;
    bool poweredOn_ = false;
    bool connected_ = false;
    bool isBle_ = false;
    bool gattSupported_ = false;
    bool busy_ = false;
    bool readTried_ = false;
    bool hasLevel_ = false;
    int level_ = 0;
    bool hasLast_ = false;
    int lastLevel_ = 0;
    long long lastAt_ = 0;
    std::string lastMac_;
    std::string address_;
    std::string clientId_;
    bool hasLowStrength_ = false;
    bool lowStrength_ = false;
    std::string firmware_;
    std::string model_;
    bool due_ = true;
};
} // namespace daemon
} // namespace nativetv
