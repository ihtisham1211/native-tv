#include "daemon/home_boot_manager.hpp"

#include "daemon/json_file.hpp"
#include "daemon/logger.hpp"
#include "daemon/luna.hpp"

#include <chrono>
#include <condition_variable>
#include <fstream>
#include <memory>
#include <mutex>
#include <regex>
#include <utility>

namespace nativetv {
namespace daemon {
namespace {
const char kHomeId[] = "com.webos.app.home";
const char kPowerReasonsConfig[] =
    "com.webos.service.tvpower.fullHomeBootingPowerOnReasons";

std::string rooted(const std::string &root, const std::string &path) {
    if (root.empty() || root == "/") return path;
    size_t end = root.size();
    while (end > 1 && root[end - 1] == '/') --end;
    return root.substr(0, end) + path;
}

struct CallCompletion {
    std::mutex mutex;
    std::condition_variable condition;
    bool complete = false;
    LunaReply reply;
};

bool call(LunaClient &luna, const std::string &uri, const Json &payload,
          LunaReply &reply, std::chrono::milliseconds timeout,
          bool allowLongWait = false,
          const std::string &applicationId = std::string()) {
    const std::shared_ptr<CallCompletion> completion =
        std::make_shared<CallCompletion>();
    if (!luna.call(uri, payload, [completion](const LunaReply &value) {
            std::lock_guard<std::mutex> lock(completion->mutex);
            completion->reply = value;
            completion->complete = true;
            completion->condition.notify_one();
        }, applicationId, false, timeout, allowLongWait))
        return false;
    std::unique_lock<std::mutex> lock(completion->mutex);
    if (!completion->condition.wait_for(lock, timeout + std::chrono::seconds(1),
                                        [completion] {
                                            return completion->complete;
                                        }))
        return false;
    reply = completion->reply;
    return !reply.timedOut && reply.error.empty() && reply.response.isObject() &&
           (!reply.response["returnValue"].isBoolean() ||
            reply.response["returnValue"].boolean());
}

std::string bootInput(const std::string &path) {
    std::ifstream input(path.c_str());
    if (!input) return {};
    static const std::regex foreground(
        "BootSequencer\\) '([^']*)' is foreground");
    std::string line;
    std::smatch match;
    while (std::getline(input, line)) {
        if (std::regex_search(line, match, foreground) &&
            match[1].str() != "com.tvweb.splash")
            return match[1].str();
    }
    return {};
}

bool supportedInput(const std::string &appId) {
    static const std::regex input(
        "^com\\.webos\\.app\\.(livetv|hdmi[0-9]|av[0-9]|comp[0-9])$");
    return std::regex_match(appId, input);
}

Json powerOnReasons(LunaClient &luna) {
    Json payload = Json::object();
    Json names = Json::array();
    names.add(Json::text(kPowerReasonsConfig));
    payload.set("configNames", names);
    LunaReply reply;
    if (call(luna, "com.webos.service.config/getConfigs", payload, reply,
             std::chrono::seconds(5))) {
        Json reasons = reply.response["configs"][kPowerReasonsConfig];
        if (!reasons.valid()) reasons = reply.response[kPowerReasonsConfig];
        if (reasons.isArray()) return reasons;
    }
    Json defaults = Json::array();
    defaults.add(Json::text("aiKey"));
    defaults.add(Json::text("lastPowerOn"));
    defaults.add(Json::text("localKey"));
    defaults.add(Json::text("remoteKey"));
    defaults.add(Json::text("seniorHelp"));
    return defaults;
}

bool includes(const Json &values, const std::string &value) {
    for (const Json &entry : values.items())
        if (entry.isString() && entry.string() == value)
            return true;
    return false;
}

Json result(bool opened, const std::string &reason = std::string()) {
    Json response = Json::object();
    response.set("ok", Json::flag(true));
    response.set("opened", Json::flag(opened));
    if (!reason.empty()) response.set("reason", Json::text(reason));
    return response;
}

Json noticeResult(bool shown, const std::string &reason = std::string()) {
    Json response = Json::object();
    response.set("ok", Json::flag(true));
    response.set("shown", Json::flag(shown));
    if (!reason.empty()) response.set("reason", Json::text(reason));
    return response;
}
} // namespace

HomeBootManager::HomeBootManager(LunaClient &luna, std::string dataRoot,
                                 std::string systemRoot, Logger *logger)
    : luna_(luna), dataRoot_(std::move(dataRoot)),
      systemRoot_(std::move(systemRoot)), logger_(logger) {}

Json HomeBootManager::openAtBoot() {
    const auto skipped = [this](const std::string &message) {
        if (logger_) logger_->info("Home boot open skipped: " + message);
        return result(false, message);
    };
    const JsonFileResult home = JsonFile(dataRoot_ + "/home.json", 256 * 1024).load();
    if (!home.found || !home.value.isObject())
        return skipped("Home settings are unavailable");
    if (!home.value["openOnPowerOn"].boolean() &&
        home.value["lgAutoLaunch"].string() != "on")
        return skipped("Home auto-open is disabled");

    Json powerPayload = Json::object();
    LunaReply powerReply;
    if (!call(luna_, "com.webos.service.tvpower/power/getPowerOnReason",
              powerPayload, powerReply, std::chrono::seconds(5)))
        return skipped("Could not read the power-on reason");
    const std::string reason = powerReply.response["reason"].string();
    if (!includes(powerOnReasons(luna_), reason)) {
        const std::string message = "power-on reason " +
            (reason.empty() ? std::string("unknown") : reason) + " is not enabled";
        return skipped(message);
    }

    const std::string bootApp = bootInput(rooted(systemRoot_, "/var/log/bootd.log"));
    if (!supportedInput(bootApp)) {
        const std::string message = "TV started on " +
            (bootApp.empty() ? std::string("nothing logged") : bootApp);
        return skipped(message);
    }

    LunaReply foregroundReply;
    if (!call(luna_, "com.webos.applicationManager/getForegroundAppInfo",
              Json::object(), foregroundReply, std::chrono::seconds(5)))
        return skipped("Could not read the foreground app");
    const std::string foreground = foregroundReply.response["appId"].string();
    if (foreground != bootApp && foreground != "com.tvweb.splash") {
        const std::string message = (foreground.empty() ? std::string("nothing") : foreground) +
            " is in front since the start on " + bootApp;
        return skipped(message);
    }

    Json launch = Json::object();
    launch.set("id", Json::text(kHomeId));
    LunaReply launchReply;
    if (!call(luna_, "com.webos.applicationManager/launch", launch, launchReply,
              std::chrono::seconds(30), true)) {
        if (logger_) logger_->warning("Home boot launch did not receive a successful reply");
        return result(false, "Home launch failed");
    }
    if (logger_)
        logger_->info("Home boot opened over " + bootApp + " (in front: " + foreground + ")");
    return result(true);
}

Json HomeBootManager::showStartingNotice() {
    const JsonFileResult remoteKeys =
        JsonFile(dataRoot_ + "/remotekeys.json", 1024 * 1024).load();
    if (!remoteKeys.found || !remoteKeys.value.isObject() ||
        !remoteKeys.value["starting"].isString() ||
        remoteKeys.value["starting"].string().empty()) {
        if (!remoteKeys.found && logger_)
            logger_->debug("Home startup notice skipped: no remote-key notice is saved");
        return noticeResult(false, "No Home startup notice is saved");
    }

    Json payload = Json::object();
    payload.set("sourceId", Json::text(kHomeId));
    payload.set("message", Json::text(remoteKeys.value["starting"].string()));
    LunaReply reply;
    if (!call(luna_, "com.webos.notification/createToast", payload, reply,
              std::chrono::seconds(5), false, kHomeId)) {
        if (logger_)
            logger_->warning("Home startup notice was not accepted by the notification service");
        Json response = noticeResult(false, "Could not show the Home startup notice");
        response.set("ok", Json::flag(false));
        return response;
    }
    if (logger_) logger_->debug("Home startup notice shown");
    return noticeResult(true);
}
} // namespace daemon
} // namespace nativetv
