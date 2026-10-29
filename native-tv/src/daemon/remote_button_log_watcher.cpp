#include "daemon/remote_button_log_watcher.hpp"
#include "core/json.hpp"
#include "daemon/json_file.hpp"
#include "daemon/logger.hpp"
#include "daemon/luna.hpp"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace nativetv {
namespace daemon {
namespace {
const size_t kLogReadLimit = 64 * 1024;
const size_t kMaximumLogLine = 16 * 1024;
const std::chrono::milliseconds kPressDebounce(500);
const std::chrono::milliseconds kToastRetry(5000);
const std::chrono::milliseconds kCommandTimeout(5000);

std::string dataFile(const std::string &root, const std::string &name) {
    const std::string directory = root.empty() ? "/var/lib/tvweb" : root;
    return directory + (directory.back() == '/' ? "" : "/") + name;
}

bool logFileIdentity(const struct stat &info, uint64_t &device, uint64_t &inode) {
    if (!S_ISREG(info.st_mode)) return false;
    device = static_cast<uint64_t>(info.st_dev);
    inode = static_cast<uint64_t>(info.st_ino);
    return true;
}

std::string buttonTypeFromLine(const std::string &line) {
    const size_t event = line.find("NL_BUTTON_CLICK");
    if (event == std::string::npos) return std::string();
    const size_t start = line.find('{', event);
    if (start == std::string::npos || line.size() - start > kMaximumLogLine)
        return std::string();
    try {
        const Json payload = Json::parse(line.substr(start));
        if (!payload.isObject() || !payload["button_type"].isString())
            return std::string();
        return payload["button_type"].string();
    } catch (...) {
        return std::string();
    }
}

bool writeMarker(const std::string &path) {
    const int descriptor = open(path.c_str(), O_WRONLY | O_CREAT | O_CLOEXEC, 0600);
    if (descriptor < 0) return false;
    const bool ok = fsync(descriptor) == 0;
    const int closeResult = close(descriptor);
    return ok && closeResult == 0;
}

} // namespace

struct RemoteButtonLogWatcher::ToastState {
    std::mutex mutex;
    bool muted = false;
    bool disablePending = false;
    bool enablePending = false;
    uint64_t generation = 0;
    std::chrono::steady_clock::time_point enableAt;
    std::chrono::steady_clock::time_point retryAt;
};

RemoteButtonLogWatcher::RemoteButtonLogWatcher(
    CommandPolicyExecutor &policy, CommandPolicyContext context,
    std::string dataRoot, Logger *logger,
    RemoteButtonLogWatcherOptions options)
    : policy_(policy), context_(std::move(context)),
      dataRoot_(std::move(dataRoot)),
      markerPath_(dataFile(dataRoot_, "remotekeys-toasts-off")), logger_(logger),
      options_(std::move(options)), toastState_(std::make_shared<ToastState>()) {
    toastState_->muted = access(markerPath_.c_str(), F_OK) == 0;
    if (toastState_->muted)
        toastState_->enableAt = std::chrono::steady_clock::now();
    if (options_.pollIntervalMilliseconds == 0)
        options_.pollIntervalMilliseconds = 200;
}

RemoteButtonLogWatcher::~RemoteButtonLogWatcher() { stop(); }

bool RemoteButtonLogWatcher::start() {
    std::lock_guard<std::mutex> lock(lifecycleMutex_);
    if (running_.load(std::memory_order_acquire)) return true;
    running_.store(true, std::memory_order_release);
    try {
        worker_ = std::thread(&RemoteButtonLogWatcher::run, this);
    } catch (...) {
        running_.store(false, std::memory_order_release);
        return false;
    }
    return true;
}

void RemoteButtonLogWatcher::stop() {
    std::lock_guard<std::mutex> lock(lifecycleMutex_);
    running_.store(false, std::memory_order_release);
    wake_.notify_all();
    if (worker_.joinable()) worker_.join();
}

bool RemoteButtonLogWatcher::running() const {
    return running_.load(std::memory_order_acquire);
}

void RemoteButtonLogWatcher::run() {
    while (running_.load(std::memory_order_acquire)) {
        pollOnce();
        std::unique_lock<std::mutex> lock(waitMutex_);
        wake_.wait_for(lock,
            std::chrono::milliseconds(options_.pollIntervalMilliseconds),
            [this]() { return !running_.load(std::memory_order_acquire); });
    }
}

void RemoteButtonLogWatcher::pollOnce() {
    if (refreshTargets())
        advanceLogCursorToEnd();
    if (pressTargets_.empty())
        advanceLogCursorToEnd();
    else
        readLogUpdates();
    handleToastDeadline();
}

bool RemoteButtonLogWatcher::refreshTargets() {
    JsonFile mapFile(dataFile(dataRoot_, "remotekeys.json"), 64 * 1024);
    JsonFile lgFile(dataFile(dataRoot_, "remotekeys-lg.json"), 256 * 1024);
    const Json map = mapFile.load().value["keys"];
    const Json lg = lgFile.load().value;
    std::unordered_map<std::string, std::string> targets;
    if (map.isObject() && lg.isObject()) {
        if (options_.watchAi && map.has("ai") && map["ai"].isString() &&
            lg["services"]["voiceconductor"].boolean())
            targets["KEY_VOICE"] = map["ai"].string();
        if (map.has("alexa") && map["alexa"].isString() &&
            lg["blocks"]["amazon.alexa.view"].boolean())
            targets["KEY_ALEXAVOICE"] = map["alexa"].string();
    }
    const bool changed = !targetsInitialized_ || targets != pressTargets_;
    targetsInitialized_ = true;
    pressTargets_ = std::move(targets);
    return changed;
}

void RemoteButtonLogWatcher::advanceLogCursorToEnd() {
    struct stat info = {};
    if (stat(options_.logPath.c_str(), &info) != 0 ||
        !logFileIdentity(info, logDevice_, logInode_)) {
        logOffset_ = 0;
        logDevice_ = 0;
        logInode_ = 0;
        logCursorInitialized_ = false;
        partialLine_.clear();
        return;
    }
    logOffset_ = static_cast<uint64_t>(std::max<off_t>(0, info.st_size));
    logCursorInitialized_ = true;
    partialLine_.clear();
}

void RemoteButtonLogWatcher::readLogUpdates() {
    const int descriptor = open(options_.logPath.c_str(), O_RDONLY | O_CLOEXEC);
    if (descriptor < 0) return;
    struct stat info = {};
    uint64_t device = 0;
    uint64_t inode = 0;
    if (fstat(descriptor, &info) != 0 || !logFileIdentity(info, device, inode)) {
        close(descriptor);
        return;
    }
    const uint64_t size = static_cast<uint64_t>(std::max<off_t>(0, info.st_size));
    if (!logCursorInitialized_ || device != logDevice_ || inode != logInode_ ||
        size < logOffset_) {
        logDevice_ = device;
        logInode_ = inode;
        logOffset_ = 0;
        partialLine_.clear();
        logCursorInitialized_ = true;
    }
    if (size <= logOffset_) {
        close(descriptor);
        return;
    }
    uint64_t start = logOffset_;
    if (size - start > kLogReadLimit) {
        start = size - kLogReadLimit;
        partialLine_.clear();
    }
    std::vector<char> bytes(static_cast<size_t>(size - start));
    ssize_t count = 0;
    do {
        count = pread(descriptor, bytes.data(), bytes.size(),
                      static_cast<off_t>(start));
    } while (count < 0 && errno == EINTR);
    close(descriptor);
    if (count <= 0) return;
    logOffset_ = start + static_cast<uint64_t>(count);
    std::string text = partialLine_;
    text.append(bytes.data(), static_cast<size_t>(count));
    size_t begin = 0;
    // The presses of one read are one moment: stamped per press, a slow
    // handler for the first (a loaded TV) pushed the second past the debounce.
    const auto readAt = std::chrono::steady_clock::now();
    for (;;) {
        const size_t end = text.find('\n', begin);
        if (end == std::string::npos) break;
        const std::string line = text.substr(begin, end - begin);
        const std::string button = buttonTypeFromLine(line);
        if (!button.empty()) handleButton(button, readAt);
        begin = end + 1;
    }
    partialLine_ = text.substr(begin);
    if (partialLine_.size() > kMaximumLogLine)
        partialLine_.clear();
}

bool RemoteButtonLogWatcher::submit(const std::string &action,
                                    const std::string &uri,
                                    const Json &payload,
                                    const std::string &appId,
                                    LunaClient::Callback completion) {
    CommandPolicySpec command;
    command.action = action;
    command.uri = uri;
    command.payload = payload;
    command.appId = appId;
    command.timeout = kCommandTimeout;
    // Toast disable/enable must be serialized with each other, while a toast
    // request and the app launch caused by the same press must be allowed to
    // run concurrently. An empty resource leaves launches independent.
    if (action == "remoteKeys.toasts-disable" ||
        action == "remoteKeys.toasts-enable")
        command.resource = "remote-keys.toasts";
    return policy_.submit(command, context_, false, std::move(completion)).kind ==
           CommandDecisionKind::Queued;
}

void RemoteButtonLogWatcher::handleButton(const std::string &button,
                                          std::chrono::steady_clock::time_point now) {
    const char *key = nullptr;
    if (button == "KEY_VOICE") key = "ai";
    else if (button == "KEY_ALEXAVOICE") key = "alexa";
    if (!key) return;
    const auto target = pressTargets_.find(button);
    if (target == pressTargets_.end()) return;

    const auto last = lastPress_.find(button);
    if (last != lastPress_.end() && now >= last->second &&
        now - last->second < kPressDebounce)
        return;
    lastPress_[button] = now;

    if (button == "KEY_ALEXAVOICE") {
        bool shouldDisable = false;
        {
            std::lock_guard<std::mutex> lock(toastState_->mutex);
            ++toastState_->generation;
            toastState_->enableAt = now +
                std::chrono::milliseconds(options_.alexaToastMuteMilliseconds);
            toastState_->retryAt = std::chrono::steady_clock::time_point();
            if (!toastState_->muted) {
                toastState_->muted = true;
                toastState_->disablePending = true;
                shouldDisable = true;
                if (!writeMarker(markerPath_) && logger_)
                    logger_->warning("remote keys: could not write the toast recovery marker");
            }
        }
        if (shouldDisable) {
            const std::shared_ptr<ToastState> state = toastState_;
            const bool queued = submit("remoteKeys.toasts-disable",
                "com.webos.notification/disable", Json::object(), "",
                [state](const LunaReply &) {
                    std::lock_guard<std::mutex> lock(state->mutex);
                    state->disablePending = false;
                });
            if (!queued) {
                std::lock_guard<std::mutex> lock(state->mutex);
                state->disablePending = false;
            }
        }
    }

    if (target->second.empty()) return;
    Json payload = Json::object();
    payload.set("id", Json::text(target->second));
    const std::string appId = target->second;
    (void)submit("remoteKeys.logged-button-launch",
        "com.webos.applicationManager/launch", payload, appId,
        [](const LunaReply &) {});
}

void RemoteButtonLogWatcher::handleToastDeadline() {
    const auto now = std::chrono::steady_clock::now();
    uint64_t generation = 0;
    {
        std::lock_guard<std::mutex> lock(toastState_->mutex);
        if (!toastState_->muted || toastState_->disablePending ||
            toastState_->enablePending || now < toastState_->enableAt ||
            (toastState_->retryAt.time_since_epoch().count() != 0 &&
             now < toastState_->retryAt))
            return;
        toastState_->enablePending = true;
        generation = toastState_->generation;
    }

    const std::shared_ptr<ToastState> state = toastState_;
    const std::string marker = markerPath_;
    CommandPolicyExecutor *policy = &policy_;
    const CommandPolicyContext context = context_;
    const bool queued = submit("remoteKeys.toasts-enable",
        "com.webos.notification/enable", Json::object(), "",
        [state, marker, generation, policy, context](const LunaReply &reply) {
            bool disableAgain = false;
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                state->enablePending = false;
                const bool enabled = !reply.timedOut && reply.error.empty() &&
                    reply.response["returnValue"].boolean();
                if (!enabled) {
                    state->retryAt = std::chrono::steady_clock::now() + kToastRetry;
                    state->enableAt = state->retryAt;
                } else if (state->generation == generation &&
                           std::chrono::steady_clock::now() >= state->enableAt) {
                    state->muted = false;
                    state->retryAt = std::chrono::steady_clock::time_point();
                    (void)unlink(marker.c_str());
                } else if (state->generation != generation) {
                    state->disablePending = true;
                    disableAgain = true;
                }
            }
            if (disableAgain) {
                CommandPolicySpec command;
                command.action = "remoteKeys.toasts-disable";
                command.uri = "com.webos.notification/disable";
                command.payload = Json::object();
                command.timeout = kCommandTimeout;
                command.resource = "remote-keys.toasts";
                const CommandDecision decision = policy->submit(
                    command, context, false,
                    [state](const LunaReply &) {
                        std::lock_guard<std::mutex> lock(state->mutex);
                        state->disablePending = false;
                    });
                if (decision.kind != CommandDecisionKind::Queued) {
                    std::lock_guard<std::mutex> lock(state->mutex);
                    state->disablePending = false;
                }
            }
        });
    if (!queued) {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->enablePending = false;
        state->retryAt = std::chrono::steady_clock::now() + kToastRetry;
        state->enableAt = state->retryAt;
    }
}

} // namespace daemon
} // namespace nativetv
