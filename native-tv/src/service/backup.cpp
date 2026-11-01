#include "service/backup.hpp"
#include "domain/say.hpp"
#include "domain/names.hpp"
#include "service/screen_time.hpp"

#include <cerrno>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <dirent.h>
#include <functional>
#include <fcntl.h>
#include <set>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace nativetv {
namespace service {
namespace {

size_t utf16Length(const std::string &text) {
    size_t units = 0;
    for (size_t index = 0; index < text.size();) {
        const unsigned char first = static_cast<unsigned char>(text[index]);
        size_t width = 1;
        size_t codeUnits = 1;
        if (first >= 0xf0) {
            width = 4;
            codeUnits = 2;
        } else if (first >= 0xe0) {
            width = 3;
        } else if (first >= 0xc2) {
            width = 2;
        }
        bool valid = index + width <= text.size();
        for (size_t offset = 1; valid && offset < width; ++offset) {
            const unsigned char continuation =
                static_cast<unsigned char>(text[index + offset]);
            valid = continuation >= 0x80 && continuation <= 0xbf;
        }
        units += valid ? codeUnits : 1;
        index += valid ? width : 1;
    }
    return units;
}

std::string prefixUtf16(const std::string &text, size_t maximumUnits) {
    size_t units = 0;
    size_t end = 0;
    while (end < text.size()) {
        const unsigned char first = static_cast<unsigned char>(text[end]);
        size_t width = 1;
        size_t codeUnits = 1;
        if (first >= 0xf0) {
            width = 4;
            codeUnits = 2;
        } else if (first >= 0xe0) {
            width = 3;
        } else if (first >= 0xc2) {
            width = 2;
        }
        bool valid = end + width <= text.size();
        for (size_t offset = 1; valid && offset < width; ++offset) {
            const unsigned char continuation =
                static_cast<unsigned char>(text[end + offset]);
            valid = continuation >= 0x80 && continuation <= 0xbf;
        }
        if (units + (valid ? codeUnits : 1) > maximumUnits)
            break;
        units += valid ? codeUnits : 1;
        end += valid ? width : 1;
    }
    return text.substr(0, end);
}

bool validString(const Json &value) {
    return value.isString() && utf16Length(value.string()) <= 260;
}

bool validPort(const Json &value) {
    if (!value.isNumber()) return false;
    const double number = value.number();
    return number >= 1 && number <= 65535 && number == static_cast<int>(number);
}

bool validClock(const Json &value) {
    if (!value.isString()) return false;
    const std::string text = value.string();
    if (text.empty()) return true;
    if (text.size() != 5 || text[2] != ':') return false;
    for (size_t i = 0; i < text.size(); ++i)
        if (i != 2 && (text[i] < '0' || text[i] > '9')) return false;
    const int hour = (text[0] - '0') * 10 + text[1] - '0';
    const int minute = (text[3] - '0') * 10 + text[4] - '0';
    return hour <= 23 && minute <= 59;
}

bool validMinutes(const Json &value, int minimum) {
    if (!value.isNumber()) return false;
    const double number = value.number();
    return number >= minimum && number <= 1440 && number == static_cast<int>(number);
}

bool validScreenTimeApps(const Json &value) {
    if (!value.isObject() || value.keys().size() > 50) return false;
    for (const std::string &key : value.keys())
        if (utf16Length(key) > 200 || !validMinutes(value[key], 1)) return false;
    return true;
}

bool validAction(const Json &value, bool appAction) {
    if (!value.isString()) return false;
    const std::string action = value.string();
    if (action == "off" || action == "screenOff" || action == "warn") return true;
    return appAction && action == "home";
}

bool validSetting(const std::string &section, const std::string &key,
                  const Json &value) {
    if (section == "logging" && key == "level")
        return value.isString() && (value.string() == "quiet" ||
               value.string() == "info" || value.string() == "debug");
    if (section == "syslog") {
        if (key == "server") return validString(value);
        if (key == "port") return validPort(value);
        if (key == "redact") return value.isBoolean();
        if (key == "sources") {
            if (!value.isArray()) return false;
            for (const Json &source : value.items())
                if (!source.isString() || (source.string() != "system" &&
                    source.string() != "nativetv" && source.string() != "kernel"))
                    return false;
            return true;
        }
        return false;
    }
    if (section == "ntp") {
        if (key == "server") return validString(value);
        if (key == "port") return validPort(value);
        if (key == "enabled") return value.isBoolean();
        return false;
    }
    if ((section == "history" || section == "prometheus") && key == "enabled")
        return value.isBoolean();
    if (section == "backup" && key == "daily") return value.isBoolean();
    if (section == "update" && key == "check") return value.isBoolean();
    if (section == "screenTime") {
        if (key == "enabled") return value.isBoolean();
        if (key == "dailyMinutes") return validMinutes(value, 0);
        if (key == "idleMinutes") {
            for (int choice : screenTimeIdleChoices())
                if (value.isNumber() && value.number() == choice)
                    return true;
            return false;
        }
        if (key == "bedtimeStart" || key == "bedtimeEnd") return validClock(value);
        if (key == "action") return validAction(value, false);
        if (key == "appAction") return validAction(value, true);
        if (key == "apps") return validScreenTimeApps(value);
    }
    return false;
}

std::vector<std::string> backupSettingKeys(const std::string &section) {
    if (section == "logging") return {"level"};
    if (section == "syslog") return {"server", "port", "sources", "redact"};
    if (section == "ntp") return {"server", "port", "enabled"};
    if (section == "history") return {"enabled"};
    if (section == "backup") return {"daily"};
    if (section == "screenTime")
        return {"enabled", "dailyMinutes", "idleMinutes", "bedtimeStart", "bedtimeEnd",
                "action", "appAction", "apps"};
    if (section == "prometheus") return {"enabled"};
    if (section == "update") return {"check"};
    return {};
}

constexpr int kSettingsBackupVersion = 1;
constexpr size_t kMaximumSavedBackupBytes = 16 * 1024 * 1024;
constexpr size_t kMaximumPendingBackupApps = 4096;

std::string directoryOf(const std::string &path) {
    const size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) return ".";
    if (slash == 0) return "/";
    return path.substr(0, slash);
}

bool ensureDirectory(const std::string &path, std::string *error) {
    struct stat info = {};
    if (lstat(path.c_str(), &info) == 0) {
        if (S_ISDIR(info.st_mode)) return true;
        if (error) *error = path + " is not a directory";
        return false;
    }
    if (errno != ENOENT) {
        if (error) *error = "could not inspect " + path + ": " + std::strerror(errno);
        return false;
    }
    const std::string parent = directoryOf(path);
    if (parent != path && !ensureDirectory(parent, error)) return false;
    if (mkdir(path.c_str(), 0700) == 0 || errno == EEXIST) {
        if (lstat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode)) return true;
    }
    if (error) *error = "could not create " + path + ": " + std::strerror(errno);
    return false;
}

bool writeAtomic(const std::string &path, const std::string &contents,
                 std::string *error) {
    if (!ensureDirectory(directoryOf(path), error)) return false;
    std::string pattern = path + ".tmp.XXXXXX";
    std::vector<char> name(pattern.begin(), pattern.end());
    name.push_back('\0');
    const int descriptor = mkstemp(name.data());
    if (descriptor < 0) {
        if (error) *error = "could not create temporary file: " +
                            std::string(std::strerror(errno));
        return false;
    }
    const std::string temporary(name.data());
    bool ok = fchmod(descriptor, 0600) == 0;
    size_t offset = 0;
    while (ok && offset < contents.size()) {
        const ssize_t count = write(descriptor, contents.data() + offset,
                                    contents.size() - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) ok = false;
        else offset += static_cast<size_t>(count);
    }
    if (ok && fsync(descriptor) != 0) ok = false;
    if (close(descriptor) != 0) ok = false;
    if (!ok || rename(temporary.c_str(), path.c_str()) != 0) {
        const int saved = errno;
        unlink(temporary.c_str());
        if (error) *error = "could not atomically write file: " +
                            std::string(std::strerror(saved));
        return false;
    }
    const int directory = open(directoryOf(path).c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory >= 0) {
        (void)fsync(directory);
        close(directory);
    }
    return true;
}

bool validAppId(const std::string &id) {
    if (id.empty() || id.size() > 128) return false;
    for (unsigned char character : id)
        if (!((character >= 'a' && character <= 'z') ||
              (character >= 'A' && character <= 'Z') ||
              (character >= '0' && character <= '9') || character == '.' ||
              character == '_' || character == '-'))
            return false;
    return true;
}

bool readBackupFileDescriptor(int fileFd, std::string &contents) {
    contents.clear();
    struct stat info;
    if (fstat(fileFd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < 0 ||
        info.st_size > static_cast<off_t>(kMaximumSavedBackupBytes))
        return false;

    std::string body;
    char buffer[8192];
    for (;;) {
        const ssize_t count = read(fileFd, buffer, sizeof(buffer));
        if (count == 0)
            break;
        if (count < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (body.size() + static_cast<size_t>(count) > kMaximumSavedBackupBytes)
            return false;
        body.append(buffer, static_cast<size_t>(count));
    }
    contents.swap(body);
    return true;
}

Json safeConfig(const Json &config) {
    Json result = Json::object();
    const std::string sections[] = {
        "logging", "syslog", "ntp", "history", "backup", "screenTime",
        "prometheus", "update"
    };
    for (const std::string &section : sections) {
        const Json source = config[section];
        if (!source.isObject()) continue;
        Json kept = Json::object();
        for (const std::string &key : backupSettingKeys(section))
            if (source.has(key) && validSetting(section, key, source[key]))
                kept.set(key, source[key]);
        if (!kept.keys().empty()) result.set(section, kept);
    }
    return result;
}

const char *const kSavedBackupPrefix = "nativetv-backup-";

// The length of the prefix a saved backup's name starts with, or 0.
size_t savedBackupPrefixLength(const std::string &name) {
    const size_t length = std::strlen(kSavedBackupPrefix);
    return name.compare(0, length, kSavedBackupPrefix) == 0 ? length : 0;
}

// Newest date first.
bool newerSavedBackup(const std::string &left, const std::string &right) {
    return left > right;
}

} // namespace

Json createSettingsBackup(const std::string &created, const Json &model,
                          const Json &version, const Json &config,
                          const Json &lgRows, const Json &home,
                          const Json &apps) {
    Json result = Json::object();
    result.set("format", Json::text("nativetv-backup"));
    result.set("version", Json::numeric(kSettingsBackupVersion));
    result.set("created", Json::text(created));
    Json from = Json::object();
    from.set("model", model.valid() ? model : Json());
    from.set("nativetv", version.valid() ? version : Json());
    result.set("from", from);

    Json lg = Json::object();
    if (lgRows.isArray()) {
        for (const Json &row : lgRows.items()) {
            const std::string id = row["id"].string();
            if (id.empty()) continue;
            const Json value = row["type"].string() == "switch"
                ? row["on"] : row["value"];
            if (value.valid()) lg.set(id, value);
        }
    }
    result.set("lg", lg);
    result.set("nativetv", safeConfig(config));
    if (home.valid()) result.set("home", home);
    result.set("apps", apps.isArray() ? apps : Json::array());
    return result;
}

Json safeBackupConfig(const Json &config) {
    return safeConfig(config);
}

std::string backupRestoreEnvelopeResult(const Json &backup) {
    if (!backup.isObject() || !backup["format"].isString() ||
        backup["format"].string() != "nativetv-backup" ||
        !backup["version"].isNumber())
        return "notBackup";
    if (backup["version"].number() > kSettingsBackupVersion)
        return "newer";
    return "valid";
}

bool backupAppAllowed(const std::string &id) {
    static const std::set<std::string> protectedIds = {
        "com.tvweb.dashboard", "com.tvweb.home", "com.tvweb.native",
        "com.tvweb.splash",
        "org.webosbrew.hbchannel", "com.webos.app.livetv",
        "com.palm.app.settings", "com.webos.app.settings",
        "com.webos.app.container", "container", "com.webos.app.inputcommon",
        "inputcommon", "com.webos.app.home", "com.webos.app.firstuse",
        "firstuse", "com.webos.app.eula", "eula",
        "com.webos.app.webapphost", "webapphost", "airplay",
        "com.webos.app.browser", "io.github.ihtisham1211.nativetv"
    };
    return protectedIds.find(id) == protectedIds.end() &&
           id.find("com.webos.app.input") != 0 &&
           !names::isHdmiKey(names::shortAppId(id));
}

Json planBackupRestoreApps(const Json &apps, const Json &installedIds,
                           const Json &catalogIds, bool catalogAvailable,
                           const std::string &catalogError) {
    Json wanted = Json::array();
    if (apps.isArray()) {
        for (const Json &app : apps.items()) {
            const Json id = app["id"];
            if (!app.isObject() || !id.isString() || !validAppId(id.string()) ||
                !backupAppAllowed(id.string()))
                continue;
            Json item = Json::object();
            item.set("id", id);
            const Json title = app["title"];
            item.set("title", Json::text(title.isString()
                ? prefixUtf16(title.string(), 80) : id.string()));
            wanted.add(item);
        }
    }

    std::set<std::string> installed;
    if (installedIds.isArray()) {
        for (const Json &id : installedIds.items())
            if (id.isString() && validAppId(id.string()))
                installed.insert(id.string());
    }
    std::set<std::string> catalog;
    if (catalogIds.isArray()) {
        for (const Json &id : catalogIds.items())
            if (id.isString() && validAppId(id.string()))
                catalog.insert(id.string());
    }

    Json missing = Json::array();
    size_t present = 0;
    for (const Json &app : wanted.items()) {
        if (installed.find(app["id"].string()) == installed.end())
            missing.add(app);
        else
            ++present;
    }

    Json toInstall = Json::array();
    Json manual = Json::array();
    std::string resultCatalogError;
    if (missing.size()) {
        if (!catalogAvailable) {
            toInstall = missing;
            resultCatalogError = catalogError.empty() ? say::msg("srv.repo.unavailable", "catalog unavailable") : catalogError;
        } else {
            for (const Json &app : missing.items()) {
                if (catalog.find(app["id"].string()) != catalog.end())
                    toInstall.add(app);
                else
                    manual.add(app);
            }
        }
    }

    Json result = Json::object();
    result.set("toInstall", toInstall);
    result.set("manual", manual);
    result.set("present", Json::numeric(static_cast<double>(present)));
    result.set("catalogError", resultCatalogError.empty()
        ? Json() : Json::text(resultCatalogError));
    return result;
}

std::string dailyBackupNameDue(bool enabled, const std::string &isoTimestamp) {
    if (!enabled || isoTimestamp.size() < 10) return std::string();
    const std::string date = isoTimestamp.substr(0, 10);
    // ISO dates order as text.
    if (date < "2026-01-01") return std::string();
    return kSavedBackupPrefix + date + ".json";
}

bool validSavedBackupName(const std::string &name) {
    const size_t prefix = savedBackupPrefixLength(name);
    if (prefix == 0 || name.size() != prefix + 10 + 5 ||
        name.compare(prefix + 10, 5, ".json") != 0)
        return false;
    for (size_t index = prefix; index < prefix + 10; ++index) {
        if (index == prefix + 4 || index == prefix + 7) {
            if (name[index] != '-') return false;
        } else if (name[index] < '0' || name[index] > '9') {
            return false;
        }
    }
    return true;
}

bool savedBackupExists(const std::string &directory, const std::string &name,
                       bool &exists, std::string *error) {
    exists = false;
    if (error) error->clear();
    if (directory.empty() || !validSavedBackupName(name)) {
        if (error) *error = "daily backup directory or name is invalid";
        return false;
    }

    const int directoryFd = open(directory.c_str(),
                                 O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (directoryFd < 0) {
        if (errno == ENOENT)
            return true;
        if (error) *error = "could not open daily backup directory: " +
                            std::string(std::strerror(errno));
        return false;
    }
    struct stat info = {};
    const int status = fstatat(directoryFd, name.c_str(), &info, AT_SYMLINK_NOFOLLOW);
    const int savedErrno = errno;
    close(directoryFd);
    if (status == 0) {
        // Treat any existing entry as occupied. This avoids overwriting a
        // symlink, directory or other unexpected object at the daily filename.
        exists = true;
        return true;
    }
    if (savedErrno == ENOENT)
        return true;
    if (error) *error = "could not inspect daily backup: " +
                        std::string(std::strerror(savedErrno));
    return false;
}

bool readSavedBackup(const std::string &directory, const std::string &name,
                     std::string &contents) {
    contents.clear();
    if (!validSavedBackupName(name))
        return false;

    const int directoryFd = open(directory.c_str(),
                                 O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (directoryFd < 0)
        return false;
    const int fileFd = openat(directoryFd, name.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    close(directoryFd);
    if (fileFd < 0)
        return false;
    const bool ok = readBackupFileDescriptor(fileFd, contents);
    close(fileFd);
    return ok;
}

bool listSavedBackups(const std::string &directory, Json &backups,
                      std::string *error) {
    backups = Json::array();
    if (error) error->clear();
    if (directory.empty()) {
        if (error) *error = "daily backup directory is empty";
        return false;
    }

    const int directoryFd = open(directory.c_str(),
                                 O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (directoryFd < 0) {
        if (errno == ENOENT)
            return true;
        if (error) *error = "could not open daily backup directory: " +
                            std::string(std::strerror(errno));
        return false;
    }
    const int scanFd = dup(directoryFd);
    DIR *stream = scanFd >= 0 ? fdopendir(scanFd) : nullptr;
    if (!stream) {
        const int savedErrno = errno;
        if (scanFd >= 0) close(scanFd);
        close(directoryFd);
        if (error) *error = "could not scan daily backup directory: " +
                            std::string(std::strerror(savedErrno));
        return false;
    }

    struct SavedEntry {
        std::string name;
        off_t size;
    };
    std::vector<SavedEntry> entries;
    int readError = 0;
    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(stream);
        if (!entry) {
            readError = errno;
            break;
        }
        const std::string name(entry->d_name);
        if (!validSavedBackupName(name))
            continue;
        struct stat info = {};
        if (fstatat(directoryFd, name.c_str(), &info, AT_SYMLINK_NOFOLLOW) != 0) {
            if (errno == ENOENT)
                continue;
            readError = errno;
            break;
        }
        if (S_ISREG(info.st_mode) && info.st_size >= 0)
            entries.push_back({name, info.st_size});
    }
    closedir(stream);
    close(directoryFd);
    if (readError != 0) {
        if (error) *error = "could not list daily backups: " +
                            std::string(std::strerror(readError));
        return false;
    }

    std::sort(entries.begin(), entries.end(),
              [](const SavedEntry &left, const SavedEntry &right) {
                  return newerSavedBackup(left.name, right.name);
              });
    for (const SavedEntry &entry : entries) {
        Json item = Json::object();
        item.set("name", Json::text(entry.name));
        item.set("date", Json::text(entry.name.substr(savedBackupPrefixLength(entry.name), 10)));
        item.set("size", Json::numeric(static_cast<double>(entry.size)));
        backups.add(item);
    }
    return true;
}

bool saveDailyBackup(const std::string &directory, const std::string &name,
                     const Json &backup, std::string *error) {
    if (error) error->clear();
    if (directory.empty() || !validSavedBackupName(name) ||
        !backup.isObject() || backup["format"].string() != "nativetv-backup" ||
        !backup["version"].isNumber() || backup["version"].number() != 1) {
        if (error) *error = "daily backup name or document is invalid";
        return false;
    }
    const std::string contents = backup.dump();
    if (contents.size() > kMaximumSavedBackupBytes) {
        if (error) *error = "daily backup exceeds 16 MiB";
        return false;
    }
    if (!writeAtomic(directory + "/" + name, contents, error))
        return false;

    DIR *stream = opendir(directory.c_str());
    if (!stream) {
        if (error) *error = "could not list daily backups: " +
                            std::string(std::strerror(errno));
        return false;
    }
    std::vector<std::string> names;
    struct dirent *entry = nullptr;
    int readError = 0;
    for (;;) {
        errno = 0;
        entry = readdir(stream);
        if (!entry) {
            readError = errno;
            break;
        }
        const std::string candidate(entry->d_name);
        if (!validSavedBackupName(candidate)) continue;
        struct stat info = {};
        const std::string path = directory + "/" + candidate;
        if (lstat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode))
            names.push_back(candidate);
    }
    closedir(stream);
    if (readError != 0) {
        if (error) *error = "could not list daily backups: " +
                            std::string(std::strerror(readError));
        return false;
    }
    std::sort(names.begin(), names.end(), newerSavedBackup);
    constexpr size_t kRetainedDailyBackups = 7;
    for (size_t index = kRetainedDailyBackups; index < names.size(); ++index) {
        const std::string path = directory + "/" + names[index];
        if (unlink(path.c_str()) != 0 && errno != ENOENT) {
            if (error) *error = "could not prune daily backup: " +
                                std::string(std::strerror(errno));
            return false;
        }
    }
    return true;
}

Json readPendingBackupApps(const std::string &path) {
    const int fileFd = open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fileFd < 0)
        return Json::array();
    std::string body;
    const bool ok = readBackupFileDescriptor(fileFd, body);
    close(fileFd);
    if (!ok)
        return Json::array();
    const Json saved = Json::parse(body);
    return saved.isObject() && saved["apps"].isArray()
        ? saved["apps"] : Json::array();
}

Json pendingBackupApps(const Json &pending, const Json &installedIds) {
    std::set<std::string> installed;
    if (installedIds.isArray())
        for (const Json &id : installedIds.items())
            if (id.isString()) installed.insert(id.string());

    Json result = Json::array();
    if (!pending.isArray())
        return result;
    for (const Json &app : pending.items()) {
        const Json id = app["id"];
        if (app.isObject() && id.isString() && !id.string().empty() &&
            installed.find(id.string()) == installed.end())
            result.add(app);
    }
    return result;
}

bool savePendingBackupApps(const std::string &path, const Json &saved,
                           const Json &apps, std::string *error) {
    if (error) error->clear();
    if (path.empty() || !saved.isNumber() || !std::isfinite(saved.number()) ||
        saved.number() < 0 || std::floor(saved.number()) != saved.number() ||
        !apps.isArray() || apps.size() > kMaximumPendingBackupApps) {
        if (error) *error = "pending backup app list is invalid";
        return false;
    }
    Json normalizedApps = Json::array();
    for (const Json &app : apps.items()) {
        const Json idValue = app["id"];
        const Json titleValue = app["title"];
        if (!app.isObject() || !idValue.isString() || !validAppId(idValue.string()) ||
            !backupAppAllowed(idValue.string()) ||
            (titleValue.valid() && !titleValue.isString())) {
            if (error) *error = "pending backup app entry is invalid";
            return false;
        }
        Json item = Json::object();
        item.set("id", idValue);
        item.set("title", titleValue.isString() ? titleValue : idValue);
        if (utf16Length(item["title"].string()) > 80) {
            if (error) *error = "pending backup app title exceeds 80 UTF-16 units";
            return false;
        }
        normalizedApps.add(item);
    }
    Json document = Json::object();
    document.set("saved", saved);
    document.set("apps", normalizedApps);
    return writeAtomic(path, document.dump(), error);
}

bool clearPendingBackupApps(const std::string &path, std::string *error) {
    if (error) error->clear();
    if (path.empty()) {
        if (error) *error = "pending backup app path is empty";
        return false;
    }
    if (unlink(path.c_str()) == 0 || errno == ENOENT)
        return true;
    if (error) *error = "could not clear pending backup apps: " +
                        std::string(std::strerror(errno));
    return false;
}

} // namespace service
} // namespace nativetv
