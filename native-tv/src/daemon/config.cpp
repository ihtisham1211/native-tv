#include "daemon/config.hpp"
#include "daemon/json_file.hpp"
#include "daemon/paths.hpp"
#include <cerrno>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace nativetv {
namespace daemon {
ConfigLoadOptions::ConfigLoadOptions() : fallbackPath("./config.json") {
    primaryPath = dataDirectory();
    if (!primaryPath.empty() && primaryPath[primaryPath.size() - 1] != '/')
        primaryPath += '/';
    primaryPath += "config.json";
}

namespace {
const size_t kMaximumConfigBytes = 8 * 1024 * 1024;

void applyDocument(const Json &document, ServerConfig &config, std::vector<std::string> &warnings,
                   const std::string &path) {
    if (!document.isObject())
        return;
    config.document = document;

    Json value = document["port"];
    if (value.valid()) {
        if (value.isNumber() && std::isfinite(value.number()) &&
            std::floor(value.number()) == value.number() && value.number() >= 1 &&
            value.number() <= 65535) {
            config.port = static_cast<int>(value.number());
        } else {
            warnings.push_back("config: ignored invalid port in " + path);
        }
    }

    value = document["host"];
    if (value.valid()) {
        if (value.isString())
            config.host = value.string();
        else
            warnings.push_back("config: ignored invalid host in " + path);
    }

    value = document["token"];
    if (value.valid()) {
        if (value.isString())
            config.token = value.string();
        else
            warnings.push_back("config: ignored invalid token in " + path);
    }

    value = document["allowControl"];
    if (value.valid()) {
        if (value.isBoolean())
            config.allowControl = value.boolean();
        else
            warnings.push_back("config: ignored invalid allowControl in " + path);
    }

    value = document["allowPower"];
    if (value.valid()) {
        if (value.isBoolean())
            config.allowPower = value.boolean();
        else
            warnings.push_back("config: ignored invalid allowPower in " + path);
    }

    Json apps = document["apps"];
    if (apps.valid() && !apps.isObject()) {
        warnings.push_back("config: ignored invalid apps in " + path);
    } else if (apps.isObject()) {
        value = apps["sideload"];
        if (value.valid()) {
            if (value.isBoolean())
                config.sideload = value.boolean();
            else
                warnings.push_back("config: ignored invalid apps.sideload in " + path);
        }
    }
}

bool readFile(const std::string &path, std::string &contents, std::string &error) {
    std::ifstream input(path.c_str(), std::ios::in | std::ios::binary);
    if (!input) {
        error = "could not read file";
        return false;
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    if (input.bad()) {
        error = "read failed";
        return false;
    }
    contents = buffer.str();
    if (contents.size() > kMaximumConfigBytes) {
        error = "file exceeds the 8 MiB limit";
        return false;
    }
    return true;
}

bool tightenPermissions(const std::string &path, std::string &error) {
    struct stat info;
    if (stat(path.c_str(), &info) != 0) {
        error = "could not inspect permissions";
        return false;
    }
    if ((info.st_mode & 0777) == 0600)
        return true;
    if (chmod(path.c_str(), 0600) != 0) {
        error = "could not set permissions to 0600";
        return false;
    }
    return true;
}

class ConfigFileLock {
  public:
    explicit ConfigFileLock(const std::string &configPath) : path_(configPath + ".lock") {
        acquire();
    }

    ~ConfigFileLock() { release(); }

    bool acquired() const { return acquired_; }
    const std::string &error() const { return error_; }

  private:
    void acquire() {
        const int attempts = 250;
        for (int attempt = 0; attempt < attempts; ++attempt) {
            if (::mkdir(path_.c_str(), 0700) == 0) {
                token_ = std::to_string(static_cast<long>(::getpid())) + ":" +
                         std::to_string(static_cast<long long>(std::time(nullptr)));
                const std::string ownerPath = path_ + "/owner";
                const int fd = ::open(ownerPath.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
                if (fd < 0) {
                    error_ = std::string("could not write config lock owner: ") +
                             std::strerror(errno);
                    ::rmdir(path_.c_str());
                    return;
                }
                const ssize_t written = ::write(fd, token_.data(), token_.size());
                const int savedErrno = errno;
                ::close(fd);
                if (written != static_cast<ssize_t>(token_.size())) {
                    error_ = std::string("could not write config lock owner: ") +
                             std::strerror(written < 0 ? savedErrno : EIO);
                    ::unlink(ownerPath.c_str());
                    ::rmdir(path_.c_str());
                    return;
                }
                acquired_ = true;
                return;
            }
            if (errno != EEXIST) {
                error_ = std::string("could not create config lock: ") + std::strerror(errno);
                return;
            }
            reapStale();
            ::usleep(20000);
        }
        error_ = "timed out waiting for config file lock";
    }

    void reapStale() const {
        const std::string ownerPath = path_ + "/owner";
        char contents[128] = {};
        const int fd = ::open(ownerPath.c_str(), O_RDONLY);
        bool stale = false;
        if (fd >= 0) {
            const ssize_t count = ::read(fd, contents, sizeof(contents) - 1);
            ::close(fd);
            if (count > 0) {
                char *end = nullptr;
                errno = 0;
                const long ownerPid = std::strtol(contents, &end, 10);
                if (errno == 0 && ownerPid > 0) {
                    if (::kill(static_cast<pid_t>(ownerPid), 0) == 0 || errno == EPERM)
                        return;
                    stale = errno == ESRCH;
                }
            }
        }
        if (!stale) {
            struct stat info;
            if (::stat(path_.c_str(), &info) != 0 || std::time(nullptr) - info.st_mtime < 2)
                return;
            stale = true;
        }
        if (!stale)
            return;
        const std::string retired = path_ + ".stale." +
                                    std::to_string(static_cast<long>(::getpid())) + "." +
                                    std::to_string(static_cast<long long>(std::time(nullptr)));
        if (::rename(path_.c_str(), retired.c_str()) == 0) {
            ::unlink((retired + "/owner").c_str());
            ::rmdir(retired.c_str());
        }
    }

    void release() {
        if (!acquired_)
            return;
        const std::string ownerPath = path_ + "/owner";
        char contents[128] = {};
        const int fd = ::open(ownerPath.c_str(), O_RDONLY);
        if (fd >= 0) {
            const ssize_t count = ::read(fd, contents, sizeof(contents) - 1);
            ::close(fd);
            if (count >= 0 && token_ == contents) {
                ::unlink(ownerPath.c_str());
                ::rmdir(path_.c_str());
            }
        }
        acquired_ = false;
    }

    std::string path_;
    std::string token_;
    std::string error_;
    bool acquired_ = false;
};
} // namespace

ConfigLoadResult ConfigLoader::load(const ConfigLoadOptions &options) {
    ConfigLoadResult result;
    result.path = options.overridePath.empty() ? options.primaryPath : options.overridePath;

    std::vector<std::string> paths;
    if (!options.overridePath.empty()) {
        paths.push_back(options.overridePath);
    } else {
        paths.push_back(options.primaryPath);
        if (options.fallbackPath != options.primaryPath)
            paths.push_back(options.fallbackPath);
    }

    for (const std::string &path : paths) {
        struct stat info;
        if (stat(path.c_str(), &info) != 0) {
            if (errno != ENOENT)
                result.warnings.push_back("config: could not inspect " + path);
            continue;
        }

        std::string contents;
        std::string error;
        if (!readFile(path, contents, error)) {
            result.warnings.push_back("config: " + error + " at " + path);
            continue;
        }

        Json document = Json::parse(contents);
        if (!document.valid()) {
            result.warnings.push_back("config: invalid JSON in " + path);
            continue;
        }

        result.path = path;
        result.loaded = true;
        applyDocument(document, result.config, result.warnings, path);
        if (!tightenPermissions(path, error))
            result.warnings.push_back("config: " + error + " at " + path);
        break;
    }
    return result;
}

ConfigStore::ConfigStore(std::string path) : path_(std::move(path)) {}

bool ConfigStore::setNetworkOpen(bool open, std::string *error) const {
    if (error)
        error->clear();
    ConfigFileLock lock(path_);
    if (!lock.acquired()) {
        if (error)
            *error = lock.error();
        return false;
    }
    JsonFile store(path_);
    JsonFileResult loaded = store.load();
    if (!loaded.error.empty()) {
        if (error)
            *error = loaded.error;
        return false;
    }

    Json document = loaded.found ? loaded.value : Json::object();
    if (!document.isObject()) {
        if (error)
            *error = "config document must be a JSON object";
        return false;
    }
    // An omitted host opens the dashboard on all interfaces. Use the same
    // dual-stack address explicitly so this write remains clear in the file.
    document.set("host", Json::text(open ? "::" : "127.0.0.1"));
    return store.save(document, error);
}

bool ConfigStore::setMqttSetup(const Json &mqtt, const Json &device, std::string *error) const {
    if (error)
        error->clear();
    if (!mqtt.isObject() || !device.isObject()) {
        if (error)
            *error = "MQTT setup patch must contain MQTT and device objects";
        return false;
    }
    ConfigFileLock lock(path_);
    if (!lock.acquired()) {
        if (error)
            *error = lock.error();
        return false;
    }
    JsonFile store(path_);
    JsonFileResult loaded = store.load();
    if (!loaded.error.empty()) {
        if (error)
            *error = loaded.error;
        return false;
    }
    Json document = loaded.found ? loaded.value : Json::object();
    if (!document.isObject()) {
        if (error)
            *error = "config document must be a JSON object";
        return false;
    }

    Json mqttDocument = document["mqtt"];
    if (!mqttDocument.isObject())
        mqttDocument = Json::object();
    const char *mqttKeys[] = {"enabled", "host", "port", "tls", "tlsRejectUnauthorized",
                              "username", "password", "topicPrefix", "discoveryPrefix",
                              "telemetryIntervalMs", "entities"};
    for (size_t i = 0; i < sizeof(mqttKeys) / sizeof(mqttKeys[0]); ++i) {
        if (mqtt.has(mqttKeys[i]))
            mqttDocument.set(mqttKeys[i], mqtt[mqttKeys[i]]);
    }

    Json deviceDocument = document["device"];
    if (!deviceDocument.isObject())
        deviceDocument = Json::object();
    if (device.has("id"))
        deviceDocument.set("id", device["id"]);
    if (device.has("name"))
        deviceDocument.set("name", device["name"]);

    document.set("mqtt", mqttDocument);
    document.set("device", deviceDocument);
    return store.save(document, error);
}

bool ConfigStore::writeDashboardSettings(const Json &patch, std::string *error) const {
    if (error)
        error->clear();
    if (!patch.isObject() || !patch["mqtt"].isObject() ||
        !patch["device"].isObject() ||
        (patch.has("iconServer") && !patch["iconServer"].isBoolean())) {
        if (error)
            *error = "dashboard settings patch is invalid";
        return false;
    }
    ConfigFileLock lock(path_);
    if (!lock.acquired()) {
        if (error)
            *error = lock.error();
        return false;
    }
    JsonFile store(path_);
    JsonFileResult loaded = store.load();
    if (!loaded.error.empty()) {
        if (error)
            *error = loaded.error;
        return false;
    }
    Json document = loaded.found ? loaded.value : Json::object();
    if (!document.isObject()) {
        if (error)
            *error = "config document must be a JSON object";
        return false;
    }
    const char *const sections[] = {"mqtt", "device"};
    for (const char *section : sections) {
        Json existing = document[section];
        if (!existing.isObject()) existing = Json::object();
        const Json changes = patch[section];
        for (const std::string &key : changes.keys())
            existing.set(key, changes[key]);
        document.set(section, existing);
    }
    if (patch.has("iconServer"))
        document.set("iconServer", patch["iconServer"]);
    return store.save(document, error);
}

bool ConfigStore::setNetworkDebuggerAllowed(bool allowed, std::string *error) const {
    if (error)
        error->clear();
    ConfigFileLock lock(path_);
    if (!lock.acquired()) {
        if (error)
            *error = lock.error();
        return false;
    }
    JsonFile store(path_);
    JsonFileResult loaded = store.load();
    if (!loaded.error.empty()) {
        if (error)
            *error = loaded.error;
        return false;
    }
    Json document = loaded.found ? loaded.value : Json::object();
    if (!document.isObject()) {
        if (error)
            *error = "config document must be a JSON object";
        return false;
    }
    document.set("allowNetworkDebugger", Json::flag(allowed));
    return store.save(document, error);
}

bool ConfigStore::setTopLevel(const Json &changes, std::string *error) const {
    if (error)
        error->clear();
    if (!changes.isObject()) {
        if (error)
            *error = "config changes must be an object";
        return false;
    }
    ConfigFileLock lock(path_);
    if (!lock.acquired()) {
        if (error)
            *error = lock.error();
        return false;
    }
    JsonFile store(path_);
    JsonFileResult loaded = store.load();
    if (!loaded.error.empty()) {
        if (error)
            *error = loaded.error;
        return false;
    }
    Json document = loaded.found ? loaded.value : Json::object();
    if (!document.isObject()) {
        if (error)
            *error = "config document must be a JSON object";
        return false;
    }
    Json rebuilt = Json::object();
    for (const std::string &key : document.keys()) {
        if (!changes.has(key))
            rebuilt.set(key, document[key]);
    }
    for (const std::string &key : changes.keys()) {
        // A null change removes the key.
        if (changes[key].valid())
            rebuilt.set(key, changes[key]);
    }
    return store.save(rebuilt, error);
}

bool ConfigStore::patchObjectSection(const std::string &section, const Json &patch,
                                     Json *updatedSection, std::string *error) const {
    if (error)
        error->clear();
    if (section.empty() || !patch.isObject()) {
        if (error)
            *error = "config section patch must be an object";
        return false;
    }
    ConfigFileLock lock(path_);
    if (!lock.acquired()) {
        if (error)
            *error = lock.error();
        return false;
    }
    JsonFile store(path_);
    JsonFileResult loaded = store.load();
    if (!loaded.error.empty()) {
        if (error)
            *error = loaded.error;
        return false;
    }
    Json document = loaded.found ? loaded.value : Json::object();
    if (!document.isObject()) {
        if (error)
            *error = "config document must be a JSON object";
        return false;
    }
    Json sectionDocument = document[section];
    if (!sectionDocument.isObject())
        sectionDocument = Json::object();
    for (const std::string &key : patch.keys())
        sectionDocument.set(key, patch[key]);
    document.set(section, sectionDocument);
    if (!store.save(document, error))
        return false;
    if (updatedSection)
        *updatedSection = sectionDocument;
    return true;
}

namespace {
// DEFAULTS in legacy tvweb.js for the settings a backup carries; invalid for
// the ones the server has no default for.
Json backupSettingDefault(const std::string &section, const std::string &key) {
    if (section == "logging" && key == "level") return Json::text("info");
    if (section == "syslog") {
        if (key == "server") return Json::text("");
        if (key == "port") return Json::numeric(514);
        if (key == "redact") return Json::flag(true);
        if (key == "sources") return Json::parse("[\"system\",\"nativetv\"]");
    }
    if (section == "ntp") {
        if (key == "server") return Json::text("");
        if (key == "port") return Json::numeric(123);
    }
    if (section == "history" && key == "enabled") return Json::flag(false);
    if (section == "backup" && key == "daily") return Json::flag(false);
    if (section == "screenTime") {
        if (key == "enabled") return Json::flag(false);
        if (key == "dailyMinutes") return Json::numeric(0);
        if (key == "bedtimeStart" || key == "bedtimeEnd") return Json::text("");
        if (key == "action") return Json::text("off");
        if (key == "appAction") return Json::text("home");
        if (key == "apps") return Json::object();
    }
    return Json();
}
} // namespace

bool ConfigStore::patchBackupSettings(const Json &patch, std::string *error,
                                      Json *changedFields) const {
    if (error)
        error->clear();
    if (changedFields)
        *changedFields = Json::array();
    if (!patch.isObject()) {
        if (error)
            *error = "backup config patch must be an object";
        return false;
    }
    const char *const allowedSections[] = {
        "logging", "syslog", "ntp", "history", "backup", "screenTime",
        "prometheus", "update"
    };
    for (const std::string &section : patch.keys()) {
        bool allowed = false;
        for (const char *candidate : allowedSections)
            if (section == candidate) allowed = true;
        if (!allowed || !patch[section].isObject()) {
            if (error)
                *error = "backup config patch contains an unsupported section";
            return false;
        }
    }
    if (patch.keys().empty())
        return true;

    ConfigFileLock lock(path_);
    if (!lock.acquired()) {
        if (error)
            *error = lock.error();
        return false;
    }
    JsonFile store(path_);
    JsonFileResult loaded = store.load();
    if (!loaded.error.empty()) {
        if (error)
            *error = loaded.error;
        return false;
    }
    Json document = loaded.found ? loaded.value : Json::object();
    if (!document.isObject()) {
        if (error)
            *error = "config document must be a JSON object";
        return false;
    }
    bool anyChanged = false;
    for (const std::string &section : patch.keys()) {
        Json existing = document[section];
        if (!existing.isObject())
            existing = Json::object();
        const Json changes = patch[section];
        bool sectionChanged = false;
        for (const std::string &key : changes.keys()) {
            // A setting the file leaves out stands at the server's default, so
            // restoring the default is no change (and needs no restart).
            const Json current = existing.has(key) ? existing[key]
                                                   : backupSettingDefault(section, key);
            if (current.valid() &&
                (current.dump() == changes[key].dump() ||
                 (current.isNumber() && changes[key].isNumber() &&
                  current.number() == changes[key].number())))
                continue;
            existing.set(key, changes[key]);
            sectionChanged = true;
            anyChanged = true;
            if (changedFields)
                changedFields->add(Json::text(section + "." + key));
        }
        if (sectionChanged)
            document.set(section, existing);
    }
    if (!anyChanged)
        return true;
    if (!store.save(document, error)) {
        if (changedFields)
            *changedFields = Json::array();
        return false;
    }
    return true;
}
} // namespace daemon
} // namespace nativetv
