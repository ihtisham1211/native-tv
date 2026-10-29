#include "daemon/apps_state.hpp"
#include "daemon/config.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <sstream>
#include <set>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace nativetv {
namespace daemon {
namespace {

std::string directoryOf(const std::string &path) {
    const size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) return ".";
    if (slash == 0) return "/";
    return path.substr(0, slash);
}

bool ensureDirectory(const std::string &path, std::string *error) {
    struct stat info = {};
    if (stat(path.c_str(), &info) == 0) {
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
    if (mkdir(path.c_str(), 0700) == 0 || errno == EEXIST) return true;
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
        if (error) *error = "could not create temporary file for " + path + ": " +
                            std::strerror(errno);
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
        if (error) *error = "could not atomically write " + path + ": " +
                            std::strerror(saved);
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
    if (id.empty() || id.size() > 255) return false;
    for (unsigned char character : id) {
        if (!((character >= 'a' && character <= 'z') ||
              (character >= 'A' && character <= 'Z') ||
              (character >= '0' && character <= '9') ||
              character == '_' || character == '.' || character == '-'))
            return false;
    }
    return true;
}

} // namespace

bool persistAppTileState(const std::string &hiddenAppsPath,
                         const std::string &tileHidingFlagPath,
                         const Json &state, std::string *error) {
    if (!state.isObject() || !state["hiddenApps"].isArray() ||
        !state["tileHidingEnabled"].isBoolean()) {
        if (error) *error = "apps state requires hiddenApps and tileHidingEnabled";
        return false;
    }
    const std::vector<Json> items = state["hiddenApps"].items();
    if (items.size() > 4096) {
        if (error) *error = "too many hidden apps";
        return false;
    }
    std::set<std::string> ids;
    for (const Json &item : items) {
        if (!item.isString() || !validAppId(item.string())) {
            if (error) *error = "invalid hidden app id";
            return false;
        }
        if (!ids.insert(item.string()).second) {
            if (error) *error = "duplicate hidden app id";
            return false;
        }
    }

    std::string hiddenList;
    for (const std::string &id : ids) hiddenList += id + "\n";
    if (!writeAtomic(hiddenAppsPath, hiddenList, error)) return false;
    const std::string flag = state["tileHidingEnabled"].boolean() ? "1\n" : "0\n";
    return writeAtomic(tileHidingFlagPath, flag, error);
}

bool tileHidingCarriesOver(const Json &config, bool flagEnabled, const std::string &stockType) {
    if (!config.isObject() || config.has("allowTileHiding")) return false;
    const bool slowSwitch = !stockType.empty() && stockType != "qml";
    if (slowSwitch && !config["allowOnWebos10"].boolean() &&
        !config["allowExperimentalScreensavers"].boolean())
        return false;
    return flagEnabled;
}

namespace {
std::string readTrimmedFile(const std::string &path) {
    std::string text;
    const int descriptor = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (descriptor < 0) return text;
    char buffer[128];
    const ssize_t count = read(descriptor, buffer, sizeof(buffer));
    close(descriptor);
    if (count > 0) text.assign(buffer, static_cast<size_t>(count));
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.pop_back();
    size_t first = 0;
    while (first < text.size() && std::isspace(static_cast<unsigned char>(text[first]))) ++first;
    return text.substr(first);
}
} // namespace

bool migrateTileHidingConfig(const std::string &configPath, const Json &config,
                             const std::string &dataRoot, std::string *error) {
    const bool flagEnabled = readTrimmedFile(dataRoot + "/tile_hiding_enabled") == "1";
    if (!tileHidingCarriesOver(config, flagEnabled,
                               readTrimmedFile(dataRoot + "/screensaver-stock-type")))
        return false;
    Json change = Json::object();
    change.set("allowTileHiding", Json::flag(true));
    return ConfigStore(configPath).setTopLevel(change, error);
}

bool launcherNeedsRefresh(const std::string &dataRoot, const Json &config, bool restartedThisRun) {
    const auto read = [](const std::string &path, std::string &text) {
        std::ifstream in(path.c_str(), std::ios::binary);
        if (!in) return false;
        std::ostringstream buffer;
        buffer << in.rdbuf();
        text = buffer.str();
        return true;
    };
    std::string flag;
    bool enabled = false;
    if (read(dataRoot + "/tile_hiding_enabled", flag)) {
        const size_t first = flag.find_first_not_of(" \t\r\n");
        enabled = first != std::string::npos && flag[first] == '1';
    }
    const Json allowed = config["allowTileHiding"];
    if (!(allowed.isBoolean() ? allowed.boolean() : enabled)) return false;
    if (restartedThisRun) return true;
    if (access((dataRoot + "/.from-homebrew-channel").c_str(), F_OK) == 0) return false;
    std::string hidden;
    if (!enabled || !read(dataRoot + "/hidden_apps", hidden)) return false;
    return hidden.find_first_not_of(" \t\r\n") != std::string::npos;
}

bool migrateLegacyConfig(const std::string &configPath, const Json &config,
                         std::string *error) {
    if (!config.has("allowOnWebos10")) return false;
    const bool value = config["allowOnWebos10"].isBoolean() && config["allowOnWebos10"].boolean();
    Json change = Json::object();
    change.set("allowExperimentalScreensavers",
               config.has("allowExperimentalScreensavers") ? config["allowExperimentalScreensavers"]
                                                           : Json::flag(value));
    change.set("allowOnWebos10", Json());   // a null change removes the key
    return ConfigStore(configPath).setTopLevel(change, error);
}

} // namespace daemon
} // namespace nativetv
