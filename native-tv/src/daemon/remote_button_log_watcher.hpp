#pragma once

#include "daemon/command_policy.hpp"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace nativetv {
class Json;
namespace daemon {
class Logger;

struct RemoteButtonLogWatcherOptions {
    std::string logPath = "/var/log/messages";
    unsigned pollIntervalMilliseconds = 200;
    unsigned alexaToastMuteMilliseconds = 4000;
    bool watchAi = false;
};

// Follows lginput2's named-button log records for remote buttons that are
// consumed before they reach evdev, then launches their configured targets.
class RemoteButtonLogWatcher {
  public:
    RemoteButtonLogWatcher(CommandPolicyExecutor &policy, CommandPolicyContext context,
                           std::string dataRoot,
                           Logger *logger = nullptr,
                           RemoteButtonLogWatcherOptions options =
                               RemoteButtonLogWatcherOptions());
    ~RemoteButtonLogWatcher();
    RemoteButtonLogWatcher(const RemoteButtonLogWatcher &) = delete;
    RemoteButtonLogWatcher &operator=(const RemoteButtonLogWatcher &) = delete;

    bool start();
    void stop();
    bool running() const;

  private:
    struct ToastState;
    void run();
    void pollOnce();
    bool refreshTargets();
    void advanceLogCursorToEnd();
    void readLogUpdates();
    void handleButton(const std::string &button, std::chrono::steady_clock::time_point now);
    void handleToastDeadline();
    bool submit(const std::string &action, const std::string &uri,
                const Json &payload, const std::string &appId,
                LunaClient::Callback completion);

    CommandPolicyExecutor &policy_;
    CommandPolicyContext context_;
    std::string dataRoot_;
    std::string markerPath_;
    Logger *logger_;
    RemoteButtonLogWatcherOptions options_;
    std::atomic<bool> running_{false};
    std::mutex lifecycleMutex_;
    std::mutex waitMutex_;
    std::condition_variable wake_;
    std::thread worker_;
    std::unordered_map<std::string, std::string> pressTargets_;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> lastPress_;
    std::string partialLine_;
    uint64_t logOffset_ = 0;
    uint64_t logDevice_ = 0;
    uint64_t logInode_ = 0;
    bool logCursorInitialized_ = false;
    bool targetsInitialized_ = false;
    std::string targetsStamp_;
    std::shared_ptr<ToastState> toastState_;
};

} // namespace daemon
} // namespace nativetv
