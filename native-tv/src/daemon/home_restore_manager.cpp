#include "daemon/home_restore_manager.hpp"

#include "daemon/home_takeover_lifecycle.hpp"
#include "daemon/json_file.hpp"

#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <memory>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace nativetv {
namespace daemon {
namespace {
const char kLegacyHomeApp[] = "com.tvweb.home";
const char kLiveTvApp[] = "com.webos.app.livetv";

std::string utcTimestamp() {
    struct timespec now = {};
    if (clock_gettime(CLOCK_REALTIME, &now) != 0) return std::string();
    struct tm utc = {};
    if (!gmtime_r(&now.tv_sec, &utc)) return std::string();
    char seconds[24] = {};
    if (strftime(seconds, sizeof(seconds), "%Y-%m-%dT%H:%M:%S", &utc) == 0)
        return std::string();
    std::ostringstream rendered;
    rendered << seconds << '.' << std::setfill('0') << std::setw(3)
             << now.tv_nsec / 1000000L << 'Z';
    return rendered.str();
}

Json failure(const std::string &message) {
    Json result = Json::object();
    result.set("ok", Json::flag(false));
    result.set("error", Json::text(message));
    return result;
}

bool succeeded(const Json &result) {
    return result["ok"].isBoolean() && result["ok"].boolean();
}

bool fileExists(const std::string &path) {
    struct stat info;
    return stat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode);
}

bool removeStaleRecoveryCopy(const std::string &path, std::string &error) {
    const std::string backup = path + ".last-good";
    if (unlink(backup.c_str()) == 0 || errno == ENOENT) return true;
    error = "could not clear shared-state recovery copy " + backup + ": " +
            std::strerror(errno);
    return false;
}

bool legacyAppInSettings(const Json &settings) {
    const Json defaults = settings["defaultApps"];
    if (defaults.isObject() &&
        (defaults["home"].string() == kLegacyHomeApp ||
         defaults["idleApp"].string() == kLegacyHomeApp))
        return true;
    return settings["lastInputApp"].string() == kLegacyHomeApp ||
           settings["physicalLastInputApp"].string() == kLegacyHomeApp;
}

bool legacySettingValue(const Json &value) {
    return value.isString() && value.string() == kLegacyHomeApp;
}

bool savedAppValue(const Json &value) {
    return value.isString() && !value.string().empty() &&
           value.string() != kLegacyHomeApp;
}

Json mapForConfigd(const Json &state, bool clearLegacyHome) {
    Json value = Json::object();
    value.set("keys", state["keys"].isObject()
        ? state["keys"] : Json::object());
    const Json home = state["home"];
    if (home.isString() && !home.string().empty() &&
        !(clearLegacyHome && home.string() == kLegacyHomeApp))
        value.set("home", home);
    return value;
}

Json cleanedRemoteState(const Json &state, bool clearLegacyHome) {
    Json cleaned = Json::object();
    for (const std::string &key : state.keys()) {
        if (key == "starting" || key == "splash" ||
            (clearLegacyHome && key == "home"))
            continue;
        cleaned.set(key, state[key]);
    }
    if (!cleaned["keys"].isObject())
        cleaned.set("keys", Json::object());
    return cleaned;
}
} // namespace

HomeRestoreManager::HomeRestoreManager(
    HomeTakeoverLifecycleManager &takeover, CommandPolicyExecutor &policy,
    std::string dataRoot)
    : takeover_(takeover), policy_(policy), dataRoot_(std::move(dataRoot)) {}

bool HomeRestoreManager::lunaCall(const CommandPolicyContext &context,
                                  const std::string &action,
                                  const std::string &uri, const Json &payload,
                                  const std::string &resource, Json &response,
                                  std::string &error) {
    struct Completion {
        std::mutex mutex;
        std::condition_variable condition;
        bool done = false;
        LunaReply reply;
    };
    const std::shared_ptr<Completion> completion =
        std::make_shared<Completion>();
    CommandPolicySpec command;
    command.action = action;
    command.uri = uri;
    command.payload = payload;
    command.timeout = std::chrono::milliseconds(5000);
    command.requiresControl = false;
    command.resource = resource;
    const CommandDecision decision = policy_.submit(
        command, context, false, [completion](const LunaReply &reply) {
            std::lock_guard<std::mutex> lock(completion->mutex);
            completion->reply = reply;
            completion->done = true;
            completion->condition.notify_one();
        });
    if (decision.kind != CommandDecisionKind::Queued) {
        error = decision.reason;
        return false;
    }
    std::unique_lock<std::mutex> lock(completion->mutex);
    if (!completion->condition.wait_for(
            lock, std::chrono::seconds(6),
            [completion]() { return completion->done; })) {
        error = "command policy completion timed out";
        return false;
    }
    const LunaReply &reply = completion->reply;
    if (reply.timedOut || !reply.error.empty() || !reply.response.isObject() ||
        (reply.response["returnValue"].isBoolean() &&
         !reply.response["returnValue"].boolean())) {
        error = reply.error.empty() ? "the TV did not accept the Home restore" : reply.error;
        return false;
    }
    response = reply.response;
    return true;
}

bool HomeRestoreManager::readSettings(const CommandPolicyContext &context,
                                      Json &settings, std::string &error) {
    Json payload = Json::object();
    payload.set("category", Json::text("general"));
    Json keys = Json::array();
    keys.add(Json::text("defaultApps"));
    keys.add(Json::text("lastInputApp"));
    keys.add(Json::text("physicalLastInputApp"));
    payload.set("keys", keys);
    Json response;
    if (!lunaCall(context, "home.restore.read-legacy-settings",
                  "com.webos.service.settings/getSystemSettings", payload,
                  "home-settings", response, error) ||
        !response["settings"].isObject()) {
        if (error.empty()) error = "could not read the saved Home settings";
        return false;
    }
    settings = response["settings"];
    return true;
}

bool HomeRestoreManager::writeSettings(const CommandPolicyContext &context,
                                       const Json &settings,
                                       std::string &error) {
    if (!settings.isObject() || settings.keys().empty()) return true;
    Json payload = Json::object();
    payload.set("category", Json::text("general"));
    payload.set("settings", settings);
    Json response;
    return lunaCall(context, "home.restore.write-settings",
                    "com.webos.service.settings/setSystemSettings", payload,
                    "home-settings", response, error);
}

bool HomeRestoreManager::restoreStartingNotice(
    const CommandPolicyContext &context, std::string &error) {
    const std::string path = dataRoot_ + "/remotekeys.json";
    struct stat current = {};
    if (stat(path.c_str(), &current) != 0 && errno == ENOENT &&
        !removeStaleRecoveryCopy(path, error))
        return false;
    JsonFile store(path, 64 * 1024);
    const JsonFileResult existing = store.load();
    if (!existing.error.empty()) {
        error = existing.error;
        return false;
    }
    Json remoteKeys = existing.found && existing.value.isObject()
        ? existing.value : Json::object();
    const bool filterExists = fileExists(dataRoot_ + "/remotekeys-filter.js");
    const bool clearLegacyHome = remoteKeys["home"].string() == kLegacyHomeApp;

    if (existing.found) {
        Json cleaned = cleanedRemoteState(remoteKeys, clearLegacyHome);
        const std::string updated = utcTimestamp();
        if (!updated.empty()) cleaned.set("updated", Json::text(updated));
        if (!store.save(cleaned, &error)) return false;
        if (!removeStaleRecoveryCopy(path, error)) return false;
        remoteKeys = cleaned;
    } else if (!filterExists) {
        return true;
    }

    Json configs = Json::object();
    configs.set("nativetv.remoteKeys", mapForConfigd(remoteKeys,
                                                        clearLegacyHome));
    Json payload = Json::object();
    payload.set("configs", configs);
    Json response;
    return lunaCall(context, "home.restore.clear-starting-notice",
                    "com.webos.service.config/setConfigs", payload,
                    "remote-keys", response, error);
}

bool HomeRestoreManager::restoreLegacyAppSettings(
    const CommandPolicyContext &context, const Json &current,
    const Json &snapshot, std::string &error) {
    Json restored = Json::object();
    const Json currentDefaults = current["defaultApps"];
    if (currentDefaults.isObject() &&
        (legacySettingValue(currentDefaults["home"]) ||
         legacySettingValue(currentDefaults["idleApp"]))) {
        const Json savedDefaults = snapshot["defaultApps"];
        Json defaults = Json::object();
        for (const std::string &key : currentDefaults.keys()) {
            const Json value = currentDefaults[key];
            if ((key == "home" || key == "idleApp") &&
                legacySettingValue(value)) {
                const Json saved = savedDefaults[key];
                if (savedAppValue(saved)) defaults.set(key, saved);
                continue;
            }
            defaults.set(key, value);
        }
        restored.set("defaultApps", defaults);
    }

    const char *inputKeys[] = {"lastInputApp", "physicalLastInputApp"};
    for (const char *key : inputKeys) {
        if (!legacySettingValue(current[key])) continue;
        const Json saved = snapshot[key];
        restored.set(key, savedAppValue(saved) ? saved : Json::text(kLiveTvApp));
    }
    return writeSettings(context, restored, error);
}

Json HomeRestoreManager::restore(const CommandPolicyContext &context) {
    Json legacySettings;
    std::string error;
    if (!readSettings(context, legacySettings, error))
        return failure(error);
    JsonFile homeStore(dataRoot_ + "/home.json", 8 * 1024 * 1024);
    JsonFileResult loaded = homeStore.load();
    if (!loaded.error.empty()) return failure(loaded.error);
    Json home = loaded.found && loaded.value.isObject()
        ? loaded.value : Json::object();

    const Json snapshot = home["snapshot"];
    const Json savedSettings = snapshot["settings"];
    const Json screensaver = savedSettings["screenSaverEnabled"];

    home.set("enabled", Json::flag(false));
    home.set("restorePending", Json::flag(true));
    if (!homeStore.save(home, &error)) return failure(error);

    const Json released = takeover_.release(context);
    if (!succeeded(released))
        return failure(released["error"].string("could not restore LG Home"));

    if (!restoreStartingNotice(context, error))
        return failure(error.empty() ? "could not clear the Home startup notice" : error);

    const Json lgAutoLaunch = home["lgAutoLaunch"];
    if (lgAutoLaunch.isString()) {
        Json settings = Json::object();
        settings.set("homeAutoLaunch", lgAutoLaunch);
        const Json check = home["lgAutoLaunchCheck"];
        if (check.isString()) settings.set("checkHomeAutoLaunchValue", check);
        if (!writeSettings(context, settings, error))
            return failure(error.empty() ? "could not restore LG Home auto launch" : error);
        home.set("lgAutoLaunch", Json());
        home.set("lgAutoLaunchCheck", Json());
        if (!homeStore.save(home, &error)) return failure(error);
    }

    if (savedSettings.has("screenSaverEnabled") && screensaver.valid() &&
        screensaver.dump() != "null") {
        Json settings = Json::object();
        settings.set("screenSaverEnabled", screensaver);
        if (!writeSettings(context, settings, error))
            return failure(error.empty() ? "could not restore the screen saver setting" : error);
    }

    if (legacyAppInSettings(legacySettings) &&
        !restoreLegacyAppSettings(context, legacySettings, savedSettings, error))
        return failure(error.empty()
            ? "could not restore settings from the earlier Home app" : error);

    home.set("snapshot", Json());
    home.set("restorePending", Json::flag(false));
    if (!homeStore.save(home, &error)) return failure(error);

    Json result = Json::object();
    result.set("ok", Json::flag(true));
    result.set("restored", Json::flag(true));
    return result;
}
} // namespace daemon
} // namespace nativetv
