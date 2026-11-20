#include "daemon/home_takeover_lifecycle.hpp"

#include "daemon/home_screen_manager.hpp"
#include "daemon/home_takeover_manager.hpp"
#include "daemon/home_takeover_stager.hpp"
#include "daemon/luna.hpp"
#include "daemon/sam_rescan_manager.hpp"
#include "service/preload.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <memory>
#include <regex>
#include <thread>

namespace nativetv {
namespace daemon {
namespace {
const char kHomeId[] = "com.webos.app.home";

std::string rooted(const std::string &root, const std::string &path) {
    if (root.empty() || root == "/") return path;
    size_t end = root.size();
    while (end > 1 && root[end - 1] == '/') --end;
    return root.substr(0, end) + path;
}

std::string readFile(const std::string &path) {
    std::ifstream input(path.c_str(), std::ios::in | std::ios::binary);
    if (!input) return {};
    return std::string((std::istreambuf_iterator<char>(input)),
                       std::istreambuf_iterator<char>());
}

// The major version /etc/issue names ("webOS TV 10.3.2"), or starfish-release
// where it does not; 0 where neither does.
int webosMajor(const std::string &systemRoot) {
    std::string issue = readFile(rooted(systemRoot, "/etc/issue"));
    if (issue.empty()) issue = readFile(rooted(systemRoot, "/etc/issue.net"));
    std::smatch match;
    static const std::regex issueVersion(
        "webOS(?: TV)?[[:space:]]+([0-9]+)", std::regex_constants::icase);
    if (!std::regex_search(issue, match, issueVersion)) {
        const std::string release = readFile(
            rooted(systemRoot, "/etc/starfish-release"));
        static const std::regex releaseVersion(
            "release[[:space:]]+([0-9]+)", std::regex_constants::icase);
        if (!std::regex_search(release, match, releaseVersion)) return 0;
    }
    return std::atoi(match[1].str().c_str());
}

bool blockBeforeNudge(int major) { return major == 10 || major == 11; }

Json failure(const std::string &message) {
    Json result = Json::object();
    result.set("ok", Json::flag(false));
    result.set("error", Json::text(message));
    return result;
}

bool operationSucceeded(const Json &operation) {
    return operation["ok"].isBoolean() && operation["ok"].boolean() &&
           operation["result"].isBoolean() && operation["result"].boolean();
}

unsigned manifestPollIntervalMs() {
#ifdef NATIVETV_TEST_FAST_SAM_TIMING
    return 5;
#else
    return 500;
#endif
}

unsigned manifestPollCount() {
#ifdef NATIVETV_TEST_FAST_SAM_TIMING
    return 3;
#else
    return 16;
#endif
}

bool lunaCall(LunaClient &luna, const std::string &uri, const Json &payload,
              LunaReply &reply) {
    struct Completion {
        std::mutex mutex;
        std::condition_variable condition;
        bool complete = false;
        LunaReply reply;
    };
    const std::shared_ptr<Completion> completion =
        std::make_shared<Completion>();
    if (!luna.call(uri, payload, [completion](const LunaReply &value) {
            std::lock_guard<std::mutex> lock(completion->mutex);
            completion->reply = value;
            completion->complete = true;
            completion->condition.notify_one();
        }))
        return false;
    std::unique_lock<std::mutex> lock(completion->mutex);
    if (!completion->condition.wait_for(lock, std::chrono::seconds(5),
                                        [completion] {
                                            return completion->complete;
                                        }))
        return false;
    reply = completion->reply;
    return !reply.timedOut && reply.error.empty() && reply.response.isObject() &&
           (!reply.response["returnValue"].isBoolean() ||
            reply.response["returnValue"].boolean());
}
} // namespace

HomeTakeoverLifecycleManager::HomeTakeoverLifecycleManager(
    HomeScreenManager &homeScreen, HomeTakeoverManager &takeover,
    SamRescanManager &samRescan, LunaClient &luna, std::string systemRoot)
    : homeScreen_(homeScreen), takeover_(takeover), samRescan_(samRescan),
      luna_(luna) {
    const int major = webosMajor(systemRoot);
    blockFirst_ = blockBeforeNudge(major);
    preloadHold_ = major >= 10;
}

bool HomeTakeoverLifecycleManager::holdLgHomePreload(bool hold) {
    if (!preloadHold_) return false;
    // A hold whose answer did not come back may still have been applied.
    if (hold) preloadMayBeHeld_ = true;
    LunaReply reply;
    const bool told =
        lunaCall(luna_, "com.webos.service.preloadmanager/setPreloadPolicy",
                 service::preloadPolicyRequest(kHomeId, !hold), reply);
    if (!hold && told) preloadMayBeHeld_ = false;
    return told;
}

bool HomeTakeoverLifecycleManager::appInfo(Json &info) {
    Json payload = Json::object();
    payload.set("id", Json::text(kHomeId));
    LunaReply reply;
    if (!lunaCall(luna_, "com.webos.applicationManager/getAppInfo",
                  payload, reply) ||
        !reply.response["appInfo"].isObject())
        return false;
    info = reply.response["appInfo"];
    return true;
}

// sam's reading of the app under LG's Home's id: Native Tv Home's manifest
// (native, started by its script), the web stand-in earlier releases staged in
// its place, or else LG's own.
namespace {
bool nativeTvHomeManifest(const Json &info) {
    return info["type"].isString() && info["type"].string() == "native" &&
           info["main"].isString() && info["main"].string() == kHomeStartScript;
}

bool ownManifest(const Json &info) {
    return nativeTvHomeManifest(info) ||
           (info["type"].isString() && info["type"].string() == "web");
}
} // namespace

bool HomeTakeoverLifecycleManager::manifestAccepted(bool nativeTvManifest) {
    Json info;
    if (!appInfo(info) || !info["type"].isString()) return false;
    if (!nativeTvManifest) return !ownManifest(info);
    const Json category = info["appCategory"];
    return nativeTvHomeManifest(info) && category.isArray() &&
           category.items().size() == 1 && category.items()[0].isString() &&
           category.items()[0].string() == "home";
}

std::string HomeTakeoverLifecycleManager::foregroundApp() {
    LunaReply reply;
    if (!lunaCall(luna_, "com.webos.applicationManager/getForegroundAppInfo",
                  Json::object(), reply))
        return std::string();
    return reply.response["appId"].string();
}

// Whatever ran under LG's Home's id before sam read the manifest that is there
// now, ended once sam has read it: a process sam no longer has the manifest of
// left a launch hanging for 8 s and the screen black (a G3, webOS 9.2.2). The
// web stand-in of earlier releases is ended through WebAppMgr; a native Native
// Tv Home is a process of its own.
void HomeTakeoverLifecycleManager::stopEarlierInstances() {
    Json payload = Json::object();
    payload.set("appId", Json::text(kHomeId));
    LunaReply ignored;
    (void)lunaCall(luna_, "com.palm.webappmanager/killApp", payload, ignored);
    bool stopped = false;
    (void)homeScreen_.stopNativeTvHome(stopped);
}

bool HomeTakeoverLifecycleManager::waitForManifest(bool nativeTvManifest) {
    const unsigned polls = manifestPollCount();
    for (unsigned poll = 0; poll < polls; ++poll) {
        if (manifestAccepted(nativeTvManifest)) return true;
        if (poll + 1 < polls)
            std::this_thread::sleep_for(
                std::chrono::milliseconds(manifestPollIntervalMs()));
    }
    return false;
}

bool HomeTakeoverLifecycleManager::reread(
    bool nativeTvManifest, const CommandPolicyContext &context) {
    const auto tryOperation = [this, nativeTvManifest, &context](bool block) {
        const Json operation = block
            ? samRescan_.rereadApp(kHomeId, context)
            : samRescan_.nudge(context);
        return operationSucceeded(operation) &&
               waitForManifest(nativeTvManifest);
    };
    if (blockFirst_)
        return tryOperation(true) || tryOperation(false);
    return tryOperation(false) || tryOperation(true);
}

void HomeTakeoverLifecycleManager::rollbackTakeover(
    const CommandPolicyContext &context) {
    (void)homeScreen_.startLayout(false);
    (void)takeover_.release();
    if (!manifestAccepted(false)) (void)reread(false, context);
    bool stopped = false;
    (void)homeScreen_.stopNativeTvHome(stopped);
}

Json HomeTakeoverLifecycleManager::takeover(
    bool blankStartLayout, const CommandPolicyContext &context) {
    std::lock_guard<std::mutex> lock(mutex_);
    const Json mounted = takeover_.takeover();
    if (!mounted["ok"].boolean()) return mounted;
    const bool reading = !manifestAccepted(true);
    if (reading && !reread(true, context)) {
        rollbackTakeover(context);
        return failure("sam did not take Native Tv Home as Home");
    }
    if (reading) {
        // sam had LG's manifest (its process is stopped below) or the web
        // stand-in of an earlier release.
        stopEarlierInstances();
    } else if (mounted["changed"].boolean() && foregroundApp() != kHomeId) {
        // A new release's client: one started from the files it replaced is
        // stopped, so the next Home press starts this one. One in front is left
        // to be replaced at its next start.
        bool stopped = false;
        (void)homeScreen_.stopNativeTvHome(stopped);
    }

    const Json layout = homeScreen_.startLayout(blankStartLayout);
    if (!layout["ok"].boolean() && blankStartLayout) {
        rollbackTakeover(context);
        return failure(layout["error"].isString()
            ? layout["error"].string()
            : "could not prepare LG's Home's starting layout");
    }

    bool stopped = false;
    std::string error;
    if (!homeScreen_.stopHome(stopped, &error)) {
        rollbackTakeover(context);
        return failure(error.empty() ? "could not stop LG's Home" : error);
    }
    // Best effort: a refusal is the caller's to see, where it sends it again.
    (void)holdLgHomePreload(true);
    return mounted;
}

Json HomeTakeoverLifecycleManager::release(
    const CommandPolicyContext &context) {
    std::lock_guard<std::mutex> lock(mutex_);
    const Json layout = homeScreen_.startLayout(false);
    if (!layout["ok"].boolean())
        return failure(layout["error"].isString()
            ? layout["error"].string()
            : "LG's Home layout could not be put back");

    const Json released = takeover_.release();
    if (!released["ok"].boolean()) return released;

    // sam keeps the manifest it read, so it reads it again unless it shows LG's
    // own: a getAppInfo that timed out or failed says nothing, and a sam left
    // with Native Tv Home's manifest after the unmount starts a script that is
    // gone, so the Home key does nothing until a reboot. A failed reread is
    // the caller's to see; a release sent again finds the unmount done and
    // rereads.
    if (!manifestAccepted(false) && !reread(false, context))
        return failure("sam did not take LG's Home back");

    stopEarlierInstances();
    // Also after an earlier release that unmounted but stopped at the reread
    // and so left the hold in place.
    if (preloadMayBeHeld_) (void)holdLgHomePreload(false);
    return released;
}
} // namespace daemon
} // namespace nativetv
