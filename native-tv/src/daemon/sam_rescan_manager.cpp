#include "daemon/sam_rescan_manager.hpp"
#include "daemon/child_process.hpp"
#include "daemon/logger.hpp"
#include "daemon/luna.hpp"
#include "service/app_ids.hpp"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

namespace nativetv {
namespace daemon {
namespace {
const char kBlockedListKey[] = "profile.blockedAppList";
const char kNudgeId[] = "io.github.ihtisham1211.nativetv.rescan";
const char kHomeApp[] = "com.webos.app.home";
const char kScreensaverApp[] = "com.webos.app.screensaver";

unsigned testDelay(unsigned milliseconds) {
#ifdef NATIVETV_TEST_FAST_SAM_TIMING
    return milliseconds == 0 ? 0 : 5;
#else
    return milliseconds;
#endif
}

std::string rooted(const std::string &root, const std::string &path) {
    if (root.empty() || root == "/") return path;
    size_t end = root.size();
    while (end > 1 && root[end - 1] == '/') --end;
    return root.substr(0, end) + path;
}

ChildResult runShell(ChildProcessRunner &children, const std::string &systemRoot,
                     const std::string &script) {
    ChildOptions options;
    options.arguments.push_back(rooted(systemRoot, "/bin/sh"));
    options.arguments.push_back("-c");
    options.arguments.push_back(script);
    options.timeout = std::chrono::milliseconds(6000);
    options.maximumOutputBytes = 4096;
    return children.run(options);
}

bool foregroundInfo(const Json &response) {
    return response.isObject() && response["returnValue"].isBoolean() &&
           response["returnValue"].boolean();
}

std::string launchPointSignature(const Json &response) {
    Json points = response["launchPoints"];
    if (!points.isArray()) points = response["apps"];
    if (!foregroundInfo(response) || !points.isArray() || points.items().empty())
        return std::string();
    std::vector<std::string> ids;
    for (const Json &point : points.items()) {
        const std::string id = point["launchPointId"].isString()
            ? point["launchPointId"].string() : point["id"].string();
        ids.push_back(id);
    }
    std::sort(ids.begin(), ids.end());
    std::ostringstream signature;
    for (size_t index = 0; index < ids.size(); ++index) {
        if (index) signature << ',';
        signature << ids[index];
    }
    return signature.str();
}

bool returnedTrue(const Json &response) {
    return response.isObject() && response["returnValue"].isBoolean() &&
           response["returnValue"].boolean();
}
} // namespace

SamRescanManager::SamRescanManager(CommandPolicyExecutor &policy,
                                   ChildProcessRunner &children,
                                   std::string systemRoot, Logger *logger)
    : policy_(policy), children_(children), systemRoot_(std::move(systemRoot)),
      logger_(logger) {}

Json SamRescanManager::result(bool ok, const std::string &error) {
    Json value = Json::object();
    value.set("ok", Json::flag(true));
    value.set("result", Json::flag(ok));
    if (!error.empty()) value.set("error", Json::text(error));
    return value;
}

bool SamRescanManager::call(const std::string &action, const std::string &uri,
                            const Json &payload, const std::string &resource,
                            const CommandPolicyContext &context, Json &response,
                            std::string *error, std::chrono::milliseconds timeout) {
    struct Completion {
        std::mutex mutex;
        std::condition_variable condition;
        bool complete = false;
        LunaReply reply;
    };
    const std::shared_ptr<Completion> completion = std::make_shared<Completion>();
    CommandPolicySpec command;
    command.action = action;
    command.uri = uri;
    command.payload = payload;
    command.timeout = timeout;
    command.resource = resource;
    const CommandDecision decision = policy_.submit(
        command, context, false, [completion](const LunaReply &reply) {
            std::lock_guard<std::mutex> lock(completion->mutex);
            completion->reply = reply;
            completion->complete = true;
            completion->condition.notify_one();
        });
    if (decision.kind != CommandDecisionKind::Queued) {
        if (error) *error = decision.reason;
        return false;
    }
    std::unique_lock<std::mutex> lock(completion->mutex);
    if (!completion->condition.wait_for(lock, timeout + std::chrono::milliseconds(250),
                                        [completion] { return completion->complete; })) {
        if (error) *error = "SAM command completion timed out";
        return false;
    }
    if (completion->reply.timedOut || !completion->reply.error.empty() ||
        !completion->reply.response.isObject()) {
        if (error) *error = completion->reply.error.empty()
            ? "SAM Luna response was invalid" : completion->reply.error;
        return false;
    }
    response = completion->reply.response;
    return true;
}

bool SamRescanManager::readBlockedList(const CommandPolicyContext &context, Json &list) {
    Json payload = Json::object();
    Json names = Json::array();
    names.add(Json::text(kBlockedListKey));
    payload.set("configNames", names);
    Json response;
    if (!call("sam-rescan.get-blocked-list", "com.webos.service.config/getConfigs",
              payload, "sam-blocked-app-list", context, response) ||
        !returnedTrue(response))
        return false;
    const Json configs = response["configs"];
    list = configs[kBlockedListKey];
    return configs.isObject() && list.isArray();
}

bool SamRescanManager::writeBlockedList(const Json &list,
                                        const CommandPolicyContext &context) {
    Json configs = Json::object();
    configs.set(kBlockedListKey, list);
    Json payload = Json::object();
    payload.set("configs", configs);
    Json response;
    return call("sam-rescan.set-blocked-list", "com.webos.service.config/setConfigs",
                payload, "sam-blocked-app-list", context, response) &&
           returnedTrue(response);
}

void SamRescanManager::delay(unsigned milliseconds) const {
    std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
}

Json SamRescanManager::canNudge(const CommandPolicyContext &context) {
    std::lock_guard<std::mutex> operation(operationMutex_);
    Json list;
    const bool supported = readBlockedList(context, list);
    if (!supported && logger_)
        logger_->info("sam-rescan: profile.blockedAppList is unavailable; SAM cannot be nudged");
    return result(supported);
}

Json SamRescanManager::nudge(const CommandPolicyContext &context) {
    std::unique_lock<std::mutex> operation(operationMutex_);
    Json list;
    if (!readBlockedList(context, list)) {
        if (logger_)
            logger_->info("sam-rescan: profile.blockedAppList is unavailable; SAM cannot be nudged");
        return result(false);
    }

    Json restore = Json::array();
    Json nudge = Json::array();
    const std::vector<Json> entries = list.items();
    for (const Json &entry : entries) {
        if (entry.isString() && entry.string() == kNudgeId) continue;
        restore.add(entry);
        nudge.add(entry);
    }
    nudge.add(Json::text(kNudgeId));
    if (!writeBlockedList(nudge, context)) return result(false);

    std::this_thread::sleep_for(std::chrono::milliseconds(testDelay(1500)));
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        if (writeBlockedList(restore, context)) return result(true);
        if (attempt < 2) delay(testDelay(1000));
    }
    if (logger_)
        logger_->error("sam-rescan: could not restore profile.blockedAppList; it clears at reboot");
    return result(false, "could not restore profile.blockedAppList; it clears at reboot");
}

bool SamRescanManager::needsForceParameter() const {
    const std::string root = systemRoot_.empty() ? "/" : systemRoot_;
    const std::string path = root[root.size() - 1] == '/'
        ? root + "etc/palm/schemas/sam/applicationManager.blockSystemApp.schema"
        : root + "/etc/palm/schemas/sam/applicationManager.blockSystemApp.schema";
    std::ifstream schema(path.c_str(), std::ios::in | std::ios::binary);
    if (!schema) return false;
    const std::string source((std::istreambuf_iterator<char>(schema)),
                             std::istreambuf_iterator<char>());
    return source.find("\"forceBlockSystemApp\"") != std::string::npos;
}

bool SamRescanManager::releaseApp(const std::string &appId, unsigned tries,
                                 const CommandPolicyContext &context, bool quiet) {
    for (unsigned attempt = 0; attempt < tries; ++attempt) {
        Json payload = Json::object();
        payload.set("id", Json::text(appId));
        Json response;
        if (call("sam-rescan.release-app",
                 "com.webos.applicationManager/releaseBlockSystemApp", payload,
                 "sam-app-rescan", context, response) && returnedTrue(response))
            return true;
        if (attempt + 1 < tries) delay(testDelay(1000));
    }
    if (!quiet && logger_)
        logger_->error("sam-rescan: could not release " + appId +
                       "; it remains blocked until released");
    return false;
}

Json SamRescanManager::rereadApp(const std::string &appId,
                                 const CommandPolicyContext &context) {
    if (appId != kHomeApp && appId != kScreensaverApp)
        return result(false, "unsupported SAM app id");
    std::unique_lock<std::mutex> operation(operationMutex_);
    Json payload = Json::object();
    payload.set("id", Json::text(appId));
    if (needsForceParameter()) payload.set("forceBlockSystemApp", Json::flag(false));
    Json response;
    const bool blocked = call("sam-rescan.block-app",
        "com.webos.applicationManager/blockSystemApp", payload,
        "sam-app-rescan", context, response) && returnedTrue(response);
    if (!blocked) {
        // A timed-out block can still have reached SAM, so make one quiet
        // release attempt before letting the caller choose another strategy.
        const bool released = releaseApp(appId, 1, context, true);
        if (logger_)
            logger_->info("sam-rescan: SAM did not take blockSystemApp for " + appId);
        return result(false, released ? std::string() :
                      "could not confirm SAM block or release");
    }
    if (!releaseApp(appId, 3, context, false))
        return result(false, "could not release " + appId + " from SAM");
    return result(true);
}

bool SamRescanManager::releaseBlockedApp(
    const std::string &appId, const CommandPolicyContext &context) {
    if (appId != kHomeApp && appId != kScreensaverApp) return false;
    std::lock_guard<std::mutex> operation(operationMutex_);
    return releaseApp(appId, 3, context, true);
}

bool SamRescanManager::reportsAppType(
    const std::string &appId, const std::string &type,
    const CommandPolicyContext &context, unsigned attempts) {
    if ((appId != kHomeApp && appId != kScreensaverApp) || type.empty())
        return false;
    std::lock_guard<std::mutex> operation(operationMutex_);
    for (unsigned attempt = 0; attempt < attempts; ++attempt) {
        Json payload = Json::object();
        payload.set("id", Json::text(appId));
        Json response;
        if (call("sam-rescan.check-app-type",
                 "com.webos.applicationManager/getAppInfo", payload,
                 "sam-app-rescan", context, response, nullptr,
                 std::chrono::milliseconds(1000)) &&
            returnedTrue(response) && response["appInfo"]["type"].string() == type)
            return true;
        if (attempt + 1 < attempts) delay(testDelay(1000));
    }
    return false;
}

Json SamRescanManager::refreshAppVisibility(
    const std::string &appId, bool visible,
    const CommandPolicyContext &context, unsigned attempts) {
    if (!service::appIdIsValid(appId) || attempts > 60)
        return result(false, "invalid app id for SAM visibility refresh");
    std::lock_guard<std::mutex> lifecycle(refreshMutex_);
    const Json nudged = nudge(context);
    if (nudged["result"].isBoolean() && nudged["result"].boolean()) {
        for (unsigned poll = 0; poll < attempts; ++poll) {
            Json payload = Json::object();
            payload.set("id", Json::text(appId));
            Json response;
            if (call("sam-rescan.check-app-visibility",
                     "com.webos.applicationManager/getAppInfo", payload,
                     "sam-app-rescan", context, response, nullptr,
                     std::chrono::milliseconds(2000)) &&
                returnedTrue(response) &&
                response["appInfo"]["visible"].isBoolean() &&
                response["appInfo"]["visible"].boolean() == visible)
                return result(true);
            if (poll + 1 < attempts) delay(testDelay(1000));
        }
        if (logger_)
            logger_->info("sam-rescan: visibility did not settle after the nudge for " +
                          appId);
    }
    return restart(context);
}

Json SamRescanManager::refresh(const Json &expectedVisibility,
                               const CommandPolicyContext &context) {
    std::lock_guard<std::mutex> lifecycle(refreshMutex_);
    if (expectedVisibility.valid() && !expectedVisibility.isObject())
        return result(false, "expected app visibility must be an object");
    const std::vector<std::string> ids = expectedVisibility.keys();
    if (ids.size() > 64)
        return result(false, "too many app visibility checks");

    const Json nudged = nudge(context);
    if (nudged["result"].isBoolean() && nudged["result"].boolean()) {
        bool caughtUp = true;
        for (unsigned poll = 0; poll < 16; ++poll) {
            caughtUp = true;
            for (const std::string &id : ids) {
                const Json expected = expectedVisibility[id];
                if (!expected.isBoolean())
                    return result(false, "expected app visibility must be boolean");
                Json payload = Json::object();
                payload.set("id", Json::text(id));
                Json response;
                if (!call("sam-rescan.check-app-visibility",
                          "com.webos.applicationManager/getAppInfo", payload,
                          "sam-app-rescan", context, response, nullptr,
                          std::chrono::milliseconds(500)) ||
                    !response["appInfo"].isObject()) {
                    caughtUp = false;
                    break;
                }
                const Json visible = response["appInfo"]["visible"];
                const bool current = !visible.isBoolean() || visible.boolean();
                if (current != expected.boolean()) {
                    caughtUp = false;
                    break;
                }
            }
            if (caughtUp) break;
            if (poll + 1 < 16)
                std::this_thread::sleep_for(std::chrono::milliseconds(testDelay(500)));
        }

        if (caughtUp) {
            std::string last;
            unsigned same = 0;
            for (unsigned poll = 0; poll < 24; ++poll) {
                Json points;
                if (call("sam-rescan.wait-for-launch-points",
                         "com.webos.applicationManager/listLaunchPoints", Json::object(),
                         "sam-app-rescan", context, points, nullptr,
                         std::chrono::milliseconds(500))) {
                    const std::string signature = launchPointSignature(points);
                    same = !signature.empty() && signature == last ? same + 1 : 0;
                    last = signature;
                    if (same >= 2) break;
                } else {
                    same = 0;
                    last.clear();
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(testDelay(500)));
            }
            return result(true);
        }
        if (logger_)
            logger_->info("sam-rescan: SAM had not picked up the app visibility after the nudge");
    }

    return restart(context);
}

Json SamRescanManager::restart(const CommandPolicyContext &context) {
    std::lock_guard<std::mutex> lifecycle(restartMutex_);
    std::string savedAppId;
    Json foreground;
    if (call("sam-rescan.foreground-before-restart",
             "com.webos.applicationManager/getForegroundAppInfo", Json::object(),
             "sam-app-rescan", context, foreground) && foregroundInfo(foreground) &&
        foreground["appId"].isString())
        savedAppId = foreground["appId"].string();

    static const std::string restartScript =
        "if command -v systemctl >/dev/null 2>&1; then "
        "for u in sam.service $(systemctl list-dependencies --reverse --plain "
        "--no-legend sam.service 2>/dev/null | sed -n "
        "'s/^[^A-Za-z0-9]*\\([A-Za-z0-9@._-]*\\.service\\)$/\\1/p'); do "
        "systemctl reset-failed \"$u\" >/dev/null 2>&1 || true; done; "
        "killall -9 LunaExecutable >/dev/null 2>&1 || true; "
        "systemctl kill -s 9 sam.service >/dev/null 2>&1 || "
        "systemctl restart --no-block sam >/dev/null 2>&1 || true; "
        "elif command -v initctl >/dev/null 2>&1; then "
        "initctl restart sam >/dev/null 2>&1 || "
        "pkill -9 -x sam >/dev/null 2>&1 || true; "
        "else pkill -9 -x sam >/dev/null 2>&1 || true; fi";
    static const std::string dependentsScript =
        "if command -v systemctl >/dev/null 2>&1; then "
        "for u in sam.service $(systemctl list-dependencies --reverse --plain "
        "--no-legend sam.service 2>/dev/null | sed -n "
        "'s/^[^A-Za-z0-9]*\\([A-Za-z0-9@._-]*\\.service\\)$/\\1/p'); do "
        "if systemctl is-failed --quiet \"$u\"; then echo \"$u\"; "
        "systemctl reset-failed \"$u\"; systemctl start --no-block \"$u\"; "
        "fi; done; "
        "elif command -v initctl >/dev/null 2>&1; then "
        "initctl restart eim >/dev/null 2>&1 && echo eim || true; fi";

    bool restartSucceeded = false;
    std::string restartError;
    CommandPolicySpec command;
    command.action = "sam-rescan.restart";
    command.uri = "native/sam/restart";
    command.timeout = std::chrono::milliseconds(60000);
    command.resource = "sam-service-restart";
    LunaReply localReply;
    const CommandDecision decision = policy_.executeLocal(
        command, context, false, [this, &context, &savedAppId,
                                  &restartSucceeded, &restartError]() {
            const ChildResult restarted = runShell(children_, systemRoot_, restartScript);
            restartSucceeded = restarted.succeeded();
            if (!restartSucceeded) {
                restartError = !restarted.error.empty() ? restarted.error
                    : !restarted.standardError.empty() ? restarted.standardError
                    : restarted.timedOut ? "SAM restart command timed out"
                    : "SAM restart command failed";
                if (logger_) logger_->error("sam-rescan: " + restartError);
            }

            std::string last;
            unsigned same = 0;
            for (unsigned poll = 0; poll < 24; ++poll) {
                Json points;
                Json payload = Json::object();
                if (call("sam-rescan.launch-points-after-restart",
                         "com.webos.applicationManager/listLaunchPoints", payload,
                         "sam-app-rescan", context, points, nullptr,
                         std::chrono::milliseconds(500))) {
                    const std::string signature = launchPointSignature(points);
                    same = !signature.empty() && signature == last ? same + 1 : 0;
                    last = signature;
                    if (same >= 2) break;
                } else {
                    same = 0;
                    last.clear();
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(testDelay(500)));
            }

            const ChildResult dependents = runShell(children_, systemRoot_, dependentsScript);
            if (!dependents.succeeded() && logger_)
                logger_->warning("sam-rescan: could not restart failed SAM dependents");

            if (!savedAppId.empty() && savedAppId != kHomeApp &&
                savedAppId != kScreensaverApp) {
                std::this_thread::sleep_for(std::chrono::milliseconds(testDelay(300)));
                for (unsigned attempt = 0; attempt < 10; ++attempt) {
                    Json current;
                    if (call("sam-rescan.foreground-after-restart",
                             "com.webos.applicationManager/getForegroundAppInfo",
                             Json::object(), "sam-app-rescan", context, current,
                             nullptr, std::chrono::milliseconds(500)) &&
                        foregroundInfo(current)) {
                        Json launch = Json::object();
                        launch.set("id", Json::text(savedAppId));
                        Json ignored;
                        (void)call("sam-rescan.restore-foreground-app",
                                   "com.webos.applicationManager/launch", launch,
                                   "sam-app-rescan", context, ignored);
                        break;
                    }
                    if (attempt + 1 < 10)
                        std::this_thread::sleep_for(std::chrono::milliseconds(testDelay(200)));
                }
            }
            return restartSucceeded;
        }, localReply);
    if (decision.kind != CommandDecisionKind::Allowed)
        return result(false, decision.reason);
    if (!localReply.response["returnValue"].isBoolean() ||
        !localReply.response["returnValue"].boolean())
        return result(false, restartError.empty()
            ? (localReply.error.empty() ? "SAM restart failed" : localReply.error)
            : restartError);
    return result(true);
}
} // namespace daemon
} // namespace nativetv
