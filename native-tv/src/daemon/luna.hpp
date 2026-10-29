#pragma once

#include "core/json.hpp"
#include "daemon/child_process.hpp"
#include "daemon/event_loop.hpp"
#include "daemon/logger.hpp"
#include <chrono>
#include <cstddef>
#include <deque>
#include <functional>
#include <atomic>
#include <mutex>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace nativetv {
namespace daemon {
class LunaClient;

struct LunaReply {
    Json response;
    std::string raw;
    std::string error;
    bool timedOut = false;
    std::chrono::steady_clock::time_point startedAt;
    std::chrono::steady_clock::time_point endedAt;
};

struct LunaClientOptions {
    std::string executable;
    std::vector<std::string> environment;
    std::chrono::milliseconds timeout{3500};
    size_t maximumReplyBytes = 4 * 1024 * 1024;
};

struct LunaSubscriptionHandlers {
    std::function<void(const Json &)> message;
    std::function<void(const std::string &)> error;
    std::function<void(int, int)> close;
};

class LunaSubscription : public std::enable_shared_from_this<LunaSubscription> {
  public:
    ~LunaSubscription();
    LunaSubscription(const LunaSubscription &) = delete;
    LunaSubscription &operator=(const LunaSubscription &) = delete;

    bool start();
    void stop();

  private:
    friend class LunaClient;
    LunaSubscription(LunaClient &client, std::string uri, Json payload,
                     std::string appId, LunaSubscriptionHandlers handlers);
    void startOnLoop();
    void stopOnLoop();
    void connectOnLoop();
    void onOutput(unsigned long long generation, const std::string &chunk);
    void onExit(unsigned long long generation, const ChildResult &result,
                std::exception_ptr failure);
    void reportError(const std::string &error);
    std::shared_ptr<std::atomic<bool>> cancellationToken() const;

    LunaClient &client_;
    std::string uri_;
    Json payload_;
    std::string appId_;
    LunaSubscriptionHandlers handlers_;
    bool stopped_ = true;
    bool active_ = false;
    bool restartAfterStop_ = false;
    bool retryImmediately_ = false;
    unsigned long long generation_ = 0;
    std::chrono::milliseconds retryDelay_{1000};
    EventLoop::TimerId retryTimer_ = 0;
    mutable std::mutex cancellationMutex_;
    std::shared_ptr<std::atomic<bool>> cancellation_;
    std::string buffer_;
};

class LunaClient {
  public:
    using Callback = std::function<void(const LunaReply &)>;

    LunaClient(EventLoop &loop, ChildProcessRunner &children, Logger *logger = nullptr,
               LunaClientOptions options = LunaClientOptions());
    LunaClient(const LunaClient &) = delete;
    LunaClient &operator=(const LunaClient &) = delete;

    // Calls are accepted from any thread. Completion always runs on the event
    // loop thread. At most two normal calls and one serialized urgent call run.
    bool call(const std::string &uri, const Json &payload, Callback callback,
              const std::string &appId = "", bool urgent = false,
              std::chrono::milliseconds timeout = std::chrono::milliseconds(0),
              bool allowLongWait = false);
    std::shared_ptr<LunaSubscription> subscribe(
        const std::string &uri, const Json &payload,
        const std::string &appId = "", LunaSubscriptionHandlers handlers = {});

  private:
    friend class LunaSubscription;
    struct Job {
        std::string uri;
        Json payload;
        std::string appId;
        Callback callback;
        std::chrono::milliseconds timeout{0};
        bool allowLongWait = false;
        bool urgent = false;
        bool retried = false;
        bool serviceRetried = false;
    };
    struct ServiceAlias {
        std::string newer;
        std::string older;
        int state = 0; // 0 unknown, 1 newer exists, -1 use older
    };

    void enqueue(const std::shared_ptr<Job> &job, bool retryFirst = false);
    void startWaiting();
    void run(const std::shared_ptr<Job> &job);
    void complete(const std::shared_ptr<Job> &job,
                  const std::shared_ptr<ChildResult> &result,
                  std::exception_ptr failure);
    std::string resolveUri(const std::string &uri) const;
    ServiceAlias *findAlias(const std::string &uri);
    const ServiceAlias *findAlias(const std::string &uri) const;
    void note(const std::string &message);

    EventLoop &loop_;
    ChildProcessRunner &children_;
    Logger *logger_;
    LunaClientOptions options_;
    std::deque<std::shared_ptr<Job>> waiting_;
    size_t running_ = 0;
    size_t urgentRunning_ = 0;
    std::vector<ServiceAlias> aliases_;
};
} // namespace daemon
} // namespace nativetv
