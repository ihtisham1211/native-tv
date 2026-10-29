#pragma once

#include "core/json.hpp"
#include "daemon/event_loop.hpp"
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace nativetv {
namespace daemon {
class Logger;
class LunaClient;
class LunaSubscription;

// Maintains the Game Optimizer input pipeline only while the dashboard polls
// its frame-rate endpoint. Luna callbacks and timer transitions run on loop_.
class GameFrameRate {
  public:
    GameFrameRate(EventLoop &loop, LunaClient &luna, Logger *logger = nullptr);
    ~GameFrameRate();
    GameFrameRate(const GameFrameRate &) = delete;
    GameFrameRate &operator=(const GameFrameRate &) = delete;

    // Returns the latest reading and asynchronously refreshes the 15-second
    // idle timer/start request. The first request may return an empty reading.
    Json frameRate();
    void stop();

    // The stats' reading (game.js read): the reading of the HDMI input on screen
    // as {frameRate, vrrType, port}, from the stream when it runs and from a
    // one-shot read otherwise, or an invalid Json when there is none. While
    // inputShown the bind is held (a stats reader alone never starts the
    // stream), and a read just after a switch waits up to 1.5 s for the bind.
    // The caller decides whether the reading is the app on screen. Blocks.
    Json readForStats(bool inputShown);

  private:
    struct ReadRequest {
        std::mutex mutex;
        std::condition_variable done;
        bool finished = false;
        Json reading;
    };
    void readOnLoop(const std::shared_ptr<ReadRequest> &request, bool inputShown);
    void readPipeline(const std::shared_ptr<ReadRequest> &request);
    void finishRead(const std::shared_ptr<ReadRequest> &request, const Json &reading);
    void flushWaiting();
    void setInputShown(bool shown);
    void holdBind();
    void requestOnLoop();
    void startBind();
    void startReading();
    void receiveBind(unsigned long long generation, const Json &response);
    void receiveReading(unsigned long long generation, const Json &response);
    void receiveBindClose(unsigned long long generation, int code, int signal);
    void stopStream();
    void stopOnLoop();
    void clearPipeline();
    void setLatest(const Json &reading);

    EventLoop &loop_;
    LunaClient &luna_;
    Logger *logger_;
    std::shared_ptr<LunaSubscription> bindSubscription_;
    std::shared_ptr<LunaSubscription> readingSubscription_;
    std::string pipeline_;
    bool supported_ = true;
    unsigned long long generation_ = 0;
    EventLoop::TimerId idleTimer_ = 0;
    mutable std::mutex latestMutex_;
    Json latest_;
    bool inputShown_ = false;
    EventLoop::TimerId readIdleTimer_ = 0;
    EventLoop::TimerId waitTimer_ = 0;
    std::vector<std::shared_ptr<ReadRequest>> waiting_;
    // The last one-shot read, shared for 2 s like the cached Luna read it replaces.
    std::string oneShotPipeline_;
    std::chrono::steady_clock::time_point oneShotAt_;
    Json oneShotReading_;
};
} // namespace daemon
} // namespace nativetv
