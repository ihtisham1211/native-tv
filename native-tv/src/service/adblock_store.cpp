#include "service/adblock_store.hpp"
#include "service/preload.hpp"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sstream>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <utility>
#if defined(__linux__)
#include <sys/mount.h>
#endif

namespace nativetv {
namespace service {
namespace {
const char *const kTableMarker =
    "# LG Ad & Telemetry Blackhole (lg-webos-dashboard)";
const char *const kLegacyTableMarker =
    "# LG Ad & Telemetry Blackhole (lg-webos-mqtt)";
const char *const kCustomFilename = "adblock_custom.json";
const char *const kEnabledFilename = "adblock_enabled";
const char *const kTableFilename = "adblock_hosts";
const char *const kLocalLookupsFilename = "local_lookups";
const char *const kUpdatesFlagPath = "/var/luna/preferences/webosbrew_block_updates";
const char *const kHostsPath = "/etc/hosts";

std::string joinPath(const std::string &root, const std::string &path) {
    if (root.empty() || root == "/")
        return path;
    if (root[root.size() - 1] == '/')
        return root.substr(0, root.size() - 1) + path;
    return root + path;
}

std::string readText(const std::string &path) {
    std::ifstream input(path.c_str(), std::ios::in | std::ios::binary);
    if (!input)
        return std::string();
    std::ostringstream contents;
    contents << input.rdbuf();
    return contents.str();
}

std::string trim(const std::string &value) {
    size_t start = 0;
    while (start < value.size() &&
           std::isspace(static_cast<unsigned char>(value[start])))
        ++start;
    size_t end = value.size();
    while (end > start &&
           std::isspace(static_cast<unsigned char>(value[end - 1])))
        --end;
    return value.substr(start, end - start);
}

std::string systemError(const std::string &action, const std::string &path) {
    return action + " " + path + ": " + std::strerror(errno);
}

bool writeInPlace(const std::string &path, const std::string &contents,
                  std::string *error) {
    const int descriptor = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (descriptor < 0) {
        if (error)
            *error = systemError("open", path);
        return false;
    }
    size_t offset = 0;
    bool ok = true;
    while (offset < contents.size()) {
        const ssize_t written = ::write(descriptor, contents.data() + offset,
                                        contents.size() - offset);
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0) {
            ok = false;
            if (error)
                *error = written == 0 ? "write made no progress for " + path
                                      : systemError("write", path);
            break;
        }
        offset += static_cast<size_t>(written);
    }
    if (ok && ::fsync(descriptor) != 0) {
        ok = false;
        if (error)
            *error = systemError("sync", path);
    }
    if (::close(descriptor) != 0 && ok) {
        ok = false;
        if (error)
            *error = systemError("close", path);
    }
    return ok;
}

bool writeAtomic(const std::string &path, const std::string &contents,
                 std::string *error) {
    std::string pattern = path + ".tmp.XXXXXX";
    std::vector<char> name(pattern.begin(), pattern.end());
    name.push_back('\0');
    const int descriptor = ::mkstemp(name.data());
    if (descriptor < 0) {
        if (error)
            *error = systemError("create temporary file for", path);
        return false;
    }
    const std::string temporary(name.data());
    bool ok = ::fchmod(descriptor, 0600) == 0;
    if (!ok && error)
        *error = systemError("set permissions on", temporary);
    size_t offset = 0;
    while (ok && offset < contents.size()) {
        const ssize_t written = ::write(descriptor, contents.data() + offset,
                                        contents.size() - offset);
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0) {
            ok = false;
            if (error)
                *error = written == 0 ? "write made no progress for " + temporary
                                      : systemError("write", temporary);
            break;
        }
        offset += static_cast<size_t>(written);
    }
    if (ok && ::fsync(descriptor) != 0) {
        ok = false;
        if (error)
            *error = systemError("sync", temporary);
    }
    if (::close(descriptor) != 0 && ok) {
        ok = false;
        if (error)
            *error = systemError("close", temporary);
    }
    if (ok && ::rename(temporary.c_str(), path.c_str()) == 0)
        return true;
    if (ok && error)
        *error = systemError("replace", path);
    ::unlink(temporary.c_str());
    return false;
}

bool defaultBindMount(const std::string &source, const std::string &target) {
#if defined(__linux__)
    return ::mount(source.c_str(), target.c_str(), nullptr, MS_BIND, nullptr) == 0;
#else
    (void)source;
    (void)target;
    errno = ENOTSUP;
    return false;
#endif
}

bool defaultUnmount(const std::string &target) {
#if defined(__linux__)
    return ::umount2(target.c_str(), 0) == 0;
#else
    (void)target;
    errno = ENOTSUP;
    return false;
#endif
}

std::vector<LocalInterface> defaultInterfaces() {
    std::vector<LocalInterface> result;
    struct ifaddrs *list = nullptr;
    if (::getifaddrs(&list) != 0)
        return result;
    for (const struct ifaddrs *entry = list; entry; entry = entry->ifa_next) {
        if (!entry->ifa_addr || !entry->ifa_netmask ||
            entry->ifa_addr->sa_family != AF_INET ||
            (entry->ifa_flags & (IFF_UP | IFF_LOOPBACK)) != IFF_UP)
            continue;
        LocalInterface found;
        found.address = ntohl(reinterpret_cast<const struct sockaddr_in *>(
            entry->ifa_addr)->sin_addr.s_addr);
        found.netmask = ntohl(reinterpret_cast<const struct sockaddr_in *>(
            entry->ifa_netmask)->sin_addr.s_addr);
        result.push_back(found);
    }
    ::freeifaddrs(list);
    return result;
}

std::string cleanFilename(const Json &value) {
    const std::string input = javascriptTruthy(value) ? javascriptString(value) : "";
    std::string output;
    size_t units = 0;
    for (size_t offset = 0; offset < input.size();) {
        const unsigned char first = static_cast<unsigned char>(input[offset]);
        size_t bytes = first < 0x80 ? 1 : (first & 0xe0) == 0xc0 ? 2
                            : (first & 0xf0) == 0xe0 ? 3
                            : (first & 0xf8) == 0xf0 ? 4 : 1;
        if (offset + bytes > input.size())
            bytes = 1;
        bool control = bytes == 1 && first < 0x20;
        unsigned codepoint = first;
        if (bytes == 2 && offset + 1 < input.size())
            codepoint = ((first & 0x1f) << 6) |
                (static_cast<unsigned char>(input[offset + 1]) & 0x3f);
        else if (bytes == 3 && offset + 2 < input.size())
            codepoint = ((first & 0x0f) << 12) |
                ((static_cast<unsigned char>(input[offset + 1]) & 0x3f) << 6) |
                (static_cast<unsigned char>(input[offset + 2]) & 0x3f);
        else if (bytes == 4 && offset + 3 < input.size())
            codepoint = ((first & 0x07) << 18) |
                ((static_cast<unsigned char>(input[offset + 1]) & 0x3f) << 12) |
                ((static_cast<unsigned char>(input[offset + 2]) & 0x3f) << 6) |
                (static_cast<unsigned char>(input[offset + 3]) & 0x3f);
        if (codepoint < 0x20)
            control = true;
        const size_t needed = codepoint > 0xffff ? 2 : 1;
        if (!control && units + needed <= 120) {
            output.append(input, offset, bytes);
            units += needed;
        }
        offset += bytes;
        if (units >= 120)
            break;
    }
    return output;
}

std::string nowIso8601() {
    const auto now = std::chrono::system_clock::now();
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count() % 1000;
    const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
    std::tm utc;
    if (!gmtime_r(&seconds, &utc))
        return std::string();
    char buffer[32];
    if (!std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S", &utc))
        return std::string();
    std::ostringstream result;
    result << buffer << '.';
    result.width(3);
    result.fill('0');
    result << milliseconds << 'Z';
    return result.str();
}

Json failure(const std::string &message) {
    Json result = Json::object();
    result.set("ok", Json::flag(false));
    result.set("error", Json::text(message));
    return result;
}

} // namespace

AdBlockStore::AdBlockStore(std::string dataRoot, std::string systemRoot,
                           AdBlockMountOperations mountOperations)
    : dataRoot_(std::move(dataRoot)), systemRoot_(std::move(systemRoot)),
      mountOperations_(std::move(mountOperations)) {
    if (!mountOperations_.bind)
        mountOperations_.bind = defaultBindMount;
    if (!mountOperations_.unmount)
        mountOperations_.unmount = defaultUnmount;
    if (!mountOperations_.interfaces)
        mountOperations_.interfaces = defaultInterfaces;
}

std::string AdBlockStore::rootedSystemPath(const std::string &absolutePath) const {
    return joinPath(systemRoot_, absolutePath);
}

std::string AdBlockStore::dataPath(const std::string &filename) const {
    if (dataRoot_.empty() || dataRoot_[dataRoot_.size() - 1] == '/')
        return dataRoot_ + filename;
    return dataRoot_ + "/" + filename;
}

std::string AdBlockStore::mode() const {
    const std::string flag = trim(readText(dataPath(kEnabledFilename)));
    if (flag.empty())
        return "off";
    return flag == "ads" ? "ads" : "full";
}

bool AdBlockStore::tvUpdatesBlocked() const {
    return ::access(rootedSystemPath(kUpdatesFlagPath).c_str(), F_OK) == 0;
}

bool AdBlockStore::localLookups() const {
    return ::access(dataPath(kLocalLookupsFilename).c_str(), F_OK) == 0;
}

std::vector<std::string> AdBlockStore::localLookupAddresses() const {
    return localLookups() ? adBlockLocalLookupAddresses(mountOperations_.interfaces())
                          : std::vector<std::string>();
}

bool AdBlockStore::tableMounted() const {
    const std::string hosts = readText(rootedSystemPath(kHostsPath));
    return hosts.find(kTableMarker) != std::string::npos ||
        hosts.find(kLegacyTableMarker) != std::string::npos;
}

bool AdBlockStore::waitsForSdp() const {
    const std::string currentMode = mode();
    if (currentMode == "full")
        return true;
    if (currentMode == "off")
        return false;
    const AdBlockCustomList custom = customHosts();
    if (!custom.enabled)
        return false;
    for (const std::string &name : custom.names) {
        const std::string suffix = "nextlgsdp.com";
        if (name == suffix ||
            (name.size() > suffix.size() &&
             name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0 &&
             name[name.size() - suffix.size() - 1] == '.'))
            return true;
    }
    return false;
}

bool AdBlockStore::updateCountry(const std::string &countryValue,
                                 bool sdpGraceActive,
                                 std::string *error) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string normalized = countryValue;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
    if (normalized.size() != 2 ||
        !std::isalpha(static_cast<unsigned char>(normalized[0])) ||
        !std::isalpha(static_cast<unsigned char>(normalized[1])) ||
        normalized == country())
        return true;
    if (!writeInPlace(dataPath("country"), normalized, error))
        return false;
    if (!tableMounted())
        return true;
    return writeHostsTable(mode(), sdpGraceActive, currentForeignHostsLines(), error);
}

std::string AdBlockStore::country() const {
    std::string value = trim(readText(dataPath("country")));
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::string AdBlockStore::serverHost() const {
    const Json data = Json::parse(readText(rootedSystemPath(
        "/var/palm/data/com.webos.appInstallService/serverInfo")));
    const std::string url = data["serverUrl"].string();
    const size_t scheme = url.find("://");
    if (!data.isObject() || scheme == std::string::npos)
        return std::string();
    const size_t start = scheme + 3;
    const size_t end = url.find_first_of("/:?#", start);
    std::string host = url.substr(start, end == std::string::npos
                                           ? std::string::npos : end - start);
    std::transform(host.begin(), host.end(), host.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return host;
}

AdBlockCustomList AdBlockStore::customHosts() const {
    AdBlockCustomList result;
    const Json data = Json::parse(readText(dataPath(kCustomFilename)));
    if (!data.isObject() || !data["names"].isArray() ||
        data["names"].size() == 0 ||
        (data["mode"].string() != "add" && data["mode"].string() != "replace"))
        return result;
    result.mode = data["mode"].string();
    for (const Json &name : data["names"].items())
        if (name.isString())
            result.names.push_back(name.string());
    if (result.names.empty())
        return AdBlockCustomList();
    result.enabled = !(data["enabled"].isBoolean() && !data["enabled"].boolean());
    result.file = data["file"].isString() ? data["file"].string() : std::string();
    result.updated = data["updated"].isString() ? data["updated"].string() : std::string();
    return result;
}

Json AdBlockStore::customSummary(const AdBlockCustomList *custom,
                                 bool sdpGraceActive) const {
    if (!custom || custom->names.empty())
        return Json();
    const std::string currentMode = mode();
    const std::vector<std::string> own = adBlockOwnList(
        currentMode == "off" ? "ads" : currentMode, country(), serverHost(),
        sdpGraceActive);
    size_t shared = 0;
    for (const std::string &name : custom->names)
        if (std::find(own.begin(), own.end(), name) != own.end())
            ++shared;
    const std::vector<std::string> blocked = currentMode == "off"
        ? std::vector<std::string>()
        : adBlockList(currentMode, country(), serverHost(), sdpGraceActive, custom);
    Json result = Json::object();
    result.set("mode", Json::text(custom->mode));
    result.set("enabled", Json::flag(custom->enabled));
    result.set("file", custom->file.empty() ? Json() : Json::text(custom->file));
    result.set("count", Json::numeric(static_cast<double>(custom->names.size())));
    result.set("updated", custom->updated.empty() ? Json() : Json::text(custom->updated));
    result.set("shared", Json::numeric(static_cast<double>(shared)));
    result.set("ownCount", Json::numeric(static_cast<double>(own.size())));
    result.set("blocked", Json::numeric(static_cast<double>(blocked.size())));
    return result;
}

Json AdBlockStore::privacyState(bool sdpGraceActive) const {
    const std::string currentMode = mode();
    const std::string currentCountry = country();
    const std::string currentHost = serverHost();
    const AdBlockCustomList custom = customHosts();
    const std::vector<std::string> full = adBlockOwnList(
        "full", currentCountry, currentHost, sdpGraceActive);
    const std::vector<std::string> ads = adBlockAds(currentCountry);
    Json platform = Json::array();
    for (const std::string &name : adBlockPlatform(
             currentCountry, currentHost, sdpGraceActive))
        platform.add(Json::text(name));
    Json result = Json::object();
    result.set("enabled", Json::flag(currentMode != "off" && tableMounted()));
    result.set("mode", Json::text(tableMounted() ? currentMode : "off"));
    result.set("count", Json::numeric(static_cast<double>(full.size())));
    result.set("adCount", Json::numeric(static_cast<double>(ads.size())));
    result.set("platform", platform);
    result.set("custom", customSummary(custom.names.empty() ? nullptr : &custom,
                                        sdpGraceActive));
    return result;
}

std::vector<std::string> AdBlockStore::currentForeignHostsLines() const {
    return adBlockForeignHostsLines(
        readText(rootedSystemPath(kHostsPath)),
        adBlockManagedNames(country(), serverHost()));
}

bool AdBlockStore::writeHostsTable(const std::string &currentMode,
                                   bool sdpGraceActive,
                                   const std::vector<std::string> &foreign,
                                   std::string *error) const {
    const AdBlockCustomList loadedCustom = customHosts();
    const AdBlockCustomList *custom = loadedCustom.names.empty() ? nullptr : &loadedCustom;
    const std::vector<std::string> local = localLookupAddresses();
    localNetworkKey_.clear();
    for (const std::string &address : local)
        localNetworkKey_ += address + ",";
    const std::string contents = adBlockHostsTable(
        currentMode, country(), serverHost(), sdpGraceActive,
        tvUpdatesBlocked(), custom, foreign, local);
    return writeInPlace(dataPath(kTableFilename), contents, error);
}

bool AdBlockStore::applyHostsTable(bool sdpGraceActive, std::string *error) const {
    const std::string currentMode = mode();
    const bool mounted = tableMounted();
    const std::string hostsPath = rootedSystemPath(kHostsPath);
    const std::string tablePath = dataPath(kTableFilename);
    if (currentMode != "off" || tvUpdatesBlocked() || localLookups()) {
        if (!writeHostsTable(currentMode, sdpGraceActive,
                             currentForeignHostsLines(), error)) {
            if (error)
                *error = "could not write the hosts table: " + *error;
            return false;
        }
        if (mounted)
            return true;
        if (!mountOperations_.bind(tablePath, hostsPath)) {
            if (error)
                *error = "could not mount the hosts table";
            return false;
        }
        return true;
    }

    const std::vector<std::string> foreign = mounted
        ? currentForeignHostsLines() : std::vector<std::string>();
    const auto settle = [&]() {
        const std::vector<std::string> below = currentForeignHostsLines();
        bool lost = false;
        for (const std::string &line : foreign)
            if (std::find(below.begin(), below.end(), line) == below.end()) {
                lost = true;
                break;
            }
        if (!lost)
            return true;
        return mountOperations_.bind(tablePath, hostsPath);
    };
    const auto liftHomebrewHosts = [&]() {
        const std::string current = readText(hostsPath);
        if (current.find("webosbrew startup script") != std::string::npos)
            (void)mountOperations_.unmount(hostsPath);
        return settle();
    };
    if (!mounted)
        return liftHomebrewHosts();
    if (!foreign.empty() && !writeHostsTable("off", sdpGraceActive, foreign, error)) {
        if (error)
            *error = "could not write the hosts table: " + *error;
        return false;
    }
    (void)mountOperations_.unmount(hostsPath);
    if (!liftHomebrewHosts()) {
        if (error)
            *error = "could not mount the hosts table";
        return false;
    }
    return true;
}

bool AdBlockStore::restoreAtBoot(bool sdpGraceActive, std::string *error) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (mode() == "off" && !tvUpdatesBlocked() && !localLookups())
        return true;
    if (!writeHostsTable(mode(), sdpGraceActive, currentForeignHostsLines(), error)) {
        if (error)
            *error = "could not write the hosts table: " + *error;
        return false;
    }
    if (tableMounted())
        return true;
    if (!mountOperations_.bind(dataPath(kTableFilename),
                               rootedSystemPath(kHostsPath))) {
        if (error)
            *error = "could not mount the hosts table";
        return false;
    }
    return true;
}

bool AdBlockStore::refreshMountedTable(bool sdpGraceActive, std::string *error) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!tableMounted())
        return true;
    return writeHostsTable(mode(), sdpGraceActive, currentForeignHostsLines(), error);
}

Json AdBlockStore::setMode(const std::string &requestedMode,
                           bool sdpGraceActive) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (requestedMode != "off" && requestedMode != "ads" && requestedMode != "full")
        return failure("invalid ad-block mode");
    std::string error;
    if (requestedMode == "off") {
        if (::unlink(dataPath(kEnabledFilename).c_str()) != 0 && errno != ENOENT)
            return failure("could not save the ad block setting: " +
                           std::string(std::strerror(errno)));
    } else if (!writeInPlace(dataPath(kEnabledFilename), requestedMode, &error)) {
        return failure("could not save the ad block setting: " + error);
    }
    if (!applyHostsTable(sdpGraceActive, &error))
        return failure(error);
    Json result = Json::object();
    result.set("ok", Json::flag(true));
    result.set("enabled", Json::flag(mode() != "off" && tableMounted()));
    result.set("mode", Json::text(mode()));
    return result;
}

Json AdBlockStore::setTvUpdatesBlocked(bool blocked, bool sdpGraceActive) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::string flagPath = rootedSystemPath(kUpdatesFlagPath);
    std::string error;
    if (blocked) {
        if (!writeInPlace(flagPath, std::string(), &error))
            return failure("could not save the update setting: " + error);
    } else if (::unlink(flagPath.c_str()) != 0 && errno != ENOENT) {
        return failure("could not save the update setting: " +
                       std::string(std::strerror(errno)));
    }
    if (!applyHostsTable(sdpGraceActive, &error))
        return failure(error);
    Json result = Json::object();
    result.set("ok", Json::flag(true));
    result.set("tvUpdatesBlocked", Json::flag(tvUpdatesBlocked()));
    return result;
}

Json AdBlockStore::setLocalLookups(bool on, bool sdpGraceActive) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::string flagPath = dataPath(kLocalLookupsFilename);
    const bool before = localLookups();
    std::string error;
    const auto saveFlag = [&](bool wanted) {
        if (wanted)
            return writeInPlace(flagPath, std::string(), &error);
        if (::unlink(flagPath.c_str()) != 0 && errno != ENOENT) {
            error = std::strerror(errno);
            return false;
        }
        return true;
    };
    if (!saveFlag(on))
        return failure("could not save the setting: " + error);
    if (!applyHostsTable(sdpGraceActive, &error)) {
        // Put the flag back, or the dashboards would show a switch that did nothing.
        std::string ignored;
        (void)(before ? writeInPlace(flagPath, std::string(), &ignored)
                      : (::unlink(flagPath.c_str()) == 0 || errno == ENOENT));
        return failure(error);
    }
    Json result = Json::object();
    result.set("ok", Json::flag(true));
    result.set("localLookups", Json::flag(localLookups()));
    return result;
}

bool AdBlockStore::refreshForNetworkChange(bool sdpGraceActive,
                                           std::string *error) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!localLookups() || !tableMounted())
        return true;
    std::string key;
    for (const std::string &address : localLookupAddresses())
        key += address + ",";
    if (key == localNetworkKey_)
        return true;
    return writeHostsTable(mode(), sdpGraceActive, currentForeignHostsLines(), error);
}

bool AdBlockStore::applyCustomHosts(bool sdpGraceActive, std::string *error) const {
    if (mode() == "off" || !tableMounted())
        return true;
    return applyHostsTable(sdpGraceActive, error);
}

Json AdBlockStore::setCustomHosts(const Json &options, bool sdpGraceActive) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string error;
    size_t skipped = 0;
    const auto respond = [&](bool ok, const std::string &failureMessage) {
        Json result = Json::object();
        result.set("ok", Json::flag(ok));
        if (!ok)
            result.set("error", Json::text(failureMessage));
        const AdBlockCustomList current = customHosts();
        result.set("custom", customSummary(current.names.empty() ? nullptr : &current,
                                             sdpGraceActive));
        result.set("skipped", Json::numeric(static_cast<double>(skipped)));
        return result;
    };
    if (javascriptTruthy(options["clear"])) {
        if (::unlink(dataPath(kCustomFilename).c_str()) != 0 && errno != ENOENT)
            return respond(false, "could not save your hosts list: " +
                                  std::string(std::strerror(errno)));
        if (!applyCustomHosts(sdpGraceActive, &error))
            return respond(false, error);
        return respond(true, std::string());
    }

    AdBlockCustomList kept = customHosts();
    const bool enabledOnly = options["enabled"].isBoolean() &&
        !options.has("mode") && !options.has("text");
    std::string requestedMode = options["mode"].string();
    if (enabledOnly) {
        if (kept.names.empty())
            return respond(false, "no hosts list has been uploaded");
        requestedMode = kept.mode;
    }
    if (requestedMode != "add" && requestedMode != "replace")
        return respond(false, "mode must be add or replace");

    std::vector<std::string> names;
    const bool uploaded = options["text"].isString();
    if (uploaded) {
        const AdBlockParseResult parsed = parseAdBlockCustomHosts(options["text"].string());
        names = parsed.names;
        skipped = parsed.skipped;
        if (names.empty())
            return respond(false,
                "No host names found in the file. A hosts file has lines such as "
                "0.0.0.0 ads.example.com, and a plain list has one name on each line.");
        if (names.size() > 10000)
            return respond(false, "the file has " + std::to_string(names.size()) +
                                  " names; the most the TV takes is 10000");
    } else {
        if (kept.names.empty())
            return respond(false, "no hosts list has been uploaded");
        names = kept.names;
    }

    AdBlockCustomList saved;
    saved.mode = requestedMode;
    saved.names = names;
    saved.enabled = uploaded ? true
        : options["enabled"].isBoolean() ? options["enabled"].boolean()
                                         : kept.enabled;
    saved.file = uploaded ? cleanFilename(options["file"]) : kept.file;
    saved.updated = uploaded ? nowIso8601() : kept.updated;
    Json document = Json::object();
    document.set("mode", Json::text(saved.mode));
    Json serializedNames = Json::array();
    for (const std::string &name : saved.names)
        serializedNames.add(Json::text(name));
    document.set("names", serializedNames);
    document.set("enabled", Json::flag(saved.enabled));
    document.set("file", saved.file.empty() ? Json() : Json::text(saved.file));
    document.set("updated", saved.updated.empty() ? Json() : Json::text(saved.updated));
    if (!writeAtomic(dataPath(kCustomFilename), document.dump(), &error))
        return respond(false, "could not save your hosts list: " + error);
    if (!applyCustomHosts(sdpGraceActive, &error))
        return respond(false, error);
    return respond(true, std::string());
}

const std::string &AdBlockStore::dataRoot() const { return dataRoot_; }
const std::string &AdBlockStore::systemRoot() const { return systemRoot_; }

} // namespace service
} // namespace nativetv
