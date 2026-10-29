#include "daemon/http_frontdoor.hpp"
#include "daemon/app_elevation_store.hpp"
#include "daemon/app_elevation_executor.hpp"
#include "daemon/app_install_executor.hpp"
#include "daemon/install_job_store.hpp"
#include "daemon/adblock_manager.hpp"
#include "daemon/app_tile_manager.hpp"
#include "daemon/backup_restore.hpp"
#include "core/json.hpp"
#include "daemon/child_process.hpp"
#include "daemon/devtools_firewall.hpp"
#include "daemon/event_loop.hpp"
#include "daemon/home_screen_manager.hpp"
#include "daemon/home_restore_manager.hpp"
#include "daemon/home_takeover_lifecycle.hpp"
#include "daemon/install_jobs.hpp"
#include "daemon/screensaver_control.hpp"
#include "daemon/home_orchestrator.hpp"
#include "daemon/home_probe_gate.hpp"
#include "daemon/stats_collector.hpp"
#include "daemon/mqtt_publish_payload.hpp"
#include "daemon/screensaver_manager.hpp"
#include "daemon/tv_app_manager.hpp"
#include "daemon/sam_rescan_manager.hpp"
#include "daemon/game_frame_rate.hpp"
#include "daemon/ha_entity_catalogue.hpp"
#include "daemon/apps_state.hpp"
#include "daemon/http_caps.hpp"
#include "daemon/http_csrf.hpp"
#include "daemon/hdmi_seen.hpp"
#include "daemon/last_app.hpp"
#include "daemon/json_file.hpp"
#include "daemon/luna.hpp"
#include "daemon/logger.hpp"
#include "daemon/mqtt_setup.hpp"
#include "daemon/screen_time_enforcer.hpp"
#include "daemon/state_subscriptions.hpp"
#include "domain/names.hpp"
#include "domain/say.hpp"
#include "service/hdmi_inputs.hpp"
#include "service/ntp.hpp"
#include "service/kernel_files.hpp"
#include "service/app_icons.hpp"
#include "service/app_ids.hpp"
#include "service/app_list.hpp"
#include "service/app_package_inspector.hpp"
#include "service/app_install_preview.hpp"
#include "service/app_install_space.hpp"
#include "service/app_package_fetcher.hpp"
#include "service/app_package_policy.hpp"
#include "service/logs.hpp"
#include "service/lg_settings.hpp"
#include "service/process_metrics.hpp"
#include "service/prometheus.hpp"
#include "service/piccap.hpp"
#include "service/quick_access.hpp"
#include "service/preload.hpp"
#include "service/control_commands.hpp"
#include "service/config_settings.hpp"
#include "service/remote_input.hpp"
#include "service/remote_keys.hpp"
#include "service/screen_time.hpp"
#include "service/oled_care.hpp"
#include "service/privacy_consent.hpp"
#include "service/privacy_service.hpp"
#include "service/ssh_keys.hpp"
#include "service/background_services.hpp"
#include "service/history.hpp"
#include "service/history_recorder.hpp"
#include "service/home_channels.hpp"
#include "service/home_catalogue.hpp"
#include "service/home_guide.hpp"
#include "service/home_now.hpp"
#include "service/home_wallpapers.hpp"
#include "service/home_recent_apps.hpp"
#include "service/home_recent_history.hpp"
#include "service/home_profiles.hpp"
#include "service/home_status.hpp"
#include "service/saved_pages.hpp"
#include "service/backup.hpp"
#include "service/preload.hpp"
#include "service/system_metrics.hpp"
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <algorithm>
#include <arpa/inet.h>
#include <netdb.h>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <fcntl.h>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <regex>
#include <signal.h>
#include <set>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <vector>

namespace nativetv {
namespace daemon {
namespace {
void sendSetupError(HttpExchange &exchange, int status, const std::string &message,
                    bool closeConnection = false) {
    std::vector<HttpHeader> headers = {{"Content-Type", "application/json"},
                                       {"Cache-Control", "no-store"},
                                       {"X-Content-Type-Options", "nosniff"}};
    if (closeConnection)
        headers.push_back({"Connection", "close"});
    (void)exchange.respond(status, headers, "{\"ok\":false,\"error\":\"" + message + "\"}");
}

void sendInstallError(HttpExchange &exchange, int status, const std::string &message,
                      bool closeConnection = false) {
    Json failure = Json::object();
    failure.set("ok", Json::flag(false));
    failure.set("error", Json::text(message));
    std::vector<HttpHeader> headers = {{"Content-Type", "application/json; charset=utf-8"},
                                       {"Cache-Control", "no-store"},
                                       {"X-Content-Type-Options", "nosniff"}};
    if (closeConnection) headers.push_back({"Connection", "close"});
    (void)exchange.respond(status, headers, failure.dump());
}

std::string appUploadTooBig() {
    Json variables = Json::object();
    variables.set("limit", Json::text("512 MB"));
    return say::msg("srv.install.tooBig",
                    "the package is larger than the {limit} limit", variables);
}

bool appInstallJsTruthy(const Json &value) {
    if (!value.valid()) return false;
    if (value.isBoolean()) return value.boolean();
    if (value.isNumber()) return value.number() != 0;
    if (value.isString()) return !value.string().empty();
    return value.isArray() || value.isObject();
}

bool appInstallSameStringNames(const Json &provided, const Json &expected) {
    if (!provided.isArray() || !expected.isArray() ||
        provided.size() != expected.size())
        return false;
    std::vector<std::string> left;
    std::vector<std::string> right;
    for (const Json &value : provided.items()) {
        if (!value.isString()) return false;
        left.push_back(value.string());
    }
    for (const Json &value : expected.items()) {
        if (!value.isString()) return false;
        right.push_back(value.string());
    }
    std::sort(left.begin(), left.end());
    std::sort(right.begin(), right.end());
    return left == right;
}

std::string rootedSystemPath(const std::string &root, const std::string &path) {
    if (root.empty() || root == "/") return path;
    size_t end = root.size();
    while (end > 1 && root[end - 1] == '/') --end;
    return root.substr(0, end) + path;
}

bool appInstallElevationAvailable(const std::string &systemRoot) {
    const char *configured = std::getenv("TVWEB_ELEVATE_SERVICE");
    const std::string path = configured && configured[0]
        ? configured
        : rootedSystemPath(systemRoot,
            "/media/developer/apps/usr/palm/services/org.webosbrew.hbchannel.service/elevate-service");
    return access(path.c_str(), F_OK) == 0;
}

bool appInstallPreviewIdsAreSafe(const Json &input) {
    if (!input.isObject() || !input["package"].isString() ||
        !service::appPackageIdIsSafe(input["package"].string()) ||
        !input["apps"].isArray() || input["apps"].size() > 20000)
        return false;
    for (const Json &app : input["apps"].items())
        if (!app.isObject() || !app["id"].isString() ||
            !service::appPackageIdIsSafe(app["id"].string()))
            return false;
    return true;
}

bool appInstallStoreInstalled(const Json &input, const std::string &systemRoot) {
    const auto isStoreApp = [&systemRoot](const std::string &id) {
        const std::string path = rootedSystemPath(systemRoot,
            "/media/cryptofs/apps/usr/palm/applications/" + id + "/appinfo.json");
        return access(path.c_str(), F_OK) == 0;
    };
    if (isStoreApp(input["package"].string())) return true;
    for (const Json &app : input["apps"].items()) {
        const std::string id = app["id"].string();
        if (isStoreApp(id)) return true;
    }
    return false;
}

bool serviceMenuAppPresent(const std::string &systemRoot) {
    const std::string root = systemRoot.empty() ? "/" : systemRoot;
    const std::string appPath = root[root.size() - 1] == '/'
        ? root + "usr/palm/applications/com.webos.app.factorywin"
        : root + "/usr/palm/applications/com.webos.app.factorywin";
    return access(appPath.c_str(), F_OK) == 0;
}

// JsonFile::load() brings a missing file back from its .last-good copy, so a
// record deleted on purpose has to take the copy with it, or the next load (the
// log watcher makes one every 200 ms) puts the old record back.
int unlinkJsonFile(const std::string &path) {
    const int result = unlink(path.c_str());
    const int kept = errno;
    (void)unlink((path + ".last-good").c_str());
    errno = kept;
    return result;
}

bool readSetupPayload(HttpExchange &exchange, Json &payload,
                      size_t maximumBytes = 1024) {
    const HttpRequest &request = exchange.request();
    if (!isLoopbackPeer(request.remoteAddress)) {
        sendSetupError(exchange, 403, "TV-local setup action required", true);
        return false;
    }
    if (request.method != "POST") {
        sendSetupError(exchange, 405, "method not allowed", true);
        return false;
    }
    if (!exchange.setBodyLimit(maximumBytes)) {
        sendSetupError(exchange, 413, "request body too large", true);
        return false;
    }
    std::string body;
    char buffer[512];
    for (;;) {
        const int count = exchange.readBody(buffer, sizeof(buffer));
        if (count == 0)
            break;
        if (count < 0) {
            sendSetupError(exchange, exchange.bodyLimitExceeded() ? 413 : 400,
                           exchange.bodyLimitExceeded() ? "request body too large"
                                                        : "invalid request body",
                           true);
            return false;
        }
        body.append(buffer, static_cast<size_t>(count));
    }
    payload = Json::parse(body);
    if (!payload.isObject()) {
        sendSetupError(exchange, 400, "malformed JSON");
        return false;
    }
    return true;
}

Json idleAppInstallSnapshot() {
    Json snapshot = Json::object();
    snapshot.set("state", Json::text("idle"));
    snapshot.set("jobId", Json());
    snapshot.set("source", Json());
    snapshot.set("appId", Json());
    snapshot.set("progress", Json());
    snapshot.set("preview", Json());
    snapshot.set("error", Json());
    snapshot.set("result", Json());
    return snapshot;
}

bool validateAppInstallSnapshot(const Json &snapshot, std::string *error) {
    const Json state = snapshot["state"];
    const std::string stateName = state.string();
    const char *validStates[] = {
        "idle", "downloading", "verifying", "awaiting-confirm", "installing",
        "elevating", "installed", "error", "interrupted"
    };
    bool knownState = false;
    for (const char *validState : validStates) {
        if (stateName == validState) {
            knownState = true;
            break;
        }
    }
    if (!snapshot.isObject() || !knownState || !state.isString() ||
        !snapshot.has("jobId") || !snapshot.has("progress") ||
        !snapshot.has("preview") || !snapshot.has("error") ||
        !snapshot.has("result")) {
        if (error) *error = "installer snapshot is incomplete";
        return false;
    }
    const auto optionalString = [&snapshot](const std::string &key, size_t maximum) {
        if (!snapshot.has(key)) return true;
        const Json value = snapshot[key];
        return !value.valid() || (value.isString() && value.string().size() <= maximum);
    };
    const auto nullableObject = [&snapshot](const std::string &key) {
        const Json value = snapshot[key];
        return !value.valid() || value.isObject();
    };
    if (!optionalString("jobId", 128) || !optionalString("source", 32) ||
        !optionalString("appId", 128) || !optionalString("repo", 2048) ||
        !nullableObject("progress") || !nullableObject("preview") ||
        !nullableObject("result") || !optionalString("error", 4096) ||
        snapshot.dump().size() > 1024 * 1024) {
        if (error) *error = "installer snapshot has invalid fields or exceeds the size limit";
        return false;
    }
    return true;
}

bool validateAppInstallStartedSnapshot(const Json &snapshot, std::string *error) {
    if (!validateAppInstallSnapshot(snapshot, error)) return false;
    if (!snapshot["jobId"].isString() || snapshot["jobId"].string().empty() ||
        snapshot["state"].string() == "idle") {
        if (error) *error = "installer start returned an idle snapshot";
        return false;
    }
    return true;
}

bool appInstallSnapshotBusy(const Json &snapshot) {
    const std::string state = snapshot["state"].string();
    return state == "downloading" || state == "verifying" ||
           state == "awaiting-confirm" || state == "installing" ||
           state == "elevating";
}

bool appInstallSnapshotWorking(const Json &snapshot) {
    const std::string state = snapshot["state"].string();
    return state == "downloading" || state == "verifying" ||
           state == "installing" || state == "elevating";
}

std::string appInstallFormatMb(uint64_t bytes) {
    std::string value = compactNumber(
        std::round(static_cast<double>(bytes) / 104857.6) / 10.0, 1);
    if (value.size() >= 2 && value.compare(value.size() - 2, 2, ".0") == 0)
        value.erase(value.size() - 2);
    return value + " MB";
}

bool setupJsonContentType(const HttpRequest &request) {
    std::string contentType = request.header("content-type");
    std::transform(contentType.begin(), contentType.end(), contentType.begin(),
                   [](unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });
    return contentType.compare(0, 16, "application/json") == 0;
}

bool readAppTileStateFiles(const std::string &hiddenPath,
                           const std::string &flagPath,
                           std::set<std::string> &hiddenIds,
                           bool &enabled, std::string *error) {
    hiddenIds.clear();
    enabled = false;
    std::string hiddenText;
    const int hiddenDescriptor = open(hiddenPath.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (hiddenDescriptor >= 0) {
        struct stat info = {};
        if (fstat(hiddenDescriptor, &info) != 0 || !S_ISREG(info.st_mode) ||
            info.st_size < 0 || info.st_size > 1024 * 1024) {
            if (error) *error = "hidden app list is not a regular file or exceeds the size limit";
            close(hiddenDescriptor);
            return false;
        }
        char buffer[16384];
        for (;;) {
            const ssize_t count = read(hiddenDescriptor, buffer, sizeof(buffer));
            if (count < 0 && errno == EINTR) continue;
            if (count < 0) {
                if (error) *error = "could not read hidden app list: " +
                                    std::string(std::strerror(errno));
                close(hiddenDescriptor);
                return false;
            }
            if (count == 0) break;
            if (hiddenText.size() + static_cast<size_t>(count) > 1024 * 1024) {
                if (error) *error = "hidden app list exceeds the size limit";
                close(hiddenDescriptor);
                return false;
            }
            hiddenText.append(buffer, static_cast<size_t>(count));
        }
        close(hiddenDescriptor);
    } else if (errno != ENOENT) {
        if (error) *error = "could not open hidden app list: " +
                            std::string(std::strerror(errno));
        return false;
    }

    size_t start = 0;
    while (start <= hiddenText.size()) {
        const size_t end = hiddenText.find('\n', start);
        std::string id = hiddenText.substr(start, end == std::string::npos
            ? std::string::npos : end - start);
        const size_t first = id.find_first_not_of(" \t\r");
        const size_t last = id.find_last_not_of(" \t\r");
        if (first != std::string::npos) {
            id = id.substr(first, last - first + 1);
            if (appTileIdIsValid(id) && !appTileIdIsProtected(id))
                hiddenIds.insert(id);
            if (hiddenIds.size() > 4096) {
                if (error) *error = "too many hidden apps";
                return false;
            }
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }

    const int flagDescriptor = open(flagPath.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (flagDescriptor >= 0) {
        struct stat info = {};
        if (fstat(flagDescriptor, &info) != 0 || !S_ISREG(info.st_mode) ||
            info.st_size < 0 || info.st_size > 32) {
            if (error) *error = "tile hiding flag is not a regular file or exceeds the size limit";
            close(flagDescriptor);
            return false;
        }
        char value[33] = {};
        const ssize_t count = read(flagDescriptor, value, sizeof(value) - 1);
        const int readError = errno;
        close(flagDescriptor);
        if (count < 0) {
            if (error) *error = "could not read tile hiding flag: " +
                                std::string(std::strerror(readError));
            return false;
        }
        std::string flag(value, static_cast<size_t>(count));
        const size_t first = flag.find_first_not_of(" \t\r\n");
        const size_t last = flag.find_last_not_of(" \t\r\n");
        enabled = first != std::string::npos && flag.substr(first, last - first + 1) == "1";
    } else if (errno == ENOENT) {
        enabled = !hiddenIds.empty();
    } else {
        if (error) *error = "could not open tile hiding flag: " +
                            std::string(std::strerror(errno));
        return false;
    }
    return true;
}

std::string dataFilePath(const std::string &dataRoot, const std::string &name) {
    const std::string root = dataRoot.empty() ? "/var/lib/tvweb" : dataRoot;
    return root[root.size() - 1] == '/' ? root + name : root + "/" + name;
}

bool homeTakeoverMounted(const std::string &systemRoot,
                         const std::string &dataRoot) {
    const std::string appDirectory = rootedSystemPath(
        systemRoot, "/usr/palm/applications/com.webos.app.home");
    const std::string stagedDirectory = dataFilePath(dataRoot, "home-takeover");
    struct stat appInfo = {};
    struct stat stagedInfo = {};
    return stat(appDirectory.c_str(), &appInfo) == 0 &&
           stat(stagedDirectory.c_str(), &stagedInfo) == 0 &&
           appInfo.st_dev == stagedInfo.st_dev && appInfo.st_ino == stagedInfo.st_ino;
}

std::string homeStatusWebosVersion(const std::string &systemRoot) {
    const auto readText = [&systemRoot](const std::string &path) {
        std::ifstream input(rootedSystemPath(systemRoot, path).c_str(),
                            std::ios::in | std::ios::binary);
        std::ostringstream contents;
        if (input) contents << input.rdbuf();
        return contents.str();
    };
    std::smatch match;
    static const std::regex issueVersion(
        "webOS(?: TV)?[[:space:]]+([0-9.]+)", std::regex_constants::icase);
    static const std::regex releaseVersion(
        "release[[:space:]]+([0-9.]+)", std::regex_constants::icase);
    std::string issue = readText("/etc/issue");
    if (issue.empty()) issue = readText("/etc/issue.net");
    if (std::regex_search(issue, match, issueVersion)) return match[1].str();
    const std::string release = readText("/etc/starfish-release");
    if (std::regex_search(release, match, releaseVersion))
        return match[1].str();
    return std::string();
}

bool sameOriginRequest(const HttpRequest &request) {
    std::string origin = request.header("origin");
    if (origin.empty()) return true;
    if (origin.compare(0, 7, "http://") == 0)
        origin.erase(0, 7);
    else if (origin.compare(0, 8, "https://") == 0)
        origin.erase(0, 8);
    return origin == request.header("host");
}

std::string normalizedHostName(const std::string &header) {
    std::string host = header;
    if (!host.empty() && host[0] == '[') {
        const size_t closing = host.find(']');
        if (closing == std::string::npos)
            return std::string();
        const std::string suffix = host.substr(closing + 1);
        if (!suffix.empty() && suffix[0] != ':')
            return std::string();
        for (size_t index = 1; index < suffix.size(); ++index) {
            if (!std::isdigit(static_cast<unsigned char>(suffix[index])))
                return std::string();
        }
        host = host.substr(1, closing - 1);
    } else {
        const size_t colon = host.rfind(':');
        if (colon != std::string::npos && host.find(':') == colon)
            host.erase(colon);
    }
    std::transform(host.begin(), host.end(), host.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    if (!host.empty() && host.back() == '.')
        host.pop_back();
    return host;
}

bool isIpLiteral(const std::string &host) {
    unsigned char address[sizeof(struct in6_addr)] = {};
    return inet_pton(AF_INET, host.c_str(), address) == 1 ||
           inet_pton(AF_INET6, host.c_str(), address) == 1;
}

bool appInstallHostAllowed(const HttpRequest &request, const Json &config) {
    const std::string host = normalizedHostName(request.header("host"));
    if (host.empty())
        return false;
    if (host == "localhost" || isIpLiteral(host))
        return true;
    const Json hosts = config["apps"]["hosts"];
    if (!hosts.isArray())
        return false;
    for (const Json &allowed : hosts.items()) {
        if (!allowed.isString())
            continue;
        std::string name = allowed.string();
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
        if (!name.empty() && name.back() == '.')
            name.pop_back();
        if (name == host)
            return true;
    }
    return false;
}

std::string quickAccessStringValue(const Json &value) {
    if (!value.valid())
        return "";
    if (value.isString())
        return value.string();
    if (value.isNumber())
        return compactNumber(value.number(), 15);
    if (value.isBoolean())
        return value.boolean() ? "true" : "false";
    if (value.isObject())
        return "[object Object]";
    if (value.isArray()) {
        std::string result;
        const std::vector<Json> values = value.items();
        for (size_t index = 0; index < values.size(); ++index) {
            if (index)
                result.push_back(',');
            result += quickAccessStringValue(values[index]);
        }
        return result;
    }
    return "";
}

bool exceedsUtf16CodeUnitLimit(const std::string &body, size_t maximumCodeUnits) {
    size_t codeUnits = 0;
    for (size_t index = 0; index < body.size();) {
        const unsigned char lead = static_cast<unsigned char>(body[index]);
        size_t byteCount = 1;
        unsigned long codePoint = lead;
        if (lead >= 0xc2 && lead <= 0xdf && index + 1 < body.size()) {
            const unsigned char second = static_cast<unsigned char>(body[index + 1]);
            if ((second & 0xc0) == 0x80) {
                byteCount = 2;
                codePoint = ((lead & 0x1f) << 6) | (second & 0x3f);
            }
        } else if (lead >= 0xe0 && lead <= 0xef && index + 2 < body.size()) {
            const unsigned char second = static_cast<unsigned char>(body[index + 1]);
            const unsigned char third = static_cast<unsigned char>(body[index + 2]);
            if ((second & 0xc0) == 0x80 && (third & 0xc0) == 0x80 &&
                !(lead == 0xe0 && second < 0xa0) &&
                !(lead == 0xed && second >= 0xa0)) {
                byteCount = 3;
                codePoint = ((lead & 0x0f) << 12) | ((second & 0x3f) << 6) |
                            (third & 0x3f);
            }
        } else if (lead >= 0xf0 && lead <= 0xf4 && index + 3 < body.size()) {
            const unsigned char second = static_cast<unsigned char>(body[index + 1]);
            const unsigned char third = static_cast<unsigned char>(body[index + 2]);
            const unsigned char fourth = static_cast<unsigned char>(body[index + 3]);
            if ((second & 0xc0) == 0x80 && (third & 0xc0) == 0x80 &&
                (fourth & 0xc0) == 0x80 &&
                !(lead == 0xf0 && second < 0x90) &&
                !(lead == 0xf4 && second >= 0x90)) {
                byteCount = 4;
                codePoint = ((lead & 0x07) << 18) | ((second & 0x3f) << 12) |
                            ((third & 0x3f) << 6) | (fourth & 0x3f);
            }
        }
        codeUnits += codePoint > 0xffff ? 2 : 1;
        if (codeUnits > maximumCodeUnits)
            return true;
        index += byteCount;
    }
    return false;
}

bool readAppInstallJson(HttpExchange &exchange, Json &payload,
                        bool allowPrimitives = false) {
    if (rejectCrossSitePost(exchange))
        return false;
    if (!exchange.setBodyLimit(32768)) {
        sendInstallError(exchange, 413, "request body too large", true);
        return false;
    }
    std::string body;
    char buffer[1024];
    for (;;) {
        const int count = exchange.readBody(buffer, sizeof(buffer));
        if (count == 0)
            break;
        if (count < 0) {
            sendInstallError(exchange, exchange.bodyLimitExceeded() ? 413 : 400,
                             exchange.bodyLimitExceeded() ? "request body too large"
                                                          : "invalid request body",
                             exchange.bodyLimitExceeded());
            return false;
        }
        body.append(buffer, static_cast<size_t>(count));
    }
    if (exceedsUtf16CodeUnitLimit(body, 8192)) {
        sendInstallError(exchange, 413, "request body too large", true);
        return false;
    }
    payload = Json::parse(body);
    if (!payload.valid() || (!allowPrimitives && !payload.isObject() &&
                             !payload.isArray())) {
        sendInstallError(exchange, 400, "malformed JSON");
        return false;
    }
    return true;
}

bool setupFlag(const Json &value) {
    if (value.isBoolean())
        return value.boolean();
    if (!value.isString())
        return false;
    return value.string() == "1" || value.string() == "true";
}

bool setupDemo(const ServerConfig &config, const HttpRequest &request, const Json &payload) {
    if (setupFlag(config.document["emulateSetup"]) || setupFlag(payload["demo"]))
        return true;
    size_t start = 0;
    while (start <= request.query.size()) {
        const size_t end = request.query.find('&', start);
        const size_t length = (end == std::string::npos ? request.query.size() : end) - start;
        const std::string parameter = request.query.substr(start, length);
        const size_t equals = parameter.find('=');
        if (parameter.substr(0, equals) == "demo" && equals != std::string::npos) {
            const std::string value = parameter.substr(equals + 1);
            if (value == "1" || value == "true")
                return true;
        }
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    return false;
}

bool getSetupPayload(HttpExchange &exchange, const Json *providedPayload, Json &payload) {
    if (providedPayload) {
        payload = *providedPayload;
        return true;
    }
    return readSetupPayload(exchange, payload);
}

bool setupNetworkIsOpen(const std::string &host) {
    return host.compare(0, 4, "127.") != 0 && host != "::1" && host != "localhost";
}

bool setupTruthy(const Json &value) {
    if (value.isBoolean())
        return value.boolean();
    if (value.isNumber())
        return value.number() != 0;
    if (value.isString())
        return !value.string().empty();
    return value.isArray() || value.isObject();
}

int queryHexValue(char value) {
    if (value >= '0' && value <= '9')
        return value - '0';
    if (value >= 'a' && value <= 'f')
        return value - 'a' + 10;
    if (value >= 'A' && value <= 'F')
        return value - 'A' + 10;
    return -1;
}

bool decodeQueryValue(const std::string &input, std::string &output) {
    output.clear();
    for (size_t index = 0; index < input.size(); ++index) {
        if (input[index] == '+') {
            output.push_back(' ');
        } else if (input[index] == '%') {
            if (index + 2 >= input.size())
                return false;
            const int high = queryHexValue(input[index + 1]);
            const int low = queryHexValue(input[index + 2]);
            if (high < 0 || low < 0)
                return false;
            output.push_back(static_cast<char>((high << 4) | low));
            index += 2;
        } else {
            output.push_back(input[index]);
        }
    }
    return true;
}

// Started by tvwebctl with no Node at all (TVWEB_NODE_FREE=1).
bool nodeFreeStart() {
    const char *value = std::getenv("TVWEB_NODE_FREE");
    return value && std::string(value) == "1";
}

bool queryParameter(const std::string &query, const std::string &wanted,
                    std::string &value) {
    bool found = false;
    size_t start = 0;
    while (start <= query.size()) {
        const size_t end = query.find('&', start);
        const size_t pairEnd = end == std::string::npos ? query.size() : end;
        const size_t equals = query.find('=', start);
        const size_t keyEnd = equals == std::string::npos || equals > pairEnd ? pairEnd : equals;
        const size_t valueStart = keyEnd < pairEnd ? keyEnd + 1 : pairEnd;
        std::string key;
        std::string decoded;
        if (decodeQueryValue(query.substr(start, keyEnd - start), key) && key == wanted &&
            decodeQueryValue(query.substr(valueStart, pairEnd - valueStart), decoded)) {
            value = decoded;
            found = true;
        }
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    return found;
}

bool uniqueQueryParameter(const std::string &query, const std::string &wanted,
                          std::string &value) {
    unsigned matches = 0;
    bool valid = true;
    size_t start = 0;
    while (start <= query.size()) {
        const size_t end = query.find('&', start);
        const size_t pairEnd = end == std::string::npos ? query.size() : end;
        const size_t equals = query.find('=', start);
        const size_t keyEnd = equals == std::string::npos || equals > pairEnd ? pairEnd : equals;
        const size_t valueStart = keyEnd < pairEnd ? keyEnd + 1 : pairEnd;
        std::string key;
        std::string decoded;
        if (decodeQueryValue(query.substr(start, keyEnd - start), key) && key == wanted) {
            ++matches;
            if (!decodeQueryValue(query.substr(valueStart, pairEnd - valueStart), decoded))
                valid = false;
            else
                value = decoded;
        }
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    return valid && matches == 1;
}

std::vector<std::string> splitCommaSeparated(const std::string &value) {
    std::vector<std::string> result;
    size_t start = 0;
    while (start <= value.size()) {
        const size_t end = value.find(',', start);
        const size_t length = (end == std::string::npos ? value.size() : end) - start;
        result.push_back(value.substr(start, length));
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    return result;
}

std::string findDmesgExecutable() {
    std::vector<std::string> candidates;
    const char *pathValue = std::getenv("PATH");
    if (pathValue && *pathValue) {
        const std::string path(pathValue);
        size_t start = 0;
        while (start <= path.size()) {
            const size_t end = path.find(':', start);
            const std::string directory = path.substr(
                start, (end == std::string::npos ? path.size() : end) - start);
            candidates.push_back((directory.empty() ? "." : directory) + "/dmesg");
            if (end == std::string::npos)
                break;
            start = end + 1;
        }
    }
    candidates.push_back("/bin/dmesg");
    candidates.push_back("/usr/bin/dmesg");
    candidates.push_back("/sbin/dmesg");
    candidates.push_back("/usr/sbin/dmesg");
    for (const std::string &candidate : candidates) {
        if (access(candidate.c_str(), X_OK) == 0)
            return candidate;
    }
    return "";
}

bool allProcessesRequested(const std::string &query) {
    bool all = false;
    size_t start = 0;
    while (start <= query.size()) {
        const size_t end = query.find('&', start);
        const size_t pairEnd = end == std::string::npos ? query.size() : end;
        const size_t equals = query.find('=', start);
        const size_t keyEnd = equals == std::string::npos || equals > pairEnd ? pairEnd : equals;
        std::string key;
        std::string value;
        const size_t valueStart = keyEnd < pairEnd ? keyEnd + 1 : pairEnd;
        if (decodeQueryValue(query.substr(start, keyEnd - start), key) && key == "all" &&
            decodeQueryValue(query.substr(valueStart, pairEnd - valueStart), value))
            all = value == "1" || value == "true";
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    return all;
}

void sendMetricsJson(HttpExchange &exchange, const std::string &body) {
    (void)exchange.respond(200,
                           {{"Content-Type", "application/json"},
                            {"Cache-Control", "no-store"},
                            {"X-Content-Type-Options", "nosniff"}},
                           body);
}

std::string utcHourKey(int64_t milliseconds) {
    const time_t seconds = static_cast<time_t>(milliseconds / 1000);
    struct tm utc;
    if (!gmtime_r(&seconds, &utc))
        return std::string();
    char value[14] = {};
    return ::strftime(value, sizeof(value), "%Y-%m-%dT%H", &utc) == 0
               ? std::string()
               : std::string(value);
}

std::string utcIsoTimestamp() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const int64_t milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    const time_t seconds = static_cast<time_t>(milliseconds / 1000);
    const int remainder = static_cast<int>(milliseconds % 1000);
    struct tm utc;
    if (!gmtime_r(&seconds, &utc)) return std::string();
    char value[32] = {};
    const size_t length = ::strftime(value, sizeof(value), "%Y-%m-%dT%H:%M:%S", &utc);
    if (!length) return std::string();
    char result[40] = {};
    std::snprintf(result, sizeof(result), "%s.%03dZ", value, remainder);
    return std::string(result);
}

int roundedSeconds(double seconds) {
    if (!std::isfinite(seconds) || seconds <= 0)
        return 0;
    if (seconds >= static_cast<double>(std::numeric_limits<int>::max()))
        return std::numeric_limits<int>::max();
    return static_cast<int>(std::floor(seconds + 0.5));
}

void sendProcessMetrics(HttpExchange &exchange, bool all, const std::string &procRoot) {
    sendMetricsJson(exchange, service::collectMemory(procRoot, all).dump());
}

bool setNetworkAccess(HttpExchange &exchange, const ServerConfig &config,
                      const std::string &configPath, const HttpProxy &proxy,
                      const Json *providedPayload = nullptr) {
    if (!setupJsonContentType(exchange.request())) {
        sendSetupError(exchange, 415, "Content-Type must be application/json");
        return true;
    }
    Json payload;
    if (!getSetupPayload(exchange, providedPayload, payload))
        return true;
    if (payload["action"].string() != "network") {
        sendSetupError(exchange, 400, "invalid setup action");
        return true;
    }
    if (setupDemo(config, exchange.request(), payload)) {
        (void)exchange.respond(200,
                               {{"Content-Type", "application/json"},
                                {"Cache-Control", "no-store"},
                                {"X-Content-Type-Options", "nosniff"}},
                               "{\"ok\":true,\"restarting\":false,\"demo\":true}");
        return true;
    }
    if (!config.allowControl) {
        sendSetupError(exchange, 403, "controls disabled in config");
        return true;
    }

    const bool open = setupTruthy(payload["open"]);
    if (open == setupNetworkIsOpen(config.host)) {
        (void)exchange.respond(200,
                               {{"Content-Type", "application/json"},
                                {"Cache-Control", "no-store"},
                                {"X-Content-Type-Options", "nosniff"}},
                               "{\"ok\":true,\"restarting\":false}");
        return true;
    }

    int status = 0;
    std::string body;
    std::string error;
    if (!proxy.requestNodeRestart(true, status, body, error)) {
        sendSetupError(exchange, 502, error.empty() ? "could not check restart readiness" : error,
                       true);
        return true;
    }
    if (status != 200) {
        (void)exchange.respond(status,
                               {{"Content-Type", "application/json"},
                                {"Cache-Control", "no-store"},
                                {"X-Content-Type-Options", "nosniff"}},
                               body);
        return true;
    }

    std::string saveError;
    if (!ConfigStore(configPath).setNetworkOpen(open, &saveError)) {
        sendSetupError(exchange, 500, "could not save the setting");
        return true;
    }
    if (!proxy.requestNodeRestart(false, status, body, error)) {
        sendSetupError(exchange, 502,
                       "network setting was saved but restart request failed: " + error, true);
        return true;
    }
    (void)exchange.respond(status,
                           {{"Content-Type", "application/json"},
                            {"Cache-Control", "no-store"},
                            {"X-Content-Type-Options", "nosniff"}},
                           body);
    return true;
}

bool waitForLuna(LunaClient &luna, const std::string &uri, const Json &payload,
                 LunaReply &reply) {
    struct Result {
        std::mutex mutex;
        std::condition_variable condition;
        bool complete = false;
        LunaReply reply;
    };
    const std::shared_ptr<Result> result = std::make_shared<Result>();
    if (!luna.call(uri, payload,
                   [result](const LunaReply &value) {
                       std::lock_guard<std::mutex> lock(result->mutex);
                       result->reply = value;
                       result->complete = true;
                       result->condition.notify_one();
                   }))
        return false;
    std::unique_lock<std::mutex> lock(result->mutex);
    if (!result->condition.wait_for(lock, std::chrono::seconds(5),
                                    [&result] { return result->complete; }))
        return false;
    reply = result->reply;
    return reply.error.empty() && reply.response.isObject();
}

struct LunaBatchState {
    std::mutex mutex;
    std::condition_variable condition;
    std::vector<LunaReply> replies;
    size_t remaining = 0;
};

bool waitForLunaBatch(LunaClient &luna,
                      const std::vector<std::pair<std::string, Json>> &calls,
                      std::vector<LunaReply> &replies) {
    const std::shared_ptr<LunaBatchState> state = std::make_shared<LunaBatchState>();
    state->replies.resize(calls.size());
    state->remaining = calls.size();
    for (size_t index = 0; index < calls.size(); ++index) {
        const bool started = luna.call(
            calls[index].first, calls[index].second,
            [state, index](const LunaReply &reply) {
                std::lock_guard<std::mutex> lock(state->mutex);
                state->replies[index] = reply;
                if (state->remaining > 0) --state->remaining;
                state->condition.notify_one();
            });
        if (!started) {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (state->remaining > 0) --state->remaining;
            state->condition.notify_one();
        }
    }
    std::unique_lock<std::mutex> lock(state->mutex);
    const bool complete = state->condition.wait_for(
        lock, std::chrono::seconds(5), [state] { return state->remaining == 0; });
    replies = state->replies;
    return complete;
}

void readMediaStateForControl(LunaClient &luna, bool &acbAbsent,
                              std::string &playState, std::string &playerType) {
    playState.clear();
    playerType.clear();
    const auto truthy = [](const Json &value) {
        if (value.isBoolean()) return value.boolean();
        if (value.isNumber()) return value.number() != 0;
        if (value.isString()) return !value.string().empty();
        return value.valid();
    };
    const auto asString = [](const Json &value) {
        if (value.isString()) return value.string();
        if (value.isNumber()) return value.dump();
        if (value.isBoolean()) return value.boolean() ? std::string("true")
                                                       : std::string("false");
        return std::string();
    };

    if (!acbAbsent) {
        LunaReply acb;
        if (waitForLuna(luna, "com.webos.service.acb/getForegroundAppInfo",
                        Json::object(), acb)) {
            const Json acbs = acb.response["acbs"];
            const Json returnValue = acb.response["returnValue"];
            if (acbs.isArray() &&
                (!returnValue.isBoolean() || returnValue.boolean())) {
                const Json pipe = acbs[0];
                if (truthy(pipe["playStateNow"])) {
                    playState = asString(pipe["playStateNow"]);
                    playerType = asString(pipe["playerType"]);
                }
                return;
            }
            if (acb.response["errorText"].string().find("Service does not exist") !=
                std::string::npos)
                acbAbsent = true;
        }
    }

    LunaReply media;
    if (!waitForLuna(luna, "com.webos.media/getForegroundAppInfo",
                     Json::object(), media))
        return;
    const Json apps = media.response["foregroundAppInfo"];
    const Json returnValue = media.response["returnValue"];
    if (!apps.isArray() || (returnValue.isBoolean() && !returnValue.boolean()))
        return;
    for (const Json &app : apps.items()) {
        if (!truthy(app["playState"]))
            continue;
        playState = asString(app["playState"]);
        playerType = asString(app["type"]);
        return;
    }
}

bool waitForCommandPolicyLuna(LunaClient &luna, CommandPolicyExecutor *policy,
                              const CommandPolicyContext &context,
                              const std::string &action, const std::string &uri,
                              const Json &payload, const std::string &resource,
                              bool requiresPower, LunaReply &reply,
                              const std::string &appId = std::string(),
                              bool urgent = false,
                              std::chrono::milliseconds timeout =
                                  std::chrono::milliseconds(5000),
                              bool requiresControl = true) {
    if (!policy)
        return waitForLuna(luna, uri, payload, reply);
    struct Result {
        std::mutex mutex;
        std::condition_variable condition;
        bool complete = false;
        LunaReply reply;
    };
    const std::shared_ptr<Result> result = std::make_shared<Result>();
    CommandPolicySpec command;
    command.action = action;
    command.uri = uri;
    command.payload = payload;
    command.appId = appId;
    command.timeout = timeout;
    command.urgent = urgent;
    command.requiresPower = requiresPower;
    command.requiresControl = requiresControl;
    command.resource = resource;
    const CommandDecision decision = policy->submit(
        command, context, false, [result](const LunaReply &value) {
            std::lock_guard<std::mutex> lock(result->mutex);
            result->reply = value;
            result->complete = true;
            result->condition.notify_one();
        });
    if (decision.kind != CommandDecisionKind::Queued) {
        reply = LunaReply{};
        reply.error = decision.reason;
        return false;
    }
    std::unique_lock<std::mutex> lock(result->mutex);
    const std::chrono::milliseconds wait = timeout + std::chrono::milliseconds(1000);
    if (!result->condition.wait_for(lock, wait, [&result] { return result->complete; })) {
        reply = LunaReply{};
        reply.error = "command policy completion timed out";
        return false;
    }
    reply = result->reply;
    return !reply.timedOut && reply.error.empty() && reply.response.isObject();
}

CommandPolicyContext controlPolicyContext(const ServerConfig &config) {
    CommandPolicyContext context;
    context.allowControl = config.allowControl;
    context.allowPower = config.allowPower;
    const Json firmware = config.document["device"]["sw_version"];
    if (firmware.isString())
        context.firmwareVersion = firmware.string();
    return context;
}

bool isSetupDemo(const ServerConfig &config, const HttpRequest &request) {
    if (setupTruthy(config.document["emulateSetup"]))
        return true;
    size_t start = 0;
    while (start <= request.query.size()) {
        const size_t end = request.query.find('&', start);
        const size_t length = (end == std::string::npos ? request.query.size() : end) - start;
        const std::string parameter = request.query.substr(start, length);
        const size_t equals = parameter.find('=');
        if (parameter.substr(0, equals) == "demo" && equals != std::string::npos) {
            const std::string value = parameter.substr(equals + 1);
            if (value == "1" || value == "true")
                return true;
        }
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    return false;
}

bool setupRead(HttpExchange &exchange, const ServerConfig &config, const std::string &dataRoot,
               LunaClient &luna, const HttpProxy &proxy,
               const std::string &nativeHandoffUrl) {
    const HttpRequest &request = exchange.request();
    const bool demo = isSetupDemo(config, request);
    if (!isLoopbackPeer(request.remoteAddress) && !demo) {
        sendSetupError(exchange, 403, "only from the TV itself", true);
        return true;
    }
    if (request.method != "GET") {
        sendSetupError(exchange, 405, "GET or POST");
        return true;
    }

    Json context;
    std::string contextError;
    if (!proxy.requestNodeSetupContext(demo, context, contextError)) {
        sendSetupError(exchange, 502,
                       contextError.empty() ? "could not read setup state" : contextError, true);
        return true;
    }

    Json state = Json::object();
    state.set("ok", Json::flag(true));
    state.set("needed", Json::flag(demo || access((dataRoot + "/.setup-pending").c_str(), F_OK) == 0));
    state.set("writable", Json::flag(config.allowControl));
    state.set("network", Json::flag(setupNetworkIsOpen(config.host)));
    state.set("token", Json::flag(!config.token.empty()));
    std::string address = localIpv4Address();
    if (!address.empty()) {
        state.set("address", Json::text(address + ":" + std::to_string(config.port)));
    } else if (demo) {
        state.set("address", Json::text("192.168.1.131:" + std::to_string(config.port)));
    } else {
        state.set("address", Json());
    }
    state.set("hostname", context["hostname"]);

    const Json mqtt = config.document["mqtt"];
    Json homeAssistant = Json::object();
    homeAssistant.set("configured", Json::flag(setupTruthy(mqtt["enabled"]) &&
                                                setupTruthy(mqtt["host"])));
    homeAssistant.set("state", context["homeAssistant"]["state"]);
    if (!homeAssistant["state"].valid())
        homeAssistant.set("state", Json::text("unknown"));
    state.set("homeAssistant", homeAssistant);
    Json handoff = nativeHandoffUrl.empty() ? context["handoff"]
                                            : Json::text(nativeHandoffUrl);
    if (!handoff.valid() && demo) {
        const std::string phoneAddress = address.empty() ? "192.168.1.131" : address;
        handoff = Json::text("http://" + phoneAddress + ":" + std::to_string(config.port) +
                             "/?c=demo");
    }
    state.set("handoff", handoff);

    if (demo) {
        state.set("demo", Json::flag(true));
        state.set("alwaysReady", Json::flag(false));
    } else {
        Json descriptionRequest = Json::object();
        descriptionRequest.set("category", Json::text("general"));
        Json keys = Json::array();
        keys.add(Json::text("alwaysOn"));
        descriptionRequest.set("keys", keys);
        LunaReply descriptionReply;
        if (waitForLuna(luna, "com.webos.service.settings/getSystemSettingDesc",
                        descriptionRequest, descriptionReply) &&
            descriptionReply.response["returnValue"].boolean(true)) {
            const Json descriptions = descriptionReply.response["results"];
            const Json description = descriptions[static_cast<size_t>(0)];
            const Json visible = description["ui"]["visible"];
            const bool supported = description.valid() &&
                                   !(visible.isBoolean() && !visible.boolean());
            if (supported) {
                Json settingsRequest = Json::object();
                settingsRequest.set("category", Json::text("general"));
                settingsRequest.set("keys", keys);
                LunaReply settingsReply;
                if (waitForLuna(luna, "com.webos.service.settings/getSystemSettings",
                                settingsRequest, settingsReply) &&
                    settingsReply.response["returnValue"].boolean(true)) {
                    const Json alwaysOn = settingsReply.response["settings"]["alwaysOn"];
                    if (alwaysOn.valid()) {
                        state.set("alwaysReady",
                                  Json::flag(alwaysOn.isBoolean()
                                                 ? alwaysOn.boolean()
                                                 : alwaysOn.string() == "on"));
                    }
                }
            }
        }
    }

    (void)exchange.respond(200,
                           {{"Content-Type", "application/json"},
                            {"Cache-Control", "no-store"},
                            {"X-Content-Type-Options", "nosniff"}},
                           state.dump());
    return true;
}

bool handoffCodeFromQuery(const std::string &query, std::string &code) {
    size_t start = 0;
    while (start <= query.size()) {
        const size_t end = query.find('&', start);
        const size_t length = (end == std::string::npos ? query.size() : end) - start;
        const std::string parameter = query.substr(start, length);
        const size_t equals = parameter.find('=');
        if (parameter.substr(0, equals) == "c" && equals != std::string::npos) {
            code = parameter.substr(equals + 1);
            if (code.size() != 16)
                return false;
            for (size_t i = 0; i < code.size(); ++i) {
                if (!((code[i] >= '0' && code[i] <= '9') ||
                      (code[i] >= 'a' && code[i] <= 'f')))
                    return false;
            }
            return true;
        }
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    return false;
}

bool createHandoffCode(std::string &code) {
    int descriptor = open("/dev/urandom", O_RDONLY);
    if (descriptor < 0)
        return false;
    unsigned char bytes[8];
    size_t offset = 0;
    while (offset < sizeof(bytes)) {
        const ssize_t count = read(descriptor, bytes + offset, sizeof(bytes) - offset);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0) {
            close(descriptor);
            return false;
        }
        offset += static_cast<size_t>(count);
    }
    close(descriptor);
    static const char digits[] = "0123456789abcdef";
    code.clear();
    code.reserve(sizeof(bytes) * 2);
    for (size_t i = 0; i < sizeof(bytes); ++i) {
        code.push_back(digits[bytes[i] >> 4]);
        code.push_back(digits[bytes[i] & 0x0f]);
    }
    return true;
}

void sendExpiredHandoff(HttpExchange &exchange) {
    (void)exchange.respond(403,
                           {{"Content-Type", "text/plain; charset=utf-8"},
                            {"Cache-Control", "no-store"}},
                           "This link has expired. Start again on the TV.");
}

size_t javascriptStringLength(const std::string &value) {
    size_t units = 0;
    for (size_t i = 0; i < value.size();) {
        const unsigned char first = static_cast<unsigned char>(value[i]);
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
        bool valid = i + width <= value.size();
        for (size_t j = 1; valid && j < width; ++j) {
            const unsigned char continuation = static_cast<unsigned char>(value[i + j]);
            valid = continuation >= 0x80 && continuation <= 0xbf;
        }
        units += valid ? codeUnits : 1;
        i += valid ? width : 1;
    }
    return units;
}

bool savePhoneMqttSettings(HttpExchange &exchange, const std::string &configPath,
                           const HttpProxy &proxy, bool &consumeCode) {
    const HttpRequest &request = exchange.request();
    int status = 0;
    std::string body;
    std::string error;
    if (request.method != "POST") {
        (void)exchange.respond(404, {{"Content-Type", "text/plain; charset=utf-8"}}, "not found");
        return true;
    }
    if (request.header("content-type").compare(0, 16, "application/json") != 0) {
        sendSetupError(exchange, 415, "Content-Type must be application/json");
        return true;
    }
    if (!exchange.setBodyLimit(16384)) {
        sendSetupError(exchange, 413, "request body too large", true);
        return true;
    }
    std::string requestBody;
    char buffer[1024];
    for (;;) {
        const int count = exchange.readBody(buffer, sizeof(buffer));
        if (count == 0)
            break;
        if (count < 0) {
            sendSetupError(exchange, exchange.bodyLimitExceeded() ? 413 : 400,
                           exchange.bodyLimitExceeded() ? "request body too large"
                                                        : "invalid request body",
                           true);
            return true;
        }
        requestBody.append(buffer, static_cast<size_t>(count));
    }
    if (javascriptStringLength(requestBody) > 4096) {
        sendSetupError(exchange, 413, "request body too large", true);
        return true;
    }
    const Json payload = Json::parse(requestBody);
    if (!payload.isObject()) {
        sendSetupError(exchange, 400, "malformed JSON");
        return true;
    }

    Json context;
    if (!proxy.requestNodeSetupContext(false, context, error)) {
        sendSetupError(exchange, 502, "could not read setup settings", true);
        return true;
    }
    ConfigLoadOptions loadOptions;
    loadOptions.overridePath = configPath;
    const ConfigLoadResult loaded = ConfigLoader::load(loadOptions);
    const Json currentConfig = loaded.config.document;
    Json mqtt;
    Json device;
    if (!buildPhoneMqttSettings(payload, currentConfig, context["identity"], mqtt, device,
                                error)) {
        sendSetupError(exchange, 400, error);
        return true;
    }

    if (!proxy.requestNodeRestart(true, status, body, error)) {
        sendSetupError(exchange, 502, "could not check restart readiness", true);
        return true;
    }
    if (status != 200) {
        (void)exchange.respond(status,
                               {{"Content-Type", "application/json"},
                                {"Cache-Control", "no-store"},
                                {"X-Content-Type-Options", "nosniff"}},
                               body);
        return true;
    }

    if (!ConfigStore(configPath).setMqttSetup(mqtt, device, &error)) {
        sendSetupError(exchange, 500, "could not save the settings");
        return true;
    }
    consumeCode = true;
    if (!proxy.requestNodeRestart(false, status, body, error)) {
        sendSetupError(exchange, 502, "settings saved but server restart failed", true);
        return true;
    }
    (void)exchange.respond(200,
                           {{"Content-Type", "application/json"},
                            {"Cache-Control", "no-store"},
                            {"X-Content-Type-Options", "nosniff"}},
                           "{\"ok\":true}");
    return true;
}

bool finishSetup(HttpExchange &exchange, const ServerConfig &config,
                 const std::string &dataRoot, const Json *providedPayload = nullptr) {
    Json payload;
    if (!getSetupPayload(exchange, providedPayload, payload))
        return true;
    if (payload["action"].string() != "done") {
        sendSetupError(exchange, 400, "invalid setup action");
        return true;
    }
    if (setupDemo(config, exchange.request(), payload)) {
        (void)exchange.respond(200,
                               {{"Content-Type", "application/json"},
                                {"Cache-Control", "no-store"},
                                {"X-Content-Type-Options", "nosniff"}},
                               "{\"ok\":true,\"demo\":true}");
        return true;
    }
    if (dataRoot.empty()) {
        sendSetupError(exchange, 500, "setup storage unavailable");
        return true;
    }
    std::string marker = dataRoot;
    while (marker.size() > 1 && marker.back() == '/')
        marker.pop_back();
    marker += "/.setup-pending";
    if (unlink(marker.c_str()) != 0 && errno != ENOENT) {
        sendSetupError(exchange, 500, "could not finish setup");
        return true;
    }
    (void)exchange.respond(200,
                           {{"Content-Type", "application/json"},
                            {"Cache-Control", "no-store"},
                            {"X-Content-Type-Options", "nosniff"}},
                           "{\"ok\":true}");
    return true;
}

bool setAlwaysReady(HttpExchange &exchange, const ServerConfig &config, LunaClient &luna,
                    const Json *providedPayload = nullptr) {
    Json payload;
    if (!getSetupPayload(exchange, providedPayload, payload))
        return true;
    if (payload["action"].string() != "alwaysReady" || !payload["on"].isBoolean()) {
        sendSetupError(exchange, 400, "invalid setup action");
        return true;
    }
    if (setupDemo(config, exchange.request(), payload)) {
        (void)exchange.respond(200,
                               {{"Content-Type", "application/json"},
                                {"Cache-Control", "no-store"},
                                {"X-Content-Type-Options", "nosniff"}},
                               "{\"ok\":true,\"demo\":true}");
        return true;
    }
    if (!config.allowControl) {
        sendSetupError(exchange, 403, "controls disabled in config");
        return true;
    }

    Json settings = Json::object();
    settings.set("alwaysOn", Json::text(payload["on"].boolean() ? "on" : "off"));
    Json request = Json::object();
    request.set("category", Json::text("general"));
    request.set("settings", settings);
    struct Result {
        std::mutex mutex;
        std::condition_variable condition;
        bool complete = false;
        LunaReply reply;
    };
    const std::shared_ptr<Result> result = std::make_shared<Result>();
    const bool queued = luna.call(
        "com.webos.service.settings/setSystemSettings", request,
        [result](const LunaReply &reply) {
            std::lock_guard<std::mutex> lock(result->mutex);
            result->reply = reply;
            result->complete = true;
            result->condition.notify_one();
        },
        "", true);
    if (!queued) {
        sendSetupError(exchange, 503, "TV settings service is busy");
        return true;
    }
    std::unique_lock<std::mutex> lock(result->mutex);
    if (!result->condition.wait_for(lock, std::chrono::seconds(5),
                                    [&result] { return result->complete; })) {
        sendSetupError(exchange, 504, "TV settings request timed out");
        return true;
    }
    const bool succeeded =
        result->reply.error.empty() && result->reply.response["returnValue"].boolean();
    lock.unlock();
    if (!succeeded) {
        sendSetupError(exchange, 500, "the TV would not change it");
        return true;
    }
    (void)exchange.respond(200,
                           {{"Content-Type", "application/json"},
                            {"Cache-Control", "no-store"},
                            {"X-Content-Type-Options", "nosniff"}},
                           "{\"ok\":true}");
    return true;
}

std::string findSystemExecutable(const std::string &root,
                                 const std::vector<std::string> &paths) {
    for (const std::string &path : paths) {
        const std::string candidate = rootedSystemPath(root, path);
        if (access(candidate.c_str(), X_OK) == 0) return candidate;
    }
    return std::string();
}

bool writeServiceStub(const std::string &path, bool javascript) {
    const std::string contents = javascript ? "process.exit(0);\n" : "#!/bin/sh\nexit 0\n";
    const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW,
                        javascript ? 0644 : 0755);
    if (fd < 0) return false;
    size_t offset = 0;
    bool ok = fchmod(fd, javascript ? 0644 : 0755) == 0;
    while (ok && offset < contents.size()) {
        const ssize_t count = write(fd, contents.data() + offset, contents.size() - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) {
            ok = false;
            break;
        }
        offset += static_cast<size_t>(count);
    }
    if (close(fd) != 0) ok = false;
    return ok;
}

bool containsJavascriptSuffix(const std::string &path) {
    return path.size() >= 3 && path.compare(path.size() - 3, 3, ".js") == 0;
}

Json stampHomeRecentHistoryFile(HomeRecentHistoryState &history,
                                const std::string &row, const std::string &id,
                                int64_t timestamp) {
    std::lock_guard<std::mutex> lock(history.mutex);
    JsonFile file(history.path, 512 * 1024);
    const JsonFileResult stored = file.load();
    if (!stored.error.empty() && !stored.found)
        return stored.value.isObject() ? stored.value : Json::object();
    const Json current = stored.found ? stored.value : Json::object();
    const Json recent = service::stampHomeRecentHistory(current, row, id, timestamp);
    std::string error;
    if (!file.save(recent, &error)) return current;
    return recent;
}

void clearHomeRecentHistoryRows(HomeRecentHistoryState &history,
                                const Json &rows) {
    if (!rows.isArray() || rows.size() == 0) return;
    std::lock_guard<std::mutex> lock(history.mutex);
    JsonFile file(history.path, 512 * 1024);
    const JsonFileResult stored = file.load();
    if (!stored.error.empty() && !stored.found) return;
    const Json current = stored.found ? stored.value : Json::object();
    const Json cleared = service::mergeHomeRecentHistory(
        current, Json::object(), rows);
    std::string error;
    (void)file.save(cleared, &error);
}

bool homeReservedApp(const std::string &appId) {
    return appId.empty() || appId == "com.tvweb.home" ||
           appId == "com.tvweb.native" || appId == "com.webos.app.home";
}

bool homeInputApp(const std::string &appId) {
    if (appId == "com.webos.app.livetv") return true;
    const char *prefixes[] = {"com.webos.app.hdmi", "com.webos.app.av",
                              "com.webos.app.comp"};
    for (const char *prefix : prefixes) {
        const std::string value(prefix);
        if (appId.size() == value.size() + 1 &&
            appId.compare(0, value.size(), value) == 0 &&
            appId[value.size()] >= '0' && appId[value.size()] <= '9')
            return true;
    }
    return false;
}

std::string homeDeviceRecentId(const std::string &appId) {
    static const std::string prefix = "com.webos.app.hdmi";
    if (appId.size() != prefix.size() + 1 ||
        appId.compare(0, prefix.size(), prefix) != 0 ||
        appId[prefix.size()] < '0' || appId[prefix.size()] > '9')
        return std::string();
    return "hdmi" + appId.substr(prefix.size());
}

int64_t homeNowMilliseconds() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}
} // namespace

HttpFrontDoor::HttpFrontDoor(ServerConfig config, std::string version, std::string dataRoot,
                             std::string configPath, HttpProxy proxy, HttpProxy handoffProxy,
                             LunaClient &luna, ChildProcessRunner &children,
                             std::string assetsRoot, std::string procRoot,
                             std::string systemRoot, unsigned cpuWindowMs,
                             Logger *logger, service::HistoryRecorder *historyRecorder,
                             ScreenTimeEnforcer *screenTimeEnforcer,
                             CommandPolicyExecutor *commandPolicy,
                             GameFrameRate *gameFrameRate,
                             StateSubscriptions *stateSubscriptions,
                             AdBlockManager *adBlockManager,
                             DevtoolsFirewall *devtoolsFirewall,
                             ScreensaverManager *screensaverManager,
                             HomeScreenManager *homeScreenManager,
                             SamRescanManager *samRescanManager,
                             AppTileManager *appTileManager,
                             service::AppCatalogFetcher appCatalogFetcher,
                             service::AppPackageDownload appPackageFetcher,
                             HomeTakeoverLifecycleManager *homeTakeoverLifecycleManager,
                             EventLoop *eventLoop)
    : config_(std::move(config)), screenTimeSettings_(config_.document["screenTime"]),
      version_(std::move(version)), appCatalogRepos_(config_.document["apps"]["repos"]),
      appCatalog_(version_, std::move(appCatalogFetcher)),
      appPackageFetcher_(std::move(appPackageFetcher)), dataRoot_(std::move(dataRoot)),
      homeContent_(dataRoot_),
      homeDiscover_(dataRoot_),
      homeImageProxy_(dataRoot_),
      configPath_(std::move(configPath)), hdmiSeenPath_(dataFilePath(dataRoot_, "hdmi_seen")),
      lastAppPath_(dataFilePath(dataRoot_, "last_app.json")),
      hiddenAppsPath_(dataFilePath(dataRoot_, "hidden_apps")),
      tileHidingFlagPath_(dataFilePath(dataRoot_, "tile_hiding_enabled")),
      elevatedAppsPath_(dataFilePath(dataRoot_, "elevated.json")),
      installJobPath_(dataFilePath(dataRoot_, "install-job.json")),
      proxySecret_(std::getenv("TVWEB_PROXY_SECRET") ? std::getenv("TVWEB_PROXY_SECRET") : ""),
      proxy_(std::move(proxy)),
      handoffProxy_(std::move(handoffProxy)), luna_(luna), children_(children),
      logger_(logger), historyRecorder_(historyRecorder),
      screenTimeEnforcer_(screenTimeEnforcer), commandPolicy_(commandPolicy),
      gameFrameRate_(gameFrameRate), stateSubscriptions_(stateSubscriptions),
      adBlockManager_(adBlockManager), devtoolsFirewall_(devtoolsFirewall),
      screensaverManager_(screensaverManager),
      homeScreenManager_(homeScreenManager),
      homeTakeoverLifecycleManager_(homeTakeoverLifecycleManager),
      samRescanManager_(samRescanManager),
      appTileManager_(appTileManager),
      eventLoop_(eventLoop),
      assetsRoot_(assetsRoot), staticAssets_(std::move(assetsRoot), version_),
      procRoot_(std::move(procRoot)),
      systemRoot_(std::move(systemRoot)), cpuWindowMs_(cpuWindowMs) {
    if (!appPackageFetcher_)
        appPackageFetcher_ = service::AppPackageFetcher::download;
    lgSettingsCacheSettings_ = Json::object();
    lgSettingsCacheDescriptions_ = Json::object();
    lgSettingsCacheDescriptionCoverage_ = Json::object();
    lgSettingsCacheDimensions_ = Json::object();
    inputNameCache_ = Json::object();
    appInstallSnapshot_ = idleAppInstallSnapshot();
    appInstallSnapshotReady_ = false;
    if (nativeInstallEnabled()) (void)installJobs();
    homePictureState_.reset(new HomePictureTrackingState());
    homeNowState_.reset(new HomeNowTrackingState());
    homeRecentHistoryState_.reset(new HomeRecentHistoryState());
    homeRecentHistoryState_->path = dataFilePath(dataRoot_, "home-recent.json");
    homeTileInputState_.reset(new HomeTileInputState());
    homeEscapeActive_.reset(new std::atomic<bool>(false));
    homeEscapeReturnPending_.reset(new std::atomic<bool>(false));
    if (stateSubscriptions_) {
        const std::weak_ptr<HomeNowTrackingState> weakNowState = homeNowState_;
        const std::shared_ptr<HomeRecentHistoryState> recentState =
            homeRecentHistoryState_;
        const std::shared_ptr<HomeTileInputState> tileState =
            homeTileInputState_;
        const std::string homeConfigPath = dataFilePath(dataRoot_, "home.json");
        LunaClient *const luna = &luna_;
        stateSubscriptions_->cache().onChange(
            [this, weakNowState, recentState, tileState,
             homeConfigPath, luna](const state::Event &event) {
            if (event.group != "application" || event.key != "app_id" ||
                !event.value.isString())
                return;
            const std::string appId = event.value.string();
            if (!appId.empty() && appId != "com.webos.app.home" &&
                homeEscapeActive_->load(std::memory_order_acquire))
                queueHomeForegroundReturn(appId);
            const std::shared_ptr<HomeNowTrackingState> nowState = weakNowState.lock();
            if (!nowState) return;
            std::string previousAppId;
            {
                std::lock_guard<std::mutex> lock(nowState->mutex);
                previousAppId = nowState->observedAppId;
                nowState->observedAppId = appId;
                if (!homeReservedApp(appId)) {
                    nowState->appId = appId;
                    nowState->since = event.sourceTime;
                }
            }

            std::string row;
            std::string id;
            const std::string deviceId = homeDeviceRecentId(appId);
            if (!homeReservedApp(appId) && !deviceId.empty()) {
                row = "dev";
                id = deviceId;
            } else if (!homeReservedApp(appId) && !homeInputApp(appId)) {
                row = "app";
                id = appId;
            }
            const bool leftLiveTv = previousAppId == "com.webos.app.livetv" &&
                                    appId != "com.webos.app.livetv";
            if (row.empty() && !leftLiveTv) return;

            std::ifstream homeFile(homeConfigPath.c_str(),
                                   std::ios::in | std::ios::binary);
            std::string homeContents;
            if (homeFile) {
                const size_t maximumBytes = 8 * 1024 * 1024;
                char buffer[4096];
                while (homeFile) {
                    homeFile.read(buffer, sizeof(buffer));
                    const std::streamsize count = homeFile.gcount();
                    if (count <= 0) break;
                    if (homeContents.size() + static_cast<size_t>(count) > maximumBytes) {
                        homeContents.clear();
                        break;
                    }
                    homeContents.append(buffer, static_cast<size_t>(count));
                }
            }
            const Json home = service::normalizeHomeDocument(
                homeContents.empty() ? Json() : Json::parse(homeContents));
            if (!home["enabled"].boolean()) return;
            bool fromTile = false;
            if (row == "dev") {
                std::lock_guard<std::mutex> tileLock(tileState->mutex);
                fromTile = tileState->appId == appId &&
                           service::homeTileInputFresh(homeNowMilliseconds(),
                                                       tileState->at);
                tileState->appId.clear();
                tileState->at = 0;
            }
            if (!fromTile && !row.empty() &&
                home["sort"][row].string() == "recent")
                (void)stampHomeRecentHistoryFile(*recentState, row, id,
                                                 event.sourceTime);
            if (leftLiveTv && home["sort"]["ch"].string() == "recent") {
                (void)luna->call(
                    "com.webos.service.utp.boot/getLastChannelId", Json::object(),
                    [recentState](const LunaReply &reply) {
                        const std::string channelId = reply.response["channelId"].string();
                        if (!channelId.empty())
                            (void)stampHomeRecentHistoryFile(
                                *recentState, "ch", channelId, homeNowMilliseconds());
                    });
            }
        });
    }
    ensureHomePictureTracking();
}

HttpFrontDoor::~HttpFrontDoor() {
    std::shared_ptr<LunaSubscription> subscription;
    {
        std::lock_guard<std::mutex> lock(homePictureMutex_);
        subscription = std::move(homePictureVideoSubscription_);
    }
    if (subscription) subscription->stop();
}

void HttpFrontDoor::ensureHomePictureTracking() const {
    Json rawHome;
    std::ifstream homeFile(dataFilePath(dataRoot_, "home.json").c_str(),
                           std::ios::in | std::ios::binary);
    if (homeFile) {
        const size_t maximumBytes = 8 * 1024 * 1024;
        std::string contents;
        char buffer[4096];
        while (homeFile) {
            homeFile.read(buffer, sizeof(buffer));
            const std::streamsize count = homeFile.gcount();
            if (count <= 0) break;
            if (contents.size() + static_cast<size_t>(count) > maximumBytes) {
                contents.clear();
                break;
            }
            contents.append(buffer, static_cast<size_t>(count));
        }
        if (homeFile.eof() && !contents.empty())
            rawHome = Json::parse(contents);
    }
    const Json document = service::normalizeHomeDocument(rawHome);
    const bool wanted = document["enabled"].boolean() &&
                        document["picture"].boolean();

    std::shared_ptr<LunaSubscription> start;
    std::shared_ptr<LunaSubscription> stop;
    {
        std::lock_guard<std::mutex> lock(homePictureMutex_);
        if (!wanted) {
            stop = std::move(homePictureVideoSubscription_);
        } else if (!homePictureVideoSubscription_) {
            Json payload = Json::object();
            payload.set("subscribe", Json::flag(true));
            const std::weak_ptr<HomePictureTrackingState> weakState = homePictureState_;
            LunaSubscriptionHandlers handlers;
            handlers.message = [weakState](const Json &reply) {
                const std::shared_ptr<HomePictureTrackingState> state = weakState.lock();
                if (!state) return;
                const Json observed = service::homePictureAppVideo(reply);
                if (!observed.isObject()) return;
                const std::string appId = observed["appId"].string();
                if (appId.empty()) return;
                std::lock_guard<std::mutex> lock(state->mutex);
                state->played[appId] = observed["signal"];
                ++state->generations[appId];
                state->cache.erase("app:" + appId);
            };
            homePictureVideoSubscription_ = luna_.subscribe(
                "com.webos.service.videooutput/getStatus", payload, "",
                std::move(handlers));
            start = homePictureVideoSubscription_;
        }
    }
    if (stop) stop->stop();
    if (start) (void)start->start();
}

bool HttpFrontDoor::collectNativeScreenTimeStatus(Json &response,
                                                  std::string &error) const {
    error.clear();
    const int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::system_clock::now().time_since_epoch())
                              .count();
    const int64_t fromMs = nowMs - 30LL * 86400000LL;
    Json overlay;
    if (historyRecorder_) {
        overlay = historyRecorder_->overlay(fromMs, nowMs);
    } else {
        overlay = Json::object();
        overlay.set("enabled", Json::flag(false));
        overlay.set("running", Json::flag(false));
        overlay.set("readPersisted", Json::flag(true));
        overlay.set("current", Json());
        overlay.set("clockSet", Json::flag(nowMs >= 1767225600000LL));
        overlay.set("hours", Json::object());
        overlay.set("titles", Json::object());
        overlay.set("ids", Json::object());
    }
    Json titleOverlay;
    std::string titleError;
    if (proxy_.requestNodeHistoryOverlay(fromMs, nowMs, overlay["ids"],
                                         titleOverlay, titleError) &&
        titleOverlay.isObject()) {
        if (!historyRecorder_) {
            overlay = titleOverlay;
        } else if (titleOverlay["titles"].isObject()) {
            Json titles = overlay["titles"];
            for (const std::string &key : titleOverlay["titles"].keys()) {
                if (titleOverlay["titles"][key].isString())
                    titles.set(key, titleOverlay["titles"][key]);
            }
            overlay.set("titles", titles);
        }
    }

    Json recent;
    if (!service::queryHistory(dataRoot_ + "/history", fromMs, nowMs,
                               overlay, recent, error))
        return false;

    const std::string root = systemRoot_.empty() ? "/" : systemRoot_;
    const std::string homePath = root[root.size() - 1] == '/'
                                     ? root + "usr/palm/applications/com.webos.app.home"
                                     : root + "/usr/palm/applications/com.webos.app.home";
    const bool homeApp = ::access(homePath.c_str(), F_OK) == 0;
    Json configuredScreenTime;
    {
        std::lock_guard<std::mutex> lock(screenTimeSettingsMutex_);
        configuredScreenTime = screenTimeSettings_;
    }
    Json screenTimeDocument = Json::object();
    screenTimeDocument.set("screenTime", configuredScreenTime);
    const Json settings = service::screenTimeSettings(
        screenTimeDocument, config_.allowPower, homeApp);

    const Json historyStatus = historyRecorder_ ? historyRecorder_->status() : Json();
    const bool historyOn = historyRecorder_ ? historyStatus["running"].boolean()
                                            : overlay["running"].boolean();
    bool clockSet = nowMs >= 1767225600000LL;
    bool todayAvailable = false;
    std::string todayHour;
    if (clockSet) {
        const time_t nowSeconds = static_cast<time_t>(nowMs / 1000);
        struct tm local;
        if (!localtime_r(&nowSeconds, &local)) {
            clockSet = false;
        } else {
            local.tm_hour = 0;
            local.tm_min = 0;
            local.tm_sec = 0;
            local.tm_isdst = -1;
            const time_t midnight = std::mktime(&local);
            if (midnight == static_cast<time_t>(-1)) {
                clockSet = false;
            } else {
                todayHour = utcHourKey(static_cast<int64_t>(midnight) * 1000);
                todayAvailable = historyOn && !todayHour.empty();
            }
        }
    }

    Json keys = Json::object();
    Json today = Json::object();
    double watchedToday = 0;
    const Json hours = recent["hours"];
    for (const std::string &hour : hours.keys()) {
        const Json values = hours[hour];
        const bool todayHourValue = todayAvailable && hour >= todayHour;
        for (const std::string &key : values.keys()) {
            keys.set(key, Json::flag(true));
            const Json seconds = values[key];
            if (!todayHourValue || !seconds.isNumber() || !std::isfinite(seconds.number()) ||
                seconds.number() <= 0)
                continue;
            const double total = today[key].number() + seconds.number();
            today.set(key, Json::numeric(total));
            if (key != "screen_off" && key != "screen_saver")
                watchedToday += seconds.number();
        }
    }
    const Json limitedApps = settings["apps"];
    for (const std::string &key : limitedApps.keys())
        keys.set(key, Json::flag(true));

    Json choices = Json::array();
    const Json titles = recent["titles"];
    for (const std::string &key : keys.keys()) {
        if (key == "screen_off" || key == "screen_saver")
            continue;
        Json choice = Json::object();
        choice.set("key", Json::text(key));
        choice.set("title", titles[key].isString() ? titles[key] : Json());
        choice.set("today", todayAvailable
                                 ? Json::numeric(roundedSeconds(today[key].number()))
                                 : Json());
        choices.add(choice);
    }

    response = Json::object();
    response.set("ok", Json::flag(true));
    response.set("writable", Json::flag(config_.allowControl));
    response.set("settings", settings);
    response.set("historyOn", Json::flag(historyOn));
    response.set("clockSet", Json::flag(clockSet));
    response.set("allowPower", Json::flag(config_.allowPower));
    response.set("homeApp", Json::flag(homeApp));
    response.set("watchedToday", todayAvailable
                                      ? Json::numeric(roundedSeconds(watchedToday))
                                      : Json());
    response.set("choices", choices);
    return true;
}

std::string HttpFrontDoor::collectCpuMetrics() const {
    std::unique_lock<std::mutex> lock(cpuMetricsMutex_);
    const uint64_t generation = cpuMetricsGeneration_;
    if (cpuMetricsInFlight_) {
        cpuMetricsCondition_.wait(lock, [this, generation] {
            return cpuMetricsGeneration_ != generation;
        });
        return cpuMetricsResponse_;
    }

    cpuMetricsInFlight_ = true;
    lock.unlock();

    std::string response;
    try {
        response = service::collectCpu(procRoot_, cpuWindowMs_).dump();
    } catch (const std::exception &) {
        Json failure = Json::object();
        failure.set("ok", Json::flag(false));
        failure.set("error", Json::text("could not collect CPU metrics"));
        response = failure.dump();
    }

    lock.lock();
    cpuMetricsResponse_ = response;
    ++cpuMetricsGeneration_;
    cpuMetricsInFlight_ = false;
    lock.unlock();
    cpuMetricsCondition_.notify_all();
    return response;
}

Json HttpFrontDoor::withStatsWindows(const Json &system, const std::string &reader) const {
    const Json cpu = system["cpu"];
    const Json times = cpu["times"];
    const Json cpuTimes = times["cpus"];
    std::vector<service::CpuCoreTicks> ticks;
    for (const std::string &id : cpuTimes.keys()) {
        const Json modes = cpuTimes[id];
        uint64_t total = 0;
        uint64_t idle = 0;
        bool any = false;
        for (const std::string &mode : modes.keys()) {
            const Json value = modes[mode];
            const double raw = value.number(-1);
            if (!value.isNumber() || !std::isfinite(raw) || raw < 0 ||
                raw >= static_cast<double>(std::numeric_limits<uint64_t>::max()))
                continue;
            const uint64_t count = static_cast<uint64_t>(raw);
            if (count > std::numeric_limits<uint64_t>::max() - total ||
                ((mode == "idle" || mode == "iowait") &&
                 count > std::numeric_limits<uint64_t>::max() - idle))
                continue;
            total += count;
            if (mode == "idle" || mode == "iowait")
                idle += count;
            any = true;
        }
        if (any)
            ticks.push_back({id, total, idle});
    }

    const int64_t atMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now().time_since_epoch())
                             .count();
    const unsigned totalCores = static_cast<unsigned>(
        std::max(0.0, cpu["coresTotal"].number()));
    const bool publishReader = reader == "publish";
    const Json window = statsCpuWindows_.sample(totalCores, atMs, reader,
                                                publishReader ? 0 : cpuWindowMs_, ticks);
    Json result = Json::parse(system.dump());
    Json resultCpu = result["cpu"];
    resultCpu.set("statsWindow", window);
    result.set("cpu", resultCpu);
    const Json net = result["net"];
    const double rx = net["rx"].number(-1);
    const double tx = net["tx"].number(-1);
    if (net.isObject() && net["iface"].isString() && !net["iface"].string().empty() &&
        net["rx"].isNumber() && net["tx"].isNumber() && std::isfinite(rx) &&
        std::isfinite(tx) && rx >= 0 && tx >= 0 &&
        rx < static_cast<double>(std::numeric_limits<uint64_t>::max()) &&
        tx < static_cast<double>(std::numeric_limits<uint64_t>::max())) {
        const service::NetworkCounters counters{
            net["iface"].string(), static_cast<uint64_t>(rx), static_cast<uint64_t>(tx)};
        Json resultNet = result["net"];
        resultNet.set("rate", statsNetworkRates_.sample(atMs, reader,
                                                         publishReader ? 0 : 1500, counters));
        result.set("net", resultNet);
    }
    return result;
}

void HttpFrontDoor::sendLogs(HttpExchange &exchange) const {
    const HttpRequest &request = exchange.request();
    service::LogsQuery query;
    query.root = systemRoot_;

    std::string value;
    if (queryParameter(request.query, "sources", value) && !value.empty())
        query.sources = splitCommaSeparated(value);
    if (queryParameter(request.query, "limit", value))
        query.limit = value;
    if (queryParameter(request.query, "filter", value))
        query.filter = value;
    if (queryParameter(request.query, "since", value))
        query.since = value;
    if (queryParameter(request.query, "redact", value))
        query.redact = value == "1" || value == "true";

    const bool wantsKernel = std::find(query.sources.begin(), query.sources.end(), "kernel") !=
                             query.sources.end();
    if (wantsKernel) {
        const std::string executable = findDmesgExecutable();
        if (executable.empty()) {
            query.kernelError = "dmesg executable not found";
        } else {
            ChildOptions options;
            options.arguments = {executable};
            options.timeout = std::chrono::milliseconds(3500);
            options.maximumOutputBytes = 2 * 1024 * 1024;
            const ChildResult result = children_.run(options);
            if (result.succeeded()) {
                query.kernelText = result.standardOutput;
            } else if (!result.error.empty()) {
                query.kernelError = result.error;
            } else if (result.timedOut) {
                query.kernelError = "dmesg timed out";
            } else if (result.outputLimitExceeded) {
                query.kernelError = "dmesg output exceeded limit";
            } else if (result.signal != 0) {
                query.kernelError = "dmesg terminated by signal " +
                                    std::to_string(result.signal);
            } else {
                query.kernelError = "dmesg exited with code " +
                                    std::to_string(result.exitCode);
            }
        }
    }

    sendMetricsJson(exchange, service::collectLogs(query).dump());
}

Json HttpFrontDoor::hdmiReceiverMap() const {
    {
        std::unique_lock<std::mutex> lock(hdmiReceiverMutex_);
        if (hdmiReceiverLoaded_)
            return hdmiReceiverMap_;
        if (hdmiReceiverLoading_) {
            hdmiReceiverCondition_.wait_for(lock, std::chrono::seconds(6), [this] {
                return !hdmiReceiverLoading_;
            });
            return hdmiReceiverLoaded_ ? hdmiReceiverMap_ : service::hdmiBaseInputMap();
        }
        hdmiReceiverLoading_ = true;
    }

    Json payload = Json::object();
    Json configNames = Json::array();
    configNames.add(Json::text("inputMap.videoInputMapIndexInfo0"));
    payload.set("configNames", configNames);
    LunaReply reply;
    const bool answered = waitForLuna(luna_, "com.webos.service.config/getConfigs",
                                      payload, reply);
    const bool cache = answered && reply.response.isObject();
    Json result = cache ? service::hdmiInputMapFromConfig(reply.response)
                        : service::hdmiBaseInputMap();
    {
        std::lock_guard<std::mutex> lock(hdmiReceiverMutex_);
        if (cache) {
            hdmiReceiverMap_ = result;
            hdmiReceiverLoaded_ = true;
        }
        hdmiReceiverLoading_ = false;
    }
    hdmiReceiverCondition_.notify_all();
    return result;
}

void HttpFrontDoor::sendHdmiInputs(HttpExchange &exchange) const {
    const Json receiverMap = hdmiReceiverMap();
    LunaReply devices, foreground;
    (void)waitForLuna(luna_, "com.webos.service.eim/getAllInputStatus",
                      Json::object(), devices);
    (void)waitForLuna(luna_, "com.webos.applicationManager/getForegroundAppInfo",
                      Json::object(), foreground);
    const Json result = service::hdmiInputs(service::collectSystem(systemRoot_, dataRoot_),
                                            devices.response["devices"],
                                            foreground.response, receiverMap);
    sendMetricsJson(exchange, result.dump());
}

void HttpFrontDoor::sendServiceMenuState(HttpExchange &exchange) const {
    Json request = Json::object();
    request.set("category", Json::text("other"));
    Json keys = Json::array();
    keys.add(Json::text("svcMenuFlag"));
    request.set("keys", keys);
    LunaReply reply;
    {
        std::lock_guard<std::mutex> settingsLock(systemSettingsControlMutex_);
        (void)waitForLuna(luna_, "com.webos.settingsservice/getSystemSettings",
                          request, reply);
    }
    const Json flag = reply.response["settings"]["svcMenuFlag"];
    const bool lockable = reply.response["returnValue"].boolean() && flag.valid();
    Json response = Json::object();
    response.set("ok", Json::flag(true));
    response.set("app", Json::flag(serviceMenuAppPresent(systemRoot_)));
    response.set("lockable", Json::flag(lockable));
    response.set("locked", lockable
        ? Json::flag(flag.isBoolean() && flag.boolean()) : Json());
    response.set("writable", Json::flag(config_.allowControl));
    sendMetricsJson(exchange, response.dump());
}

void HttpFrontDoor::sendScreensaverList(HttpExchange &exchange) const {
    const Json document = currentConfigDocument();
    const bool allowedAnyway = document["allowExperimentalScreensavers"].boolean() ||
                               document["allowOnWebos10"].boolean();
    if (!screensaverManager_) {
        sendSetupError(exchange, 503, "screen saver manager unavailable");
        return;
    }
    sendMetricsJson(exchange, service::screensaverList(
        screensaverManager_->state(config_.allowControl, allowedAnyway)).dump());
}

void HttpFrontDoor::setStatusProviders(std::function<Json()> ntp,
                                       std::function<Json()> syslog) {
    ntpStatus_ = std::move(ntp);
    syslogStatus_ = std::move(syslog);
}

namespace {
bool returnedFalseReply(const Json &reply) {
    return reply["returnValue"].isBoolean() && !reply["returnValue"].boolean();
}

std::string trimmedText(const std::string &value) {
    size_t start = 0, end = value.size();
    while (start < end && std::isspace(static_cast<unsigned char>(value[start]))) ++start;
    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1]))) --end;
    return value.substr(start, end - start);
}

// Resident set size in bytes, or -1 where /proc/self/statm is unreadable.
double residentBytes() {
    std::ifstream statm("/proc/self/statm");
    unsigned long long size = 0, resident = 0;
    if (!(statm >> size >> resident)) return -1;
    return static_cast<double>(resident) * static_cast<double>(sysconf(_SC_PAGESIZE));
}
} // namespace

Json HttpFrontDoor::currentConfigDocument() const {
    struct stat info;
    std::string stamp;
    if (stat(configPath_.c_str(), &info) == 0)
        stamp = std::to_string(static_cast<long long>(info.st_mtime)) + ":" +
                std::to_string(static_cast<long long>(info.st_size)) + ":" +
                std::to_string(static_cast<long long>(info.st_ino));
    std::lock_guard<std::mutex> lock(liveConfigMutex_);
    if (!stamp.empty() && stamp == liveConfigStamp_ && liveConfig_.isObject())
        return liveConfig_;
    JsonFile store(configPath_);
    const JsonFileResult loaded = store.load();
    if (loaded.error.empty() && loaded.found && loaded.value.isObject()) {
        liveConfig_ = loaded.value;
        liveConfigStamp_ = stamp;
        return liveConfig_;
    }
    return config_.document;
}

// The shared server-status response: the Server tab's switches and the
// daemon's own vitals. Node assembled the same document from its modules.
Json HttpFrontDoor::serverStatus() const {
    const Json document = currentConfigDocument();
    Json status = Json::object();
    status.set("ok", Json::flag(true));
    status.set("writable", Json::flag(config_.allowControl));

    service::AdBlockStore fallbackStore(dataRoot_, systemRoot_);
    const service::AdBlockStore &adBlockStore = adBlockManager_
        ? adBlockManager_->store() : fallbackStore;
    status.set("tvUpdatesBlocked", Json::flag(adBlockStore.tvUpdatesBlocked()));

    // "pending" is the firewall's state before its first pass; Node had null.
    std::string devtools = devtoolsFirewall_ ? devtoolsFirewall_->status() : "pending";
    status.set("devtools", devtools == "pending" ? Json() : Json::text(devtools));
    status.set("allowNetworkDebugger", Json::flag(document["allowNetworkDebugger"].boolean()));

    std::set<std::string> hiddenIds;
    bool tileHidingEnabled = false;
    std::string stateError;
    (void)readAppTileStateFiles(hiddenAppsPath_, tileHidingFlagPath_, hiddenIds,
                                tileHidingEnabled, &stateError);
    const Json allowTileHiding = document["allowTileHiding"];
    status.set("allowTileHiding", Json::flag(
        allowTileHiding.isBoolean() ? allowTileHiding.boolean() : tileHidingEnabled));

    const bool allowedAnyway = document["allowExperimentalScreensavers"].boolean() ||
                               document["allowOnWebos10"].boolean();
    const service::ScreensaverState saver = screensaverManager_
        ? screensaverManager_->state(config_.allowControl, allowedAnyway)
        : service::ScreensaverState();
    status.set("allowExperimentalScreensavers", Json::flag(allowedAnyway));
    // Where the Server tab offers the switch: custom screen savers are
    // experimental there.
    status.set("isWebos10", Json::flag(service::screensaverExperimental(saver)));

    Json homeLayout;
    if (homeScreenManager_) {
        const Json layout = homeScreenManager_->state();
        if (layout["experimental"].boolean()) {
            homeLayout = Json::object();
            homeLayout.set("allowed", Json::flag(document["allowHomeLayout"].boolean()));
        }
    }
    status.set("homeLayout", homeLayout);
    status.set("prometheusEnabled", Json::flag(document["prometheus"]["enabled"].isBoolean() &&
                                               document["prometheus"]["enabled"].boolean()));
    status.set("watchHistory", historyRecorder_ ? historyRecorder_->status() : Json());

    Json backupDaily = Json::object();
    backupDaily.set("enabled", Json::flag(document["backup"]["daily"].isBoolean() &&
                                          document["backup"]["daily"].boolean()));
    status.set("backupDaily", backupDaily);
    status.set("ntp", ntpStatus_ ? ntpStatus_() : Json());

    const Json ntpConfig = document["ntp"];
    const std::string ntpServer = ntpConfig["server"].isString()
        ? trimmedText(ntpConfig["server"].string()) : std::string();
    Json ntpSettings = Json::object();
    ntpSettings.set("server", Json::text(ntpServer));
    ntpSettings.set("port", Json::numeric(ntpConfig["port"].number(0) != 0
        ? ntpConfig["port"].number() : 123));
    ntpSettings.set("enabled", Json::flag(
        !ntpServer.empty() &&
        !(ntpConfig["enabled"].isBoolean() && !ntpConfig["enabled"].boolean())));
    status.set("ntpSettings", ntpSettings);
    status.set("syslog", syslogStatus_ ? syslogStatus_() : Json());

    Json daemon = Json::object();
    daemon.set("uptime", Json::numeric(static_cast<double>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - startedAt_).count())));
    daemon.set("pid", Json::numeric(static_cast<double>(getpid())));
    daemon.set("port", Json::numeric(config_.port != 0 ? config_.port : 8080));
    const double rss = residentBytes();
    daemon.set("rss", rss >= 0 ? Json::numeric(rss) : Json());
    // The JavaScript heap figures have no native counterpart.
    daemon.set("heapUsed", Json());
    daemon.set("heapTotal", Json());
    status.set("daemon", daemon);
    return status;
}

namespace {
// Whether nothing is listening on host:port. SO_REUSEADDR as a Node listener
// sets it, so a socket left in TIME_WAIT does not count as taken.
bool localPortIsFree(const std::string &host, int port) {
    const bool dual = host == "::" || host.empty();
    int fd = dual ? socket(AF_INET6, SOCK_STREAM, 0) : -1;
    bool ipv6 = fd >= 0;
    if (!ipv6) fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;
    const int enable = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));
    bool bound = false;
    if (ipv6) {
        const int off = 0;
        (void)setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off));
        sockaddr_in6 address = {};
        address.sin6_family = AF_INET6;
        address.sin6_addr = in6addr_any;
        address.sin6_port = htons(static_cast<uint16_t>(port));
        bound = bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0;
    } else {
        sockaddr_in address = {};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = dual || host == "0.0.0.0"
            ? htonl(INADDR_ANY) : inet_addr(host.c_str());
        address.sin_port = htons(static_cast<uint16_t>(port));
        bound = bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0;
    }
    close(fd);
    return bound;
}
} // namespace

bool HttpFrontDoor::serverSettingControl(const std::string &action, const Json &value,
                                         Json &response) const {
    const bool on = (value.isBoolean() && value.boolean()) ||
                    (value.isString() && (value.string() == "on" || value.string() == "true"));
    const auto fail = [&response](const std::string &message) {
        response = Json::object();
        response.set("ok", Json::flag(false));
        response.set("error", Json::text(message));
        return true;
    };
    const auto reloadNode = [this]() {
        std::string reloadError;
        if (!proxy_.requestNodeConfigReload(reloadError) && logger_)
            logger_->warning("config: saved a setting but Node did not reload it: " + reloadError);
    };
    // Saves one section's keys; false (with the reply set) when the write failed.
    const auto saveSection = [&](const std::string &section, const Json &patch) {
        std::string error;
        if (ConfigStore(configPath_).patchObjectSection(section, patch, nullptr, &error)) {
            reloadNode();
            return true;
        }
        fail("could not save setting: " + error);
        return false;
    };
    const auto answer = [&]() {
        response = serverStatus();
        return true;
    };
    Json patch = Json::object();

    if (action == "setPrometheusEnabled") {
        patch.set("enabled", Json::flag(on));
        return saveSection("prometheus", patch) ? answer() : true;
    }
    if (action == "setNtpEnabled" || action == "setNtpServer") {
        if (action == "setNtpServer") {
            const service::NtpServerInput target = service::parseNtpServerInput(
                value.isString() ? value.string() : value.isNumber() ? compactNumber(value.number()) : "");
            if (target.configured && !target.valid) {
                return fail(target.error == "port"
                    ? say::msg("srv.ntp.badPort",
                               "The port after the colon must be a number from 1 to 65535")
                    : say::msg("srv.ntp.badServer", "That is not a server name or address"));
            }
            // A server saved is a server wanted: saving one switches it on.
            if (target.configured) {
                patch.set("server", Json::text(target.settings.server));
                patch.set("port", Json::numeric(target.settings.port));
                patch.set("enabled", Json::flag(true));
            } else {
                patch.set("server", Json::text(""));
            }
        } else {
            patch.set("enabled", Json::flag(on));
        }
        if (!saveSection("ntp", patch)) return true;
        // The client picks the saved setting up within a second; wait for its
        // first answer from the new server so the tab shows whether it replied.
        service::NtpSettings wanted;
        if (service::readNtpSettings(currentConfigDocument(), wanted) && ntpStatus_) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            while (std::chrono::steady_clock::now() < deadline) {
                const Json status = ntpStatus_();
                if (status.isObject() && status["server"].string() == wanted.server &&
                    static_cast<int>(status["port"].number(123)) == wanted.port &&
                    (status["last"].isObject() || status["error"].isString()))
                    break;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
        return answer();
    }
    if (action == "setHistoryEnabled" || action == "clearHistory") {
        if (!historyRecorder_) return fail("no history configured");
        std::string error;
        if (action == "setHistoryEnabled") {
            patch.set("enabled", Json::flag(on));
            if (!saveSection("history", patch)) return true;
            if (!historyRecorder_->setEnabled(on, &error))
                return fail("could not update watch time: " + error);
        } else if (!historyRecorder_->clear(&error)) {
            return fail("could not clear watch time: " + error);
        }
        return answer();
    }
    if (action == "setBackupDaily") {
        patch.set("daily", Json::flag(on));
        if (!saveSection("backup", patch)) return true;
        // Turning it on makes today's backup straight away.
        std::string error;
        if (on && !createDailyBackupIfDue(error) && logger_)
            logger_->warning("backup: daily backup after switching on failed: " + error);
        return answer();
    }
    if (action == "setNetworkDebuggerAllowed") {
        std::string error;
        if (!ConfigStore(configPath_).setNetworkDebuggerAllowed(on, &error))
            return fail("could not save setting: " + error);
        reloadNode();
        if (devtoolsFirewall_ && commandPolicy_) {
            CommandPolicySpec localCommand;
            localCommand.action = "devtools-firewall";
            localCommand.uri = "native/devtools/firewall";
            localCommand.timeout = std::chrono::milliseconds(25000);
            localCommand.requiresControl = false;
            localCommand.resource = "devtools-firewall";
            LunaReply localReply;
            (void)commandPolicy_->executeLocal(
                localCommand, controlPolicyContext(config_), false,
                [this, on]() {
                    const Json result = devtoolsFirewall_->setNetworkAllowed(on);
                    return result["ok"].isBoolean() && result["ok"].boolean();
                }, localReply);
        }
        return answer();
    }
    if (action == "tileHiding" || action == "setTileHidingAllowed") {
        const std::string heldText = say::msg(
            "srv.apps.tilesHeld",
            "Hiding system apps is turned off on this TV for now. On webOS 10 and later it "
            "can leave the picture, the sound and HDMI control off until the TV is unplugged. "
            "Apps already hidden come back after the TV is next fully restarted.");
        int status = 200;
        if (action == "tileHiding") {
            if (access(dataFilePath(dataRoot_, ".from-homebrew-channel").c_str(), F_OK) == 0)
                return fail("hiding home-screen tiles is not available when installed from "
                            "the Homebrew Channel");
            const bool enable = service::javascriptTruthy(value);
            Json result = appTileOperation(enable ? "enable-all" : "disable-all", status);
            if (status == 403 && enable) return fail(heldText);
            response = result;
            return true;
        }
        Json change = Json::object();
        change.set("allowTileHiding", Json::flag(on));
        std::string error;
        if (!ConfigStore(configPath_).setTopLevel(change, &error))
            return fail("could not save setting: " + error);
        reloadNode();
        // Switched off, the tiles already hidden come back.
        if (!on) (void)appTileOperation("disable-all", status);
        return answer();
    }
    if (action == "setWebos10ScreensaversAllowed") {
        // allowOnWebos10 is the old name: a null leaves it out of the file.
        Json change = Json::object();
        change.set("allowExperimentalScreensavers", Json::flag(on));
        change.set("allowOnWebos10", Json());
        std::string error;
        if (!ConfigStore(configPath_).setTopLevel(change, &error))
            return fail("could not save setting: " + error);
        reloadNode();
        return answer();
    }
    if (action == "setDashboardPort") {
        const std::string text = value.isString() ? trimmedText(value.string())
            : value.isNumber() ? compactNumber(value.number()) : std::string();
        bool digits = !text.empty() && text.size() <= 5;
        for (char c : text) digits = digits && std::isdigit(static_cast<unsigned char>(c));
        const int port = digits ? std::atoi(text.c_str()) : 0;
        if (!(port >= 1024 && port <= 65533) || (port >= 9996 && port <= 9998))
            return fail(say::msg("srv.port.invalid",
                "The port must be a number from 1024 to 65533, other than 9996 to 9998"));
        if (port == config_.port) return answer();
        if (!localPortIsFree(config_.host, port))
            return fail(say::msg("srv.port.inUse",
                "Another service on the TV is already using that port"));
        Json change = Json::object();
        change.set("port", Json::numeric(port));
        std::string error;
        if (!ConfigStore(configPath_).setTopLevel(change, &error))
            return fail("could not save setting: " + error);
        answer();
        response.set("restarting", Json::flag(true));
        response.set("newPort", Json::numeric(port));
        if (!restartServer(600, error) && logger_)
            logger_->error("port: " + error);
        return true;
    }
    if (action == "privacyAllOff") {
        {
            std::lock_guard<std::mutex> lock(privacyCacheMutex_);
            privacyCacheValid_ = false;
        }
        HttpRequest read;
        read.method = "GET";
        read.uri = "/api/privacy";
        read.version = "1.1";
        read.remoteAddress = "127.0.0.1";
        HttpExchange readExchange(std::move(read), std::string());
        sendPrivacyProjection(readExchange);
        const Json projection = Json::parse(readExchange.responseBody());
        std::vector<Json> todo;
        for (const Json &area : projection["simple"]["areas"].items())
            for (const Json &item : area["items"].items()) todo.push_back(item);
        // The ad block goes first: it is the slow one, and others may depend on its state.
        std::stable_sort(todo.begin(), todo.end(), [](const Json &a, const Json &b) {
            return (a["action"].string() == "setAdBlock" ? 1 : 0) >
                   (b["action"].string() == "setAdBlock" ? 1 : 0);
        });
        Json failed = Json::array();
        std::string failedText;
        for (const Json &item : todo) {
            Json step = Json::object();
            if (item["service"].isString()) {
                // Background services are switched off through their own route.
                Json toggle = Json::object();
                toggle.set("id", item["service"]);
                toggle.set("disabled", Json::flag(true));
                const std::string toggleBody = toggle.dump();
                HttpRequest request;
                request.method = "POST";
                request.uri = "/api/services/toggle";
                request.version = "1.1";
                request.remoteAddress = "127.0.0.1";
                request.contentLength = static_cast<long long>(toggleBody.size());
                request.headers.push_back({"Content-Type", "application/json"});
                HttpExchange toggleExchange(std::move(request), toggleBody);
                setBackgroundService(toggleExchange);
                step = Json::parse(toggleExchange.responseBody());
            } else {
                Json control = Json::object();
                control.set("action", item["action"]);
                control.set("value", item["value"]);
                step = executeMqttControl(control);
            }
            if (!step.isObject() || !step["ok"].boolean()) {
                failed.add(item["label"]);
                failedText += (failedText.empty() ? "" : ", ") + item["label"].string();
            }
        }
        {
            std::lock_guard<std::mutex> lock(privacyCacheMutex_);
            privacyCacheValid_ = false;
        }
        response = Json::object();
        response.set("ok", Json::flag(failed.size() == 0));
        response.set("done", Json::numeric(static_cast<double>(todo.size() - failed.size())));
        response.set("failed", failed);
        if (failed.size() != 0)
            response.set("error", Json::text("could not switch off: " + failedText));
        return true;
    }
    if (action == "reboot") {
        if (!config_.allowPower)
            return fail(say::msg("srv.powerOff", "power actions disabled (set allowPower)"));
        // Answer first; the shell then syncs and reboots.
        ChildProcessRunner *const children = &children_;
        std::thread([children]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
            ChildOptions options;
            options.arguments = {"/bin/sh", "-c", "sync; /sbin/reboot"};
            options.timeout = std::chrono::milliseconds(30000);
            (void)children->run(options);
        }).detach();
        response = Json::object();
        response.set("ok", Json::flag(true));
        response.set("note", Json::text("rebooting"));
        return true;
    }
    return false;
}

bool HttpFrontDoor::restartServer(unsigned delayMs, std::string &error) const {
    {
        std::lock_guard<std::mutex> lock(installJobMutex_);
        if (appInstallSnapshotReady_ && appInstallSnapshotWorking(appInstallSnapshot_)) {
            error = say::msg("srv.install.busy.restart",
                             "an install is in progress; restart when it has finished");
            return false;
        }
    }
    const std::string control = dataFilePath(dataRoot_, "tvwebctl");
    if (access(control.c_str(), F_OK) != 0) {
        error = "no tvwebctl found - restart manually to apply";
        return false;
    }
    ChildProcessRunner *const children = &children_;
    std::thread([this, children, control, delayMs]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
        // An install can start in the moment between the check above and the
        // restart, which would cut it off: the restart waits for it instead.
        for (;;) {
            {
                std::lock_guard<std::mutex> lock(installJobMutex_);
                if (!(appInstallSnapshotReady_ && appInstallSnapshotWorking(appInstallSnapshot_)))
                    break;
            }
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        ChildOptions options;
        // Its output goes nowhere: it outlives this process, whose pipes would
        // close under it, and a write to a closed pipe kills a shell.
        options.arguments = {"/bin/sh", "-c", "exec /bin/sh \"$0\" restart >/dev/null 2>&1 </dev/null", control};
        options.timeout = std::chrono::milliseconds(60000);
        (void)children->run(options);
    }).detach();
    return true;
}

bool HttpFrontDoor::nodeFreeRestart(bool checkOnly, int &status, std::string &body,
                                    std::string &error) const {
    Json answer = Json::object();
    {
        std::lock_guard<std::mutex> lock(installJobMutex_);
        if (appInstallSnapshotReady_ && appInstallSnapshotWorking(appInstallSnapshot_)) {
            status = 409;
            answer.set("ok", Json::flag(false));
            answer.set("error", Json::text(say::msg(
                "srv.install.busy.restart",
                "an install is in progress; restart when it has finished")));
            body = answer.dump();
            return true;
        }
    }
    if (!checkOnly && !restartServer(250, error)) {
        status = 500;
        answer.set("ok", Json::flag(false));
        answer.set("error", Json::text(error));
        body = answer.dump();
        return true;
    }
    status = 200;
    answer.set("ok", Json::flag(true));
    body = answer.dump();
    return true;
}

bool HttpFrontDoor::nodeFreeSetupContext(bool, Json &context, std::string &) const {
    context = Json::object();
    const std::string address = localIpv4Address();
    const std::string hostname = localHostname(address);
    context.set("hostname", hostname.empty() ? Json() : Json::text(hostname));
    Json homeAssistant = Json::object();
    homeAssistant.set("state", mqttStatus_ ? mqttStatus_()["state"] : Json::text("disabled"));
    context.set("homeAssistant", homeAssistant);
    context.set("handoff", Json());   // the daemon's own link is used where there is one
    // A TV not yet set up gets its Home Assistant names from its model and the
    // tail of its MAC (routes.js ownIdentity); one that has them keeps them.
    const Json file = currentConfigDocument();
    Json identity;
    if (!(service::javascriptTruthy(file["device"]["id"]) ||
          service::javascriptTruthy(file["mqtt"]["topicPrefix"]) ||
          service::javascriptTruthy(file["mqtt"]["host"]))) {
        std::string model;
        for (const char character : statsCollector()->deviceModel())
            if (std::isalnum(static_cast<unsigned char>(character)))
                model += static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
        if (model.empty() || model == "webostv") model = "tv";
        std::string tail;
        for (const char character : localMacAddress(address))
            if (std::isxdigit(static_cast<unsigned char>(character)))
                tail += static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
        if (tail.size() > 4) tail = tail.substr(tail.size() - 4);
        std::string key = model + (tail.empty() ? std::string() : "_" + tail);
        if (key.size() > 40) key.resize(40);
        identity = Json::object();
        identity.set("id", Json::text("lg_" + key));
        identity.set("prefix", Json::text("lgtv_" + key));
    }
    context.set("identity", identity);
    return true;
}

bool HttpFrontDoor::nodeFreeHistoryOverlay(int64_t, int64_t, const Json &ids, Json &overlay,
                                           std::string &) const {
    // The installed apps' names for the ids the history holds: all Node added.
    const Json known = statsCollector()->appTitles();
    Json titles = Json::object();
    if (ids.isObject() && known.isObject()) {
        for (const std::string &key : ids.keys())
            if (known[key].isString()) titles.set(key, known[key]);
    }
    overlay = Json::object();
    overlay.set("titles", titles);
    return true;
}

std::string HttpFrontDoor::localHostname(const std::string &address) const {
    return hostnames_.name(address);
}

StatsCollector *HttpFrontDoor::statsCollector() const {
    std::call_once(statsCollectorOnce_, [this]() {
        StatsDeps deps;
        deps.luna = [this](const std::string &uri, const Json &payload, Json &reply) {
            LunaReply answer;
            const bool answered = waitForLuna(luna_, uri, payload, answer);
            reply = answered ? answer.response : Json();
            return answered;
        };
        deps.liveGroup = [this](const std::string &group) {
            return stateSubscriptions_ && stateSubscriptions_->isLive(group);
        };
        deps.config = [this]() { return currentConfigDocument(); };
        deps.systemRoot = systemRoot_;
        deps.version = displayVersion();
        deps.address4 = []() { return localIpv4Address(); };
        deps.hostname = [this](const std::string &address) { return localHostname(address); };
        deps.hdmiReceiverMap = [this]() { return hdmiReceiverMap(); };
        deps.liveMedia = [this]() -> Json {
            if (!stateSubscriptions_ || !stateSubscriptions_->isLive("media")) return Json();
            const Json encoded = stateSubscriptions_->snapshot()["media"]["snapshot"];
            return encoded.isString() ? Json::parse(encoded.string()) : Json();
        };
        deps.adBlock = [this]() {
            service::AdBlockStore fallbackStore(dataRoot_, systemRoot_);
            const service::AdBlockStore &store = adBlockManager_ ? adBlockManager_->store() : fallbackStore;
            const Json state = store.privacyState(adBlockManager_ ? adBlockManager_->sdpGraceActive() : true);
            Json shape = Json::object();
            shape.set("enabled", state["enabled"]);
            shape.set("count", state["count"]);
            return shape;
        };
        deps.powerOn = [this]() {
            // tvpower gives the reason and the times only when asked; they are
            // read at start and after each transition, and the times moved on.
            std::lock_guard<std::mutex> lock(powerOnMutex_);
            const long long now = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            if (!powerOnFacts_.valid() || now - powerOnReadAt_ > 300000) {
                LunaReply reason, time;
                (void)waitForLuna(luna_, "com.webos.service.tvpower/power/getPowerOnReason", Json::object(), reason);
                (void)waitForLuna(luna_, "com.webos.service.tvpower/power/getPowerOnTime", Json::object(), time);
                Json facts = Json::object();
                facts.set("onReason", reason.response["returnValue"].isBoolean() &&
                                      !reason.response["returnValue"].boolean()
                    ? Json() : (reason.response["reason"].isString() && !reason.response["reason"].string().empty()
                        ? reason.response["reason"] : Json()));
                const Json t = time.response["time"];
                Json parsed;
                if (t.isObject() && !(time.response["returnValue"].isBoolean() && !time.response["returnValue"].boolean())) {
                    parsed = Json::object();
                    const auto seconds = [](const Json &v) {
                        if (v.isNumber()) return Json::numeric(v.number());
                        if (v.isString() && !v.string().empty()) {
                            char *end = nullptr;
                            const double n = std::strtod(v.string().c_str(), &end);
                            if (end != v.string().c_str()) return Json::numeric(n);
                        }
                        return Json();
                    };
                    parsed.set("uptime", seconds(t["uptime"]));
                    parsed.set("ontime", seconds(t["ontime"]));
                }
                facts.set("onTime", parsed);
                powerOnFacts_ = facts;
                powerOnReadAt_ = now;
            }
            Json out = Json::object();
            out.set("onReason", powerOnFacts_["onReason"]);
            const Json stored = powerOnFacts_["onTime"];
            if (stored.isObject()) {
                const double since = static_cast<double>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count() - powerOnReadAt_) / 1000.0;
                const auto movedOn = [](double v, double by) { return std::round((v + by) * 100) / 100; };
                Json moved = Json::object();
                moved.set("uptime", stored["uptime"].isNumber()
                    ? Json::numeric(movedOn(stored["uptime"].number(), since)) : Json());
                moved.set("ontime", stored["ontime"].isNumber() && stored["ontime"].number() != 0
                    ? Json::numeric(movedOn(stored["ontime"].number(), since)) : stored["ontime"]);
                out.set("onTime", moved);
            } else {
                out.set("onTime", Json());
            }
            return out;
        };
        struct OledStatsState {
            std::mutex mutex;
            int panel = -1;  // -1 undecided, 0 not OLED, 1 OLED
            Json cached;
            service::OledCareSources cachedSources;
            std::chrono::steady_clock::time_point cachedAt;
        };
        const std::shared_ptr<OledStatsState> oledState = std::make_shared<OledStatsState>();
        deps.isOled = [this, oledState]() {
            std::lock_guard<std::mutex> lock(oledState->mutex);
            if (oledState->panel >= 0) return oledState->panel == 1;
            const Json forced = service::javascriptTruthy(config_.document["panel"])
                ? config_.document["panel"] : config_.document["device"]["panel"];
            if (service::javascriptTruthy(forced)) {
                oledState->panel = service::oledModel(service::javascriptString(forced)) ? 1 : 0;
            } else if (service::hasOledPanelTypeMarker(systemRoot_)) {
                oledState->panel = 1;
            } else {
                Json request = Json::object();
                Json keys = Json::array();
                keys.add(Json::text("modelName"));
                request.set("keys", keys);
                LunaReply reply;
                const bool answered = waitForLuna(
                    luna_, "com.webos.service.tv.systemproperty/getSystemProperties", request, reply);
                std::string model = service::javascriptTruthy(reply.response["modelName"])
                    ? service::javascriptString(reply.response["modelName"]) : std::string();
                // "webOS TV" is the placeholder telemetry sets when it could not read one.
                if (model == "webOS TV") model.clear();
                if (model.empty() && service::javascriptTruthy(config_.document["device"]["model"]))
                    model = service::javascriptString(config_.document["device"]["model"]);
                // A lost answer at start must not settle the panel type for good.
                if (model.empty() && (!answered || !reply.response["returnValue"].boolean()))
                    return false;
                oledState->panel = service::oledModel(model) || service::hasOledPanelRecords(systemRoot_) ? 1 : 0;
            }
            return oledState->panel == 1;
        };
        deps.oledStats = [this, oledState](const Json &pictureSettings, const Json &) {
            std::lock_guard<std::mutex> lock(oledState->mutex);
            const auto now = std::chrono::steady_clock::now();
            const bool fresh = oledState->cached.valid() && now - oledState->cachedAt < std::chrono::seconds(30);
            if (fresh) {
                // Inside the 30 s the Luna reads stand; the picture settings and
                // the compensation state, which are cheap, are brought up to date.
                // Copies throughout: a Json is a handle, and the answer that holds
                // the last one may be on its way out on another thread.
                Json out = Json::parse(oledState->cached.dump());
                if (service::javascriptTruthy(pictureSettings["screenShift"]))
                    out.set("screen_shift", pictureSettings["screenShift"]);
                if (service::javascriptTruthy(pictureSettings["logoLuminanceAdjust"]))
                    out.set("logo_dimming", pictureSettings["logoLuminanceAdjust"]);
                const Json now = service::oledStatsObject(oledState->cachedSources);
                out.set("comp_status", now["comp_status"]);
                out.set("comp_status_label", now["comp_status_label"]);
                oledState->cached = Json::parse(out.dump());
                return out;
            }
            service::OledCareSources sources;
            sources.systemRoot = systemRoot_;
            sources.isOled = true;
            sources.pictureSettings = pictureSettings;
            Json propertiesRequest = Json::object();
            Json propertyKeys = Json::array();
            propertyKeys.add(Json::text("panelUsageTime"));
            propertyKeys.add(Json::text("lastCompensationTimestamp"));
            propertiesRequest.set("keys", propertyKeys);
            LunaReply propertiesReply;
            if (waitForLuna(luna_, "com.webos.service.tv.systemproperty/getSystemProperties",
                            propertiesRequest, propertiesReply))
                sources.systemProperties = propertiesReply.response;
            if (!service::javascriptTruthy(sources.systemProperties["panelUsageTime"])) {
                Json request = Json::object();
                request.set("subscribe", Json::flag(false));
                LunaReply panelReply;
                if (waitForLuna(luna_, "com.webos.service.panelcontroller/getPanelUsageTime",
                                request, panelReply))
                    sources.panelController = panelReply.response;
            }
            LunaReply displayReply;
            (void)waitForLuna(luna_, "com.webos.service.tv.display/getClearPanelNoiseStatus",
                              Json::object(), displayReply);
            sources.displayStatus = displayReply.response;
            LunaReply gsrReply;
            if (waitForLuna(luna_, "com.webos.service.oledepl/getGlobalStressReduction",
                            Json::object(), gsrReply) &&
                gsrReply.response["returnValue"].boolean()) {
                Json protections = Json::object();
                protections.set("gsr", Json::flag(gsrReply.response["enable"].boolean()));
                protections.set("gsrStressCount", gsrReply.response["stressCount"].isNumber()
                    ? gsrReply.response["stressCount"] : Json());
                LunaReply tpcReply;
                protections.set("tpc", waitForLuna(luna_, "com.webos.service.oledepl/getTemporalPeakControl",
                                                   Json::object(), tpcReply) &&
                                       tpcReply.response["returnValue"].boolean()
                    ? Json::flag(tpcReply.response["enable"].boolean()) : Json());
                sources.liveProtections = protections;
            } else {
                Json request = Json::object();
                Json keys = Json::array();
                keys.add(Json::text("OledTPC"));
                keys.add(Json::text("OledGSR"));
                request.set("keys", keys);
                LunaReply reply;
                if (waitForLuna(luna_, "com.webos.service.tv.systemproperty/getProperties", request, reply) &&
                    reply.response["returnValue"].boolean() && reply.response["OledTPC"].valid()) {
                    Json protections = Json::object();
                    protections.set("gsr", Json::flag(service::javascriptString(reply.response["OledGSR"]) == "true"));
                    protections.set("tpc", Json::flag(service::javascriptString(reply.response["OledTPC"]) == "true"));
                    protections.set("gsrStressCount", Json());
                    sources.liveProtections = protections;
                }
            }
            oledState->cachedSources = sources;
            oledState->cached = service::oledStatsObject(sources);
            oledState->cachedAt = std::chrono::steady_clock::now();
            return Json::parse(oledState->cached.dump());
        };
        deps.sourceFrameRate = [this](const std::string &appId) -> Json {
            // Asked with the app id of the HDMI input on screen, or empty off one.
            if (!gameFrameRate_) return Json();
            const Json reading = gameFrameRate_->readForStats(!appId.empty());
            if (appId.empty() || !reading.isObject()) return Json();
            Json aliases = Json::object();
            for (const Json &device : hdmiInputDevices().items()) {
                const Json key = names::hdmiKeyForPortId(device["id"].string());
                const std::string deviceApp = device["appId"].string();
                if (key.isString() && !deviceApp.empty() && names::shortAppId(deviceApp) != key.string())
                    aliases.set(names::shortAppId(deviceApp), key);
            }
            // A reading counts only while its input is the app on screen: the
            // input's pipeline keeps running behind an app.
            const std::string port = reading["port"].isString() ? reading["port"].string() : std::string();
            const Json portKey = names::hdmiKeyForPortId(port);
            if (!portKey.isString() || portKey.string() != names::shortAppId(appId, aliases)) return Json();
            Json rate = Json::object();
            rate.set("hz", reading["frameRate"]);
            rate.set("vrr_type", reading["vrrType"]);
            rate.set("port", reading["port"]);
            return rate;
        };
        deps.screensaverMode = [this]() {
            const Json document = currentConfigDocument();
            return screensaverManager_
                ? service::screensaverMode(screensaverManager_->state(config_.allowControl, false))
                : std::string("stock");
        };
        deps.screensaverLevel = [this]() {
            return screensaverManager_
                ? service::screensaverLevel(screensaverManager_->state(config_.allowControl, false))
                : std::string("dim");
        };
        deps.screensaverLaunched = [this]() {
            return access(rootedSystemPath(systemRoot_, "/var/run/tvweb-screensaver-launched").c_str(), F_OK) == 0;
        };
        deps.hasMediaState = [this]() {
            return access(dataFilePath(dataRoot_, "media_seen").c_str(), F_OK) == 0;
        };
        deps.nowMs = []() {
            return static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        };
        deps.isoTime = [](long long ms) {
            const time_t seconds = static_cast<time_t>(ms / 1000);
            struct tm parts;
            gmtime_r(&seconds, &parts);
            char text[96];  // wide enough for any int, as the ARM compiler checks
            snprintf(text, sizeof(text), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", parts.tm_year + 1900,
                     parts.tm_mon + 1, parts.tm_mday, parts.tm_hour, parts.tm_min, parts.tm_sec,
                     static_cast<int>(ms % 1000));
            return std::string(text);
        };
        statsCollector_.reset(new StatsCollector(std::move(deps)));
    });
    return statsCollector_.get();
}

Json HttpFrontDoor::mqttDiscoveryInputs() const { return statsCollector()->discoveryInputs(); }

Json HttpFrontDoor::composeMqttTelemetry(const Json &system, const Json &extra) const {
    const Json stats = statsCollector()->collect(withStatsWindows(system, "publish"));
    if (!stats["ok"].boolean()) return Json();
    MqttPublishOptions options;
    options.lgRows = extra["rows"].isArray() ? extra["rows"] : Json::array();
    const Json systemOn = stateSubscriptions_ ? stateSubscriptions_->snapshot()["power"]["systemOn"] : Json();
    options.tvOff = systemOn.isBoolean() && !systemOn.boolean();

    const long long nowMs = static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
    if (historyRecorder_) {
        // The TV's UTC offset, from the local time and UTC it reports together,
        // read hourly as daylight saving changes it.
        bool due;
        {
            std::lock_guard<std::mutex> lock(utcOffsetMutex_);
            due = !utcOffsetReadAt_ || nowMs - utcOffsetReadAt_ > 3600000;
            if (due) utcOffsetReadAt_ = nowMs;
        }
        if (due) {
            LunaReply time;
            const Json local = waitForLuna(luna_, "com.webos.service.systemservice/time/getSystemTime",
                                           Json::object(), time) ? time.response["localtime"] : Json();
            if (local.isObject() && time.response["utc"].isNumber()) {
                struct tm parts = {};
                parts.tm_year = static_cast<int>(local["year"].number()) - 1900;
                parts.tm_mon = static_cast<int>(local["month"].number()) - 1;
                parts.tm_mday = static_cast<int>(local["day"].number());
                parts.tm_hour = static_cast<int>(local["hour"].number());
                parts.tm_min = static_cast<int>(local["minute"].number());
                parts.tm_sec = static_cast<int>(local["second"].number());
                const double offset = (static_cast<double>(timegm(&parts)) - time.response["utc"].number()) / 60;
                std::lock_guard<std::mutex> lock(utcOffsetMutex_);
                // To the quarter hour, the finest any time zone uses.
                utcOffsetMinutes_ = static_cast<int>(std::floor(offset / 15 + 0.5)) * 15;
            }
        }
        int offsetMinutes;
        {
            std::lock_guard<std::mutex> lock(utcOffsetMutex_);
            offsetMinutes = utcOffsetMinutes_;
        }
        options.watch = historyRecorder_->summary(nowMs, offsetMinutes);
    }

    const Json capture = piccapStatus()->read();
    if (capture.isObject()) {
        Json state = Json::object();
        state.set("isRunning", capture["running"]);
        options.piccapState = state;
    }

    // Kept on disk too, so a server started while the TV is off still knows
    // what was last on screen.
    const std::string app = stats["app"].isString() ? stats["app"].string() : std::string();
    const std::string appId = stats["app_id"].isString() ? stats["app_id"].string() : std::string();
    {
        std::lock_guard<std::mutex> lock(lastAppMutex_);
        if (!mqttLastAppLoaded_) {
            mqttLastAppLoaded_ = true;
            const Json saved = JsonFile(lastAppPath_, 4096).load().value;
            mqttLastApp_ = saved["app"].isString() ? saved["app"].string() : std::string();
            mqttLastAppId_ = saved["app_id"].isString() ? saved["app_id"].string() : std::string();
        }
        if (!options.tvOff && ((!app.empty() && app != mqttLastApp_) ||
                               (!appId.empty() && appId != mqttLastAppId_))) {
            if (!app.empty()) mqttLastApp_ = app;
            if (!appId.empty()) mqttLastAppId_ = appId;
            Json snapshot = Json::object();
            snapshot.set("app", mqttLastApp_.empty() ? Json() : Json::text(mqttLastApp_));
            snapshot.set("app_id", mqttLastAppId_.empty() ? Json() : Json::text(mqttLastAppId_));
            std::string error;
            if (!recordLastApp(lastAppPath_, snapshot, &error) && logger_)
                logger_->warning("last_app: " + error);
        }
        options.lastApp = mqttLastApp_;
        options.lastAppId = mqttLastAppId_;
    }
    return mqttPublishPayload(stats, options);
}

service::PiccapStatus *HttpFrontDoor::piccapStatus() const {
    std::call_once(piccapStatusOnce_, [this]() {
        piccapStatus_.reset(new service::PiccapStatus(
            systemRoot_,
            [this](Json &reply, std::string &raw) {
                LunaReply answer;
                const bool answered = waitForLuna(
                    luna_, "org.webosbrew.piccap.service/status", Json::object(), answer);
                reply = answered ? answer.response : Json();
                raw = answer.raw;
                return answered;
            },
            []() {
                return static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count());
            }));
    });
    return piccapStatus_.get();
}

HomeOrchestrator *HttpFrontDoor::homeOrchestrator() const {
    const char *enabled = std::getenv("TVWEB_NATIVE_HOME_ORCH");
    if (!enabled || std::string(enabled) != "1" || !homeTakeoverLifecycleManager_ || !eventLoop_)
        return nullptr;
    std::call_once(homeOrchestratorOnce_, [this]() {
        HomeOrchestratorDeps deps;
        const auto readHome = [this]() {
            std::lock_guard<std::mutex> lock(homeConfigMutex_);
            JsonFile file(dataFilePath(dataRoot_, "home.json"), 8 * 1024 * 1024);
            const JsonFileResult stored = file.load();
            return service::normalizeHomeDocument(stored.found ? stored.value : Json::object());
        };
        deps.config = readHome;
        deps.update = [this](const std::function<void(Json &)> &mutate, std::string &error) {
            std::lock_guard<std::mutex> lock(homeConfigMutex_);
            JsonFile file(dataFilePath(dataRoot_, "home.json"), 8 * 1024 * 1024);
            const JsonFileResult stored = file.load();
            if (!stored.error.empty() && !stored.found) {
                error = stored.error;
                return false;
            }
            Json document = service::normalizeHomeDocument(stored.found ? stored.value : Json::object());
            document = Json::parse(document.dump());
            mutate(document);
            return file.save(service::normalizeHomeDocument(document), &error);
        };
        deps.supported = [this]() { return homeSupported(false); };
        deps.clientRunning = [this]() {
            LunaReply reply;
            if (!waitForLuna(luna_, "com.webos.applicationManager/running", Json::object(), reply) ||
                !reply.response["running"].isArray())
                return -1;
            for (const Json &app : reply.response["running"].items())
                if (app["id"].string() == "com.tvweb.native") return 1;
            return 0;
        };
        deps.release = [this]() {
            Json failed = Json::object();
            failed.set("ok", Json::flag(false));
            failed.set("error", Json::text("native Home takeover is unavailable"));
            if (!homeTakeoverLifecycleManager_) return failed;
            std::lock_guard<std::mutex> operationLock(homeOperationMutex_);
            return homeTakeoverLifecycleManager_->release(controlPolicyContext(config_));
        };
        deps.nowMs = []() {
            return static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
        };
        // config.json's nativeClient.stubGraceMs tunes the watch without a
        // rebuild (a TV that starts the client slowly, a test).
        const Json nativeClient = config_.document["nativeClient"];
        if (nativeClient["stubGraceMs"].isNumber() && nativeClient["stubGraceMs"].number() > 0)
            deps.watch.stubGraceMs = static_cast<long long>(nativeClient["stubGraceMs"].number());
        deps.mounted = [this]() { return homeTakeoverMounted(systemRoot_, dataRoot_); };
        deps.escaped = [this]() { return homeEscapeActive_ && homeEscapeActive_->load(); };
        deps.startLayoutAvailable = [this]() {
            return homeStatusWebosVersion(systemRoot_).compare(0, 2, "9.") == 0;
        };
        deps.blankKept = [this]() {
            // The empty layout is in place, or LG's is kept aside from it.
            static const char *const files[] = {"home_layoutShelfView.xml", "home_layout.xml"};
            for (const char *file : files) {
                std::ifstream stream(rootedSystemPath(systemRoot_, std::string("/mnt/lg/cmn_data/manifest/") + file));
                std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
                if (text.find("<!-- glasshouse-home-starting -->") != std::string::npos) return true;
            }
            return access((dataRoot_ + "/home-takeover-lg-layout").c_str(), F_OK) == 0;
        };
        deps.takeover = [this](bool blank) {
            Json failed = Json::object();
            failed.set("ok", Json::flag(false));
            failed.set("error", Json::text("native Home takeover is unavailable"));
            if (!homeTakeoverLifecycleManager_) return failed;
            std::lock_guard<std::mutex> operationLock(homeOperationMutex_);
            return homeTakeoverLifecycleManager_->takeover(blank, controlPolicyContext(config_));
        };
        deps.disable = [this]() {
            // The switch-off the dashboard's route runs, asked in-process.
            const std::string body = "{}";
            HttpRequest request;
            request.method = "POST";
            request.uri = "/api/home/disable";
            request.version = "1.1";
            request.remoteAddress = "127.0.0.1";
            request.contentLength = static_cast<long long>(body.size());
            request.headers.push_back({"Content-Type", "application/json"});
            HttpExchange exchange(std::move(request), body);
            handleHomeDisablePost(exchange);
            const Json answer = Json::parse(exchange.responseBody());
            Json result = Json::object();
            result.set("ok", Json::flag(answer["ok"].boolean()));
            if (!answer["ok"].boolean())
                result.set("error", Json::text(answer["error"].string("the switch-off failed")));
            return result;
        };
        deps.startingNotice = [this, readHome](bool on) {
            Json home = Json::parse(readHome().dump());
            home.set("enabled", Json::flag(on));
            return updateHomeStartingNotice(home);
        };
        deps.syncAutoLaunch = [this](bool escaped) { return synchronizeHomeAutoLaunch(escaped); };
        deps.launch = [this](const std::string &appId) {
            Json payload = Json::object();
            payload.set("id", Json::text(appId));
            LunaReply reply;
            return waitForLuna(luna_, "com.webos.applicationManager/launch", payload, reply) &&
                   reply.response["returnValue"].boolean();
        };
        deps.powerOnReason = [this]() {
            LunaReply reply;
            if (!waitForLuna(luna_, "com.webos.service.tvpower/power/getPowerOnReason", Json::object(), reply))
                return std::string();
            return reply.response["reason"].isString() ? reply.response["reason"].string() : std::string();
        };
        deps.readSettings = [this](const Json &keys) {
            Json payload = Json::object();
            payload.set("category", Json::text("general"));
            payload.set("keys", keys);
            LunaReply reply;
            if (!waitForLuna(luna_, "com.webos.service.settings/getSystemSettings", payload, reply) ||
                (reply.response["returnValue"].isBoolean() && !reply.response["returnValue"].boolean()) ||
                !reply.response["settings"].isObject())
                return Json();
            return reply.response["settings"];
        };
        deps.writeSettings = [this](const Json &settings) {
            Json payload = Json::object();
            payload.set("category", Json::text("general"));
            payload.set("settings", settings);
            LunaReply reply;
            return waitForLuna(luna_, "com.webos.service.settings/setSystemSettings", payload, reply) &&
                   reply.response["returnValue"].boolean();
        };
        deps.homeKeyApp = [this]() {
            JsonFile file(dataFilePath(dataRoot_, "remotekeys.json"), 64 * 1024);
            const JsonFileResult stored = file.load();
            return stored.found ? stored.value["home"].string() : std::string();
        };
        deps.clearHomeKey = [this, readHome]() {
            // On JavaScript-filter firmware the compositor's filter keeps the old
            // app until Node's remotekeys applies the map again.
            JsonFile file(dataFilePath(dataRoot_, "remotekeys.json"), 64 * 1024);
            const JsonFileResult stored = file.load();
            if (!stored.found || !stored.value.isObject()) return true;
            Json document = Json::object();
            for (const std::string &key : stored.value.keys())
                if (key != "home") document.set(key, stored.value[key]);
            std::string error;
            return file.save(document, &error) && updateHomeStartingNotice(readHome());
        };
        deps.legacyAppInstalled = [this]() {
            Json payload = Json::object();
            payload.set("id", Json::text("com.tvweb.home"));
            LunaReply reply;
            if (!waitForLuna(luna_, "com.webos.applicationManager/getAppInfo", payload, reply)) return -1;
            if (reply.response["returnValue"].isBoolean() && reply.response["returnValue"].boolean()) return 1;
            std::string text = reply.response["errorText"].string();
            std::transform(text.begin(), text.end(), text.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return reply.response["returnValue"].isBoolean() && text.find("invalid appid") != std::string::npos ? 0 : -1;
        };
        deps.legacyAppRunning = [this]() {
            LunaReply reply;
            if (!waitForLuna(luna_, "com.webos.applicationManager/running", Json::object(), reply) ||
                !reply.response["running"].isArray())
                return -1;
            for (const Json &app : reply.response["running"].items())
                if (app["id"].string() == "com.tvweb.home") return 1;
            return 0;
        };
        deps.killLegacyApp = [this]() {
            Json payload = Json::object();
            payload.set("appId", Json::text("com.tvweb.home"));
            LunaReply reply;
            (void)waitForLuna(luna_, "com.palm.webappmanager/killApp", payload, reply);
        };
        deps.removeLegacyApp = [this]() { (void)runTvApp("remove", "home-app"); };
        deps.after = [this](long long delay, std::function<void()> task) {
            return eventLoop_->scheduleAfter(std::chrono::milliseconds(delay), std::move(task));
        };
        deps.cancel = [this](unsigned long long id) { (void)eventLoop_->cancelTimer(id); };
        deps.blocking = [this](std::function<void()> work) { (void)eventLoop_->submitBlocking(std::move(work)); };
        deps.log = [this](const std::string &line) {
            if (logger_) logger_->info(line);
        };
        deps.logError = [this](const std::string &line) {
            if (logger_) logger_->error(line);
        };
        homeOrchestrator_.reset(new HomeOrchestrator(std::move(deps)));
    });
    return homeOrchestrator_.get();
}

HomeProbeGate *HttpFrontDoor::homeProbeGate() const {
    std::call_once(homeProbeGateOnce_, [this]() {
        homeProbeGate_.reset(new HomeProbeGate(
            children_, homeProbeOptions(config_.document, systemRoot_, dataRoot_)));
    });
    return homeProbeGate_.get();
}

Json HttpFrontDoor::homeSupported(bool probe) const {
    const Json supported = service::homeStatusSupported(
        homeStatusWebosVersion(systemRoot_),
        config_.document["allowExperimentalHome"].boolean());
    if (!supported["ok"].boolean()) return supported;
    return homeSupportedWithProbe(supported, homeProbeGate()->verdict(probe));
}

bool HttpFrontDoor::homeHalted() const {
    const HomeOrchestrator *orchestrator = homeOrchestrator();
    return orchestrator && orchestrator->halted();
}

ScreensaverControl *HttpFrontDoor::screensaverControl() const {
    if (!screensaverManager_ || !samRescanManager_ || !commandPolicy_) return nullptr;
    std::call_once(screensaverControlOnce_, [this]() {
        const std::string appDir = rootedSystemPath(
            systemRoot_, "/usr/palm/applications/com.webos.app.screensaver");
        const std::string marker = rootedSystemPath(systemRoot_, "/var/run/tvweb-screensaver-launched");
        ScreensaverControlDeps deps;
        deps.luna = [this](const std::string &uri, const Json &payload, Json &reply) {
            LunaReply answer;
            const bool answered = waitForLuna(luna_, uri, payload, answer);
            reply = answered ? answer.response : Json();
            return answered;
        };
        deps.injectBack = [this]() { return service::injectRemoteBackKey(procRoot_, systemRoot_); };
        deps.state = [this]() {
            const Json document = currentConfigDocument();
            return screensaverManager_->state(
                config_.allowControl,
                document["allowExperimentalScreensavers"].boolean() ||
                    document["allowOnWebos10"].boolean());
        };
        deps.stage = [this](const std::string &mode, const std::string &level) {
            CommandPolicySpec localCommand;
            localCommand.action = "screensaver-stage";
            localCommand.uri = "native/screensaver/stage";
            localCommand.timeout = std::chrono::milliseconds(25000);
            localCommand.requiresControl = false;
            localCommand.resource = "screensaver-mount";
            Json result = Json::object();
            LunaReply localReply;
            const CommandDecision decision = commandPolicy_->executeLocal(
                localCommand, controlPolicyContext(config_), false,
                [this, &mode, &level, &result]() {
                    result = screensaverManager_->prepare(
                        mode, level, static_cast<unsigned short>(config_.port), config_.token);
                    return result["ok"].boolean();
                }, localReply);
            if (decision.kind != CommandDecisionKind::Allowed || !result.isObject()) {
                result = Json::object();
                result.set("ok", Json::flag(false));
                result.set("error", Json::text(!localReply.error.empty() ? localReply.error
                    : !decision.reason.empty() ? decision.reason
                    : "could not update the screen saver staging"));
            }
            return result;
        };
        deps.restoreStock = [this]() {
            CommandPolicySpec localCommand;
            localCommand.action = "screensaver-stage";
            localCommand.uri = "native/screensaver/stage";
            localCommand.timeout = std::chrono::milliseconds(25000);
            localCommand.requiresControl = false;
            localCommand.resource = "screensaver-mount";
            LunaReply localReply;
            (void)commandPolicy_->executeLocal(
                localCommand, controlPolicyContext(config_), false,
                [this]() { return screensaverManager_->restoreStock()["ok"].boolean(); },
                localReply);
        };
        deps.mountedType = [appDir]() -> std::string {
            std::ifstream input(appDir + "/appinfo.json");
            std::string contents((std::istreambuf_iterator<char>(input)),
                                 std::istreambuf_iterator<char>());
            const Json manifest = Json::parse(contents);
            return manifest.isObject() ? manifest["type"].string() : std::string();
        };
        deps.samReread = [this]() {
            CommandPolicyContext context = controlPolicyContext(config_);
            return samRescanManager_->rereadApp("com.webos.app.screensaver", context)
                ["result"].boolean();
        };
        deps.samNudge = [this]() {
            return samRescanManager_->nudge(controlPolicyContext(config_))["result"].boolean();
        };
        deps.sleepMs = [](unsigned milliseconds) {
            std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
        };
        deps.writeLaunchedMarker = [marker](bool on) {
            if (on) {
                std::ofstream output(marker.c_str(), std::ios::trunc);
                output << "1\n";
            } else {
                (void)unlink(marker.c_str());
            }
        };
        deps.launchedMarkerExists = [marker]() { return access(marker.c_str(), F_OK) == 0; };
        screensaverControl_.reset(new ScreensaverControl(std::move(deps)));
    });
    return screensaverControl_.get();
}

Json HttpFrontDoor::homeScreenAction(const Json &command) const {
    const std::string body = command.dump();
    HttpRequest request;
    request.method = "POST";
    request.uri = "/_internal/home-screen";
    request.version = "1.1";
    request.remoteAddress = "127.0.0.1";
    request.contentLength = static_cast<long long>(body.size());
    request.headers.push_back({"Content-Type", "application/json"});
    request.headers.push_back({"X-NativeTv-Proxy-Secret", proxySecret_});
    HttpExchange exchange(std::move(request), body);
    setNativeHomeScreen(exchange);
    const Json envelope = Json::parse(exchange.responseBody());
    Json failure = Json::object();
    failure.set("ok", Json::flag(false));
    if (!envelope.isObject()) {
        failure.set("error", Json::text("Home screen operation failed"));
        return failure;
    }
    if (!envelope["ok"].boolean()) {
        failure.set("error", Json::text(envelope["error"].string().empty()
            ? "Home screen operation failed" : envelope["error"].string()));
        return failure;
    }
    return envelope["result"];
}

namespace {
// The tile's title for an HDMI input: the CEC names of what is connected, then
// the TV's own name for the input where it differs from the port's.
std::string cecInputTitle(const std::string &id, const Json &device) {
    std::vector<std::string> deviceNames;
    for (const Json &sub : device["subList"].items()) {
        if (sub["id"].string() != "SIMPLINK" || !sub["osdName"].isString()) continue;
        std::string name;
        bool space = false;
        for (unsigned char c : sub["osdName"].string()) {
            if (std::isspace(c)) { space = true; continue; }
            if (space && !name.empty()) name.push_back(' ');
            space = false;
            name.push_back(static_cast<char>(c));
        }
        if (!name.empty() &&
            std::find(deviceNames.begin(), deviceNames.end(), name) == deviceNames.end())
            deviceNames.push_back(name);
    }
    Json portVariables = Json::object();
    portVariables.set("n", Json::text(id.substr(4)));
    const std::string port = say::msg("srv.home.hdmi", "HDMI {n}", portVariables);
    const std::string label = device["label"].isString()
        ? trimmedText(device["label"].string()) : std::string();
    if (deviceNames.empty() && !label.empty() && label != port && label != device["id"].string())
        deviceNames.push_back(label);
    if (deviceNames.empty()) return port;
    std::string joined;
    for (const std::string &name : deviceNames) joined += (joined.empty() ? "" : " / ") + name;
    Json variables = Json::object();
    variables.set("port", Json::text(port));
    variables.set("devices", Json::text(joined));
    return say::msg("srv.home.hdmiDevices", "{devices} \xE2\x80\x94 {port}", variables);
}
} // namespace

void HttpFrontDoor::refreshHomeInputTitles() const {
    if (!homeScreenManager_ || !commandPolicy_ || !homeScreenManager_->isWebos9()) return;
    std::unique_lock<std::mutex> guard(homeTitleRefreshMutex_, std::try_to_lock);
    if (!guard.owns_lock()) return;
    const Json added = homeScreenManager_->state()["addedInputs"];
    if (!added.isObject() || added.keys().empty()) return;
    const auto call = [this](const std::string &action, const std::string &uri,
                             const Json &payload, const std::string &resource,
                             LunaReply &reply) {
        return waitForCommandPolicyLuna(
            luna_, commandPolicy_, controlPolicyContext(config_), action, uri, payload,
            resource, false, reply, std::string(), false, std::chrono::milliseconds(5000));
    };
    LunaReply inputs;
    if (!call("home-screen.luna.read", "com.webos.service.eim/getAllInputStatus",
              Json::object(), "home-screen-input-status", inputs) ||
        returnedFalseReply(inputs.response) || !inputs.response["devices"].isArray())
        return;
    std::map<std::string, Json> devices;
    for (const Json &device : inputs.response["devices"].items()) {
        const std::string id = device["id"].string();
        if (id.size() == 6 && id.compare(0, 5, "HDMI_") == 0 && id[5] >= '1' && id[5] <= '4')
            devices["hdmi" + id.substr(5)] = device;
    }
    LunaReply points;
    if (!call("home-screen.luna.read", "com.webos.applicationManager/listLaunchPoints",
              Json::object(), "home-screen-launch-points", points) ||
        returnedFalseReply(points.response) || !points.response["launchPoints"].isArray())
        return;
    const std::string prefix = "com.webos.app.";
    for (const Json &point : points.response["launchPoints"].items()) {
        const std::string appId = point["id"].string();
        if (appId.compare(0, prefix.size(), prefix) != 0) continue;
        const std::string input = appId.substr(prefix.size());
        const auto device = devices.find(input);
        if (device == devices.end() || added[input].string() != point["launchPointId"].string())
            continue;
        const std::string title = cecInputTitle(input, device->second);
        if (point["title"].string() == title && point["titleVisible"].isBoolean() &&
            point["titleVisible"].boolean())
            continue;
        Json update = Json::object();
        update.set("launchPointId", point["launchPointId"]);
        update.set("title", Json::text(title));
        update.set("titleVisible", Json::flag(true));
        LunaReply updated;
        if (!call("home-screen.luna.update-input",
                  "com.webos.applicationManager/updateLaunchPoint", update,
                  "home-screen-launch-points", updated) ||
            !updated.response["returnValue"].boolean()) {
            if (logger_) logger_->error("homescreen: could not update an HDMI tile title");
        }
    }
}

namespace {
// LG's list with the owner's apps switched off. Apps not switched off keep
// LG's own isEnabled, which is false for some (AirPlay on a B8 and a C2).
Json preloadListWithout(const Json &list, const Json &off) {
    Json want = Json::array();
    for (const Json &entry : list.items()) {
        Json copy = Json::object();
        for (const std::string &key : entry.keys()) copy.set(key, entry[key]);
        for (const Json &id : off.items())
            if (id.isString() && id.string() == entry["id"].string())
                copy.set("isEnabled", Json::flag(false));
        want.add(copy);
    }
    return want;
}
} // namespace

void HttpFrontDoor::resendPreload() const {
    std::lock_guard<std::mutex> lock(preloadMutex_);
    JsonFile stateFile(dataFilePath(dataRoot_, "preload.json"));
    const JsonFileResult loaded = stateFile.load();
    const Json state = service::normalizePreloadState(loaded.found ? loaded.value : Json::object());
    const CommandPolicyContext context = controlPolicyContext(config_);
    for (const Json &id : state["off"].items()) {
        if (!id.isString()) continue;
        LunaReply reply;
        (void)waitForCommandPolicyLuna(
            luna_, commandPolicy_, context, "preload.apply",
            "com.webos.service.preloadmanager/setPreloadPolicy",
            service::preloadPolicyRequest(id.string(), false), "preload-manager", false, reply);
    }
}

void HttpFrontDoor::reapplyPreloadAfterBoot() const {
    if (!eventLoop_) return;
    // Late, so configd is up: 3 s after boot-done, waiting at most 3 minutes
    // for a TV that has no such file.
    const auto attempt = std::make_shared<std::function<void(int)>>();
    *attempt = [this, attempt](int tries) {
        const bool booted = access(rootedSystemPath(systemRoot_, "/tmp/boot-done").c_str(), F_OK) == 0;
        if (booted || tries >= 90) {
            (void)eventLoop_->scheduleAfter(std::chrono::milliseconds(3000), [this]() {
                (void)eventLoop_->submitBlocking([this]() {
                    std::lock_guard<std::mutex> lock(preloadMutex_);
                    JsonFile stateFile(dataFilePath(dataRoot_, "preload.json"));
                    const JsonFileResult loaded = stateFile.load();
                    const Json state = service::normalizePreloadState(
                        loaded.found ? loaded.value : Json::object());
                    if (state["off"].size() == 0 || !state["lg"].isArray()) return;
                    Json configNames = Json::array();
                    configNames.add(Json::text(service::preloadConfigKey()));
                    Json getRequest = Json::object();
                    getRequest.set("configNames", configNames);
                    LunaReply listReply;
                    if (!waitForLuna(luna_, "com.webos.service.config/getConfigs", getRequest,
                                     listReply) ||
                        !listReply.response["configs"][service::preloadConfigKey()].isArray())
                        return;
                    const Json list = listReply.response["configs"][service::preloadConfigKey()];
                    const Json want = preloadListWithout(list, state["off"]);
                    if (want.dump() != list.dump()) {
                        LunaReply written;
                        const bool ok = waitForCommandPolicyLuna(
                            luna_, commandPolicy_, controlPolicyContext(config_),
                            "preload.persist", "com.webos.service.config/setConfigs",
                            service::preloadConfigsRequest(want), "preload-manager", false,
                            written) && written.response["returnValue"].boolean();
                        if (logger_)
                            logger_->info(ok ? "preload: switched apps off again"
                                             : "preload: could not switch apps off again");
                        if (!ok) return;
                    }
                    for (const Json &id : state["off"].items()) {
                        if (!id.isString()) continue;
                        LunaReply reply;
                        (void)waitForCommandPolicyLuna(
                            luna_, commandPolicy_, controlPolicyContext(config_), "preload.apply",
                            "com.webos.service.preloadmanager/setPreloadPolicy",
                            service::preloadPolicyRequest(id.string(), false), "preload-manager",
                            false, reply);
                    }
                });
            });
            return;
        }
        (void)eventLoop_->scheduleAfter(std::chrono::milliseconds(2000),
                                        [attempt, tries]() { (*attempt)(tries + 1); });
    };
    (*attempt)(0);
}

void HttpFrontDoor::runStartupTasks() const {
    ScreensaverControl *const saver = screensaverControl();
    if (saver && screensaverManager_ && commandPolicy_) {
        const Json document = currentConfigDocument();
        const service::ScreensaverState state = screensaverManager_->state(
            config_.allowControl,
            document["allowExperimentalScreensavers"].boolean() ||
                document["allowOnWebos10"].boolean());
        const std::string mode = service::screensaverMode(state);
        if (mode != "stock") {
            CommandPolicySpec localCommand;
            localCommand.action = "screensaver-stage";
            localCommand.uri = "native/screensaver/stage";
            localCommand.timeout = std::chrono::milliseconds(25000);
            localCommand.requiresControl = false;
            localCommand.resource = "screensaver-mount";
            Json result = Json::object();
            LunaReply localReply;
            (void)commandPolicy_->executeLocal(
                localCommand, controlPolicyContext(config_), false,
                [this, &mode, &state, &result]() {
                    result = screensaverManager_->restage(
                        mode, service::screensaverLevel(state),
                        static_cast<unsigned short>(config_.port), config_.token);
                    return result["ok"].boolean();
                }, localReply);
            if (!result["ok"].boolean() && logger_)
                logger_->error("screensaver: could not restage " + mode + ": " +
                               result["error"].string());
        }
        saver->adoptLaunched();
    }
    refreshHomeInputTitles();
    reapplyPreloadAfterBoot();
}

void HttpFrontDoor::refreshLauncher() const {
    if (!samRescanManager_ ||
        !launcherNeedsRefresh(dataRoot_, currentConfigDocument(), samRestartedThisRun_))
        return;
    if (logger_) logger_->info("apps: refreshing sam so the home screen updates");
    const CommandPolicyContext context = controlPolicyContext(config_);
    if (samRescanManager_->refresh(Json::object(), context)["result"].boolean()) return;
    if (samRescanManager_->restart(context)["result"].boolean()) samRestartedThisRun_ = true;
    else if (logger_) logger_->warning("apps: sam could not be refreshed after the app change");
}

void HttpFrontDoor::setHomeScreen(HttpExchange &exchange) const {
    const auto reply = [&exchange](int status, const Json &value) {
        (void)exchange.respond(status,
                               {{"Content-Type", "application/json; charset=utf-8"},
                                {"Cache-Control", "no-store"},
                                {"X-Content-Type-Options", "nosniff"}},
                               value.dump());
    };
    const auto fail = [&reply](int status, const std::string &error) {
        Json value = Json::object();
        value.set("ok", Json::flag(false));
        value.set("error", Json::text(error));
        reply(status, value);
    };
    if (!config_.allowControl) {
        fail(403, say::msg("srv.controlsOff", "controls disabled in config"));
        return;
    }
    if (!exchange.setBodyLimit(8192)) {
        sendSetupError(exchange, 413, "request body too large", true);
        return;
    }
    std::string body;
    char buffer[1024];
    for (;;) {
        const int count = exchange.readBody(buffer, sizeof(buffer));
        if (count == 0) break;
        if (count < 0) {
            sendSetupError(exchange, exchange.bodyLimitExceeded() ? 413 : 400,
                           exchange.bodyLimitExceeded() ? "request body too large"
                                                        : "invalid request body", true);
            return;
        }
        body.append(buffer, static_cast<size_t>(count));
    }
    const Json payload = Json::parse(body);
    if (!payload.isObject() && !payload.isArray()) {
        fail(400, "malformed JSON");
        return;
    }
    if (!homeScreenManager_) {
        fail(503, "home screen module unavailable");
        return;
    }
    const auto done = [&reply](const Json &result) {
        reply(result["ok"].boolean() ? 200 : 400, result);
    };
    const auto unsupported = []() {
        Json value = Json::object();
        value.set("ok", Json::flag(false));
        value.set("error", Json::text(say::msg("srv.home.unsupported",
            "This TV's Home screen cannot be changed from here")));
        return value;
    };
    const Json layout = homeScreenManager_->state();

    if (payload["allow"].isBoolean()) {
        const bool allow = payload["allow"].boolean();
        Json change = Json::object();
        change.set("allowHomeLayout", Json::flag(allow));
        std::string error;
        if (!ConfigStore(configPath_).setTopLevel(change, &error)) {
            fail(400, "could not save setting: " + error);
            return;
        }
        {
            std::string reloadError;
            if (!proxy_.requestNodeConfigReload(reloadError) && logger_)
                logger_->warning("config: saved allowHomeLayout but Node did not reload it: " +
                                 reloadError);
        }
        // Where the layout is not experimental there is nothing to put back.
        if (!layout["experimental"].boolean()) {
            Json ok = Json::object();
            ok.set("ok", Json::flag(true));
            done(ok);
            return;
        }
        Json command = Json::object();
        command.set("action", Json::text("allowChanged"));
        command.set("allow", Json::flag(allow));
        const Json result = homeScreenAction(command);
        if (allow && result["ok"].boolean()) {
            Json ok = Json::object();
            ok.set("ok", Json::flag(true));
            done(ok);
        } else {
            done(result);
        }
        return;
    }
    if (payload["restore"].isBoolean() && payload["restore"].boolean()) {
        Json command = Json::object();
        command.set("action", Json::text("restore"));
        done(homeScreenAction(command));
        return;
    }
    if (payload["part"].isString()) {
        if (!layout["supported"].boolean()) {
            done(unsupported());
            return;
        }
        Json command = Json::object();
        command.set("action", Json::text("setPart"));
        command.set("id", payload["part"]);
        command.set("shown", Json::flag(service::javascriptTruthy(payload["shown"])));
        const Json result = homeScreenAction(command);
        Json ok = Json::object();
        ok.set("ok", Json::flag(true));
        done(result["ok"].boolean() ? ok : result);
        return;
    }
    if (payload["input"].isString()) {
        const std::string id = payload["input"].string();
        if (!layout["supported"].boolean()) {
            done(unsupported());
            return;
        }
        if (layout["experimental"].boolean() && !layout["allowed"].boolean()) {
            fail(400, say::msg("srv.home.notAllowed",
                "Switch on Home screen layout first. It is experimental on this TV."));
            return;
        }
        if (id != "hdmi1" && id != "hdmi2" && id != "hdmi3" && id != "hdmi4") {
            fail(400, say::msg("srv.home.unknownInput", "There is no such input"));
            return;
        }
        const bool onHome = service::javascriptTruthy(payload["onHome"]);
        const Json known = lgSettingsInputNames();
        Json portVariables = Json::object();
        portVariables.set("n", Json::text(id.substr(4)));
        Json command = Json::object();
        command.set("action", Json::text("setInput"));
        command.set("id", Json::text(id));
        command.set("onHome", Json::flag(onHome));
        command.set("title", Json::text(known[id].isString() && !known[id].string().empty()
            ? known[id].string() : say::msg("srv.home.hdmi", "HDMI {n}", portVariables)));
        const Json result = homeScreenAction(command);
        if (!result["ok"].boolean()) {
            done(result);
            return;
        }
        if (onHome) refreshHomeInputTitles();
        Json ok = Json::object();
        ok.set("ok", Json::flag(true));
        done(ok);
        return;
    }
    fail(400, "part, input or restore expected");
}

std::string HttpFrontDoor::displayVersion() const {
    std::ifstream file(dataFilePath(dataRoot_, "build.json"));
    std::string contents((std::istreambuf_iterator<char>(file)),
                         std::istreambuf_iterator<char>());
    const Json build = Json::parse(contents);
    const std::string commit = build["commit"].string();
    bool hex = commit.size() >= 4 && commit.size() <= 40;
    for (char c : commit)
        hex = hex && ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
    if (!build.isObject() || !hex || build["version"].string() != version_) return version_;
    return version_ + "+" + commit + (build["dirty"].boolean() ? ".dirty" : "");
}

void HttpFrontDoor::sendHomeBeat(HttpExchange &exchange) const {
    // The body is not used; the page only needs to be answered.
    char buffer[512];
    while (exchange.readBody(buffer, sizeof(buffer)) > 0) {
    }
    std::ifstream homeFile(dataFilePath(dataRoot_, "home.json"), std::ios::in | std::ios::binary);
    std::string contents((std::istreambuf_iterator<char>(homeFile)),
                         std::istreambuf_iterator<char>());
    const Json document = service::normalizeHomeDocument(
        contents.size() <= 8 * 1024 * 1024 ? Json::parse(contents) : Json());
    struct stat homeInfo = {};
    int64_t layoutVersion = 0;
    if (stat(dataFilePath(dataRoot_, "home.json").c_str(), &homeInfo) == 0) {
#if defined(__APPLE__)
        layoutVersion = static_cast<int64_t>(homeInfo.st_mtimespec.tv_sec) * 1000 +
                        homeInfo.st_mtimespec.tv_nsec / 1000000;
#else
        layoutVersion = static_cast<int64_t>(homeInfo.st_mtim.tv_sec) * 1000 +
                        homeInfo.st_mtim.tv_nsec / 1000000;
#endif
    }
    Json response = Json::object();
    response.set("ok", Json::flag(true));
    response.set("enabled", document["enabled"]);
    response.set("build", Json::text(displayVersion() + "@" + std::to_string(startedAtMs_)));
    response.set("wallpapers", service::homeWallpaperStatus(dataRoot_));
    response.set("layoutVersion", Json::text(std::to_string(layoutVersion)));
    (void)exchange.respond(200,
                           {{"Content-Type", "application/json; charset=utf-8"},
                            {"Cache-Control", "no-store"},
                            {"X-Content-Type-Options", "nosniff"}},
                           response.dump());
}

namespace {
// A part of LG's Home screen, as the Home screen section lists it.
Json homePart(const std::string &id, bool shown) {
    struct Text { const char *id; const char *titleKey; const char *title; const char *descKey; const char *desc; };
    static const Text kParts[] = {
        {"banner", "srv.home.banner", "Banner", "srv.home.banner.desc",
         "The large picture at the top, with LG's adverts or its own message, and the profile, notifications, settings and search buttons beside it."},
        {"quickCards", "srv.home.quickCards", "Quick Cards", "srv.home.quickCards.desc",
         "The coloured buttons for LG's offers, Sports, Gaming, Music and others."},
        {"apps", "srv.home.apps", "App tiles", "srv.home.apps.desc", "The row of apps and inputs."},
        {"deviceFunctions", "srv.home.deviceFunctions", "Recent Input and Device Functions",
         "srv.home.deviceFunctions.desc",
         "The last input used, and cards for editing the app list, the TV guide, the user guide and Home Hub."},
        {"sideIcons", "srv.home.sideIcons", "Side buttons", "srv.home.sideIcons.desc",
         "The profile, notifications, settings and search buttons beside the banner. Settings and search stay on the remote's own buttons."},
        {"recommended", "srv.home.recommended", "Recommended shelf", "srv.home.recommended.desc",
         "The row of programmes and films LG recommends, below the apps."},
    };
    Json part = Json::object();
    part.set("id", Json::text(id));
    for (const Text &text : kParts) {
        if (id != text.id) continue;
        part.set("title", Json::text(say::msg(text.titleKey, text.title)));
        part.set("desc", Json::text(say::msg(text.descKey, text.desc)));
        part.set("shown", Json::flag(shown));
        return part;
    }
    part.set("title", Json::text(id));
    part.set("desc", Json::text(""));
    part.set("shown", Json::flag(shown));
    return part;
}
} // namespace

std::string HttpFrontDoor::tvModelName() const {
    const Json configured = currentConfigDocument()["device"]["model"];
    if (service::javascriptTruthy(configured)) return service::javascriptString(configured);
    std::lock_guard<std::mutex> lock(tvModelMutex_);
    if (!tvModel_.empty()) return tvModel_;
    Json request = Json::object();
    Json keys = Json::array();
    keys.add(Json::text("modelName"));
    request.set("keys", keys);
    LunaReply reply;
    if (waitForLuna(luna_, "com.webos.service.tv.systemproperty/getSystemProperties",
                    request, reply) &&
        service::javascriptTruthy(reply.response["modelName"])) {
        const std::string model = service::javascriptString(reply.response["modelName"]);
        if (model != "webOS TV") tvModel_ = model;
    }
    return tvModel_;
}

bool HttpFrontDoor::collectSettingsContext(Json &context, std::string &error,
                                           bool withStatus) const {
    error.clear();
    const Json document = currentConfigDocument();
    context = Json::object();
    context.set("categories", haEntityCategories());
    context.set("entityCatalogue", haEntityCatalogue());
    context.set("backupDaily", Json::flag(document["backup"]["daily"].isBoolean() &&
                                          document["backup"]["daily"].boolean()));
    std::ifstream homeFile(dataFilePath(dataRoot_, "home.json"), std::ios::in | std::ios::binary);
    std::string homeContents((std::istreambuf_iterator<char>(homeFile)),
                             std::istreambuf_iterator<char>());
    context.set("homeLayout", service::homeLayout(service::normalizeHomeDocument(
        homeContents.size() <= 8 * 1024 * 1024 ? Json::parse(homeContents) : Json())));
    const std::string display = displayVersion();
    if (!display.empty()) context.set("displayVersion", Json::text(display));

    if (!withStatus) return true;   // a backup has no use for the bridge's standing
    // With the daemon owning the bridge and MQTT not switched on there is no client
    // to ask and nothing in Node to ask either: the bridge stands as disabled.
    const char *mqttProduction = std::getenv("TVWEB_NATIVE_MQTT_PRODUCTION");
    const bool mqttIdle = mqttProduction && std::string(mqttProduction) == "1" &&
                          !(document["mqtt"]["enabled"].isBoolean() &&
                            document["mqtt"]["enabled"].boolean());
    if (mqttStatus_ || mqttIdle || nodeFreeStart()) {
        const Json mqtt = document["mqtt"];
        // With MQTT unset and no Node to ask, the bridge stands as Node reports it off.
        static const long long startedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        Json standing;
        if (mqttStatus_) {
            standing = mqttStatus_();
        } else {
            standing = Json::object();
            standing.set("state", Json::text("disabled"));
            standing.set("detail", Json::text(
                mqtt["enabled"].isBoolean() && mqtt["enabled"].boolean() &&
                mqtt["host"].string().empty() ? "no broker address set" : ""));
            standing.set("sinceMs", Json::numeric(static_cast<double>(startedMs)));
        }
        const bool tls = mqtt["tls"].isBoolean() && mqtt["tls"].boolean();
        const int port = mqtt["port"].isNumber() ? static_cast<int>(mqtt["port"].number())
                                                 : (tls ? 8883 : 1883);
        const long long nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        Json status = Json::object();
        status.set("state", standing["state"]);
        status.set("broker", Json::text(mqtt["host"].string().empty() ? std::string()
            : mqtt["host"].string() + ":" + std::to_string(port)));
        status.set("tls", Json::flag(tls));
        status.set("detail", standing["detail"]);
        status.set("forMs", Json::numeric(static_cast<double>(
            nowMs - static_cast<long long>(standing["sinceMs"].number(static_cast<double>(nowMs))))));
        const double lastPublish = standing["lastPublishMs"].number(0);
        status.set("lastPublishMs", lastPublish > 0
            ? Json::numeric(static_cast<double>(nowMs) - lastPublish) : Json());
        const double latency = standing["latencyMs"].number(-1);
        status.set("latencyMs", latency > 0 ? Json::numeric(latency) : Json());
        context.set("status", status);
        return true;
    }
    // MQTT still belongs to Node: take its standing, as before.
    Json fromNode;
    if (proxy_.requestNodeSettingsContext(fromNode, error) && fromNode["status"].isObject()) {
        context.set("status", fromNode["status"]);
        return true;
    }
    return false;
}

Json HttpFrontDoor::appsRuntimeContext(const Json &appsReply,
                                       const Json &launchPointsReply) const {
    Json context = Json::object();
    const Json document = currentConfigDocument();

    std::set<std::string> hiddenIds;
    bool tileHidingEnabled = false;
    std::string stateError;
    (void)readAppTileStateFiles(hiddenAppsPath_, tileHidingFlagPath_, hiddenIds,
                                tileHidingEnabled, &stateError);
    const Json allowTileHiding = document["allowTileHiding"];
    const bool tilesAllowed = allowTileHiding.isBoolean() ? allowTileHiding.boolean()
                                                          : tileHidingEnabled;
    const bool fromHomebrew =
        access(dataFilePath(dataRoot_, ".from-homebrew-channel").c_str(), F_OK) == 0;
    context.set("tileHidingAvailable", Json::flag(tilesAllowed && !fromHomebrew));
    context.set("tileHidingHeld", Json::flag(false));
    context.set("tileHidingOverridden", Json::flag(false));

    std::ifstream homeFile(dataFilePath(dataRoot_, "home.json"), std::ios::in | std::ios::binary);
    std::string homeContents((std::istreambuf_iterator<char>(homeFile)),
                             std::istreambuf_iterator<char>());
    context.set("nativeTvHome", Json::flag(service::normalizeHomeDocument(
        homeContents.size() <= 8 * 1024 * 1024 ? Json::parse(homeContents) : Json())
        ["enabled"].boolean()));

    if (samRescanManager_) {
        const Json nudge = samRescanManager_->canNudge(controlPolicyContext(config_))["result"];
        context.set("tileHidingNudge", nudge.isBoolean() ? nudge : Json());
    }

    {   // The same list /api/services answers.
        HttpRequest request;
        request.method = "GET";
        request.uri = "/api/services";
        request.version = "1.1";
        request.remoteAddress = "127.0.0.1";
        HttpExchange exchange(std::move(request), std::string());
        sendBackgroundServices(exchange);
        const Json services = Json::parse(exchange.responseBody())["services"];
        if (services.valid()) context.set("services", services);
    }

    {   // Preloading: installed, not essential, preloaded by LG.
        Json preload = Json::array();
        Json configNames = Json::array();
        configNames.add(Json::text(service::preloadConfigKey()));
        Json getRequest = Json::object();
        getRequest.set("configNames", configNames);
        LunaReply listReply;
        LunaReply running;
        if (waitForLuna(luna_, "com.webos.service.config/getConfigs", getRequest, listReply) &&
            listReply.response["configs"][service::preloadConfigKey()].isArray()) {
            (void)waitForLuna(luna_, "com.webos.applicationManager/running", Json::object(), running);
            JsonFile stateFile(dataFilePath(dataRoot_, "preload.json"));
            const JsonFileResult loaded = stateFile.load();
            preload = service::preloadApps(
                listReply.response["configs"][service::preloadConfigKey()],
                appsReply["apps"], running.response["running"],
                loaded.found ? loaded.value : Json::object());
        }
        context.set("preload", preload);
    }

    {   // Quick Access: the nine keys and what each can be set to.
        Json keysRequest = Json::object();
        keysRequest.set("category", Json::text("option"));
        keysRequest.set("keys", service::quickAccessSettingKeys());
        LunaReply settings;
        const bool whole = waitForLuna(luna_, "com.webos.service.settings/getSystemSettings",
                                       keysRequest, settings) &&
                           settings.response["returnValue"].boolean(true) &&
                           service::quickAccessSettingsSupported(settings.response["settings"]);
        Json quickAccess;
        if (whole) {
            const Json apps = appsReply["apps"].isArray() && appsReply["apps"].size() > 0
                ? appsReply["apps"] : Json();
            const bool known = apps.valid();
            std::map<std::string, std::string> titles;
            if (known)
                for (const Json &app : apps.items())
                    if (app["id"].isString())
                        titles[app["id"].string()] = app["title"].string().empty()
                            ? app["id"].string() : app["title"].string();
            // The inputs a key can open: each HDMI input the TV names, then Live TV.
            Json inputs = Json::array();
            const Json inputNames = lgSettingsInputNames();
            std::vector<std::string> inputKeys = inputNames.keys();
            std::sort(inputKeys.begin(), inputKeys.end());
            for (const std::string &key : inputKeys) {
                if (key.size() != 5 || key.compare(0, 4, "hdmi") != 0 || key[4] < '1' || key[4] > '4')
                    continue;
                Json choice = Json::object();
                const std::string title = inputNames[key].string().empty()
                    ? key : inputNames[key].string();
                choice.set("id", Json::text("com.webos.app." + key));
                choice.set("title", Json::text(title));
                choice.set("kind", Json::text("input"));
                inputs.add(choice);
                titles["com.webos.app." + key] = title;
            }
            Json liveTv = Json::object();
            liveTv.set("id", Json::text("com.webos.app.livetv"));
            liveTv.set("title", Json::text(say::msg("srv.quickAccess.liveTv", "Live TV")));
            liveTv.set("kind", Json::text("input"));
            inputs.add(liveTv);
            titles["com.webos.app.livetv"] = liveTv["title"].string();

            Json keys = Json::array();
            for (int n = 1; n <= 9; ++n) {
                const Json value = settings.response["settings"]["magicNum" + std::to_string(n)];
                const std::string id = value["id"].string();
                const Json params = value["params"].isObject() ? value["params"] : Json::object();
                std::string title;
                if (!id.empty()) {
                    const auto found = titles.find(id);
                    title = found != titles.end() ? found->second
                        : known ? say::msg("srv.quickAccess.notInstalled", "App not installed") : id;
                }
                if (id == "com.webos.app.livetv" && service::javascriptTruthy(params["channelNumber"])) {
                    Json variables = Json::object();
                    variables.set("channel", Json::text(service::javascriptString(params["channelNumber"])));
                    title = say::msg("srv.quickAccess.channel", "Live TV, channel {channel}", variables);
                }
                Json row = Json::object();
                row.set("key", Json::numeric(n));
                row.set("id", Json::text(id));
                row.set("title", Json::text(title));
                row.set("lgDefault", Json::flag(value["override"].isBoolean() && value["override"].boolean()));
                row.set("installed", Json::flag(id.empty() || !known || titles.count(id) != 0));
                keys.add(row);
            }
            // The apps a key can open: those on the Home screen, by title.
            std::vector<std::pair<std::string, Json>> choices;
            std::set<std::string> seen;
            if (known)
                for (const Json &app : apps.items()) {
                    const std::string id = app["id"].string();
                    if (id.empty() || seen.count(id) || (app["visible"].isBoolean() && !app["visible"].boolean()))
                        continue;
                    if (id == "com.webos.app.livetv" || id.compare(0, 18, "com.webos.app.hdmi") == 0 ||
                        id.compare(0, 22, "com.webos.app.container") == 0)
                        continue;
                    seen.insert(id);
                    Json choice = Json::object();
                    const std::string title = app["title"].string().empty() ? id : app["title"].string();
                    choice.set("id", Json::text(id));
                    choice.set("title", Json::text(title));
                    choice.set("kind", Json::text("app"));
                    std::string lowered = title;
                    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                    choices.push_back({lowered, choice});
                }
            std::stable_sort(choices.begin(), choices.end(),
                             [](const std::pair<std::string, Json> &a,
                                const std::pair<std::string, Json> &b) { return a.first < b.first; });
            Json all = inputs;
            for (const auto &choice : choices) all.add(choice.second);
            quickAccess = Json::object();
            quickAccess.set("ok", Json::flag(true));
            quickAccess.set("supported", Json::flag(true));
            quickAccess.set("keys", keys);
            quickAccess.set("choices", all);
        }
        context.set("quickAccess", quickAccess);
    }

    {   // LG's Home screen: its parts, and the HDMI inputs as tiles.
        const Json layout = homeScreenManager_ ? homeScreenManager_->state() : Json();
        Json screen;
        if (layout["supported"].boolean()) {
            screen = Json::object();
            Json parts = Json::array();
            for (const Json &part : layout["parts"].items())
                parts.add(homePart(part["id"].string(), part["shown"].boolean()));
            screen.set("parts", parts);
            Json experimental;
            if (layout["experimental"].boolean()) {
                experimental = Json::object();
                experimental.set("allowed", layout["allowed"]);
            }
            screen.set("experimental", experimental);
            Json inputs;
            if (launchPointsReply["launchPoints"].isArray()) {
                std::set<std::string> onHome;
                for (const Json &point : launchPointsReply["launchPoints"].items()) {
                    const std::string appId = point["id"].string();
                    if (appId.compare(0, 14, "com.webos.app.") == 0) onHome.insert(appId.substr(14));
                }
                const Json known = lgSettingsInputNames();
                inputs = Json::array();
                for (const char *id : {"hdmi1", "hdmi2", "hdmi3", "hdmi4"}) {
                    Json input = Json::object();
                    Json variables = Json::object();
                    variables.set("n", Json::text(std::string(id).substr(4)));
                    input.set("id", Json::text(id));
                    input.set("title", Json::text(known[id].isString() && !known[id].string().empty()
                        ? known[id].string() : say::msg("srv.home.hdmi", "HDMI {n}", variables)));
                    input.set("onHome", Json::flag(onHome.count(id) != 0));
                    inputs.add(input);
                }
            }
            screen.set("inputs", inputs);
        }
        context.set("homeScreen", screen);
    }
    return context;
}

Json HttpFrontDoor::appTileOperation(const std::string &action, int &status) const {
    Json payload = Json::object();
    payload.set("action", Json::text(action));
    payload.set("id", Json::text(""));
    const std::string body = payload.dump();
    HttpRequest request;
    request.method = "POST";
    request.uri = "/_internal/apps/tile-hiding";
    request.version = "1.1";
    request.remoteAddress = "127.0.0.1";
    request.contentLength = static_cast<long long>(body.size());
    request.headers.push_back({"Content-Type", "application/json"});
    request.headers.push_back({"X-NativeTv-Proxy-Secret", proxySecret_});
    HttpExchange exchange(std::move(request), body);
    setNativeAppTileOperation(exchange);
    status = exchange.responseStatus();
    const Json result = Json::parse(exchange.responseBody());
    if (result.isObject()) return result;
    Json failure = Json::object();
    failure.set("ok", Json::flag(false));
    failure.set("error", Json::text("tile hiding did not return a response"));
    return failure;
}

Json HttpFrontDoor::runTvApp(const std::string &action, const std::string &appDir) const {
    {
        std::lock_guard<std::mutex> lock(installJobMutex_);
        if (appInstallSnapshotReady_ && appInstallSnapshotWorking(appInstallSnapshot_)) {
            Json busy = Json::object();
            busy.set("ok", Json::flag(false));
            busy.set("error", Json::text(say::msg(
                "srv.install.busy.tvApp",
                "an install is in progress; try again when it has finished")));
            return busy;
        }
    }
    if (!tvAppManager_) {
        Json missing = Json::object();
        missing.set("ok", Json::flag(true));
        missing.set("supported", Json::flag(false));
        missing.set("installed", Json::flag(false));
        return missing;
    }
    return tvAppManager_->run(action, appDir);
}

void HttpFrontDoor::invalidateOledCareCache() const {
    std::lock_guard<std::mutex> lock(oledCareCacheMutex_);
    oledCareCacheValid_ = false;
    oledCareCache_ = Json();
}

void HttpFrontDoor::sendOledCare(HttpExchange &exchange) const {
    std::lock_guard<std::mutex> commandLock(oledControlMutex_);
    const auto now = std::chrono::steady_clock::now();
    std::unique_lock<std::mutex> settingsReadLock(systemSettingsControlMutex_);
    {
        std::lock_guard<std::mutex> cacheLock(oledCareCacheMutex_);
        if (oledCareCacheValid_ &&
            now - oledCareCachedAt_ < std::chrono::seconds(30)) {
            sendMetricsJson(exchange, oledCareCache_.dump());
            return;
        }
    }
    settingsReadLock.unlock();

    service::OledCareSources sources;
    sources.systemRoot = systemRoot_;
    sources.writable = config_.allowControl;
    std::unique_lock<std::mutex> pictureSettingsLock;

    const Json configuredPanel = service::javascriptTruthy(config_.document["panel"])
        ? config_.document["panel"] : config_.document["device"]["panel"];
    if (service::javascriptTruthy(configuredPanel)) {
        sources.isOled = service::oledModel(
            service::javascriptString(configuredPanel));
    } else if (service::hasOledPanelTypeMarker(systemRoot_)) {
        sources.isOled = true;
    } else {
        Json modelRequest = Json::object();
        Json modelKeys = Json::array();
        modelKeys.add(Json::text("modelName"));
        modelRequest.set("keys", modelKeys);
        LunaReply modelReply;
        const bool modelAnswered = waitForLuna(
            luna_, "com.webos.service.tv.systemproperty/getSystemProperties",
            modelRequest, modelReply);
        std::string model = service::javascriptString(
            modelReply.response["modelName"]);
        if (!service::javascriptTruthy(modelReply.response["modelName"]))
            model.clear();
        if (model == "webOS TV")
            model.clear();
        const Json configuredModel = config_.document["device"]["model"];
        if (model.empty() && service::javascriptTruthy(configuredModel))
            model = service::javascriptString(configuredModel);
        if (model.empty() && (!modelAnswered ||
                              !modelReply.response["returnValue"].boolean())) {
            // The legacy detector retries a lost answer before deciding the
            // panel is LCD. Let Node retain that retry behavior in this case;
            // with no Node the records alone decide, and the answer is not kept long.
            if (!nodeFreeStart()) {
                (void)proxy_.forward(exchange);
                return;
            }
        }
        sources.isOled = service::oledModel(model) ||
                         service::hasOledPanelRecords(systemRoot_);
    }

    if (sources.isOled) {
        Json propertiesRequest = Json::object();
        Json propertyKeys = Json::array();
        propertyKeys.add(Json::text("panelUsageTime"));
        propertyKeys.add(Json::text("lastCompensationTimestamp"));
        propertiesRequest.set("keys", propertyKeys);
        LunaReply propertiesReply;
        if (waitForLuna(
                luna_, "com.webos.service.tv.systemproperty/getSystemProperties",
                propertiesRequest, propertiesReply))
            sources.systemProperties = propertiesReply.response;

        const Json usage = sources.systemProperties["panelUsageTime"];
        if (!service::javascriptTruthy(usage)) {
            // `subscribe` is mandatory: webOS 10.3.2 answers {} with "Missing
            // mandatory field: 'subscribe'" and the hours fall back to a file.
            Json panelRequest = Json::object();
            panelRequest.set("subscribe", Json::flag(false));
            LunaReply panelReply;
            if (waitForLuna(
                    luna_, "com.webos.service.panelcontroller/getPanelUsageTime",
                    panelRequest, panelReply))
                sources.panelController = panelReply.response;
        }

        LunaReply displayReply;
        (void)waitForLuna(
            luna_, "com.webos.service.tv.display/getClearPanelNoiseStatus",
            Json::object(), displayReply);
        sources.displayStatus = displayReply.response;

        LunaReply gsrReply;
        if (waitForLuna(luna_, "com.webos.service.oledepl/getGlobalStressReduction",
                        Json::object(), gsrReply) &&
            gsrReply.response["returnValue"].boolean()) {
            sources.serviceControls = true;
            Json protections = Json::object();
            protections.set("gsr", Json::flag(gsrReply.response["enable"].boolean()));
            protections.set("gsrStressCount",
                gsrReply.response["stressCount"].isNumber()
                    ? gsrReply.response["stressCount"] : Json());
            LunaReply tpcReply;
            if (waitForLuna(
                    luna_, "com.webos.service.oledepl/getTemporalPeakControl",
                    Json::object(), tpcReply) &&
                tpcReply.response["returnValue"].boolean())
                protections.set("tpc", Json::flag(tpcReply.response["enable"].boolean()));
            else
                protections.set("tpc", Json());
            sources.liveProtections = protections;
        } else {
            Json syspropRequest = Json::object();
            Json syspropKeys = Json::array();
            syspropKeys.add(Json::text("OledTPC"));
            syspropKeys.add(Json::text("OledGSR"));
            syspropRequest.set("keys", syspropKeys);
            LunaReply syspropReply;
            if (waitForLuna(
                    luna_, "com.webos.service.tv.systemproperty/getProperties",
                    syspropRequest, syspropReply) &&
                syspropReply.response["returnValue"].boolean() &&
                syspropReply.response["OledTPC"].valid()) {
                sources.serviceControls = true;
                Json protections = Json::object();
                protections.set("gsr", Json::flag(
                    service::javascriptString(syspropReply.response["OledGSR"]) ==
                    "true"));
                protections.set("tpc", Json::flag(
                    service::javascriptString(syspropReply.response["OledTPC"]) ==
                    "true"));
                protections.set("gsrStressCount", Json());
                sources.liveProtections = protections;
            }
        }

        Json pictureRequest = Json::object();
        pictureRequest.set("category", Json::text("picture"));
        Json pictureKeys = Json::array();
        pictureKeys.add(Json::text("screenShift"));
        pictureKeys.add(Json::text("logoLuminanceAdjust"));
        pictureRequest.set("keys", pictureKeys);
        LunaReply pictureReply;
        pictureSettingsLock =
            std::unique_lock<std::mutex>(systemSettingsControlMutex_);
        if (waitForLuna(
                luna_, "com.webos.service.settings/getSystemSettings",
                pictureRequest, pictureReply))
            sources.pictureSettings = pictureReply.response["settings"];
    }

    Json response = service::oledCareResponse(sources);
    {
        std::lock_guard<std::mutex> cacheLock(oledCareCacheMutex_);
        oledCareCache_ = response;
        oledCareCacheValid_ = true;
        oledCareCachedAt_ = std::chrono::steady_clock::now();
    }
    if (pictureSettingsLock.owns_lock())
        pictureSettingsLock.unlock();
    sendMetricsJson(exchange, response.dump());
}

void HttpFrontDoor::invalidatePrivacyCache() const {
    std::lock_guard<std::mutex> lock(privacyCacheMutex_);
    privacyCacheValid_ = false;
    privacyCache_ = Json();
}

void HttpFrontDoor::sendPrivacyProjection(HttpExchange &exchange) const {
    const auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(privacyCacheMutex_);
        if (privacyCacheValid_ &&
            now - privacyCachedAt_ < std::chrono::seconds(20)) {
            sendMetricsJson(exchange, privacyCache_.dump());
            return;
        }
    }

    const auto systemPath = [this](const std::string &absolutePath) {
        std::string root = systemRoot_.empty() ? "/" : systemRoot_;
        while (root.size() > 1 && root[root.size() - 1] == '/') root.pop_back();
        return root == "/" ? root + absolutePath.substr(1) : root + absolutePath;
    };
    const auto readFile = [](const std::string &path) {
        std::ifstream input(path.c_str(), std::ios::in | std::ios::binary);
        if (!input) return std::string();
        std::ostringstream contents;
        contents << input.rdbuf();
        return contents.str();
    };
    const auto lunaRead = [this](const std::string &uri, const Json &payload) {
        LunaReply reply;
        if (waitForLuna(luna_, uri, payload, reply)) return reply.response;
        return Json();
    };

    std::set<std::string> commands;
    for (const std::string &command : service::privacyProcessFirstArguments(procRoot_))
        commands.insert(command);
    const auto commandRunning = [&commands](const std::string &name) {
        return commands.find(name) != commands.end() ||
            commands.find("/usr/sbin/" + name) != commands.end();
    };

    const Json disabled = Json::parse(readFile(
        (dataRoot_.empty() ? std::string("/var/lib/tvweb") : dataRoot_) +
        "/disabled_services.json"));
    const auto isDisabled = [&disabled](const std::string &name) {
        if (!disabled.isArray()) return false;
        for (const Json &item : disabled.items())
            if (item.isString() && item.string() == name) return true;
        return false;
    };
    const bool hasSystemctl = access(systemPath("/bin/systemctl").c_str(), F_OK) == 0 ||
        access(systemPath("/usr/bin/systemctl").c_str(), F_OK) == 0;
    const bool hasInitctl = access(systemPath("/sbin/initctl").c_str(), F_OK) == 0 ||
        access(systemPath("/usr/sbin/initctl").c_str(), F_OK) == 0;
    const auto unitExists = [&systemPath](const std::string &unit) {
        const std::string paths[] = {
            "/etc/systemd/system/" + unit, "/lib/systemd/system/" + unit,
            "/usr/lib/systemd/system/" + unit
        };
        for (const std::string &path : paths)
            if (access(systemPath(path).c_str(), F_OK) == 0) return true;
        return false;
    };
    const auto serviceAvailable = [&](const std::string &name) {
        if (name == "admanager")
            return access(systemPath("/usr/sbin/admanager").c_str(), F_OK) == 0;
        if (name != "uploadd" && name != "rdxd") return false;
        const std::string unit = name + ".service";
        if (hasSystemctl && unitExists(unit)) return true;
        return hasInitctl &&
            access(systemPath("/etc/init/" + name + ".conf").c_str(), F_OK) == 0;
    };
    struct PrivacyDaemon {
        const char *name;
        const char *label;
        const char *detail;
        bool onDemand;
    };
    const PrivacyDaemon privacyDaemons[] = {
        {"acr2", "Content recognition service", "Identifies what is on screen", true},
        {"admanager", "Advertising service", "Fetches and displays ads on the TV", true},
        {"uploadd", "Diagnostics uploader", "Sends diagnostic data to LG", false},
        {"rdxd", "Diagnostics collector", "Gathers crash and diagnostic reports", false}
    };
    Json daemonRows = Json::array();
    for (const PrivacyDaemon &definition : privacyDaemons) {
        const std::string name = definition.name;
        Json row = Json::object();
        row.set("name", Json::text(name));
        row.set("label", Json::text(definition.label));
        row.set("detail", Json::text(definition.detail));
        row.set("running", Json::flag(commandRunning(name)));
        row.set("onDemand", Json::flag(definition.onDemand));
        // A service the bus starts on demand is not offered a stop: asked about
        // again, it would be started again.
        row.set("stoppable", Json::flag(!definition.onDemand && serviceAvailable(name)));
        row.set("heldDown", Json::flag(isDisabled(name)));
        daemonRows.add(row);
    }

    Json flags = Json::parse(readFile(systemPath("/var/luna/preferences/eula")));
    // Node reads every "xxxAllowed" flag in the file wherever it sits; on a TV
    // (a B3 on webOS 10.3.2) they are under eulaStatus, beside the agreement list.
    if (flags.isObject() && flags["eulaStatus"].isObject()) flags = flags["eulaStatus"];
    const Json licenseMapping = Json::parse(
        readFile(systemPath("/var/palm/license/eulaInfoNetwork.json")));
    Json documentRequest = Json::object();
    Json documentKeys = Json::array();
    documentKeys.add(Json::text("eulaInfoNetwork"));
    documentRequest.set("keys", documentKeys);
    const Json documentReply = lunaRead(
        "com.webos.settingsservice/getSystemSettings", documentRequest);
    const Json documents = documentReply["settings"]["eulaInfoNetwork"];
    const Json consentProjection = service::privacyConsentReadProjection(
        flags.isObject() ? flags : Json::object(), licenseMapping, documents);

    Json response = Json::object();
    response.set("ok", Json::flag(true));
    response.set("consent", Json());
    if (flags.isObject()) {
        Json consent = Json::object();
        consent.set("known", consentProjection["known"]);
        consent.set("other", consentProjection["other"]);
        response.set("consent", consent);
    }
    response.set("consentWritable", Json::flag(config_.allowControl));
    response.set("consentGroups", consentProjection["groups"]);
    response.set("tvUpdates", Json::object());
    Json tvUpdates = Json::object();

    service::AdBlockStore fallbackStore(dataRoot_, systemRoot_);
    const service::AdBlockStore &adBlockStore = adBlockManager_
        ? adBlockManager_->store() : fallbackStore;
    const bool sdpGraceActive = adBlockManager_
        ? adBlockManager_->sdpGraceActive() : true;
    response.set("adblock", adBlockStore.privacyState(sdpGraceActive));
    tvUpdates.set("blocked", Json::flag(adBlockStore.tvUpdatesBlocked()));
    response.set("tvUpdates", tvUpdates);
    response.set("daemons", daemonRows);

    const bool acrIdle = service::privacyServiceWouldStart(
        systemRoot_, procRoot_, "com.webos.service.acr");
    const bool adIdle = service::privacyServiceWouldStart(
        systemRoot_, procRoot_, "com.webos.service.admanager");
    const auto askIfRunning = [&lunaRead](bool idle, const std::string &uri) {
        return idle ? Json() : lunaRead(uri, Json::object());
    };
    const Json acrStatus = askIfRunning(acrIdle,
        "com.webos.service.acr/getACRSolutionStatus");
    const Json captureStatus = askIfRunning(acrIdle,
        "com.webos.service.acr/getVideoCaptureStatus");
    Json acr = Json::object();
    acr.set("label", Json::text("Screen content recognition"));
    acr.set("detail", Json::text("LG’s ACR captures what is on screen, from apps and HDMI alike, to work out what you watch and target ads at you."));
    acr.set("active", Json::flag(service::javascriptTruthy(
        acrStatus["ACRSolutionStatus"])));
    bool acrRunning = false;
    for (const Json &daemon : daemonRows.items())
        if (daemon["name"].string() == "acr2") acrRunning = daemon["running"].boolean();
    acr.set("running", Json::flag(acrRunning));
    if (acrIdle) acr.set("stopped", Json::flag(true));
    const Json captureState = captureStatus["status"];
    acr.set("capturing", Json::flag(service::javascriptTruthy(captureState) &&
                                    captureState.string() != "stopped"));
    acr.set("captureState", Json::text(acrIdle ? "stopped"
        : service::javascriptTruthy(captureState) ? service::javascriptString(captureState)
                                                   : "unknown"));

    std::string acrBinary = readFile(systemPath("/usr/sbin/acr2"));
    std::vector<std::string> acrKeys;
    for (const std::string key : {"acrAllowed", "acrOnAllowed"})
        if (acrBinary.find(key) != std::string::npos) acrKeys.push_back(key);
    if (acrKeys.empty()) acrKeys.push_back("acrAllowed");
    Json acrConsents = Json::array();
    const Json consentRows[] = {consentProjection["known"], consentProjection["other"]};
    for (const std::string &key : acrKeys) {
        for (const Json &row : consentRows) {
            for (const Json &flag : row.items()) {
                if (flag["key"].string() != key) continue;
                Json entry = Json::object();
                entry.set("key", Json::text(key));
                entry.set("label", flag["label"].isString()
                    ? flag["label"] : Json::text(key));
                entry.set("on", Json::flag(flag["enabled"].boolean()));
                acrConsents.add(entry);
            }
        }
    }
    acr.set("consents", acrConsents);
    Json livePlusRequest = Json::object();
    livePlusRequest.set("category", Json::text("option"));
    Json livePlusKeys = Json::array();
    livePlusKeys.add(Json::text("livePlus"));
    livePlusRequest.set("keys", livePlusKeys);
    const Json livePlusReply = lunaRead(
        "com.webos.settingsservice/getSystemSettings", livePlusRequest);
    const Json livePlusValue = livePlusReply["settings"]["livePlus"];
    const bool livePlusKnown = livePlusValue.valid();
    std::string livePlusText = service::javascriptString(livePlusValue);
    std::transform(livePlusText.begin(), livePlusText.end(), livePlusText.begin(),
                   [](unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });
    acr.set("livePlus", livePlusKnown ? Json::flag(livePlusText == "on") : Json());
    bool consentOn = livePlusKnown && livePlusText == "on";
    for (const Json &entry : acrConsents.items())
        if (entry["on"].boolean()) consentOn = true;
    acr.set("consentOn", Json::flag(consentOn));
    response.set("acr", acr);

    const Json adStatus = askIfRunning(adIdle,
        "com.webos.service.admanager/getAdid");
    const bool adAvailable = !(adStatus["returnValue"].isBoolean() &&
                               !adStatus["returnValue"].boolean()) &&
                             adStatus.has("IFA");
    const Json adId = adStatus["IFA"];
    const Json adLmt = adStatus["LMT"];
    std::string adLmtText = service::javascriptString(adLmt);
    std::transform(adLmtText.begin(), adLmtText.end(), adLmtText.begin(),
                   [](unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });
    Json advertisingId = Json::object();
    advertisingId.set("available", Json::flag(adAvailable));
    advertisingId.set("label", Json::text("Advertising identifier"));
    advertisingId.set("detail", Json::text("A unique ID your TV hands to advertisers so they can target ads at you. Resetting it breaks the link to your past activity."));
    advertisingId.set("present", Json::flag(service::javascriptTruthy(adId)));
    advertisingId.set("limitTracking", Json::flag(adLmtText == "on"));
    advertisingId.set("lmtKnown", Json::flag(adAvailable));
    advertisingId.set("limitTrackingLabel", Json::text("Limit ad tracking"));
    advertisingId.set("limitTrackingDetail", Json::text("When on, apps are asked not to use this ID to profile you."));
    if (adIdle) {
        advertisingId.set("stopped", Json::flag(true));
        Json lmtRequest = Json::object();
        lmtRequest.set("category", Json::text("general"));
        Json lmtKeys = Json::array();
        lmtKeys.add(Json::text("lmt"));
        lmtRequest.set("keys", lmtKeys);
        const Json lmtReply = lunaRead(
            "com.webos.settingsservice/getSystemSettings", lmtRequest);
        const Json lmtValue = lmtReply["settings"]["lmt"];
        if (lmtValue.valid()) {
            std::string setting = service::javascriptString(lmtValue);
            std::transform(setting.begin(), setting.end(), setting.begin(),
                           [](unsigned char character) {
                               return static_cast<char>(std::tolower(character));
                           });
            advertisingId.set("limitTracking", Json::flag(setting == "on"));
            advertisingId.set("lmtKnown", Json::flag(true));
        }
    }
    response.set("advertisingId", advertisingId);

    const Json configReply = lunaRead("com.webos.service.config/getConfigs", [] {
        Json request = Json::object();
        Json names = Json::array();
        names.add(Json::text("tv.model.supportFarFieldVoice"));
        request.set("configNames", names);
        return request;
    }());
    const std::string voiceSupport = configReply["configs"][
        "tv.model.supportFarFieldVoice"].string();
    if (voiceSupport == "on with switch" &&
        !service::privacyServiceWouldStart(
            systemRoot_, procRoot_, "com.webos.service.voiceinput.sound")) {
        const Json micStatus = lunaRead(
            "com.webos.service.voiceinput.sound/getMicSwitchStatus", Json::object());
        const std::string status = micStatus["micSwitchStatus"].string();
        if (status == "high" || status == "low") {
            Json microphone = Json::object();
            microphone.set("label", Json::text("Microphone switch"));
            microphone.set("detail", Json::text("The switch on the TV itself. With it off, the TV’s hands-free voice stops listening."));
            microphone.set("on", Json::flag(status == "high"));
            response.set("microphone", microphone);
        }
    }

    const Json settingsData = collectLgSettingsData({"promotions"});
    const Json settingsResponse = service::lgSettingsResponse(
        {"promotions"}, settingsData["settings"], settingsData["descriptions"],
        settingsData["dimensions"], Json::object());
    response.set("lgSettings", settingsResponse["rows"]);
    response.set("simple", service::privacySimpleSummary(response));
    {
        std::lock_guard<std::mutex> lock(privacyCacheMutex_);
        privacyCache_ = response;
        privacyCacheValid_ = true;
        privacyCachedAt_ = std::chrono::steady_clock::now();
    }
    sendMetricsJson(exchange, response.dump());
}

void HttpFrontDoor::setSshKeys(HttpExchange &exchange) const {
    const HttpRequest &request = exchange.request();
    const bool needsToken = config_.token.empty();
    if (request.method != "POST") {
        Json response = service::sshKeysStatus(systemRoot_, procRoot_);
        response.set("writable", Json::flag(config_.allowControl && !needsToken));
        response.set("needsToken", Json::flag(config_.allowControl && needsToken));
        if (needsToken) {
            Json visibleKeys = Json::array();
            for (const Json &key : response["keys"].items()) {
                Json visible = Json::object();
                visible.set("type", key["type"]);
                visible.set("fingerprint", key["fingerprint"]);
                visibleKeys.add(visible);
            }
            response.set("keys", visibleKeys);
        }
        sendMetricsJson(exchange, response.dump());
        return;
    }
    if (!config_.allowControl) {
        sendSetupError(exchange, 403, "controls disabled in config");
        return;
    }
    if (needsToken) {
        sendSetupError(exchange, 403,
                       "Changing SSH keys needs a token in config.json.");
        return;
    }
    if (!setupJsonContentType(request)) {
        sendSetupError(exchange, 415, "Content-Type must be application/json");
        return;
    }
    if (!exchange.setBodyLimit(8192)) {
        sendSetupError(exchange, 413, "request body too large", true);
        return;
    }
    std::string body;
    char buffer[1024];
    for (;;) {
        const int count = exchange.readBody(buffer, sizeof(buffer));
        if (count == 0) break;
        if (count < 0) {
            sendSetupError(exchange, exchange.bodyLimitExceeded() ? 413 : 400,
                           exchange.bodyLimitExceeded() ? "request body too large"
                                                        : "invalid request body",
                           true);
            return;
        }
        body.append(buffer, static_cast<size_t>(count));
    }
    if (exceedsUtf16CodeUnitLimit(body, 8192)) {
        sendSetupError(exchange, 413, "request body too large", true);
        return;
    }
    const Json payload = Json::parse(body);
    if (!payload.isObject()) {
        sendSetupError(exchange, 400, "malformed JSON");
        return;
    }

    const std::string action = payload["action"].string();
    Json response;
    if (action == "add") {
        response = service::addSshKey(systemRoot_, procRoot_, payload["key"].string());
    } else if (action == "remove") {
        response = service::removeSshKey(systemRoot_, procRoot_,
                                         payload["fingerprint"].string());
    } else if (action == "deleteFile") {
        response = service::deleteSshKeysFile(systemRoot_, procRoot_);
    } else {
        response.set("ok", Json::flag(false));
        response.set("error", Json::text("unknown action"));
    }
    const bool succeeded = response["ok"].boolean();
    if (succeeded) response.set("writable", Json::flag(true));
    (void)exchange.respond(succeeded ? 200 : 400,
                           {{"Content-Type", "application/json; charset=utf-8"},
                            {"Cache-Control", "no-store"},
                            {"X-Content-Type-Options", "nosniff"}},
                           response.dump());
}

void HttpFrontDoor::sendBackgroundServices(HttpExchange &exchange) const {
    std::map<std::string, bool> managerRunning;
    const std::vector<service::BackgroundServiceProbe> probes =
        service::backgroundServiceProbes(systemRoot_);
    for (const service::BackgroundServiceProbe &probe : probes) {
        ChildOptions options;
        options.timeout = std::chrono::milliseconds(3500);
        options.maximumOutputBytes = 16 * 1024;
        if (!probe.unit.empty()) {
            options.arguments = {service::backgroundSystemctlExecutable(systemRoot_),
                                 "is-active", probe.unit};
        } else if (!probe.upstart.empty()) {
            options.arguments = {service::backgroundInitctlExecutable(systemRoot_),
                                 "status", probe.upstart};
        } else {
            continue;
        }
        const ChildResult result = children_.run(options);
        const std::string output = result.standardOutput;
        const size_t first = output.find_first_not_of(" \t\r\n");
        const size_t last = output.find_last_not_of(" \t\r\n");
        const std::string state = first == std::string::npos
            ? std::string() : output.substr(first, last - first + 1);
        managerRunning[probe.id] = result.succeeded() &&
            (state == "active" || state.find("start/running") != std::string::npos);
    }
    const Json response = service::backgroundServicesStatus(
        systemRoot_, procRoot_, dataRoot_, managerRunning);
    sendMetricsJson(exchange, response.dump());
}

void HttpFrontDoor::setNativeDevtools(HttpExchange &exchange) const {
    const HttpRequest &request = exchange.request();
    if (!isLoopbackPeer(request.remoteAddress) || proxySecret_.empty() ||
        !tokenMatchesConstantTime(request.header("x-nativetv-proxy-secret"), proxySecret_) ||
        !request.header("x-nativetv-original-remote").empty()) {
        sendSetupError(exchange, 403, "native DevTools control required", true);
        return;
    }
    if (request.method != "POST") {
        sendSetupError(exchange, 405, "method not allowed");
        return;
    }
    if (!setupJsonContentType(request)) {
        sendSetupError(exchange, 415, "Content-Type must be application/json");
        return;
    }
    if (!devtoolsFirewall_ || !commandPolicy_) {
        sendSetupError(exchange, 503, "native DevTools firewall is unavailable");
        return;
    }
    if (!exchange.setBodyLimit(8192)) {
        sendSetupError(exchange, 413, "request body too large", true);
        return;
    }
    std::string body;
    char buffer[1024];
    for (;;) {
        const int count = exchange.readBody(buffer, sizeof(buffer));
        if (count == 0) break;
        if (count < 0) {
            sendSetupError(exchange, exchange.bodyLimitExceeded() ? 413 : 400,
                           exchange.bodyLimitExceeded() ? "request body too large"
                                                        : "invalid request body",
                           true);
            return;
        }
        body.append(buffer, static_cast<size_t>(count));
    }
    if (exceedsUtf16CodeUnitLimit(body, 8192)) {
        sendSetupError(exchange, 413, "request body too large", true);
        return;
    }
    const Json payload = Json::parse(body);
    if (!payload.isObject() || !payload["allowNetworkDebugger"].isBoolean()) {
        sendSetupError(exchange, 400, "allowNetworkDebugger boolean expected");
        return;
    }

    const bool allowed = payload["allowNetworkDebugger"].boolean();
    std::string configError;
    if (!ConfigStore(configPath_).setNetworkDebuggerAllowed(allowed, &configError)) {
        Json failure = Json::object();
        failure.set("ok", Json::flag(false));
        failure.set("handled", Json::flag(true));
        failure.set("persisted", Json::flag(false));
        failure.set("error", Json::text("could not save setting: " + configError));
        (void)exchange.respond(500,
                               {{"Content-Type", "application/json; charset=utf-8"},
                                {"Cache-Control", "no-store"},
                                {"X-Content-Type-Options", "nosniff"}},
                               failure.dump());
        return;
    }
    CommandPolicySpec localCommand;
    localCommand.action = "devtools-firewall";
    localCommand.uri = "native/devtools/firewall";
    localCommand.payload = payload;
    localCommand.timeout = std::chrono::milliseconds(25000);
    localCommand.requiresControl = false;
    localCommand.resource = "devtools-firewall";
    const CommandPolicyContext context = controlPolicyContext(config_);
    Json result = Json::object();
    LunaReply localReply;
    const CommandDecision decision = commandPolicy_->executeLocal(
        localCommand, context, false,
        [this, allowed, &result]() {
            result = devtoolsFirewall_->setNetworkAllowed(allowed);
            return result["ok"].isBoolean() && result["ok"].boolean();
        }, localReply);
    const bool succeeded = decision.kind == CommandDecisionKind::Allowed &&
        localReply.response["returnValue"].isBoolean() &&
        localReply.response["returnValue"].boolean();
    result.set("handled", Json::flag(true));
    result.set("persisted", Json::flag(true));
    if (!succeeded) {
        result = Json::object();
        result.set("ok", Json::flag(false));
        result.set("handled", Json::flag(true));
        result.set("persisted", Json::flag(true));
        result.set("error", Json::text(!localReply.error.empty() ? localReply.error
            : !decision.reason.empty() ? decision.reason
            : "could not update the DevTools firewall"));
    }
    const int status = succeeded ? 200
        : decision.kind == CommandDecisionKind::Allowed ? 500 : 503;
    (void)exchange.respond(status,
                           {{"Content-Type", "application/json; charset=utf-8"},
                            {"Cache-Control", "no-store"},
                            {"X-Content-Type-Options", "nosniff"}},
                           result.dump());
}

void HttpFrontDoor::setNativeScreensaver(HttpExchange &exchange) const {
    const HttpRequest &request = exchange.request();
    if (!isLoopbackPeer(request.remoteAddress) || proxySecret_.empty() ||
        !tokenMatchesConstantTime(request.header("x-nativetv-proxy-secret"), proxySecret_) ||
        !request.header("x-nativetv-original-remote").empty()) {
        sendSetupError(exchange, 403, "native screen saver control required", true);
        return;
    }
    if (request.method != "POST") {
        sendSetupError(exchange, 405, "method not allowed");
        return;
    }
    if (!setupJsonContentType(request)) {
        sendSetupError(exchange, 415, "Content-Type must be application/json");
        return;
    }
    if (!screensaverManager_ || !commandPolicy_) {
        sendSetupError(exchange, 503, "native screen saver manager is unavailable");
        return;
    }
    if (!exchange.setBodyLimit(8192)) {
        sendSetupError(exchange, 413, "request body too large", true);
        return;
    }
    std::string body;
    char buffer[1024];
    for (;;) {
        const int count = exchange.readBody(buffer, sizeof(buffer));
        if (count == 0) break;
        if (count < 0) {
            sendSetupError(exchange, exchange.bodyLimitExceeded() ? 413 : 400,
                           exchange.bodyLimitExceeded() ? "request body too large"
                                                        : "invalid request body",
                           true);
            return;
        }
        body.append(buffer, static_cast<size_t>(count));
    }
    if (exceedsUtf16CodeUnitLimit(body, 8192)) {
        sendSetupError(exchange, 413, "request body too large", true);
        return;
    }
    const Json payload = Json::parse(body);
    if (!payload.isObject() || !payload["action"].isString()) {
        sendSetupError(exchange, 400, "screen saver action expected");
        return;
    }
    const std::string action = payload["action"].string();
    if (action != "prepare" && action != "restore" && action != "unmount" &&
        action != "restage") {
        sendSetupError(exchange, 400, "unknown screen saver action");
        return;
    }
    if ((action == "prepare" || action == "restage") &&
        (!payload["mode"].isString() || !payload["level"].isString() ||
         !payload["port"].isNumber() || payload["port"].number() < 1 ||
         payload["port"].number() > 65535 || !payload["token"].isString())) {
        sendSetupError(exchange, 400, "screen saver mode, level, port and token expected");
        return;
    }

    CommandPolicySpec localCommand;
    localCommand.action = "screensaver-stage";
    localCommand.uri = "native/screensaver/stage";
    localCommand.payload = payload;
    localCommand.timeout = std::chrono::milliseconds(25000);
    localCommand.requiresControl = false;
    localCommand.resource = "screensaver-mount";
    const CommandPolicyContext context = controlPolicyContext(config_);
    Json result = Json::object();
    LunaReply localReply;
    const CommandDecision decision = commandPolicy_->executeLocal(
        localCommand, context, false,
        [this, action, &payload, &result]() {
            if (action == "restore") result = screensaverManager_->restoreStock();
            else if (action == "unmount") result = screensaverManager_->unmount();
            else if (action == "restage") {
                result = screensaverManager_->restage(
                    payload["mode"].string(), payload["level"].string(),
                    static_cast<unsigned short>(payload["port"].number()),
                    payload["token"].string());
            } else {
                result = screensaverManager_->prepare(
                    payload["mode"].string(), payload["level"].string(),
                    static_cast<unsigned short>(payload["port"].number()),
                    payload["token"].string());
            }
            return result["ok"].isBoolean() && result["ok"].boolean();
        }, localReply);
    const bool succeeded = decision.kind == CommandDecisionKind::Allowed &&
        localReply.response["returnValue"].isBoolean() &&
        localReply.response["returnValue"].boolean();
    if (!succeeded) {
        result = Json::object();
        result.set("ok", Json::flag(false));
        result.set("error", Json::text(!localReply.error.empty() ? localReply.error
            : !decision.reason.empty() ? decision.reason
            : "could not update the screen saver staging"));
    }
    const int status = succeeded ? 200
        : decision.kind == CommandDecisionKind::Allowed ? 500 : 503;
    (void)exchange.respond(status,
                           {{"Content-Type", "application/json; charset=utf-8"},
                            {"Cache-Control", "no-store"},
                            {"X-Content-Type-Options", "nosniff"}},
                           result.dump());
}

void HttpFrontDoor::setNativeSamRescan(HttpExchange &exchange) const {
    const HttpRequest &request = exchange.request();
    if (!isLoopbackPeer(request.remoteAddress) || proxySecret_.empty() ||
        !tokenMatchesConstantTime(request.header("x-nativetv-proxy-secret"), proxySecret_) ||
        !request.header("x-nativetv-original-remote").empty()) {
        sendSetupError(exchange, 403, "native SAM control required", true);
        return;
    }
    if (request.method != "POST") {
        sendSetupError(exchange, 405, "method not allowed");
        return;
    }
    if (!setupJsonContentType(request)) {
        sendSetupError(exchange, 415, "Content-Type must be application/json");
        return;
    }
    if (!commandPolicy_) {
        sendSetupError(exchange, 503, "native SAM command policy is unavailable");
        return;
    }
    if (!exchange.setBodyLimit(8192)) {
        sendSetupError(exchange, 413, "request body too large", true);
        return;
    }
    std::string body;
    char buffer[1024];
    for (;;) {
        const int count = exchange.readBody(buffer, sizeof(buffer));
        if (count == 0) break;
        if (count < 0) {
            sendSetupError(exchange, exchange.bodyLimitExceeded() ? 413 : 400,
                           exchange.bodyLimitExceeded() ? "request body too large"
                                                        : "invalid request body",
                           true);
            return;
        }
        body.append(buffer, static_cast<size_t>(count));
    }
    if (exceedsUtf16CodeUnitLimit(body, 8192)) {
        sendSetupError(exchange, 413, "request body too large", true);
        return;
    }
    const Json requestPayload = Json::parse(body);
    if (requestPayload.isObject() && requestPayload["action"].isString()) {
        if (!samRescanManager_) {
            sendSetupError(exchange, 503, "native SAM manager is unavailable");
            return;
        }
        const std::string operation = requestPayload["action"].string();
        Json result;
        if (operation == "canNudge") {
            result = samRescanManager_->canNudge(controlPolicyContext(config_));
        } else if (operation == "nudge") {
            result = samRescanManager_->nudge(controlPolicyContext(config_));
        } else if (operation == "refresh") {
            const Json expected = requestPayload["expected"];
            if (expected.valid() && !expected.isObject()) {
                sendSetupError(exchange, 400, "SAM expected visibility object required");
                return;
            }
            if (expected.keys().size() > 64) {
                sendSetupError(exchange, 400, "too many SAM visibility checks");
                return;
            }
            for (const std::string &id : expected.keys()) {
                if (!appTileIdIsValid(id) || !expected[id].isBoolean()) {
                    sendSetupError(exchange, 400, "invalid SAM visibility check");
                    return;
                }
            }
            result = samRescanManager_->refresh(expected, controlPolicyContext(config_));
        } else if (operation == "rereadApp" && requestPayload["id"].isString()) {
            const std::string id = requestPayload["id"].string();
            if (id != "com.webos.app.home" && id != "com.webos.app.screensaver") {
                sendSetupError(exchange, 400, "unsupported SAM app id");
                return;
            }
            result = samRescanManager_->rereadApp(id, controlPolicyContext(config_));
        } else if (operation == "restart") {
            result = samRescanManager_->restart(controlPolicyContext(config_));
        } else {
            sendSetupError(exchange, 400, "unknown SAM operation");
            return;
        }
        (void)exchange.respond(200,
                               {{"Content-Type", "application/json; charset=utf-8"},
                                {"Cache-Control", "no-store"},
                                {"X-Content-Type-Options", "nosniff"}},
                               result.dump());
        return;
    }
    if (!requestPayload.isObject() || !requestPayload["uri"].isString() ||
        !requestPayload["payload"].isObject()) {
        sendSetupError(exchange, 400, "SAM URI and payload expected");
        return;
    }

    const std::string uri = requestPayload["uri"].string();
    Json payload = requestPayload["payload"];
    std::string action;
    std::string resource = "sam-rescan";
    if (uri == "com.webos.service.config/getConfigs") {
        const Json names = payload["configNames"];
        if (!names.isArray() || names.size() != 1 ||
            !names[0].isString() || names[0].string() != "profile.blockedAppList") {
            sendSetupError(exchange, 400, "SAM blocked app list query expected");
            return;
        }
        action = "sam-rescan.get-blocked-list";
        resource = "sam-blocked-app-list";
    } else if (uri == "com.webos.service.config/setConfigs") {
        const Json configs = payload["configs"];
        const Json list = configs["profile.blockedAppList"];
        if (!configs.isObject() || !list.isArray() || list.size() > 1024) {
            sendSetupError(exchange, 400, "SAM blocked app list expected");
            return;
        }
        for (const Json &item : list.items()) {
            if (!item.isString() || item.string().empty() || item.string().size() > 256) {
                sendSetupError(exchange, 400, "invalid SAM blocked app list entry");
                return;
            }
            for (unsigned char character : item.string()) {
                if (std::iscntrl(character)) {
                    sendSetupError(exchange, 400, "invalid SAM blocked app list entry");
                    return;
                }
            }
        }
        action = "sam-rescan.set-blocked-list";
        resource = "sam-blocked-app-list";
    } else if (uri == "com.webos.applicationManager/blockSystemApp" ||
               uri == "com.webos.applicationManager/releaseBlockSystemApp") {
        const std::string id = payload["id"].string();
        if (!payload["id"].isString() ||
            (id != "com.webos.app.home" && id != "com.webos.app.screensaver")) {
            sendSetupError(exchange, 400, "unsupported SAM app id");
            return;
        }
        if (uri == "com.webos.applicationManager/blockSystemApp") {
            const std::string root = systemRoot_.empty() ? std::string("/") : systemRoot_;
            std::ifstream schema(root + "/etc/palm/schemas/sam/"
                                 "applicationManager.blockSystemApp.schema",
                                 std::ios::in | std::ios::binary);
            if (schema) {
                const std::string source((std::istreambuf_iterator<char>(schema)),
                                         std::istreambuf_iterator<char>());
                if (source.find("\"forceBlockSystemApp\"") != std::string::npos)
                    payload.set("forceBlockSystemApp", Json::flag(false));
            }
        }
        action = uri == "com.webos.applicationManager/blockSystemApp"
            ? "sam-rescan.block-app" : "sam-rescan.release-app";
        resource = "sam-app-rescan";
    } else {
        sendSetupError(exchange, 400, "unsupported SAM operation");
        return;
    }

    LunaReply reply;
    if (!waitForCommandPolicyLuna(
            luna_, commandPolicy_, controlPolicyContext(config_), action, uri, payload,
            resource, false, reply,
            payload["id"].isString() ? payload["id"].string() : std::string(), false,
            std::chrono::milliseconds(5000))) {
        sendSetupError(exchange, 502,
                       reply.error.empty() ? "native SAM request failed" : reply.error);
        return;
    }
    Json result = Json::object();
    result.set("ok", Json::flag(true));
    result.set("response", reply.response);
    (void)exchange.respond(200,
                           {{"Content-Type", "application/json; charset=utf-8"},
                            {"Cache-Control", "no-store"},
                            {"X-Content-Type-Options", "nosniff"}},
                           result.dump());
}

void HttpFrontDoor::setNativeAppTileOperation(HttpExchange &exchange) const {
    const HttpRequest &request = exchange.request();
    const bool internal = request.uri == "/_internal/apps/tile-hiding";
    const bool hidingRoute = request.uri == "/api/apps/hide";
    const bool unhidingRoute = request.uri == "/api/apps/unhide";
    const bool unhideAllRoute = request.uri == "/api/apps/unhide-all";
    const bool toggleRoute = request.uri == "/api/apps/tile-hiding";
    const bool publicRoute = hidingRoute || unhidingRoute || unhideAllRoute || toggleRoute;
    if (internal && (!isLoopbackPeer(request.remoteAddress) || proxySecret_.empty() ||
        !tokenMatchesConstantTime(request.header("x-nativetv-proxy-secret"), proxySecret_) ||
        !request.header("x-nativetv-original-remote").empty())) {
        sendSetupError(exchange, 403, "native app tile operation required", true);
        return;
    }
    if (!internal && !publicRoute) {
        sendSetupError(exchange, 404, "not found");
        return;
    }
    if (request.method != "POST") {
        sendSetupError(exchange, 405, "method not allowed");
        return;
    }
    if (internal && !setupJsonContentType(request)) {
        sendSetupError(exchange, 415, "Content-Type must be application/json");
        return;
    }
    // Let Node keep the exact legacy refusal text when control is disabled or
    // this is an HBC install, where the hide/unhide routes have an extra guard.
    if (publicRoute && !config_.allowControl) {
        (void)proxy_.forward(exchange);
        return;
    }
    if (publicRoute && (hidingRoute || unhidingRoute) &&
        access(dataFilePath(dataRoot_, ".from-homebrew-channel").c_str(), F_OK) == 0) {
        (void)proxy_.forward(exchange);
        return;
    }
    if (internal && !config_.allowControl) {
        sendSetupError(exchange, 403, "Control is disabled in server configuration");
        return;
    }

    // The native SAM visibility bridge is deliberately bounded. Large legacy
    // sets keep the Node coordinator, which performs its existing per-app
    // sequence without sending an oversized visibility request.
    if (publicRoute && (unhideAllRoute || toggleRoute)) {
        std::set<std::string> currentIds;
        bool currentEnabled = false;
        std::string currentError;
        if (!readAppTileStateFiles(hiddenAppsPath_, tileHidingFlagPath_, currentIds,
                                   currentEnabled, &currentError)) {
            sendSetupError(exchange, 500, currentError.empty()
                ? "could not read app tile state" : currentError, true);
            return;
        }
        if (currentIds.size() > 64) {
            (void)proxy_.forward(exchange);
            return;
        }
    }
    if (!appTileManager_ || !commandPolicy_) {
        sendSetupError(exchange, 503, "native app tile manager is unavailable");
        return;
    }
    const size_t maximumBodyBytes = publicRoute ? 32768 : 8192;
    if (!exchange.setBodyLimit(maximumBodyBytes)) {
        sendSetupError(exchange, 413, "request body too large", true);
        return;
    }
    std::string body;
    char buffer[1024];
    for (;;) {
        const int count = exchange.readBody(buffer, sizeof(buffer));
        if (count == 0) break;
        if (count < 0) {
            sendSetupError(exchange, exchange.bodyLimitExceeded() ? 413 : 400,
                           exchange.bodyLimitExceeded() ? "request body too large"
                                                        : "invalid request body",
                           true);
            return;
        }
        body.append(buffer, static_cast<size_t>(count));
    }
    if (exceedsUtf16CodeUnitLimit(body, 8192)) {
        sendSetupError(exchange, 413, "request body too large", true);
        return;
    }
    const Json payload = Json::parse(body);
    if (internal && (!payload.isObject() || !payload["action"].isString() ||
                     !payload["id"].isString())) {
        sendSetupError(exchange, 400, "app tile action and ID expected");
        return;
    }
    if (publicRoute && !payload.isObject() && !payload.isArray()) {
        sendSetupError(exchange, 400, "malformed JSON");
        return;
    }
    std::string action;
    std::string id;
    if (internal) {
        action = payload["action"].string();
        id = payload["id"].string();
    } else if (hidingRoute || unhidingRoute) {
        const Json requestedId = payload["id"];
        if (!requestedId.isString() || !appTileIdIsValid(requestedId.string())) {
            sendSetupError(exchange, 400, "Invalid app ID format");
            return;
        }
        action = hidingRoute ? "hide-native" : "unhide-native";
        id = requestedId.string();
    } else if (unhideAllRoute) {
        action = "unhide-all-native";
    } else {
        action = setupTruthy(payload["enabled"]) ? "enable-all" : "disable-all";
    }
    const bool ownsVisibility = action == "hide-native" || action == "unhide-native";
    const bool unhidesAll = action == "unhide-all-native";
    const bool changesEnabled = action == "enable-all" || action == "disable-all";
    const bool aggregate = unhidesAll || changesEnabled;
    const std::string managerAction = action == "hide-native" ? "hide"
        : action == "unhide-native" ? "unhide" : action;
    if (!aggregate && !appTileIdIsValid(id)) {
        sendSetupError(exchange, 400, "app tile action and valid app ID expected");
        return;
    }
    if (!aggregate && managerAction != "hide" && managerAction != "unhide" &&
        managerAction != "mount" && managerAction != "unmount") {
        sendSetupError(exchange, 400, "unknown app tile operation");
        return;
    }

    std::unique_lock<std::mutex> stateLock(appsStateMutex_, std::defer_lock);
    if (ownsVisibility || aggregate) stateLock.lock();
    std::set<std::string> hiddenIds;
    bool tileHidingEnabled = false;
    std::string stateError;
    if (!readAppTileStateFiles(hiddenAppsPath_, tileHidingFlagPath_, hiddenIds,
                               tileHidingEnabled, &stateError)) {
        sendSetupError(exchange, 500, stateError.empty()
            ? "could not read app tile state" : stateError);
        return;
    }
    const Json configured = config_.document["allowTileHiding"];
    const bool tileHidingAllowed = configured.isBoolean()
        ? configured.boolean() : tileHidingEnabled;
    const bool enabling = action == "enable-all";
    if (managerAction == "hide" || managerAction == "mount" || enabling) {
        if (!tileHidingAllowed) {
            if (publicRoute && (hidingRoute || enabling)) {
                sendSetupError(exchange, 400,
                    "Hiding system apps is turned off on this TV for now. On webOS 10 and later it can leave the picture, the sound and HDMI control off until the TV is unplugged. Apps already hidden come back after the TV is next fully restarted.");
            } else {
                sendSetupError(exchange, 403,
                               "the current TV configuration does not allow app tile hiding");
            }
            return;
        }
    }

    const auto applyTileOperation = [this](const std::string &operation,
                                           const std::string &appId,
                                           Json &operationResult,
                                           int &httpStatus) {
        CommandPolicySpec localCommand;
        localCommand.action = "apps.tile-hiding." + operation;
        localCommand.uri = "native/apps/tile-hiding";
        localCommand.payload = Json::object();
        localCommand.payload.set("action", Json::text(operation));
        localCommand.payload.set("id", Json::text(appId));
        localCommand.timeout = std::chrono::milliseconds(25000);
        localCommand.resource = "app-tile-mounts";
        LunaReply localReply;
        const CommandDecision decision = commandPolicy_->executeLocal(
            localCommand, controlPolicyContext(config_), false,
            [this, &operation, &appId, &operationResult]() {
                operationResult = appTileManager_->apply(operation, appId);
                return operationResult["ok"].isBoolean() &&
                       operationResult["ok"].boolean();
            }, localReply);
        const bool succeeded = decision.kind == CommandDecisionKind::Allowed &&
            localReply.response["returnValue"].isBoolean() &&
            localReply.response["returnValue"].boolean();
        httpStatus = succeeded ? 200
            : decision.kind == CommandDecisionKind::Allowed ? 400 : 503;
        if (!operationResult.isObject()) operationResult = Json::object();
        if (!succeeded) {
            operationResult.set("ok", Json::flag(false));
            if (!operationResult["error"].isString())
                operationResult.set("error", Json::text(
                    !localReply.error.empty() ? localReply.error
                    : !decision.reason.empty() ? decision.reason
                    : "native app tile operation failed"));
        }
        return succeeded;
    };
    const auto saveState = [this](const std::set<std::string> &ids, bool enabled,
                                  std::string *error) {
        Json state = Json::object();
        Json hidden = Json::array();
        for (const std::string &hiddenId : ids) hidden.add(Json::text(hiddenId));
        state.set("hiddenApps", hidden);
        state.set("tileHidingEnabled", Json::flag(enabled));
        return persistAppTileState(hiddenAppsPath_, tileHidingFlagPath_, state, error);
    };
    const auto refreshSam = [this](const Json &expected) {
        if (!samRescanManager_) return false;
        const Json refreshed = samRescanManager_->refresh(
            expected, controlPolicyContext(config_));
        if (refreshed["result"].isBoolean() && refreshed["result"].boolean())
            return true;
        if (logger_ && refreshed["error"].isString())
            logger_->warning("sam-rescan: " + refreshed["error"].string());
        return false;
    };

    if (aggregate) {
        if (hiddenIds.size() > 64) {
            sendSetupError(exchange, 413, "too many hidden apps for managed tile operation");
            return;
        }
        Json result = Json::object();
        std::string operationError;
        bool resultTileHidingEnabled = tileHidingEnabled;
        Json expected = Json::object();

        if (changesEnabled) {
            resultTileHidingEnabled = enabling;
            if (!saveState(hiddenIds, resultTileHidingEnabled, &operationError)) {
                if (logger_) logger_->warning("apps state: " + operationError);
                sendSetupError(exchange, 500, "could not persist app tile state");
                return;
            }
            for (const std::string &hiddenId : hiddenIds) {
                const std::string operation = enabling ? "mount" : "unmount";
                Json one;
                int ignoredStatus = 200;
                if (!applyTileOperation(operation, hiddenId, one, ignoredStatus)) {
                    if (logger_) logger_->warning("apps: native tile " + operation +
                        " failed for " + hiddenId + ": " + one["error"].string());
                    continue;
                }
                if (!enabling) {
                    expected.set(hiddenId, Json::flag(true));
                } else {
                    const std::string overridePath = dataFilePath(
                        dataRoot_, "appinfo-overrides/" + hiddenId + ".json");
                    struct stat info = {};
                    if (lstat(overridePath.c_str(), &info) == 0 && S_ISREG(info.st_mode))
                        expected.set(hiddenId, Json::flag(false));
                }
            }
            const Json allowConfig = config_.document["allowTileHiding"];
            const bool mayRefresh = allowConfig.isBoolean()
                ? allowConfig.boolean() : resultTileHidingEnabled;
            const bool restarted = mayRefresh && refreshSam(expected);
            result.set("ok", Json::flag(true));
            result.set("tileHidingEnabled", Json::flag(resultTileHidingEnabled));
            result.set("samRestarted", Json::flag(restarted));
            (void)exchange.respond(200,
                                   {{"Content-Type", "application/json; charset=utf-8"},
                                    {"Cache-Control", "no-store"},
                                    {"X-Content-Type-Options", "nosniff"}},
                                   result.dump());
            return;
        }

        for (const std::string &hiddenId : hiddenIds) {
            Json one;
            int operationStatus = 200;
            if (!applyTileOperation("unhide", hiddenId, one, operationStatus)) {
                result.set("ok", Json::flag(false));
                result.set("error", Json::text(one["error"].string().empty()
                    ? "native app tile operation failed" : one["error"].string()));
                (void)exchange.respond(publicRoute ? 400 : operationStatus,
                                       {{"Content-Type", "application/json; charset=utf-8"},
                                        {"Cache-Control", "no-store"},
                                        {"X-Content-Type-Options", "nosniff"}},
                                       result.dump());
                return;
            }
            expected.set(hiddenId, Json::flag(true));
        }
        if (!saveState(std::set<std::string>(), tileHidingEnabled, &operationError)) {
            if (logger_) logger_->warning("apps state: " + operationError);
            sendSetupError(exchange, 500, "could not persist app tile state");
            return;
        }
        const Json allowConfig = config_.document["allowTileHiding"];
        const bool mayRefresh = allowConfig.isBoolean()
            ? allowConfig.boolean() : tileHidingEnabled;
        const bool restarted = mayRefresh && refreshSam(expected);
        result.set("ok", Json::flag(true));
        result.set("restoredCount", Json::numeric(static_cast<double>(hiddenIds.size())));
        if (!publicRoute)
            result.set("tileHidingEnabled", Json::flag(tileHidingEnabled));
        result.set("samRestarted", Json::flag(restarted));
        (void)exchange.respond(200,
                               {{"Content-Type", "application/json; charset=utf-8"},
                                {"Cache-Control", "no-store"},
                                {"X-Content-Type-Options", "nosniff"}},
                               result.dump());
        return;
    }

    Json result;
    int status = 200;
    const bool succeeded = applyTileOperation(managerAction, id, result, status);
    if (succeeded && ownsVisibility) {
        if (managerAction == "hide") hiddenIds.insert(id);
        else hiddenIds.erase(id);
        tileHidingEnabled = managerAction == "hide" ? true : tileHidingEnabled;

        if (!saveState(hiddenIds, tileHidingEnabled, &stateError)) {
            if (logger_) logger_->warning("apps state: " + stateError);
            result = Json::object();
            result.set("ok", Json::flag(false));
            result.set("error", Json::text("could not persist app tile state"));
            status = publicRoute ? 400 : 500;
        } else {
            result.set("tileHidingEnabled", Json::flag(tileHidingEnabled));
            const Json allowedConfig = config_.document["allowTileHiding"];
            const bool mayRefresh = allowedConfig.isBoolean()
                ? allowedConfig.boolean() : tileHidingEnabled;
            Json expected = Json::object();
            expected.set(id, Json::flag(managerAction == "unhide"));
            const bool samRestarted = mayRefresh && refreshSam(expected);
            result.set("samRestarted", Json::flag(samRestarted));
        }
    }
    if (publicRoute && ownsVisibility && succeeded) {
        Json publicResult = Json::object();
        publicResult.set("ok", Json::flag(true));
        publicResult.set("id", Json::text(id));
        publicResult.set("hidden", Json::flag(managerAction == "hide"));
        publicResult.set("samRestarted", Json::flag(result["samRestarted"].boolean()));
        result = publicResult;
    }
    (void)exchange.respond(publicRoute && !succeeded ? 400 : status,
                           {{"Content-Type", "application/json; charset=utf-8"},
                            {"Cache-Control", "no-store"},
                            {"X-Content-Type-Options", "nosniff"}},
                           result.dump());
}

void HttpFrontDoor::setNativeAppUninstall(HttpExchange &exchange) const {
    const HttpRequest &request = exchange.request();
    const bool internal = request.uri == "/_internal/apps/uninstall";
    const bool publicRoute = request.uri == "/api/apps/uninstall";
    if (!internal && !publicRoute) {
        sendSetupError(exchange, 404, "not found");
        return;
    }
    if (internal && (!isLoopbackPeer(request.remoteAddress) || proxySecret_.empty() ||
        !tokenMatchesConstantTime(request.header("x-nativetv-proxy-secret"), proxySecret_) ||
        !request.header("x-nativetv-original-remote").empty())) {
        sendSetupError(exchange, 403, "native app removal required", true);
        return;
    }
    if (request.method != "POST") {
        sendSetupError(exchange, 405, "method not allowed");
        return;
    }
    if (publicRoute && !sameOriginRequest(request)) {
        sendSetupError(exchange, 403, "cross-origin request refused");
        return;
    }
    if (!setupJsonContentType(request)) {
        sendSetupError(exchange, 415, "Content-Type must be application/json");
        return;
    }
    if (publicRoute && !config_.allowControl) {
        sendSetupError(exchange, 400, say::msg("srv.controlsOff.apps", "Control is disabled in server configuration"));
        return;
    }
    if (publicRoute && !commandPolicy_) {
        (void)proxy_.forward(exchange);
        return;
    }
    if (internal && !config_.allowControl) {
        sendSetupError(exchange, 403, "Control is disabled in server configuration");
        return;
    }
    if (!commandPolicy_) {
        sendSetupError(exchange, 503, "native app removal policy is unavailable");
        return;
    }
    if (!exchange.setBodyLimit(publicRoute ? 32768 : 8192)) {
        sendSetupError(exchange, 413, "request body too large", true);
        return;
    }
    std::string body;
    char buffer[1024];
    for (;;) {
        const int count = exchange.readBody(buffer, sizeof(buffer));
        if (count == 0) break;
        if (count < 0) {
            sendSetupError(exchange, exchange.bodyLimitExceeded() ? 413 : 400,
                           exchange.bodyLimitExceeded() ? "request body too large"
                                                        : "invalid request body",
                           true);
            return;
        }
        body.append(buffer, static_cast<size_t>(count));
    }
    if (exceedsUtf16CodeUnitLimit(body, 8192)) {
        sendSetupError(exchange, 413, "request body too large", true);
        return;
    }
    const Json payload = Json::parse(body);
    if (!payload.isObject() || !payload["id"].isString() ||
        !appTileIdIsValid(payload["id"].string())) {
        sendSetupError(exchange, 400, "Invalid app ID format");
        return;
    }
    const std::string id = payload["id"].string();
    if (appTileIdIsProtected(id)) {
        sendSetupError(exchange, 400, "Protected core application cannot be uninstalled");
        return;
    }

    // Closing is best-effort, matching the public Node path. Queue it through
    // the command policy before removal so the app lifecycle stays native in
    // managed mode.
    Json closePayload = Json::object();
    closePayload.set("id", Json::text(id));
    CommandPolicySpec closeCommand;
    closeCommand.action = "apps.uninstall.close";
    closeCommand.uri = "com.webos.applicationManager/closeByAppId";
    closeCommand.payload = closePayload;
    closeCommand.appId = id;
    closeCommand.timeout = std::chrono::milliseconds(1000);
    closeCommand.resource = "apps-uninstall-close";
    (void)commandPolicy_->submit(
        closeCommand, controlPolicyContext(config_), false,
        [](const LunaReply &) {});

    std::string lowerAppId = id;
    std::transform(lowerAppId.begin(), lowerAppId.end(), lowerAppId.begin(),
                   [](unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });
    if (publicRoute && lowerAppId.find("screensaver") != std::string::npos) {
        Json event = Json::object();
        event.set("type", Json::text("apps-uninstall-prep"));
        event.set("id", Json::text(id));
        std::string error;
        if (!proxy_.requestNodeState(event, error, 10000) && logger_)
            logger_->warning("apps: could not complete screen-saver uninstall cleanup: " +
                             error);
    }

    const auto waitForRemoval = [this, &id]() {
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::seconds(3);
        while (std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            LunaReply appsReply;
            const bool answered = waitForCommandPolicyLuna(
                luna_, commandPolicy_, controlPolicyContext(config_),
                "apps.uninstall.list-apps",
                "com.webos.applicationManager/listApps", Json::object(),
                "apps-uninstall-list", false, appsReply, id, false,
                std::chrono::milliseconds(750));
            if (!answered)
                continue;
            bool stillThere = false;
            const Json apps = appsReply.response["apps"];
            if (apps.isArray()) {
                for (const Json &app : apps.items()) {
                    if (app["id"].isString() && app["id"].string() == id) {
                        stillThere = true;
                        break;
                    }
                }
            }
            if (!stillThere)
                return;
        }
    };

    const auto remove = [this, &id](const std::string &uri,
                                    const std::string &action,
                                    LunaReply &reply) {
        Json requestPayload = Json::object();
        requestPayload.set("id", Json::text(id));
        return waitForCommandPolicyLuna(
            luna_, commandPolicy_, controlPolicyContext(config_), action, uri,
            requestPayload, "app-install", false, reply, std::string(), false,
            std::chrono::milliseconds(5000));
    };

    const auto respondSuccess = [this, &exchange, &id, &waitForRemoval,
                                 internal, publicRoute]() {
        waitForRemoval();
        if (publicRoute) {
            refreshLauncher();
            Json event = Json::object();
            event.set("type", Json::text("apps-changed"));
            std::string error;
            if (!proxy_.requestNodeState(event, error, 70000) && logger_)
                logger_->warning("apps: uninstall completed but Node app refresh failed: " +
                                 error);
        }
        Json result = Json::object();
        result.set("ok", Json::flag(true));
        result.set("id", Json::text(id));
        std::vector<HttpHeader> headers = {
            {"Content-Type", "application/json; charset=utf-8"},
            {"Cache-Control", "no-store"},
            {"X-Content-Type-Options", "nosniff"}
        };
        if (internal)
            headers.push_back({"X-NativeTv-App-Lifecycle", "complete"});
        (void)exchange.respond(200,
                               headers, result.dump());
    };

    LunaReply standard;
    const bool standardAnswered = remove(
        "com.webos.appInstallService/remove", "apps.uninstall.remove", standard);
    if (standardAnswered && standard.response["returnValue"].boolean()) {
        respondSuccess();
        return;
    }

    LunaReply developer;
    const bool developerAnswered = remove(
        "com.webos.appInstallService/dev/remove", "apps.uninstall.dev-remove",
        developer);
    if (developerAnswered && developer.response["returnValue"].boolean()) {
        respondSuccess();
        return;
    }

    std::string error = developer.response["errorText"].string();
    if (error.empty()) error = standard.response["errorText"].string();
    if (error.empty()) error = "Failed to uninstall app";
    Json result = Json::object();
    result.set("ok", Json::flag(false));
    result.set("error", Json::text(error));
    result.set("id", Json::text(id));
    (void)exchange.respond(publicRoute ? 400 : 200,
                           {{"Content-Type", "application/json; charset=utf-8"},
                            {"Cache-Control", "no-store"},
                            {"X-Content-Type-Options", "nosniff"}},
                           result.dump());
}

void HttpFrontDoor::setNativeAppSideload(HttpExchange &exchange) const {
    const HttpRequest &request = exchange.request();
    if (request.method != "POST") {
        sendSetupError(exchange, 404, "not found");
        return;
    }
    if (!config_.allowControl) {
        sendSetupError(exchange, 403, say::msg("srv.controlsOff", "controls disabled in config"));
        return;
    }
    if (!setupJsonContentType(request)) {
        sendSetupError(exchange, 415, "Content-Type must be application/json");
        return;
    }
    if (!sameOriginRequest(request)) {
        sendSetupError(exchange, 403, "cross-origin request refused");
        return;
    }
    if (!exchange.setBodyLimit(32768)) {
        sendSetupError(exchange, 413, "request body too large", true);
        return;
    }
    std::string body;
    char buffer[1024];
    for (;;) {
        const int count = exchange.readBody(buffer, sizeof(buffer));
        if (count == 0)
            break;
        if (count < 0) {
            sendSetupError(exchange, exchange.bodyLimitExceeded() ? 413 : 400,
                           exchange.bodyLimitExceeded() ? "request body too large"
                                                        : "invalid request body",
                           true);
            return;
        }
        body.append(buffer, static_cast<size_t>(count));
    }
    if (exceedsUtf16CodeUnitLimit(body, 8192)) {
        sendSetupError(exchange, 413, "request body too large", true);
        return;
    }
    const Json payload = Json::parse(body);
    if (!payload.isObject() && !payload.isArray()) {
        sendSetupError(exchange, 400, "malformed JSON");
        return;
    }

    const bool enabled = payload["enabled"].isBoolean() &&
                         payload["enabled"].boolean();
    Json patch = Json::object();
    patch.set("sideload", Json::flag(enabled));
    std::string error;
    if (!ConfigStore(configPath_).patchObjectSection("apps", patch, nullptr, &error)) {
        Json failure = Json::object();
        failure.set("ok", Json::flag(false));
        failure.set("error", Json::text(error.empty() ? "could not save setting" : error));
        (void)exchange.respond(400,
                               {{"Content-Type", "application/json; charset=utf-8"},
                                {"Cache-Control", "no-store"},
                                {"X-Content-Type-Options", "nosniff"}},
                               failure.dump());
        if (logger_)
            logger_->warning("apps: could not save sideload setting: " + error);
        return;
    }

    std::string reloadError;
    if (!proxy_.requestNodeConfigReload(reloadError)) {
        if (logger_)
            logger_->warning("apps: saved sideload setting but could not reload Node config: " +
                             reloadError);
        Json failure = Json::object();
        failure.set("ok", Json::flag(false));
        failure.set("error", Json::text(
            "sideload setting was saved but the installer policy could not be reloaded"));
        (void)exchange.respond(502,
                               {{"Content-Type", "application/json; charset=utf-8"},
                                {"Cache-Control", "no-store"},
                                {"X-Content-Type-Options", "nosniff"}},
                               failure.dump());
        return;
    }
    if (enabled) {
        if (logger_)
            logger_->warning("security: apps.sideload enabled from dashboard; arbitrary IPK installations allowed");
    } else if (logger_) {
        logger_->info("apps: apps.sideload disabled");
    }

    Json result = Json::object();
    result.set("ok", Json::flag(true));
    result.set("sideload", Json::flag(enabled));
    if (enabled)
        result.set("sideloadVia", Json::text("config"));
    else if (!config_.token.empty())
        result.set("sideloadVia", Json::text("token"));
    else if (isLoopbackPeer(request.remoteAddress))
        result.set("sideloadVia", Json::text("tv"));
    else
        result.set("sideloadVia", Json());
    (void)exchange.respond(200,
                           {{"Content-Type", "application/json; charset=utf-8"},
                            {"Cache-Control", "no-store"},
                            {"X-Content-Type-Options", "nosniff"}},
                           result.dump());
}

void HttpFrontDoor::setNativeAppsRepos(HttpExchange &exchange) const {
    const HttpRequest &request = exchange.request();
    if (!isLoopbackPeer(request.remoteAddress) || proxySecret_.empty() ||
        !tokenMatchesConstantTime(request.header("x-nativetv-proxy-secret"), proxySecret_) ||
        !request.header("x-nativetv-original-remote").empty()) {
        sendSetupError(exchange, 403, "native app repository update required", true);
        return;
    }
    if (request.method != "POST") {
        sendSetupError(exchange, 405, "method not allowed");
        return;
    }
    if (!setupJsonContentType(request)) {
        sendSetupError(exchange, 415, "Content-Type must be application/json");
        return;
    }
    if (!config_.allowControl) {
        sendSetupError(exchange, 403, "controls disabled in config");
        return;
    }
    if (!exchange.setBodyLimit(1048576)) {
        sendSetupError(exchange, 413, "request body too large", true);
        return;
    }
    std::string body;
    char buffer[1024];
    for (;;) {
        const int count = exchange.readBody(buffer, sizeof(buffer));
        if (count == 0)
            break;
        if (count < 0) {
            sendSetupError(exchange, exchange.bodyLimitExceeded() ? 413 : 400,
                           exchange.bodyLimitExceeded() ? "request body too large"
                                                        : "invalid request body",
                           true);
            return;
        }
        body.append(buffer, static_cast<size_t>(count));
    }
    const Json payload = Json::parse(body);
    const Json repos = payload["repos"];
    if (!payload.isObject() || payload.keys().size() != 1 || !repos.isArray() ||
        repos.size() > 1024) {
        sendSetupError(exchange, 400, "invalid app repository list");
        return;
    }
    for (const Json &entry : repos.items()) {
        if (!entry.isString()) {
            sendSetupError(exchange, 400, "invalid app repository list");
            return;
        }
    }

    std::lock_guard<std::mutex> mutationLock(appReposMutationMutex_);
    Json patch = Json::object();
    patch.set("repos", repos);
    std::string error;
    {
        std::lock_guard<std::mutex> lock(appCatalogReposMutex_);
        if (!ConfigStore(configPath_).patchObjectSection("apps", patch, nullptr, &error)) {
            if (logger_)
                logger_->warning("apps: could not save repository list: " + error);
            sendSetupError(exchange, 400,
                           error.empty() ? "could not save app repositories" : error);
            return;
        }
        appCatalogRepos_ = repos;
        appCatalog_.invalidate();
    }
    (void)exchange.respond(204,
                           {{"Cache-Control", "no-store"},
                            {"X-Content-Type-Options", "nosniff"}},
                           "");
}

void HttpFrontDoor::setNativePublicAppsRepos(HttpExchange &exchange) const {
    const HttpRequest &request = exchange.request();
    if (request.method != "POST") {
        sendInstallError(exchange, 404, "not found");
        return;
    }
    if (!config_.allowControl) {
        sendSetupError(exchange, 403, say::msg("srv.controlsOff", "controls disabled in config"));
        return;
    }
    if (config_.document["apps"]["sideload"].isBoolean() &&
        config_.document["apps"]["sideload"].boolean()) {
        // Explicitly enabled for arbitrary app sources.
    } else if (config_.token.empty() && !isLoopbackPeer(request.remoteAddress)) {
        sendInstallError(exchange, 403,
                         say::msg("srv.install.sideloadOff",
                                  "installing from a URL or a file is off; set \"apps\": {\"sideload\": true} in config.json, set a token, or use the TV's web browser"));
        return;
    }
    if (rejectCrossSitePost(exchange)) return;
    if (!exchange.setBodyLimit(32768)) {
        sendInstallError(exchange, 413, "request body too large", true);
        return;
    }
    std::string body;
    char buffer[1024];
    for (;;) {
        const int count = exchange.readBody(buffer, sizeof(buffer));
        if (count == 0) break;
        if (count < 0) {
            sendInstallError(exchange, exchange.bodyLimitExceeded() ? 413 : 400,
                             exchange.bodyLimitExceeded() ? "request body too large"
                                                          : "invalid request body",
                             exchange.bodyLimitExceeded());
            return;
        }
        body.append(buffer, static_cast<size_t>(count));
    }
    if (exceedsUtf16CodeUnitLimit(body, 8192)) {
        sendInstallError(exchange, 413, "request body too large", true);
        return;
    }
    const Json payload = Json::parse(body);
    if (!payload.valid() || (!payload.isObject() && !payload.isArray())) {
        sendInstallError(exchange, 400, "malformed JSON");
        return;
    }

    std::lock_guard<std::mutex> mutationLock(appReposMutationMutex_);
    Json repositories;
    {
        std::lock_guard<std::mutex> lock(appCatalogReposMutex_);
        repositories = service::AppCatalog::customRepositories(appCatalogRepos_);
    }

    const std::string action = payload["action"].string();
    if (action == "remove") {
        const Json submittedUrl = payload["url"];
        const std::string url = submittedUrl.isString() ? submittedUrl.string() : "";
        Json next = Json::array();
        bool removed = false;
        for (const Json &entry : repositories.items()) {
            if (!removed && entry.string() == url) {
                removed = true;
                continue;
            }
            next.add(entry);
        }
        if (!removed) {
            sendInstallError(exchange, 400,
                             say::msg("srv.repo.notAdded",
                                      "that catalog is not one added here"));
            return;
        }
        repositories = next;
    } else if (action == "add") {
        if (repositories.size() >= 10) {
            Json variables = Json::object();
            variables.set("n", Json::text("10"));
            sendInstallError(exchange, 400,
                             say::msg("srv.repo.tooMany",
                                      "at most {n} catalogs can be added", variables));
            return;
        }
        const Json submittedUrl = payload["url"];
        std::string input;
        if (!(submittedUrl.isBoolean() && !submittedUrl.boolean()) &&
            !(submittedUrl.isNumber() && submittedUrl.number() == 0))
            input = quickAccessStringValue(submittedUrl);
        const std::string candidate =
            service::AppCatalog::normalizeRepositoryCandidate(input);
        const std::string validationError =
            service::AppCatalog::repositoryInputError(candidate);
        if (!validationError.empty()) {
            sendInstallError(exchange, 400, validationError);
            return;
        }
        const Json checked = appCatalog_.checkRepository(candidate);
        if (!checked["ok"].isBoolean() || !checked["ok"].boolean()) {
            sendInstallError(exchange, 400,
                             checked["error"].string().empty()
                                 ? "not a package catalog: " + candidate
                                 : checked["error"].string());
            return;
        }
        const std::string found = candidate;
        bool alreadyListed = found == service::AppCatalog::defaultRepository();
        for (const Json &entry : repositories.items())
            if (entry.string() == found) alreadyListed = true;
        if (alreadyListed) {
            sendInstallError(exchange, 400,
                             say::msg("srv.repo.already",
                                      "that catalog is already listed"));
            return;
        }
        repositories.add(Json::text(found));
        if (logger_)
            logger_->warning("security: catalog added from dashboard: " + found);
    } else {
        sendInstallError(exchange, 400,
                         say::msg("srv.repo.badAction",
                                  "action must be add or remove"));
        return;
    }

    Json patch = Json::object();
    patch.set("repos", repositories);
    std::string error;
    {
        std::lock_guard<std::mutex> lock(appCatalogReposMutex_);
        if (!ConfigStore(configPath_).patchObjectSection("apps", patch, nullptr, &error)) {
            if (logger_)
                logger_->warning("apps: could not save repository list: " + error);
            sendInstallError(exchange, 400,
                             error.empty() ? "could not save app repositories" : error);
            return;
        }
        appCatalogRepos_ = repositories;
        appCatalog_.invalidate();
    }

    std::string reloadError;
    if (!proxy_.requestNodeConfigReload(reloadError)) {
        if (logger_)
            logger_->warning("apps: saved repository list but could not reload Node config: " +
                             reloadError);
        sendInstallError(exchange, 502,
                         "app repository list was saved but the installer config could not be reloaded");
        return;
    }

    if (action == "remove" && logger_)
        logger_->info("apps: catalog removed: " + payload["url"].string());
    Json result = Json::object();
    result.set("ok", Json::flag(true));
    result.set("repos", repositories);
    (void)exchange.respond(200,
                           {{"Content-Type", "application/json; charset=utf-8"},
                            {"Cache-Control", "no-store"},
                            {"X-Content-Type-Options", "nosniff"}},
                           result.dump());
}

void HttpFrontDoor::findNativeAppPackage(HttpExchange &exchange) const {
    const HttpRequest &request = exchange.request();
    if (!isLoopbackPeer(request.remoteAddress) || proxySecret_.empty() ||
        !tokenMatchesConstantTime(request.header("x-nativetv-proxy-secret"), proxySecret_) ||
        !request.header("x-nativetv-original-remote").empty()) {
        sendSetupError(exchange, 403, "native app catalog lookup required", true);
        return;
    }
    if (request.method != "POST") {
        sendSetupError(exchange, 405, "method not allowed");
        return;
    }
    if (!setupJsonContentType(request)) {
        sendSetupError(exchange, 415, "Content-Type must be application/json");
        return;
    }
    if (!config_.allowControl) {
        sendSetupError(exchange, 403, "controls disabled in config");
        return;
    }
    if (!exchange.setBodyLimit(8192)) {
        sendSetupError(exchange, 413, "request body too large", true);
        return;
    }
    std::string body;
    char buffer[1024];
    for (;;) {
        const int count = exchange.readBody(buffer, sizeof(buffer));
        if (count == 0)
            break;
        if (count < 0) {
            sendSetupError(exchange, exchange.bodyLimitExceeded() ? 413 : 400,
                           exchange.bodyLimitExceeded() ? "request body too large"
                                                        : "invalid request body",
                           true);
            return;
        }
        body.append(buffer, static_cast<size_t>(count));
    }
    const Json payload = Json::parse(body);
    const std::string id = payload["id"].string();
    const Json rawRepo = payload["repo"];
    if (!payload.isObject() || (payload.keys().size() != 1 && payload.keys().size() != 2) ||
        !payload["id"].isString() || id.empty() ||
        (rawRepo.valid() && !rawRepo.isString())) {
        sendSetupError(exchange, 400, "app catalog lookup parameters expected");
        return;
    }

    LunaReply appReply;
    if (!waitForLuna(luna_, "com.webos.applicationManager/listApps",
                     Json::object(), appReply))
        appReply.response = Json::object();
    const int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::system_clock::now().time_since_epoch())
                              .count();
    Json configuredRepos;
    {
        std::lock_guard<std::mutex> lock(appCatalogReposMutex_);
        configuredRepos = appCatalogRepos_;
    }
    const Json result = appCatalog_.findPackage(configuredRepos, appReply.response,
                                                 id, rawRepo.string(), nowMs);
    sendMetricsJson(exchange, result.dump());
}

void HttpFrontDoor::checkNativeAppsRepository(HttpExchange &exchange) const {
    const HttpRequest &request = exchange.request();
    if (!isLoopbackPeer(request.remoteAddress) || proxySecret_.empty() ||
        !tokenMatchesConstantTime(request.header("x-nativetv-proxy-secret"), proxySecret_) ||
        !request.header("x-nativetv-original-remote").empty()) {
        sendSetupError(exchange, 403, "native app repository check required", true);
        return;
    }
    if (request.method != "POST") {
        sendSetupError(exchange, 405, "method not allowed");
        return;
    }
    if (!setupJsonContentType(request)) {
        sendSetupError(exchange, 415, "Content-Type must be application/json");
        return;
    }
    if (!config_.allowControl) {
        sendSetupError(exchange, 403, "controls disabled in config");
        return;
    }
    if (!exchange.setBodyLimit(8192)) {
        sendSetupError(exchange, 413, "request body too large", true);
        return;
    }
    std::string body;
    char buffer[1024];
    for (;;) {
        const int count = exchange.readBody(buffer, sizeof(buffer));
        if (count == 0) break;
        if (count < 0) {
            sendSetupError(exchange, exchange.bodyLimitExceeded() ? 413 : 400,
                           exchange.bodyLimitExceeded() ? "request body too large"
                                                        : "invalid request body",
                           true);
            return;
        }
        body.append(buffer, static_cast<size_t>(count));
    }
    const Json payload = Json::parse(body);
    if (!payload.isObject() || payload.keys().size() != 1 ||
        !payload["url"].isString() || payload["url"].string().empty()) {
        sendSetupError(exchange, 400, "app repository URL expected");
        return;
    }
    sendMetricsJson(exchange, appCatalog_.checkRepository(payload["url"].string()).dump());
}

void HttpFrontDoor::downloadNativeAppPackage(HttpExchange &exchange) const {
    const HttpRequest &request = exchange.request();
    if (!isLoopbackPeer(request.remoteAddress) || proxySecret_.empty() ||
        !tokenMatchesConstantTime(request.header("x-nativetv-proxy-secret"), proxySecret_) ||
        !request.header("x-nativetv-original-remote").empty()) {
        sendSetupError(exchange, 403, "native app package download required", true);
        return;
    }
    if (request.method != "POST") {
        sendSetupError(exchange, 405, "method not allowed");
        return;
    }
    if (!setupJsonContentType(request)) {
        sendSetupError(exchange, 415, "Content-Type must be application/json");
        return;
    }
    if (!config_.allowControl) {
        sendSetupError(exchange, 403, "controls disabled in config");
        return;
    }
    if (!exchange.setBodyLimit(8192)) {
        sendSetupError(exchange, 413, "request body too large", true);
        return;
    }
    std::string body;
    char buffer[1024];
    for (;;) {
        const int count = exchange.readBody(buffer, sizeof(buffer));
        if (count == 0) break;
        if (count < 0) {
            sendSetupError(exchange, exchange.bodyLimitExceeded() ? 413 : 400,
                           exchange.bodyLimitExceeded() ? "request body too large"
                                                        : "invalid request body",
                           true);
            return;
        }
        body.append(buffer, static_cast<size_t>(count));
    }
    const Json payload = Json::parse(body);
    const Json urlValue = payload["url"];
    const Json limitValue = payload["maxBytes"];
    const Json expectedValue = payload["expectedBytes"];
    const Json fileValue = payload["file"];
    const bool hasLimit = payload.isObject() && payload.has("maxBytes");
    const bool hasExpected = payload.isObject() && payload.has("expectedBytes");
    const uint64_t absoluteLimit = 512ULL * 1024 * 1024;
    uint64_t maximumBytes = absoluteLimit;
    if (hasLimit) {
        const double rawLimit = limitValue.number(-1);
        if (!limitValue.isNumber() || rawLimit < 1 || rawLimit > absoluteLimit ||
            static_cast<double>(static_cast<uint64_t>(rawLimit)) != rawLimit) {
            sendSetupError(exchange, 400, "invalid package download size limit");
            return;
        }
        maximumBytes = static_cast<uint64_t>(rawLimit);
    }
    uint64_t expectedPackageBytes = 0;
    if (hasExpected) {
        const double rawExpected = expectedValue.number(-1);
        constexpr double kMaxExactExpectedBytes = 9007199254740991.0 / 3.0;
        if (!expectedValue.isNumber() || !std::isfinite(rawExpected) ||
            rawExpected < 0 || rawExpected > kMaxExactExpectedBytes ||
            std::floor(rawExpected) != rawExpected) {
            sendSetupError(exchange, 400, "invalid expected package size");
            return;
        }
        expectedPackageBytes = static_cast<uint64_t>(rawExpected);
    }
    bool validFileName = fileValue.isString() && fileValue.string().size() == 20 &&
                         fileValue.string().compare(16, 4, ".ipk") == 0;
    if (validFileName) {
        for (size_t index = 0; index < 16; ++index) {
            const char value = fileValue.string()[index];
            if (!((value >= '0' && value <= '9') || (value >= 'a' && value <= 'f'))) {
                validFileName = false;
                break;
            }
        }
    }
    bool validKeys = payload.isObject() && payload.has("url") && payload.has("file") &&
                     payload.keys().size() == static_cast<size_t>(2 +
                         (hasLimit ? 1 : 0) + (hasExpected ? 1 : 0));
    if (validKeys) {
        for (const std::string &key : payload.keys())
            if (key != "url" && key != "file" && key != "maxBytes" &&
                key != "expectedBytes")
                validKeys = false;
    }
    if (!payload.isObject() || !validKeys ||
        !urlValue.isString() || urlValue.string().empty() ||
        !validFileName || (hasLimit && !limitValue.isNumber())) {
        sendSetupError(exchange, 400, "app package URL and staging file expected");
        return;
    }
    std::string urlError;
    if (!service::AppPackageFetcher::validUrl(urlValue.string(), urlError)) {
        sendSetupError(exchange, 400, urlError);
        return;
    }

    const char *configuredDirectory = std::getenv("TVWEB_INSTALL_STAGING");
    const std::string stagingDirectory = configuredDirectory && configuredDirectory[0]
        ? configuredDirectory : "/media/developer/temp/nativetv-install";
    uint64_t freeBytes = 0;
    const bool freeBytesKnown = service::AppInstallSpace::availableBytes(
        stagingDirectory, freeBytes);
    service::AppInstallDownloadCapacity capacity;
    std::string capacityError;
    if (!service::AppInstallSpace::downloadCapacity(
            maximumBytes, expectedPackageBytes, freeBytesKnown, freeBytes,
            capacity, &capacityError)) {
        sendSetupError(exchange, 400, capacityError.empty()
            ? "invalid app download capacity request" : capacityError);
        return;
    }
    if (!capacity.capacityOkay) {
        Json failure = Json::object();
        failure.set("ok", Json::flag(false));
        failure.set("error", Json::text("not enough free space for this download"));
        failure.set("maxBytesExceeded", Json::flag(true));
        failure.set("noSpace", Json::flag(true));
        if (capacity.freeBytesKnown)
            failure.set("freeBytes", Json::numeric(static_cast<double>(capacity.freeBytes)));
        failure.set("needBytes", Json::numeric(static_cast<double>(capacity.requiredBytes)));
        (void)exchange.respond(507,
                               {{"Content-Type", "application/json; charset=utf-8"},
                                {"Cache-Control", "no-store"},
                                {"X-Content-Type-Options", "nosniff"}},
                               failure.dump());
        return;
    }
    maximumBytes = capacity.maxDownloadBytes;
    const int directory = open(stagingDirectory.c_str(), O_RDONLY | O_DIRECTORY |
                               O_CLOEXEC | O_NOFOLLOW);
    if (directory < 0) {
        sendSetupError(exchange, 500, "could not open app install staging directory");
        return;
    }
    const std::string fileName = fileValue.string();
    const int output = openat(directory, fileName.c_str(), O_WRONLY | O_CREAT | O_EXCL |
                             O_CLOEXEC | O_NOFOLLOW, 0600);
    if (output < 0) {
        close(directory);
        sendSetupError(exchange, 500, "could not create staged app package");
        return;
    }

    const std::vector<HttpHeader> streamHeaders = {
        {"Content-Type", "application/x-ndjson; charset=utf-8"},
        {"Cache-Control", "no-store"},
        {"X-Content-Type-Options", "nosniff"},
        {"Connection", "close"}
    };
    const auto beginStream = [&exchange, &streamHeaders]() {
        return exchange.responseStarted() || exchange.beginResponse(200, streamHeaders, -1);
    };
    const auto writeRecord = [&exchange, &beginStream](const Json &record) {
        if (!beginStream()) return false;
        const std::string line = record.dump() + "\n";
        return exchange.writeBody(line.data(), line.size());
    };
    uint64_t receivedBytes = 0;
    uint64_t reportedBytes = 0;
    auto lastProgress = std::chrono::steady_clock::now();
    std::string sinkError;
    const std::string jobId = fileName.substr(0, 16);
    const auto cacheNativeDownloadProgress = [
        this, &jobId, &receivedBytes]() {
        std::lock_guard<std::mutex> lock(installJobMutex_);
        const Json cachedJobId = appInstallSnapshot_["jobId"];
        if (!appInstallSnapshotReady_ || !cachedJobId.isString() ||
            cachedJobId.string() != jobId ||
            appInstallSnapshot_["state"].string() != "downloading")
            return;
        Json progress = appInstallSnapshot_["progress"];
        if (!progress.isObject()) return;
        progress.set("bytes", Json::numeric(static_cast<double>(receivedBytes)));
    };
    const auto reportProgress = [&]() {
        cacheNativeDownloadProgress();
        Json progress = Json::object();
        progress.set("type", Json::text("progress"));
        progress.set("bytes", Json::numeric(static_cast<double>(receivedBytes)));
        if (!writeRecord(progress)) return false;
        reportedBytes = receivedBytes;
        lastProgress = std::chrono::steady_clock::now();
        return true;
    };
    const service::AppPackageDownloadSink sink = [
        &output, &receivedBytes, &maximumBytes, &sinkError, &reportedBytes,
        &lastProgress, &writeRecord, &cacheNativeDownloadProgress](
        const char *bytes, size_t length) {
        if (receivedBytes > maximumBytes || length > maximumBytes - receivedBytes) {
            sinkError = "download exceeds the allowed size";
            return false;
        }
        size_t offset = 0;
        while (offset < length) {
            const ssize_t written = write(output, bytes + offset, length - offset);
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) {
                sinkError = "could not write staged app package";
                return false;
            }
            offset += static_cast<size_t>(written);
        }
        receivedBytes += static_cast<uint64_t>(length);
        const auto now = std::chrono::steady_clock::now();
        if (receivedBytes - reportedBytes >= 64 * 1024 ||
            now - lastProgress >= std::chrono::milliseconds(250)) {
            Json progress = Json::object();
            progress.set("type", Json::text("progress"));
            progress.set("bytes", Json::numeric(static_cast<double>(receivedBytes)));
            cacheNativeDownloadProgress();
            if (!writeRecord(progress)) {
                sinkError = "native app package download client disconnected";
                return false;
            }
            reportedBytes = receivedBytes;
            lastProgress = now;
        }
        return true;
    };
    long remoteStatus = 0;
    service::AppPackageDownloadFailure downloadFailure;
    const Json fetchConfig = config_.document["fetch"];
    const std::string ipPreference = fetchConfig["ip"].string();
    const std::shared_ptr<std::atomic<bool>> cancellation =
        std::make_shared<std::atomic<bool>>(false);
    {
        std::lock_guard<std::mutex> lock(appInstallDownloadsMutex_);
        appInstallDownloads_[jobId] = cancellation;
    }
    const service::AppPackageDownloadCancelled cancellationRequested =
        [cancellation]() { return cancellation->load(); };
    bool downloaded = appPackageFetcher_(urlValue.string(), "tvweb/" + version_,
                                         ipPreference, maximumBytes, sink, remoteStatus,
                                         downloadFailure, cancellationRequested);
    {
        std::lock_guard<std::mutex> lock(appInstallDownloadsMutex_);
        const auto active = appInstallDownloads_.find(jobId);
        if (active != appInstallDownloads_.end() && active->second == cancellation)
            appInstallDownloads_.erase(active);
    }
    if (cancellation->load()) {
        downloaded = false;
        downloadFailure.message = "cancelled";
    }
    if (!sinkError.empty()) downloadFailure.message = sinkError;
    if (downloaded && fsync(output) != 0) {
        downloadFailure.message = "could not flush staged app package";
        downloaded = false;
    }
    if (close(output) != 0 && downloaded) {
        downloadFailure.message = "could not close staged app package";
        downloaded = false;
    }
    const auto removeStagedFile = [&]() {
        (void)unlinkat(directory, fileName.c_str(), 0);
        (void)fsync(directory);
    };
    if (!downloaded) {
        removeStagedFile();
        if (exchange.responseStarted()) {
            Json failure = Json::object();
            failure.set("type", Json::text("error"));
            failure.set("error", Json::text(downloadFailure.message.empty()
                                                 ? "package download failed"
                                                 : downloadFailure.message));
            if (downloadFailure.offline)
                failure.set("offline", Json::flag(true));
            if (downloadFailure.timedOut)
                failure.set("timedOut", Json::flag(true));
            if (downloadFailure.message == "download exceeds the allowed size")
                failure.set("maxBytesExceeded", Json::flag(true));
            if (downloadFailure.message == "download exceeds the allowed size" &&
                capacity.spaceLimited) {
                failure.set("noSpace", Json::flag(true));
                if (capacity.freeBytesKnown)
                    failure.set("freeBytes",
                                Json::numeric(static_cast<double>(capacity.freeBytes)));
            }
            if (remoteStatus > 0)
                failure.set("remoteStatus", Json::numeric(remoteStatus));
            (void)writeRecord(failure);
            (void)exchange.finishResponse();
            close(directory);
            return;
        }
        Json failure = Json::object();
        failure.set("ok", Json::flag(false));
        failure.set("error", Json::text(downloadFailure.message.empty()
                                             ? "package download failed"
                                             : downloadFailure.message));
        if (downloadFailure.offline)
            failure.set("offline", Json::flag(true));
        if (downloadFailure.timedOut)
            failure.set("timedOut", Json::flag(true));
        std::vector<HttpHeader> headers = {
            {"Content-Type", "application/json; charset=utf-8"},
            {"Cache-Control", "no-store"},
            {"X-Content-Type-Options", "nosniff"}
        };
        if (remoteStatus > 0)
            headers.push_back({"X-NativeTv-Remote-Status", std::to_string(remoteStatus)});
        const bool maxBytesExceeded =
            downloadFailure.message == "download exceeds the allowed size";
        const bool noSpace = maxBytesExceeded && capacity.spaceLimited;
        if (maxBytesExceeded) failure.set("maxBytesExceeded", Json::flag(true));
        if (noSpace) {
            failure.set("noSpace", Json::flag(true));
            if (capacity.freeBytesKnown)
                failure.set("freeBytes",
                            Json::numeric(static_cast<double>(capacity.freeBytes)));
        }
        const int status = noSpace ? 507 : (maxBytesExceeded ? 413 : 502);
        (void)exchange.respond(status, headers, failure.dump());
        close(directory);
        return;
    }
    if (!reportProgress()) {
        removeStagedFile();
        close(directory);
        return;
    }
    Json done = Json::object();
    done.set("type", Json::text("done"));
    done.set("ok", Json::flag(true));
    done.set("bytes", Json::numeric(static_cast<double>(receivedBytes)));
    const bool sent = writeRecord(done) && exchange.finishResponse();
    if (!sent) removeStagedFile();
    close(directory);
}

void HttpFrontDoor::inspectNativeAppPackage(HttpExchange &exchange) const {
    const HttpRequest &request = exchange.request();
    if (!isLoopbackPeer(request.remoteAddress) || proxySecret_.empty() ||
        !tokenMatchesConstantTime(request.header("x-nativetv-proxy-secret"), proxySecret_) ||
        !request.header("x-nativetv-original-remote").empty()) {
        sendSetupError(exchange, 403, "native app package inspection required", true);
        return;
    }
    if (request.method != "POST") {
        sendSetupError(exchange, 405, "method not allowed");
        return;
    }
    if (!setupJsonContentType(request)) {
        sendSetupError(exchange, 415, "Content-Type must be application/json");
        return;
    }
    if (!config_.allowControl) {
        sendSetupError(exchange, 403, "controls disabled in config");
        return;
    }
    if (!exchange.setBodyLimit(8192)) {
        sendSetupError(exchange, 413, "request body too large", true);
        return;
    }
    std::string body;
    char buffer[1024];
    for (;;) {
        const int count = exchange.readBody(buffer, sizeof(buffer));
        if (count == 0) break;
        if (count < 0) {
            sendSetupError(exchange, exchange.bodyLimitExceeded() ? 413 : 400,
                           exchange.bodyLimitExceeded() ? "request body too large"
                                                        : "invalid request body",
                           true);
            return;
        }
        body.append(buffer, static_cast<size_t>(count));
    }
    const Json payload = Json::parse(body);
    if (!payload.isObject() || payload.keys().empty() || payload.keys().size() > 3 ||
        !payload["file"].isString() ||
        (payload.has("expectedPackageId") &&
         (!payload["expectedPackageId"].isString() ||
          payload["expectedPackageId"].string().empty() ||
          payload["expectedPackageId"].string().size() > 128)) ||
        (payload.has("expectedSha256") &&
         (!payload["expectedSha256"].isString() ||
          payload["expectedSha256"].string().size() != 64))) {
        sendSetupError(exchange, 400, "staged package file name expected");
        return;
    }
    for (const std::string &key : payload.keys()) {
        if (key != "file" && key != "expectedPackageId" && key != "expectedSha256") {
            sendSetupError(exchange, 400, "unsupported package inspection option");
            return;
        }
    }
    const char *configuredDirectory = std::getenv("TVWEB_INSTALL_STAGING");
    const std::string stagingDirectory = configuredDirectory && configuredDirectory[0]
        ? configuredDirectory : "/media/developer/temp/nativetv-install";
    Json metadata;
    std::string error;
    if (!service::AppPackageInspector::inspect(stagingDirectory,
                                                payload["file"].string(),
                                                metadata, error)) {
        Json failure = Json::object();
        failure.set("ok", Json::flag(false));
        failure.set("error", Json::text(error.empty()
            ? "the ipk could not be read" : error));
        (void)exchange.respond(200,
                               {{"Content-Type", "application/json; charset=utf-8"},
                                {"Cache-Control", "no-store"},
                                {"X-Content-Type-Options", "nosniff"}},
                               failure.dump());
        return;
    }
    service::AppPackagePolicyFailure policyFailure;
    if (!service::AppPackagePolicy::validate(
            metadata, service::AppPackagePolicy::systemAppDirectories(),
            policyFailure)) {
        Json failure = Json::object();
        failure.set("ok", Json::flag(false));
        failure.set("code", Json::text(policyFailure.code));
        failure.set("id", Json::text(policyFailure.id));
        failure.set("package", Json::text(policyFailure.package));
        (void)exchange.respond(200,
                               {{"Content-Type", "application/json; charset=utf-8"},
                                {"Cache-Control", "no-store"},
                                {"X-Content-Type-Options", "nosniff"}},
                               failure.dump());
        return;
    }
    if (!service::AppPackagePolicy::validateCatalogSelection(
            metadata, payload["expectedPackageId"].string(),
            payload["expectedSha256"].string(), policyFailure)) {
        Json failure = Json::object();
        failure.set("ok", Json::flag(false));
        failure.set("code", Json::text(policyFailure.code));
        failure.set("id", Json::text(policyFailure.id));
        failure.set("package", Json::text(policyFailure.package));
        if (!policyFailure.expected.empty())
            failure.set("expected", Json::text(policyFailure.expected));
        if (!policyFailure.actual.empty())
            failure.set("actual", Json::text(policyFailure.actual));
        (void)exchange.respond(200,
                               {{"Content-Type", "application/json; charset=utf-8"},
                                {"Cache-Control", "no-store"},
                                {"X-Content-Type-Options", "nosniff"}},
                               failure.dump());
        return;
    }
    Json result = Json::object();
    result.set("ok", Json::flag(true));
    result.set("info", metadata);
    sendMetricsJson(exchange, result.dump());
}

void HttpFrontDoor::buildAppInstallPreview(HttpExchange &exchange) const {
    const HttpRequest &request = exchange.request();
    if (!isLoopbackPeer(request.remoteAddress) || proxySecret_.empty() ||
        !tokenMatchesConstantTime(request.header("x-nativetv-proxy-secret"), proxySecret_) ||
        !request.header("x-nativetv-original-remote").empty()) {
        sendSetupError(exchange, 403, "native app install preview access required", true);
        return;
    }
    if (request.method != "POST") {
        sendSetupError(exchange, 405, "method not allowed");
        return;
    }
    if (!setupJsonContentType(request)) {
        sendSetupError(exchange, 415, "Content-Type must be application/json");
        return;
    }
    if (!config_.allowControl) {
        sendSetupError(exchange, 403, "controls disabled in config");
        return;
    }
    constexpr size_t kPreviewBodyLimit = 2 * 1024 * 1024;
    if (!exchange.setBodyLimit(kPreviewBodyLimit)) {
        sendSetupError(exchange, 413, "request body too large", true);
        return;
    }
    std::string body;
    char buffer[4096];
    for (;;) {
        const int count = exchange.readBody(buffer, sizeof(buffer));
        if (count == 0) break;
        if (count < 0) {
            sendSetupError(exchange, exchange.bodyLimitExceeded() ? 413 : 400,
                           exchange.bodyLimitExceeded() ? "request body too large"
                                                        : "invalid request body",
                           true);
            return;
        }
        body.append(buffer, static_cast<size_t>(count));
    }
    const Json input = Json::parse(body);
    if (!appInstallPreviewIdsAreSafe(input)) {
        sendSetupError(exchange, 400, "invalid installer preview input");
        return;
    }
    Json trustedInput = input;
    const char *configuredStaging = std::getenv("TVWEB_INSTALL_STAGING");
    const std::string stagingDirectory = configuredStaging && configuredStaging[0]
        ? configuredStaging : "/media/developer/temp/nativetv-install";
    uint64_t availableBytes = 0;
    if (service::AppInstallSpace::availableBytes(stagingDirectory, availableBytes))
        trustedInput.set("freeBytes",
                         Json::numeric(static_cast<double>(availableBytes)));
    else
        trustedInput.set("freeBytes", Json());
    const bool storeInstalled = appInstallStoreInstalled(input, systemRoot_);
    const bool elevationAvailable = appInstallElevationAvailable(systemRoot_);
    trustedInput.set("storeInstalled", Json::flag(storeInstalled));
    trustedInput.set("elevationAvailable", Json::flag(elevationAvailable));
    Json preview;
    bool needsReview = false;
    bool capacityOkay = false;
    std::string error;
    if (!service::AppInstallPreview::build(trustedInput, preview, needsReview,
                                           capacityOkay, error)) {
        sendSetupError(exchange, 400, error.empty()
            ? "invalid installer preview input" : error);
        return;
    }
    Json response = Json::object();
    response.set("ok", Json::flag(true));
    response.set("preview", preview);
    response.set("needsReview", Json::flag(needsReview));
    response.set("elevationAvailable", Json::flag(elevationAvailable));
    response.set("capacityOkay", Json::flag(capacityOkay));
    (void)exchange.respond(200,
                           {{"Content-Type", "application/json; charset=utf-8"},
                            {"Cache-Control", "no-store"},
                            {"X-Content-Type-Options", "nosniff"}},
                           response.dump());
}

void HttpFrontDoor::readAppInstallSpace(HttpExchange &exchange) const {
    const HttpRequest &request = exchange.request();
    if (!isLoopbackPeer(request.remoteAddress) || proxySecret_.empty() ||
        !tokenMatchesConstantTime(request.header("x-nativetv-proxy-secret"), proxySecret_) ||
        !request.header("x-nativetv-original-remote").empty()) {
        sendSetupError(exchange, 403, "native app install space access required", true);
        return;
    }
    if (request.method != "POST") {
        sendSetupError(exchange, 405, "method not allowed");
        return;
    }
    if (!setupJsonContentType(request)) {
        sendSetupError(exchange, 415, "Content-Type must be application/json");
        return;
    }
    if (!config_.allowControl) {
        sendSetupError(exchange, 403, "controls disabled in config");
        return;
    }
    Json payload;
    if (!readAppInstallJson(exchange, payload)) return;
    if (!payload.isObject() || !payload.keys().empty()) {
        sendSetupError(exchange, 400, "app install space request must be an empty object");
        return;
    }

    const char *configured = std::getenv("TVWEB_INSTALL_STAGING");
    const std::string path = configured && configured[0]
        ? configured : "/media/developer/temp/nativetv-install";
    Json response = Json::object();
    response.set("ok", Json::flag(true));
    uint64_t availableBytes = 0;
    if (service::AppInstallSpace::availableBytes(path, availableBytes))
        response.set("freeBytes", Json::numeric(static_cast<double>(availableBytes)));
    else
        response.set("freeBytes", Json());
    (void)exchange.respond(200,
                           {{"Content-Type", "application/json; charset=utf-8"},
                            {"Cache-Control", "no-store"},
                            {"X-Content-Type-Options", "nosniff"}},
                           response.dump());
}

bool HttpFrontDoor::nativeInstallEnabled() const {
    const char *flag = std::getenv("TVWEB_NATIVE_INSTALL");
    return flag && std::string(flag) == "1";
}

InstallJobs *HttpFrontDoor::installJobs() const {
    std::call_once(installJobsOnce_, [this]() {
        InstallJobsEnvironment environment;
        const char *staging = std::getenv("TVWEB_INSTALL_STAGING");
        environment.stagingDirectory = staging && staging[0]
            ? staging : "/media/developer/temp/nativetv-install";
        const char *lunaSend = std::getenv("TVWEB_LUNA_SEND");
        environment.lunaSendPath = lunaSend && lunaSend[0] ? lunaSend : "/usr/bin/luna-send";
        environment.systemRoot = systemRoot_;
        environment.jobPath = installJobPath_;
        environment.elevatedPath = elevatedAppsPath_;
        environment.userAgent = "tvweb/" + version_;
        environment.ipPreference = [this]() { return config_.document["fetch"]["ip"].string(); };
        environment.fetcher = appPackageFetcher_;
        environment.children = &children_;
        environment.luna = [this](const std::string &uri, const Json &payload, Json &reply) {
            LunaReply answer;
            const bool answered = waitForLuna(luna_, uri, payload, answer);
            reply = answered ? answer.response : Json();
            return answered;
        };
        environment.publish = [this](const Json &snapshot) {
            std::lock_guard<std::mutex> lock(installJobMutex_);
            appInstallSnapshot_ = snapshot;
            appInstallSnapshotReady_ = true;
        };
        // Node reads the app list again and refreshes the launcher.
        environment.onInstalled = [this]() {
            refreshLauncher();
            Json event = Json::object();
            event.set("type", Json::text("apps-changed"));
            std::string error;
            if (!proxy_.requestNodeState(event, error, 10000) && logger_)
                logger_->warning("app install: could not tell Node the app list changed: " + error);
        };
        environment.warn = [this](const std::string &text) {
            if (logger_) logger_->warning(text);
        };
        installJobs_.reset(new InstallJobs(productionInstallJobsDeps(environment)));
        installJobs_->recover();
    });
    return installJobs_.get();
}

bool HttpFrontDoor::startAppInstall(const Json &payload, int &status, Json &response,
                                    std::string &error) const {
    if (!nativeInstallEnabled())
        return proxy_.requestNodeAppInstallStart(payload, status, response, error);
    const InstallReply reply = installJobs()->start(payload);
    response = Json::object();
    response.set("ok", Json::flag(reply.ok));
    if (reply.ok) response.set("install", reply.install);
    else response.set("error", Json::text(reply.error));
    status = reply.ok ? 200 : 400;
    return true;
}

bool HttpFrontDoor::startAppUpload(const std::string &file, int &status, Json &response,
                                   std::string &error) const {
    if (!nativeInstallEnabled())
        return proxy_.requestNodeAppUploadStart(file, status, response, error);
    Json request = Json::object();
    request.set("source", Json::text("file"));
    request.set("file", Json::text(file));
    return startAppInstall(request, status, response, error);
}

bool HttpFrontDoor::runAppInstallAction(const std::string &action, const Json &payload,
                                        bool validated, int &status, Json &response,
                                        std::string &error) const {
    if (!nativeInstallEnabled())
        return proxy_.requestNodeAppInstallAction(action, payload, validated, status, response, error);
    response = Json::object();
    InstallReply reply;
    if (action == "confirm") {
        Json request = Json::object();
        request.set("jobId", payload["jobId"]);
        request.set("elevate", Json::flag(payload["elevate"].isBoolean() && payload["elevate"].boolean()));
        request.set("confirmRoot", payload["confirmRoot"]);
        request.set("replaceStore", Json::flag(payload["replaceStore"].isBoolean() &&
                                               payload["replaceStore"].boolean()));
        if (validated && payload["nativeServices"].isArray())
            request.set("nativeServices", payload["nativeServices"]);
        request.set("nativeValidated", Json::flag(validated));
        reply = installJobs()->confirm(request);
        if (reply.ok) response.set("install", reply.install);
    } else {
        reply = installJobs()->cancel(payload["jobId"].string());
    }
    response.set("ok", Json::flag(reply.ok));
    if (!reply.ok) response.set("error", Json::text(reply.error));
    status = reply.ok ? 200 : 400;
    return true;
}

void HttpFrontDoor::cacheAppInstallStartSnapshot(const Json &response) const {
    const Json snapshot = response["install"];
    if (!response["ok"].isBoolean() || !response["ok"].boolean() ||
        !snapshot.isObject() || !validateAppInstallStartedSnapshot(snapshot, nullptr))
        return;
    std::lock_guard<std::mutex> lock(installJobMutex_);
    const Json currentJobId = appInstallSnapshot_["jobId"];
    const Json incomingJobId = snapshot["jobId"];
    if (appInstallSnapshotReady_ && currentJobId.isString() &&
        incomingJobId.isString() && currentJobId.string() == incomingJobId.string())
        return;
    appInstallSnapshot_ = snapshot;
    appInstallSnapshotReady_ = true;
}

void HttpFrontDoor::persistHomeConfig(HttpExchange &exchange) const {
    const HttpRequest &request = exchange.request();
    if (!isLoopbackPeer(request.remoteAddress) || proxySecret_.empty() ||
        !tokenMatchesConstantTime(request.header("x-nativetv-proxy-secret"), proxySecret_) ||
        !request.header("x-nativetv-original-remote").empty()) {
        sendSetupError(exchange, 403, "native Home config persistence required", true);
        return;
    }
    if (request.method != "POST") {
        sendSetupError(exchange, 405, "method not allowed");
        return;
    }
    if (!setupJsonContentType(request)) {
        sendSetupError(exchange, 415, "Content-Type must be application/json");
        return;
    }
    const size_t maxBytes = 8 * 1024 * 1024;
    if (!exchange.setBodyLimit(maxBytes)) {
        sendSetupError(exchange, 413, "request body too large", true);
        return;
    }
    std::string body;
    char buffer[4096];
    for (;;) {
        const int count = exchange.readBody(buffer, sizeof(buffer));
        if (count == 0) break;
        if (count < 0) {
            sendSetupError(exchange, exchange.bodyLimitExceeded() ? 413 : 400,
                           exchange.bodyLimitExceeded() ? "request body too large"
                                                        : "invalid request body",
                           true);
            return;
        }
        body.append(buffer, static_cast<size_t>(count));
    }
    if (exceedsUtf16CodeUnitLimit(body, maxBytes)) {
        sendSetupError(exchange, 413, "request body too large", true);
        return;
    }
    const Json payload = Json::parse(body);
    if (!payload.isObject()) {
        sendSetupError(exchange, 400, "Home config object expected");
        return;
    }
    if (payload["mutation"].isString() &&
        payload["mutation"].string() == "patch" && !payload["body"].isObject()) {
        sendSetupError(exchange, 400, "Home config patch object expected");
        return;
    }
    std::string error;
    bool saved = false;
    Json response = Json::object();
    {
        std::lock_guard<std::mutex> lock(homeConfigMutex_);
        JsonFile file(dataFilePath(dataRoot_, "home.json"), maxBytes);
        if (payload["mutation"].isString()) {
            const std::string action = payload["mutation"].string();
            if (action != "layout" && action != "profile" && action != "patch" &&
                action != "hideLgHomeAtStart" && action != "openOnPowerOn" &&
                action != "enabled" && action != "restorePending" &&
                action != "lifecycle") {
                error = "unknown Home config mutation";
            } else {
                const JsonFileResult loaded = file.load();
                if (!loaded.found && !loaded.error.empty()) {
                    error = loaded.error;
                } else {
                    const Json current = loaded.found ? loaded.value : Json::object();
                    if (action == "patch") {
                        Json document = current.isObject() ? current : Json::object();
                        for (const std::string &key : payload["body"].keys())
                            document.set(key, payload["body"][key]);
                        saved = file.save(document, &error);
                        if (!saved && error.empty()) error = "could not save Home config";
                        if (saved) {
                            response.set("ok", Json::flag(true));
                            response.set("document", document);
                        }
                    } else {
                        const service::HomeDocumentUpdate update = action == "layout"
                            ? service::updateHomeLayout(current, payload["body"])
                            : action == "profile"
                                ? service::updateHomeProfile(current, payload["body"])
                                : service::updateHomeBooleanSetting(
                                      current, action, payload["body"]);
                        response.set("ok", Json::flag(true));
                        response.set("response", update.response);
                        response.set("document", update.document);
                        if (update.response["ok"].boolean()) {
                            saved = file.save(update.document, &error);
                            if (!saved && error.empty()) error = "could not save Home config";
                        }
                    }
                }
            }
        } else {
            saved = file.save(payload, &error);
            if (saved) {
                response.set("ok", Json::flag(true));
                response.set("document", payload);
            }
        }
    }
    if (!error.empty()) {
        sendSetupError(exchange, 500,
                       error);
        return;
    }
    if (!response["ok"].boolean()) {
        sendSetupError(exchange, 500, "could not save Home config");
        return;
    }
    (void)exchange.respond(200,
                           {{"Content-Type", "application/json; charset=utf-8"},
                            {"Cache-Control", "no-store"},
                            {"X-Content-Type-Options", "nosniff"}},
                           response.dump());
}

void HttpFrontDoor::handleHomeProfilePost(HttpExchange &exchange) const {
    const HttpRequest &request = exchange.request();
    const auto respond = [&exchange](int status, const Json &response) {
        (void)exchange.respond(status,
                               {{"Content-Type", "application/json; charset=utf-8"},
                                {"Cache-Control", "no-store"},
                                {"X-Content-Type-Options", "nosniff"}},
                               response.dump());
    };
    const auto failure = [&respond](const std::string &message, int status) {
        Json response = Json::object();
        response.set("ok", Json::flag(false));
        response.set("error", Json::text(message));
        respond(status, response);
    };
    if (!config_.allowControl && !isLoopbackPeer(request.remoteAddress)) {
        failure(say::msg("srv.controlsOff", "controls disabled in config"), 403);
        return;
    }
    if (rejectCrossSitePost(exchange)) return;
    if (!exchange.setBodyLimit(32768)) {
        failure("request body too large", 413);
        return;
    }
    std::string bodyText;
    char buffer[1024];
    for (;;) {
        const int count = exchange.readBody(buffer, sizeof(buffer));
        if (count == 0) break;
        if (count < 0) {
            const bool tooLarge = exchange.bodyLimitExceeded();
            failure(tooLarge ? "request body too large" : "malformed JSON",
                    tooLarge ? 413 : 400);
            return;
        }
        bodyText.append(buffer, static_cast<size_t>(count));
    }
    if (exceedsUtf16CodeUnitLimit(bodyText, 8192)) {
        failure("request body too large", 413);
        return;
    }
    const Json body = Json::parse(bodyText);
    if (!body.valid() || (!body.isObject() && !body.isArray())) {
        failure("malformed JSON", 400);
        return;
    }

    const size_t maximumBytes = 8 * 1024 * 1024;
    Json current;
    service::HomeDocumentUpdate update;
    std::string saveError;
    bool saved = false;
    {
        std::lock_guard<std::mutex> lock(homeConfigMutex_);
        JsonFile file(dataFilePath(dataRoot_, "home.json"), maximumBytes);
        const JsonFileResult stored = file.load();
        if (!stored.error.empty() && !stored.found) {
            saveError = stored.error;
        } else {
            current = service::normalizeHomeDocument(
                stored.found ? stored.value : Json::object());
            update = service::updateHomeProfile(current, body);
            if (update.response["ok"].boolean())
                saved = file.save(update.document, &saveError);
        }
    }
    if (!saveError.empty() || (update.response["ok"].boolean() && !saved)) {
        failure(say::msg("srv.home.saveFailed", "could not save the layout"), 400);
        return;
    }
    if (!update.response["ok"].boolean()) {
        const std::string error = update.response["error"].string();
        std::string key;
        std::string english;
        Json variables = Json::object();
        if (error == "a profile action is expected") {
            key = "srv.home.profileAction";
            english = error;
        } else if (error == "no such profile") {
            key = "srv.home.noProfile";
            english = error;
        } else if (error == "there can be 5 profiles at most") {
            key = "srv.home.profileLimit";
            english = "there can be {n} profiles at most";
            variables.set("n", Json::text("5"));
        } else if (error == "the last profile cannot be removed") {
            key = "srv.home.profileLast";
            english = error;
        } else {
            key = "srv.home.profileRename";
            english = "a name of up to 20 characters or one of the colours is expected";
        }
        failure(say::msg(key, english, variables), 400);
        return;
    }

    Json response = update.response;
    Json profiles = response["profiles"];
    const bool hadProfiles = current["profiles"].isArray() &&
                             current["profiles"].size() != 0;
    if (!hadProfiles) {
        Json localized = Json::array();
        for (Json profile : profiles.items()) {
            if (profile["id"].string() == "main")
                profile.set("name", Json::text(
                    say::msg("srv.home.profileMain", "Main")));
            localized.add(profile);
        }
        profiles = localized;
    }
    if (body["action"].string() == "add" &&
        service::normalizeHomeProfileName(body["name"]).empty()) {
        Json localized = Json::array();
        for (Json profile : profiles.items()) {
            bool existed = false;
            for (const Json &before : current["profiles"].items())
                if (before["id"].string() == profile["id"].string()) existed = true;
            if (!existed && profile["id"].string() != "main") {
                Json variables = Json::object();
                variables.set("n", Json::text(std::to_string(profiles.size())));
                profile.set("name", Json::text(say::msg(
                    "srv.home.profileNew", "Profile {n}", variables)));
            }
            localized.add(profile);
        }
        profiles = localized;
    }
    response.set("profiles", profiles);
    struct stat homeInfo = {};
    int64_t layoutVersion = 0;
    if (stat(dataFilePath(dataRoot_, "home.json").c_str(), &homeInfo) == 0) {
#if defined(__APPLE__)
        layoutVersion = static_cast<int64_t>(homeInfo.st_mtimespec.tv_sec) * 1000 +
                        homeInfo.st_mtimespec.tv_nsec / 1000000;
#else
        layoutVersion = static_cast<int64_t>(homeInfo.st_mtim.tv_sec) * 1000 +
                        homeInfo.st_mtim.tv_nsec / 1000000;
#endif
    }
    response.set("layoutVersion", Json::text(std::to_string(layoutVersion)));
    ensureHomePictureTracking();
    respond(200, response);
}

void HttpFrontDoor::handleHomeLayoutPost(HttpExchange &exchange) const {
    const HttpRequest &request = exchange.request();
    const auto respond = [&exchange](int status, const Json &response) {
        (void)exchange.respond(status,
                               {{"Content-Type", "application/json; charset=utf-8"},
                                {"Cache-Control", "no-store"},
                                {"X-Content-Type-Options", "nosniff"}},
                               response.dump());
    };
    const auto failure = [&respond](const std::string &message, int status) {
        Json response = Json::object();
        response.set("ok", Json::flag(false));
        response.set("error", Json::text(message));
        respond(status, response);
    };
    if (!config_.allowControl && !isLoopbackPeer(request.remoteAddress)) {
        failure(say::msg("srv.controlsOff", "controls disabled in config"), 403);
        return;
    }
    if (rejectCrossSitePost(exchange)) return;
    if (!exchange.setBodyLimit(32768)) {
        failure("request body too large", 413);
        return;
    }
    std::string bodyText;
    char buffer[1024];
    for (;;) {
        const int count = exchange.readBody(buffer, sizeof(buffer));
        if (count == 0) break;
        if (count < 0) {
            const bool tooLarge = exchange.bodyLimitExceeded();
            failure(tooLarge ? "request body too large" : "malformed JSON",
                    tooLarge ? 413 : 400);
            return;
        }
        bodyText.append(buffer, static_cast<size_t>(count));
    }
    if (exceedsUtf16CodeUnitLimit(bodyText, 8192)) {
        failure("request body too large", 413);
        return;
    }
    const Json body = Json::parse(bodyText);
    if (!body.valid() || (!body.isObject() && !body.isArray())) {
        failure("malformed JSON", 400);
        return;
    }

    Json response;
    Json replaceRows = Json::array();
    std::string saveError;
    bool saved = false;
    {
        std::lock_guard<std::mutex> lock(homeConfigMutex_);
        JsonFile file(dataFilePath(dataRoot_, "home.json"), 8 * 1024 * 1024);
        const JsonFileResult stored = file.load();
        if (!stored.error.empty() && !stored.found) {
            saveError = stored.error;
        } else {
            const Json current = service::normalizeHomeDocument(
                stored.found ? stored.value : Json::object());
            const service::HomeDocumentUpdate update =
                service::updateHomeLayout(current, body);
            response = update.response;
            if (response["ok"].boolean()) {
                saved = file.save(update.document, &saveError);
                if (saved) {
                    for (const char *row : {"app", "dev", "ch"})
                        if (update.document["sort"][row].string() != "recent")
                            replaceRows.add(Json::text(row));
                }
            }
        }
    }
    if (!saveError.empty() || (!response.valid() && !saved)) {
        failure(say::msg("srv.home.saveFailed", "could not save the layout"), 200);
        return;
    }
    if (!response["ok"].boolean()) {
        respond(200, response);
        return;
    }
    if (!saved) {
        failure(say::msg("srv.home.saveFailed", "could not save the layout"), 200);
        return;
    }

    struct stat homeInfo = {};
    int64_t layoutVersion = 0;
    if (stat(dataFilePath(dataRoot_, "home.json").c_str(), &homeInfo) == 0) {
#if defined(__APPLE__)
        layoutVersion = static_cast<int64_t>(homeInfo.st_mtimespec.tv_sec) * 1000 +
                        homeInfo.st_mtimespec.tv_nsec / 1000000;
#else
        layoutVersion = static_cast<int64_t>(homeInfo.st_mtim.tv_sec) * 1000 +
                        homeInfo.st_mtim.tv_nsec / 1000000;
#endif
    }
    response.set("layoutVersion", Json::text(std::to_string(layoutVersion)));
    ensureHomePictureTracking();
    if (homeRecentHistoryState_)
        clearHomeRecentHistoryRows(*homeRecentHistoryState_, replaceRows);
    respond(200, response);
}

void HttpFrontDoor::handleHomeStartLayoutPost(HttpExchange &exchange) const {
    const HttpRequest &request = exchange.request();
    const auto respond = [&exchange](int status, const Json &response) {
        (void)exchange.respond(status,
                               {{"Content-Type", "application/json; charset=utf-8"},
                                {"Cache-Control", "no-store"},
                                {"X-Content-Type-Options", "nosniff"}},
                               response.dump());
    };
    const auto failure = [&respond](const std::string &message, int status) {
        Json response = Json::object();
        response.set("ok", Json::flag(false));
        response.set("error", Json::text(message));
        respond(status, response);
    };
    if (!config_.allowControl && !isLoopbackPeer(request.remoteAddress)) {
        failure(say::msg("srv.controlsOff", "controls disabled in config"), 403);
        return;
    }
    if (rejectCrossSitePost(exchange)) return;
    if (!exchange.setBodyLimit(32768)) {
        failure("request body too large", 413);
        return;
    }
    std::string bodyText;
    char buffer[1024];
    for (;;) {
        const int count = exchange.readBody(buffer, sizeof(buffer));
        if (count == 0) break;
        if (count < 0) {
            const bool tooLarge = exchange.bodyLimitExceeded();
            failure(tooLarge ? "request body too large" : "malformed JSON",
                    tooLarge ? 413 : 400);
            return;
        }
        bodyText.append(buffer, static_cast<size_t>(count));
    }
    if (exceedsUtf16CodeUnitLimit(bodyText, 8192)) {
        failure("request body too large", 413);
        return;
    }
    const Json body = Json::parse(bodyText);
    if (!body.valid() || (!body.isObject() && !body.isArray())) {
        failure("malformed JSON", 400);
        return;
    }
    if (!body["hideLgHomeAtStart"].isBoolean()) {
        failure("hideLgHomeAtStart expected", 200);
        return;
    }
    std::lock_guard<std::mutex> operationLock(homeOperationMutex_);
    const std::string webos = homeStatusWebosVersion(systemRoot_);
    if (webos.compare(0, 2, "9.") != 0) {
        failure(say::msg("srv.home.startLayoutWebos9",
                         "LG's Home can be hidden at start on webOS 9 only."), 200);
        return;
    }
    if (!homeScreenManager_) {
        failure("native Home screen manager is unavailable", 503);
        return;
    }

    Json previous;
    std::string saveError;
    bool saved = false;
    {
        std::lock_guard<std::mutex> lock(homeConfigMutex_);
        JsonFile file(dataFilePath(dataRoot_, "home.json"), 8 * 1024 * 1024);
        const JsonFileResult stored = file.load();
        if (!stored.error.empty() && !stored.found) {
            saveError = stored.error;
        } else {
            previous = service::normalizeHomeDocument(
                stored.found ? stored.value : Json::object());
            const service::HomeDocumentUpdate update =
                service::updateHomeBooleanSetting(previous, "hideLgHomeAtStart", body);
            if (!update.response["ok"].boolean()) {
                respond(200, update.response);
                return;
            }
            saved = file.save(update.document, &saveError);
        }
    }
    if (!saveError.empty() || !saved) {
        failure(say::msg("srv.home.saveFailed", "could not save the layout"), 200);
        return;
    }

    const bool mounted = homeTakeoverMounted(systemRoot_, dataRoot_);
    if (mounted) {
        const Json operation = homeScreenManager_->startLayout(
            body["hideLgHomeAtStart"].boolean());
        if (!operation["ok"].boolean()) {
            std::string restoreError;
            {
                std::lock_guard<std::mutex> lock(homeConfigMutex_);
                JsonFile file(dataFilePath(dataRoot_, "home.json"), 8 * 1024 * 1024);
                const JsonFileResult stored = file.load();
                if (!stored.error.empty() && !stored.found) {
                    restoreError = stored.error;
                } else {
                    Json rollback = service::normalizeHomeDocument(
                        stored.found ? stored.value : Json::object());
                    rollback = Json::parse(rollback.dump());
                    rollback.set("hideLgHomeAtStart",
                                 previous["hideLgHomeAtStart"].isBoolean()
                                     ? previous["hideLgHomeAtStart"] : Json::parse("null"));
                    rollback = service::normalizeHomeDocument(rollback);
                    (void)file.save(rollback, &restoreError);
                }
            }
            std::string message = say::msg(
                "srv.home.startLayoutFailed",
                "the TV would not change LG's Home's layout");
            if (operation["error"].isString())
                message += " (" + operation["error"].string() + ")";
            failure(message, 200);
            return;
        }
    }
    Json response = Json::object();
    response.set("ok", Json::flag(true));
    respond(200, response);
}

void HttpFrontDoor::handleHomeStartupPost(HttpExchange &exchange) const {
    const HttpRequest &request = exchange.request();
    const auto respond = [&exchange](int status, const Json &response) {
        (void)exchange.respond(status,
                               {{"Content-Type", "application/json; charset=utf-8"},
                                {"Cache-Control", "no-store"},
                                {"X-Content-Type-Options", "nosniff"}},
                               response.dump());
    };
    const auto failure = [&respond](const std::string &message, int status) {
        Json response = Json::object();
        response.set("ok", Json::flag(false));
        response.set("error", Json::text(message));
        respond(status, response);
    };
    if (!config_.allowControl && !isLoopbackPeer(request.remoteAddress)) {
        failure(say::msg("srv.controlsOff", "controls disabled in config"), 403);
        return;
    }
    if (rejectCrossSitePost(exchange)) return;
    if (!exchange.setBodyLimit(32768)) {
        failure("request body too large", 413);
        return;
    }
    std::string bodyText;
    char buffer[1024];
    for (;;) {
        const int count = exchange.readBody(buffer, sizeof(buffer));
        if (count == 0) break;
        if (count < 0) {
            const bool tooLarge = exchange.bodyLimitExceeded();
            failure(tooLarge ? "request body too large" : "malformed JSON",
                    tooLarge ? 413 : 400);
            return;
        }
        bodyText.append(buffer, static_cast<size_t>(count));
    }
    if (exceedsUtf16CodeUnitLimit(bodyText, 8192)) {
        failure("request body too large", 413);
        return;
    }
    const Json body = Json::parse(bodyText);
    if (!body.valid() || (!body.isObject() && !body.isArray())) {
        failure("malformed JSON", 400);
        return;
    }
    if (!body["openOnPowerOn"].isBoolean()) {
        failure("openOnPowerOn expected", 200);
        return;
    }

    std::lock_guard<std::mutex> operationLock(homeOperationMutex_);

    const std::string webos = homeStatusWebosVersion(systemRoot_);
    const Json supported = service::homeStatusSupported(
        webos, config_.document["allowExperimentalHome"].boolean());
    if (!supported["ok"].boolean()) {
        failure(supported["reason"].string(), 200);
        return;
    }

    Json previous;
    Json updated;
    std::string saveError;
    bool saved = false;
    {
        std::lock_guard<std::mutex> lock(homeConfigMutex_);
        JsonFile file(dataFilePath(dataRoot_, "home.json"), 8 * 1024 * 1024);
        const JsonFileResult stored = file.load();
        if (!stored.error.empty() && !stored.found) {
            saveError = stored.error;
        } else {
            previous = service::normalizeHomeDocument(
                stored.found ? stored.value : Json::object());
            const service::HomeDocumentUpdate mutation =
                service::updateHomeBooleanSetting(
                    previous, "openOnPowerOn", body);
            if (!mutation.response["ok"].boolean()) {
                respond(200, mutation.response);
                return;
            }
            updated = mutation.document;
            saved = file.save(updated, &saveError);
        }
    }
    if (!saveError.empty() || !saved) {
        failure(say::msg("srv.home.saveFailed", "could not save the layout"), 200);
        return;
    }

    if (!updateHomeStartingNotice(updated) && logger_)
        logger_->warning("home: could not update the Home startup notice");

    const Json autoLaunch = synchronizeHomeAutoLaunch(false);
    if (!autoLaunch["ok"].boolean()) {
        {
            std::lock_guard<std::mutex> lock(homeConfigMutex_);
            JsonFile file(dataFilePath(dataRoot_, "home.json"), 8 * 1024 * 1024);
            const JsonFileResult stored = file.load();
            if (stored.error.empty() || stored.found) {
                Json rollback = service::normalizeHomeDocument(
                    stored.found ? stored.value : Json::object());
                rollback = Json::parse(rollback.dump());
                rollback.set("openOnPowerOn", previous["openOnPowerOn"]);
                std::string rollbackError;
                if (!file.save(service::normalizeHomeDocument(rollback),
                               &rollbackError) && logger_)
                    logger_->error("home: could not restore the saved power-on Home setting" +
                        (rollbackError.empty() ? std::string() : ": " + rollbackError));
            } else if (logger_) {
                logger_->error("home: could not read Home config to restore the power-on setting: " +
                               stored.error);
            }
        }
        Json restored;
        {
            std::lock_guard<std::mutex> lock(homeConfigMutex_);
            JsonFile file(dataFilePath(dataRoot_, "home.json"), 8 * 1024 * 1024);
            const JsonFileResult stored = file.load();
            restored = service::normalizeHomeDocument(
                stored.found ? stored.value : previous);
        }
        if (!updateHomeStartingNotice(restored) && logger_)
            logger_->warning("home: could not restore the Home startup notice");
        (void)synchronizeHomeAutoLaunch(false);
        failure(say::msg("srv.home.startupFailed",
                         "the TV would not change the power-on Home setting"), 200);
        return;
    }

    Json response = Json::object();
    response.set("ok", Json::flag(true));
    respond(200, response);
}

bool HttpFrontDoor::updateHomeStartingNotice(const Json &home) const {
    const bool enabled = home["enabled"].boolean();
    const std::string starting = enabled
        ? say::msg("srv.home.startingToast",
                   "nativeTv Home is starting. It opens in a few seconds.")
        : std::string();
    const bool splash = !starting.empty() &&
        (home["openOnPowerOn"].boolean() ||
         home["lgAutoLaunch"].string() == "on");
    const std::string mapPath = dataFilePath(dataRoot_, "remotekeys.json");
    const std::string filterPath = dataFilePath(dataRoot_, "remotekeys-filter.js");
    JsonFile mapStore(mapPath, 64 * 1024);
    const JsonFileResult loaded = mapStore.load();
    if (!loaded.error.empty() && !loaded.found) return false;

    const Json old = loaded.found && loaded.value.isObject()
        ? loaded.value : Json::object();
    const Json keys = old["keys"].isObject() ? old["keys"] : Json::object();
    const std::string homeKey = old["home"].string();
    const bool hadRemoteState = keys.size() != 0 || !homeKey.empty() ||
        (old["starting"].isString() && !old["starting"].string().empty());
    const bool filterExists = access(filterPath.c_str(), F_OK) == 0;
    const bool hasRemoteState = keys.size() != 0 || !homeKey.empty() ||
        !starting.empty();

    std::string fileError;
    if (!hasRemoteState) {
        if (unlink(mapPath.c_str()) != 0 && errno != ENOENT) return false;
        if (unlink((mapPath + ".last-good").c_str()) != 0 && errno != ENOENT)
            return false;
    } else {
        Json document = Json::object();
        document.set("keys", keys);
        document.set("updated", Json::text(utcIsoTimestamp()));
        if (!homeKey.empty()) document.set("home", Json::text(homeKey));
        if (!starting.empty()) document.set("starting", Json::text(starting));
        if (!starting.empty() && splash)
            document.set("splash", Json::flag(true));
        if (!mapStore.save(document, &fileError)) return false;
    }

    const auto syncEarlyService = [this, &starting]() {
        std::string error;
        if (!syncHomeEarlyService(!starting.empty(), &error) && logger_)
            logger_->warning("home: could not synchronize early boot service" +
                (error.empty() ? std::string() : ": " + error));
    };
    if (!hasRemoteState && !hadRemoteState && !filterExists) {
        syncEarlyService();
        return true;
    }

    Json configValue = Json::object();
    configValue.set("keys", keys);
    if (!homeKey.empty()) configValue.set("home", Json::text(homeKey));
    if (!starting.empty()) configValue.set("starting", Json::text(starting));
    if (!starting.empty() && splash)
        configValue.set("splash", Json::flag(true));
    Json mapConfigs = Json::object();
    mapConfigs.set("glasshouse.remoteKeys", configValue);
    const CommandPolicyContext policyContext = controlPolicyContext(config_);
    const auto writeConfigs = [this, &policyContext](
        const Json &configs, const std::string &action) {
        Json payload = Json::object();
        payload.set("configs", configs);
        LunaReply reply;
        return waitForCommandPolicyLuna(
                   luna_, commandPolicy_, policyContext, action,
                   "com.webos.service.config/setConfigs", payload,
                   "remote-keys", false, reply, std::string(), false,
                   std::chrono::milliseconds(5000), false) &&
               reply.response["returnValue"].boolean();
    };

    const bool javascriptFilters = service::hasJavaScriptRemoteKeys(systemRoot_);
    Json rest = Json::array();
    bool inChain = false;
    bool filterChanged = false;
    if (javascriptFilters) {
        Json query = Json::object();
        Json names = Json::array();
        names.add(Json::text("com.webos.surfacemanager.keyFilters"));
        query.set("configNames", names);
        LunaReply chainReply;
        if (!waitForCommandPolicyLuna(
                luna_, commandPolicy_, policyContext,
                "remoteKeys.startup-read-chain",
                "com.webos.service.config/getConfigs", query,
                "remote-keys", false, chainReply, std::string(), false,
                std::chrono::milliseconds(5000), false) ||
            !chainReply.response["configs"][
                "com.webos.surfacemanager.keyFilters"].isArray())
            return false;
        const Json chain = chainReply.response["configs"][
            "com.webos.surfacemanager.keyFilters"];
        for (const Json &entry : chain.items()) {
            if (entry["file"].string() == filterPath) {
                inChain = true;
                continue;
            }
            rest.add(entry);
        }
        if (!hasRemoteState && !inChain) return true;

        std::string assetError;
        if (!service::installRemoteKeysFilterAsset(
                dataFilePath(dataRoot_, "native/remotekeys-filter.js"),
                filterPath, filterChanged, &assetError)) {
            if (logger_)
                logger_->warning("home: remote-key filter unavailable" +
                    (assetError.empty() ? std::string() : ": " + assetError));
            return false;
        }
    }

    syncEarlyService();
    if (!writeConfigs(mapConfigs, "remoteKeys.startup-map")) return false;
    if (!javascriptFilters) return true;

    Json entry = Json::object();
    entry.set("file", Json::text(filterPath));
    entry.set("handler", Json::text("handleNativeTvKeys"));
    Json full = Json::array();
    full.add(entry);
    for (const Json &item : rest.items()) full.add(item);
    if (!inChain) {
        Json chainConfigs = Json::object();
        chainConfigs.set("com.webos.surfacemanager.keyFilters", full);
        return writeConfigs(chainConfigs, "remoteKeys.startup-chain");
    }
    if (!filterChanged) return true;

    Json removeConfigs = Json::object();
    removeConfigs.set("com.webos.surfacemanager.keyFilters", rest);
    if (!writeConfigs(removeConfigs, "remoteKeys.startup-chain-remove"))
        return false;
    Json addConfigs = Json::object();
    addConfigs.set("com.webos.surfacemanager.keyFilters", full);
    return writeConfigs(addConfigs, "remoteKeys.startup-chain-add");
}

bool HttpFrontDoor::syncHomeEarlyService(bool hasStartingNotice,
                                         std::string *error) const {
    if (error) error->clear();
    std::vector<std::string> disabledServices;
    if (!service::readDisabledBackgroundServices(dataRoot_, disabledServices,
                                                  error))
        return false;

    const bool wanted = hasStartingNotice || !disabledServices.empty();
    const std::string homeFlag = rootedSystemPath(
        systemRoot_, "/var/lib/tvweb/early-home");
    if (wanted) {
        const int fd = open(homeFlag.c_str(),
                            O_WRONLY | O_CREAT | O_CLOEXEC | O_NOFOLLOW,
                            0644);
        if (fd < 0) {
            if (error) *error = "could not create the early Home flag";
            return false;
        }
        struct stat info = {};
        const bool valid = fstat(fd, &info) == 0 && S_ISREG(info.st_mode) &&
                           fchmod(fd, 0644) == 0;
        const int closeResult = close(fd);
        if (!valid || closeResult != 0) {
            if (error) *error = "could not write the early Home flag";
            return false;
        }
    } else if (unlink(homeFlag.c_str()) != 0 && errno != ENOENT) {
        if (error) *error = "could not remove the early Home flag";
        return false;
    }

    const std::string script = assetsRoot_ + "/early-service/install.sh";
    if (access(script.c_str(), R_OK) != 0) return true;
    const std::string manifest = rootedSystemPath(
        systemRoot_,
        "/var/luna-service2-dev/manifests.d/com.webos.glasshouse.boot.json");
    if (!wanted && access(manifest.c_str(), F_OK) != 0) return true;

    const std::string shell = findSystemExecutable(
        systemRoot_, {"/bin/sh", "/usr/bin/sh"});
    if (shell.empty()) {
        if (error) *error = "system shell is unavailable";
        return false;
    }
    ChildOptions options;
    options.arguments = {shell, script, wanted ? "install" : "remove"};
    if (wanted) {
        const size_t slash = script.find_last_of('/');
        options.arguments.push_back(slash == std::string::npos
            ? std::string(".") : script.substr(0, slash));
    }
    options.timeout = std::chrono::milliseconds(30000);
    options.maximumOutputBytes = 16 * 1024;
    const ChildResult result = children_.run(options);
    if (!result.succeeded()) {
        if (error) *error = result.error.empty()
            ? "early service installer failed" : result.error;
        return false;
    }
    return true;
}

void HttpFrontDoor::handleHomeEnablePost(HttpExchange &exchange) const {
    const HttpRequest &request = exchange.request();
    const auto respond = [&exchange](int status, const Json &response) {
        (void)exchange.respond(status,
                               {{"Content-Type", "application/json; charset=utf-8"},
                                {"Cache-Control", "no-store"},
                                {"X-Content-Type-Options", "nosniff"}},
                               response.dump());
    };
    const auto failure = [&respond](const std::string &message, int status) {
        Json response = Json::object();
        response.set("ok", Json::flag(false));
        response.set("error", Json::text(message));
        respond(status, response);
    };
    if (!config_.allowControl && !isLoopbackPeer(request.remoteAddress)) {
        failure(say::msg("srv.controlsOff", "controls disabled in config"), 403);
        return;
    }
    if (rejectCrossSitePost(exchange)) return;
    if (!exchange.setBodyLimit(32768)) {
        failure("request body too large", 413);
        return;
    }
    std::string bodyText;
    char buffer[1024];
    for (;;) {
        const int count = exchange.readBody(buffer, sizeof(buffer));
        if (count == 0) break;
        if (count < 0) {
            const bool tooLarge = exchange.bodyLimitExceeded();
            failure(tooLarge ? "request body too large" : "malformed JSON",
                    tooLarge ? 413 : 400);
            return;
        }
        bodyText.append(buffer, static_cast<size_t>(count));
    }
    if (exceedsUtf16CodeUnitLimit(bodyText, 8192)) {
        failure("request body too large", 413);
        return;
    }
    const Json body = Json::parse(bodyText);
    if (!body.valid() || (!body.isObject() && !body.isArray())) {
        failure("malformed JSON", 400);
        return;
    }

    std::lock_guard<std::mutex> operationLock(homeOperationMutex_);
    const std::string webos = homeStatusWebosVersion(systemRoot_);
    // Switching Home on runs the native client's probe afresh: a client or a
    // firmware updated since the last verdict may have changed it.
    const Json supported = homeSupported(true);
    if (!supported["ok"].boolean()) {
        failure(supported["reason"].string(), 200);
        return;
    }
    if (!homeTakeoverLifecycleManager_ || !homeScreenManager_) {
        failure("native Home takeover is unavailable", 503);
        return;
    }

    Json previous;
    {
        std::lock_guard<std::mutex> lock(homeConfigMutex_);
        JsonFile file(dataFilePath(dataRoot_, "home.json"), 8 * 1024 * 1024);
        const JsonFileResult stored = file.load();
        if (!stored.error.empty() && !stored.found) {
            failure(stored.error, 500);
            return;
        }
        previous = service::normalizeHomeDocument(
            stored.found ? stored.value : Json::object());
    }
    if (previous["restorePending"].boolean()) {
        failure(say::msg(
                    "srv.home.restoreFirst",
                    "LG's Home is not fully back yet; switch nativeTv Home off again first"),
                200);
        return;
    }

    if (homeEscapeActive_) homeEscapeActive_->store(false);
    if (homeEscapeReturnPending_) homeEscapeReturnPending_->store(false);
    const bool blankStartLayout = webos.compare(0, 2, "9.") == 0 &&
                                  previous["hideLgHomeAtStart"].boolean();
    const CommandPolicyContext policyContext = controlPolicyContext(config_);
    const Json takeover = homeTakeoverLifecycleManager_->takeover(
        blankStartLayout, policyContext);
    if (!takeover["ok"].boolean()) {
        std::string message = say::msg(
            "srv.home.setFailed", "the TV would not make nativeTv Home its Home");
        if (takeover["error"].isString())
            message += " (" + takeover["error"].string() + ")";
        failure(message, 200);
        return;
    }

    Json enabledBody = Json::object();
    enabledBody.set("enabled", Json::flag(true));
    bool saved = false;
    std::string saveError;
    Json updated;
    {
        std::lock_guard<std::mutex> lock(homeConfigMutex_);
        JsonFile file(dataFilePath(dataRoot_, "home.json"), 8 * 1024 * 1024);
        const JsonFileResult stored = file.load();
        if (!stored.error.empty() && !stored.found) {
            saveError = stored.error;
        } else {
            const Json current = service::normalizeHomeDocument(
                stored.found ? stored.value : Json::object());
            const service::HomeDocumentUpdate mutation =
                service::updateHomeBooleanSetting(current, "enabled", enabledBody);
            if (!mutation.response["ok"].boolean()) {
                saveError = mutation.response["error"].string();
            } else {
                updated = mutation.document;
                saved = file.save(updated, &saveError);
            }
        }
    }
    if (!saved) {
        const Json released = homeTakeoverLifecycleManager_->release(policyContext);
        if (!released["ok"].boolean() && logger_)
            logger_->error("home: takeover rollback failed after config save error: " +
                           released["error"].string());
        failure(say::msg("srv.home.saveFailed", "could not save the layout"), 200);
        return;
    }

    if (!updateHomeStartingNotice(updated) && logger_)
        logger_->warning("home: could not update the Home startup notice");
    const Json autoLaunch = synchronizeHomeAutoLaunch(false);
    if (!autoLaunch["ok"].boolean() && logger_)
        logger_->warning("home: could not synchronize LG Home auto-launch: " +
                         autoLaunch["error"].string());
    ensureHomePictureTracking();
    // The crash watch starts over: a Home given back after the client kept
    // failing is wanted again.
    if (HomeOrchestrator *orchestrator = homeOrchestrator()) orchestrator->rearm();
    if (logger_) logger_->info("home: nativeTv Home switched on");
    Json response = Json::object();
    response.set("ok", Json::flag(true));
    respond(200, response);
}

void HttpFrontDoor::handleHomeDisablePost(HttpExchange &exchange) const {
    const HttpRequest &request = exchange.request();
    const auto respond = [&exchange](int status, const Json &response) {
        (void)exchange.respond(status,
                               {{"Content-Type", "application/json; charset=utf-8"},
                                {"Cache-Control", "no-store"},
                                {"X-Content-Type-Options", "nosniff"}},
                               response.dump());
    };
    const auto failure = [&respond](const std::string &message, int status) {
        Json response = Json::object();
        response.set("ok", Json::flag(false));
        response.set("error", Json::text(message));
        respond(status, response);
    };
    if (!config_.allowControl && !isLoopbackPeer(request.remoteAddress)) {
        failure(say::msg("srv.controlsOff", "controls disabled in config"), 403);
        return;
    }
    if (rejectCrossSitePost(exchange)) return;
    if (!exchange.setBodyLimit(32768)) {
        failure("request body too large", 413);
        return;
    }
    std::string bodyText;
    char buffer[1024];
    for (;;) {
        const int count = exchange.readBody(buffer, sizeof(buffer));
        if (count == 0) break;
        if (count < 0) {
            const bool tooLarge = exchange.bodyLimitExceeded();
            failure(tooLarge ? "request body too large" : "malformed JSON",
                    tooLarge ? 413 : 400);
            return;
        }
        bodyText.append(buffer, static_cast<size_t>(count));
    }
    if (exceedsUtf16CodeUnitLimit(bodyText, 8192)) {
        failure("request body too large", 413);
        return;
    }
    const Json body = Json::parse(bodyText);
    if (!body.valid() || (!body.isObject() && !body.isArray())) {
        failure("malformed JSON", 400);
        return;
    }

    std::lock_guard<std::mutex> operationLock(homeOperationMutex_);
    if (!homeTakeoverLifecycleManager_ || !commandPolicy_) {
        failure("native Home takeover is unavailable", 503);
        return;
    }

    Json disabledBody = Json::object();
    disabledBody.set("enabled", Json::flag(false));
    disabledBody.set("restorePending", Json::flag(true));
    std::string saveError;
    bool saved = false;
    {
        std::lock_guard<std::mutex> lock(homeConfigMutex_);
        JsonFile file(dataFilePath(dataRoot_, "home.json"), 8 * 1024 * 1024);
        const JsonFileResult stored = file.load();
        if (!stored.error.empty() && !stored.found) {
            saveError = stored.error;
        } else {
            const Json current = service::normalizeHomeDocument(
                stored.found ? stored.value : Json::object());
            const service::HomeDocumentUpdate mutation =
                service::updateHomeBooleanSetting(current, "lifecycle", disabledBody);
            if (!mutation.response["ok"].boolean()) {
                saveError = mutation.response["error"].string();
            } else {
                saved = file.save(mutation.document, &saveError);
            }
        }
    }
    if (!saved) {
        failure(say::msg("srv.home.saveFailed", "could not save the layout"), 200);
        return;
    }
    if (homeEscapeActive_) homeEscapeActive_->store(false);
    if (homeEscapeReturnPending_) homeEscapeReturnPending_->store(false);
    ensureHomePictureTracking();

    CommandPolicyContext policyContext = controlPolicyContext(config_);
    HomeRestoreManager restore(*homeTakeoverLifecycleManager_, *commandPolicy_,
                               dataRoot_);
    const Json result = restore.restore(policyContext);
    if (!result["ok"].boolean()) {
        std::string message = say::msg(
            "srv.home.restoreFailed",
            "the TV would not take its earlier Home settings back");
        if (result["error"].isString())
            message += " (" + result["error"].string() + ")";
        failure(message, 200);
        return;
    }

    std::string earlyServiceError;
    if (!syncHomeEarlyService(false, &earlyServiceError) && logger_)
        logger_->warning("home: could not synchronize early boot service" +
            (earlyServiceError.empty() ? std::string()
                                       : ": " + earlyServiceError));

    if (HomeOrchestrator *orchestrator = homeOrchestrator()) orchestrator->rearm();
    if (logger_) logger_->info("home: nativeTv Home switched off");
    Json response = Json::object();
    response.set("ok", Json::flag(true));
    respond(200, response);
}

void HttpFrontDoor::persistHomeRecent(HttpExchange &exchange) const {
    const HttpRequest &request = exchange.request();
    if (!isLoopbackPeer(request.remoteAddress) || proxySecret_.empty() ||
        !tokenMatchesConstantTime(request.header("x-nativetv-proxy-secret"), proxySecret_) ||
        !request.header("x-nativetv-original-remote").empty()) {
        sendSetupError(exchange, 403, "native Home history persistence required", true);
        return;
    }
    if (request.method != "POST") {
        sendSetupError(exchange, 405, "method not allowed");
        return;
    }
    if (!setupJsonContentType(request)) {
        sendSetupError(exchange, 415, "Content-Type must be application/json");
        return;
    }
    const size_t maxBytes = 512 * 1024;
    if (!exchange.setBodyLimit(maxBytes)) {
        sendSetupError(exchange, 413, "request body too large", true);
        return;
    }
    std::string body;
    char buffer[4096];
    for (;;) {
        const int count = exchange.readBody(buffer, sizeof(buffer));
        if (count == 0) break;
        if (count < 0) {
            sendSetupError(exchange, exchange.bodyLimitExceeded() ? 413 : 400,
                           exchange.bodyLimitExceeded() ? "request body too large"
                                                        : "invalid request body",
                           true);
            return;
        }
        body.append(buffer, static_cast<size_t>(count));
    }
    if (exceedsUtf16CodeUnitLimit(body, maxBytes)) {
        sendSetupError(exchange, 413, "request body too large", true);
        return;
    }
    const Json payload = Json::parse(body);
    if (!payload.isObject()) {
        sendSetupError(exchange, 400, "Home recent-history object expected");
        return;
    }

    Json history = payload["history"];
    Json replaceRows = payload["replaceRows"];
    if (payload.has("history") && !history.isObject()) {
        sendSetupError(exchange, 400, "Home recent-history snapshot expected");
        return;
    }
    if (!payload.has("history")) {
        // Accept the original snapshot shape from an older Node process. Its
        // semantics replaced all rows, so mark each row as an explicit reset.
        history = payload;
        replaceRows = Json::array();
        replaceRows.add(Json::text("app"));
        replaceRows.add(Json::text("dev"));
        replaceRows.add(Json::text("ch"));
    } else if (payload.has("replaceRows") && !replaceRows.isArray()) {
        sendSetupError(exchange, 400, "Home recent-history replacement rows expected");
        return;
    }

    Json cleanRows = Json::array();
    for (const Json &row : replaceRows.items()) {
        const std::string name = row.string();
        if (!row.isString() ||
            (name != "app" && name != "dev" && name != "ch")) {
            sendSetupError(exchange, 400, "invalid Home recent-history replacement row");
            return;
        }
        cleanRows.add(row);
    }

    Json incoming = Json::object();
    const char *rows[] = {"app", "dev", "ch"};
    for (const char *row : rows) {
        if (!history[row].isObject()) {
            sendSetupError(exchange, 400, "Home recent-history row expected");
            return;
        }
        Json clean = Json::object();
        for (const std::string &id : history[row].keys()) {
            if (id.empty() || exceedsUtf16CodeUnitLimit(id, 200) ||
                !history[row][id].isNumber())
                continue;
            clean.set(id, history[row][id]);
        }
        incoming.set(row, clean);
    }

    std::string error;
    bool saved = false;
    {
        if (!homeRecentHistoryState_) {
            sendSetupError(exchange, 500, "Home recent-history store unavailable");
            return;
        }
        std::lock_guard<std::mutex> lock(homeRecentHistoryState_->mutex);
        JsonFile file(homeRecentHistoryState_->path, maxBytes);
        const JsonFileResult stored = file.load();
        if (!stored.error.empty() && !stored.found) {
            error = stored.error;
        } else {
            const Json current = stored.found ? stored.value : Json::object();
            const Json recent = service::mergeHomeRecentHistory(
                current, incoming, cleanRows);
            saved = file.save(recent, &error);
        }
    }
    if (!saved) {
        sendSetupError(exchange, 500,
                       error.empty() ? "could not save Home recent history" : error);
        return;
    }
    (void)exchange.respond(200,
                           {{"Content-Type", "application/json; charset=utf-8"},
                            {"Cache-Control", "no-store"},
                            {"X-Content-Type-Options", "nosniff"}},
                           "{\"ok\":true}");
}

void HttpFrontDoor::syncHomeAutoLaunch(HttpExchange &exchange) const {
    const HttpRequest &request = exchange.request();
    if (!isLoopbackPeer(request.remoteAddress) || proxySecret_.empty() ||
        !tokenMatchesConstantTime(request.header("x-nativetv-proxy-secret"), proxySecret_) ||
        !request.header("x-nativetv-original-remote").empty()) {
        sendSetupError(exchange, 403, "native Home auto-launch sync required", true);
        return;
    }
    if (request.method != "POST") {
        sendSetupError(exchange, 405, "method not allowed");
        return;
    }
    if (!setupJsonContentType(request)) {
        sendSetupError(exchange, 415, "Content-Type must be application/json");
        return;
    }
    if (!exchange.setBodyLimit(8192)) {
        sendSetupError(exchange, 413, "request body too large", true);
        return;
    }
    std::string body;
    char buffer[1024];
    for (;;) {
        const int count = exchange.readBody(buffer, sizeof(buffer));
        if (count == 0) break;
        if (count < 0) {
            sendSetupError(exchange, exchange.bodyLimitExceeded() ? 413 : 400,
                           exchange.bodyLimitExceeded() ? "request body too large"
                                                        : "invalid request body",
                           true);
            return;
        }
        body.append(buffer, static_cast<size_t>(count));
    }
    if (exceedsUtf16CodeUnitLimit(body, 8192)) {
        sendSetupError(exchange, 413, "request body too large", true);
        return;
    }
    const Json payload = Json::parse(body);
    if (!payload.isObject() || !payload["escaped"].isBoolean()) {
        sendSetupError(exchange, 400, "Home auto-launch state expected");
        return;
    }

    Json envelope = Json::object();
    envelope.set("ok", Json::flag(true));
    envelope.set("result", synchronizeHomeAutoLaunch(payload["escaped"].boolean()));
    (void)exchange.respond(200,
                           {{"Content-Type", "application/json; charset=utf-8"},
                            {"Cache-Control", "no-store"},
                            {"X-Content-Type-Options", "nosniff"}},
                           envelope.dump());
}

Json HttpFrontDoor::synchronizeHomeAutoLaunch(bool escaped) const {
    std::lock_guard<std::mutex> operationLock(homeAutoLaunchMutex_);
    const auto result = [](bool ok, const std::string &error = std::string()) {
        Json response = Json::object();
        response.set("ok", Json::flag(ok));
        if (!error.empty()) response.set("error", Json::text(error));
        return response;
    };
    const auto readConfig = [this](Json &document, std::string &error) {
        std::lock_guard<std::mutex> lock(homeConfigMutex_);
        JsonFile file(dataFilePath(dataRoot_, "home.json"), 8 * 1024 * 1024);
        const JsonFileResult stored = file.load();
        if (!stored.error.empty() && !stored.found) {
            error = stored.error;
            return false;
        }
        document = service::normalizeHomeDocument(
            stored.found ? stored.value : Json::object());
        return true;
    };
    const auto writeSettings = [this](const Json &settings, std::string &error) {
        Json request = Json::object();
        request.set("category", Json::text("general"));
        request.set("settings", settings);
        LunaReply reply;
        if (!waitForLuna(luna_, "com.webos.service.settings/setSystemSettings",
                         request, reply) || !reply.response["returnValue"].boolean()) {
            error = "the TV would not change LG's Home auto launch";
            return false;
        }
        return true;
    };
    const auto restore = [&]() {
        Json current;
        std::string error;
        if (!readConfig(current, error)) return result(false, error);
        const Json original = current["lgAutoLaunch"];
        if (!original.isString() || original.string().empty()) return result(true);

        Json settings = Json::object();
        settings.set("homeAutoLaunch", original);
        if (current["lgAutoLaunchCheck"].isString())
            settings.set("checkHomeAutoLaunchValue", current["lgAutoLaunchCheck"]);
        if (!writeSettings(settings, error)) return result(false, error);

        bool saved = false;
        {
            std::lock_guard<std::mutex> lock(homeConfigMutex_);
            JsonFile file(dataFilePath(dataRoot_, "home.json"), 8 * 1024 * 1024);
            const JsonFileResult stored = file.load();
            if (!stored.error.empty() && !stored.found) {
                error = stored.error;
            } else {
                Json latest = service::normalizeHomeDocument(
                    stored.found ? stored.value : Json::object());
                latest = Json::parse(latest.dump());
                latest.set("lgAutoLaunch", Json::parse("null"));
                latest.set("lgAutoLaunchCheck", Json::parse("null"));
                saved = file.save(service::normalizeHomeDocument(latest), &error);
            }
        }
        if (!saved)
            return result(false, error.empty()
                ? "could not save Home auto-launch state" : error);
        return result(true);
    };
    const auto hold = [&]() {
        Json query = Json::object();
        query.set("category", Json::text("general"));
        Json keys = Json::array();
        keys.add(Json::text("homeAutoLaunch"));
        keys.add(Json::text("checkHomeAutoLaunchValue"));
        query.set("keys", keys);
        LunaReply settingsReply;
        if (!waitForLuna(luna_, "com.webos.service.settings/getSystemSettings",
                         query, settingsReply) ||
            settingsReply.response["returnValue"].boolean() == false ||
            !settingsReply.response["settings"].isObject())
            return result(false, "could not read LG's Home auto launch");

        const std::string value =
            settingsReply.response["settings"]["homeAutoLaunch"].string();
        if (value.empty())
            return result(false, "could not read LG's Home auto launch");

        std::string error;
        bool saved = false;
        {
            std::lock_guard<std::mutex> lock(homeConfigMutex_);
            JsonFile file(dataFilePath(dataRoot_, "home.json"), 8 * 1024 * 1024);
            const JsonFileResult stored = file.load();
            if (!stored.error.empty() && !stored.found) {
                error = stored.error;
            } else {
                Json latest = service::normalizeHomeDocument(
                    stored.found ? stored.value : Json::object());
                if (!latest["enabled"].boolean() ||
                    (!escaped && !homeTakeoverMounted(systemRoot_, dataRoot_)))
                    return result(true);
                if (value == "off") return result(true);
                const bool captureCheck = !latest["lgAutoLaunch"].isString() ||
                    latest["lgAutoLaunch"].string() == "off";
                latest = Json::parse(latest.dump());
                if (captureCheck) {
                    const Json check = settingsReply.response["settings"]["checkHomeAutoLaunchValue"];
                    latest.set("lgAutoLaunchCheck",
                               check.isString() ? check : Json::parse("null"));
                }
                latest.set("lgAutoLaunch", Json::text(value));
                saved = file.save(service::normalizeHomeDocument(latest), &error);
            }
        }
        if (!saved)
            return result(false, error.empty()
                ? "could not save Home auto-launch state" : error);

        Json settings = Json::object();
        settings.set("homeAutoLaunch", Json::text("off"));
        if (!writeSettings(settings, error)) return result(false, error);
        return result(true);
    };

    Json current;
    std::string error;
    if (!readConfig(current, error)) return result(false, error);
    bool shouldHold = current["enabled"].boolean() &&
                      (escaped || homeTakeoverMounted(systemRoot_, dataRoot_));
    if (!shouldHold) return restore();
    if (current["openOnPowerOn"].boolean()) return hold();

    Json query = Json::object();
    query.set("category", Json::text("option"));
    Json keys = Json::array();
    keys.add(Json::text("quickStartMode"));
    query.set("keys", keys);
    LunaReply quickStartReply;
    if (!waitForLuna(luna_, "com.webos.service.settings/getSystemSettings",
                     query, quickStartReply) ||
        quickStartReply.response["returnValue"].boolean() == false)
        return result(false, "could not read Quick Start+; LG's Home auto launch left as it is");
    const std::string quickStart =
        quickStartReply.response["settings"]["quickStartMode"].string();

    if (!readConfig(current, error)) return result(false, error);
    shouldHold = current["enabled"].boolean() &&
                 (escaped || homeTakeoverMounted(systemRoot_, dataRoot_));
    if (!shouldHold) return restore();
    if (current["openOnPowerOn"].boolean()) return hold();
    if (quickStart == "on") return restore();
    if (quickStart == "off") return hold();
    return result(false, "could not read Quick Start+; LG's Home auto launch left as it is");
}

void HttpFrontDoor::setNativeHomeInput(HttpExchange &exchange, const Json &payload) const {
    const std::string id = payload["id"].string();
    const std::string title = payload["title"].string();
    const bool onHome = payload["onHome"].boolean();
    const auto validText = [](const std::string &value, size_t maximum) {
        if (value.empty() || value.size() > maximum) return false;
        for (unsigned char character : value)
            if (std::iscntrl(character)) return false;
        return true;
    };
    if ((id != "hdmi1" && id != "hdmi2" && id != "hdmi3" && id != "hdmi4") ||
        !payload["onHome"].isBoolean() || !validText(title, 256)) {
        sendSetupError(exchange, 400, "Home input parameters expected");
        return;
    }
    if (!homeScreenManager_ || !commandPolicy_) {
        sendSetupError(exchange, 503, "native Home screen manager is unavailable");
        return;
    }

    Json operation = Json::object();
    CommandPolicySpec localCommand;
    localCommand.action = "home-screen.setInput";
    localCommand.uri = "native/home-screen/setInput";
    localCommand.payload = payload;
    localCommand.timeout = std::chrono::milliseconds(30000);
    localCommand.requiresControl = false;
    localCommand.resource = "home-screen-layout";
    LunaReply localReply;
    const auto makeSuccess = [](bool changed) {
        Json result = Json::object();
        result.set("ok", Json::flag(true));
        result.set("changed", Json::flag(changed));
        return result;
    };
    const CommandDecision decision = commandPolicy_->executeLocal(
        localCommand, controlPolicyContext(config_), false,
        [this, &operation, &id, &title, onHome, &validText, makeSuccess]() {
            const Json state = homeScreenManager_->state();
            const auto failed = [&operation](const std::string &error) {
                operation = Json::object();
                operation.set("ok", Json::flag(false));
                operation.set("error", Json::text(error));
            };
            if (!state["supported"].boolean()) {
                failed("This TV's Home screen cannot be changed from here");
                return false;
            }
            if (state["experimental"].isBoolean() && state["experimental"].boolean() &&
                !state["allowed"].boolean()) {
                failed("Switch on Home screen layout first. It is experimental on this TV.");
                return false;
            }
            const std::string appId = "com.webos.app." + id;
            const auto call = [this](const std::string &action, const std::string &uri,
                                     const Json &request, LunaReply &reply) {
                return waitForCommandPolicyLuna(
                    luna_, commandPolicy_, controlPolicyContext(config_), action, uri,
                    request, "home-screen-launch-points", false, reply,
                    std::string(), false, std::chrono::milliseconds(5000));
            };
            const auto listPoints = [&call](Json &points) {
                LunaReply reply;
                if (!call("home-screen.luna.read-inputs",
                          "com.webos.applicationManager/listLaunchPoints",
                          Json::object(), reply) ||
                    !reply.response["returnValue"].boolean() ||
                    !reply.response["launchPoints"].isArray()) return false;
                points = reply.response["launchPoints"];
                return true;
            };
            const auto restartHome = [this, &failed]() {
                bool wasForeground = false;
                LunaReply foreground;
                if (waitForCommandPolicyLuna(
                        luna_, commandPolicy_, controlPolicyContext(config_),
                        "home-screen.foreground",
                        "com.webos.applicationManager/getForegroundAppInfo",
                        Json::object(), "home-screen-app", false, foreground,
                        std::string(), false, std::chrono::milliseconds(5000)))
                    wasForeground = foreground.response["appId"].string() ==
                                    "com.webos.app.home";
                std::string error;
                if (!homeScreenManager_->restartHome(&error)) {
                    failed(error.empty() ? "could not restart the Home screen" : error);
                    return false;
                }
                if (wasForeground) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(2000));
                    LunaReply launched;
                    (void)waitForCommandPolicyLuna(
                        luna_, commandPolicy_, controlPolicyContext(config_),
                        "home-screen.launch", "com.webos.applicationManager/launch",
                        Json::parse("{\"id\":\"com.webos.app.home\"}"),
                        "home-screen-app", false, launched, std::string(), false,
                        std::chrono::milliseconds(5000));
                }
                return true;
            };
            Json points;
            if (!listPoints(points)) {
                failed("Could not read the TV's Home screen tiles. Try again in a moment.");
                return false;
            }

            Json tracked = state["addedInputs"];
            if (!tracked.isObject()) tracked = Json::object();
            if (onHome) {
                for (const Json &point : points.items()) {
                    if (point["id"].string() == appId) {
                        operation = makeSuccess(false);
                        return true;
                    }
                }
                Json addPayload = Json::object();
                addPayload.set("id", Json::text(appId));
                addPayload.set("title", Json::text(title));
                addPayload.set("params", Json::object());
                if (!state["experimental"].isBoolean())
                    addPayload.set("titleVisible", Json::flag(true));
                LunaReply added;
                if (!call("home-screen.luna.add-input",
                          "com.webos.applicationManager/addLaunchPoint",
                          addPayload, added) || !added.response["returnValue"].boolean() ||
                    !validText(added.response["launchPointId"].string(), 256)) {
                    failed("the TV would not add it");
                    return false;
                }
                tracked.set(id, added.response["launchPointId"]);
                const Json saved = homeScreenManager_->setAddedInputs(tracked);
                if (!saved["ok"].boolean()) {
                    Json removePayload = Json::object();
                    removePayload.set("launchPointId", added.response["launchPointId"]);
                    LunaReply ignored;
                    (void)call("home-screen.luna.remove-input",
                                "com.webos.applicationManager/removeLaunchPoint",
                                removePayload, ignored);
                    operation = saved;
                    return false;
                }
                if (!listPoints(points)) {
                    operation = makeSuccess(false);
                    return true;
                }
                std::vector<std::string> order;
                std::vector<std::string> wanted;
                const char *const inputIds[] = {"hdmi1", "hdmi2", "hdmi3", "hdmi4"};
                for (const Json &point : points.items()) {
                    if (point["launchPointId"].isString())
                        order.push_back(point["launchPointId"].string());
                }
                for (const char *inputId : inputIds) {
                    const std::string inputApp = std::string("com.webos.app.") + inputId;
                    for (const Json &point : points.items()) {
                        if (point["id"].string() == inputApp &&
                            point["launchPointId"].isString()) {
                            wanted.push_back(point["launchPointId"].string());
                            break;
                        }
                    }
                }
                bool inPlace = true;
                for (size_t index = 0; index < wanted.size(); ++index)
                    if (index >= order.size() || order[index] != wanted[index]) inPlace = false;
                if (!inPlace) {
                    for (size_t index = 0; index < wanted.size(); ++index) {
                        Json movePayload = Json::object();
                        movePayload.set("launchPointId", Json::text(wanted[index]));
                        movePayload.set("position", Json::numeric(static_cast<double>(index)));
                        LunaReply ignored;
                        (void)call("home-screen.luna.move-input",
                                   "com.webos.applicationManager/moveLaunchPoint",
                                   movePayload, ignored);
                    }
                }
                operation = makeSuccess(!inPlace);
                if (!inPlace) {
                    operation.set("restart", Json::flag(true));
                    if (!restartHome()) return false;
                }
                return true;
            }

            for (const Json &point : points.items()) {
                if (point["id"].string() != appId ||
                    !point["launchPointId"].isString()) continue;
                Json removePayload = Json::object();
                removePayload.set("launchPointId", point["launchPointId"]);
                LunaReply ignored;
                (void)call("home-screen.luna.remove-input",
                            "com.webos.applicationManager/removeLaunchPoint",
                            removePayload, ignored);
            }
            Json remaining = Json::object();
            for (const std::string &trackedId : tracked.keys())
                if (trackedId != id) remaining.set(trackedId, tracked[trackedId]);
            operation = homeScreenManager_->setAddedInputs(remaining);
            return operation["ok"].boolean();
        }, localReply);

    const bool succeeded = decision.kind == CommandDecisionKind::Allowed &&
        localReply.response["returnValue"].isBoolean() &&
        localReply.response["returnValue"].boolean();
    if (!succeeded && (!operation["ok"].isBoolean() || operation["ok"].boolean())) {
        operation = Json::object();
        operation.set("ok", Json::flag(false));
        operation.set("error", Json::text(!localReply.error.empty() ? localReply.error :
            !decision.reason.empty() ? decision.reason : "Home input operation failed"));
    }
    Json envelope = Json::object();
    envelope.set("ok", Json::flag(true));
    envelope.set("result", operation);
    (void)exchange.respond(200,
                           {{"Content-Type", "application/json; charset=utf-8"},
                            {"Cache-Control", "no-store"},
                            {"X-Content-Type-Options", "nosniff"}},
                           envelope.dump());
}

void HttpFrontDoor::setNativeHomeScreen(HttpExchange &exchange) const {
    const HttpRequest &request = exchange.request();
    if (!isLoopbackPeer(request.remoteAddress) || proxySecret_.empty() ||
        !tokenMatchesConstantTime(request.header("x-nativetv-proxy-secret"), proxySecret_) ||
        !request.header("x-nativetv-original-remote").empty()) {
        sendSetupError(exchange, 403, "native Home screen control required", true);
        return;
    }
    if (request.method != "POST") {
        sendSetupError(exchange, 405, "method not allowed");
        return;
    }
    if (!setupJsonContentType(request)) {
        sendSetupError(exchange, 415, "Content-Type must be application/json");
        return;
    }
    if (!homeScreenManager_ || !commandPolicy_) {
        sendSetupError(exchange, 503, "native Home screen manager is unavailable");
        return;
    }
    if (!exchange.setBodyLimit(8192)) {
        sendSetupError(exchange, 413, "request body too large", true);
        return;
    }
    std::string body;
    char buffer[1024];
    for (;;) {
        const int count = exchange.readBody(buffer, sizeof(buffer));
        if (count == 0) break;
        if (count < 0) {
            sendSetupError(exchange, exchange.bodyLimitExceeded() ? 413 : 400,
                           exchange.bodyLimitExceeded() ? "request body too large"
                                                        : "invalid request body",
                           true);
            return;
        }
        body.append(buffer, static_cast<size_t>(count));
    }
    if (exceedsUtf16CodeUnitLimit(body, 8192)) {
        sendSetupError(exchange, 413, "request body too large", true);
        return;
    }
    const Json requestPayload = Json::parse(body);
    if (!requestPayload.isObject() || !requestPayload["action"].isString()) {
        sendSetupError(exchange, 400, "Home screen action expected");
        return;
    }
    const std::string action = requestPayload["action"].string();
    if (action == "setStartLayout") {
        if (!requestPayload["blank"].isBoolean()) {
            sendSetupError(exchange, 400, "Home start layout flag expected");
            return;
        }
        Json envelope = Json::object();
        envelope.set("ok", Json::flag(true));
        envelope.set("result", homeScreenManager_->startLayout(
            requestPayload["blank"].boolean()));
        (void)exchange.respond(200,
                               {{"Content-Type", "application/json; charset=utf-8"},
                                {"Cache-Control", "no-store"},
                                {"X-Content-Type-Options", "nosniff"}},
                               envelope.dump());
        return;
    }
    if (action == "takeover" || action == "releaseTakeover") {
        if (!homeTakeoverLifecycleManager_) {
            sendSetupError(exchange, 503, "native Home takeover lifecycle is unavailable");
            return;
        }
        if (action == "takeover" && !requestPayload["blank"].isBoolean()) {
            sendSetupError(exchange, 400, "Home starting-layout flag expected");
            return;
        }
        Json envelope = Json::object();
        envelope.set("ok", Json::flag(true));
        const CommandPolicyContext context = controlPolicyContext(config_);
        const Json operation = action == "takeover"
            ? homeTakeoverLifecycleManager_->takeover(
                  requestPayload["blank"].boolean(), context)
            : homeTakeoverLifecycleManager_->release(context);
        envelope.set("result", operation);
        (void)exchange.respond(200,
                               {{"Content-Type", "application/json; charset=utf-8"},
                                {"Cache-Control", "no-store"},
                                {"X-Content-Type-Options", "nosniff"}},
                               envelope.dump());
        return;
    }
    if (action == "setInput") {
        setNativeHomeInput(exchange, requestPayload);
        return;
    }
    const auto validText = [](const Json &value, size_t maximum) {
        if (!value.isString() || value.string().empty() || value.string().size() > maximum)
            return false;
        for (unsigned char character : value.string())
            if (std::iscntrl(character)) return false;
        return true;
    };
    const auto validInputApp = [](const std::string &id) {
        return id == "com.webos.app.hdmi1" || id == "com.webos.app.hdmi2" ||
               id == "com.webos.app.hdmi3" || id == "com.webos.app.hdmi4";
    };
    const auto failedOperation = [](const std::string &error) {
        Json result = Json::object();
        result.set("ok", Json::flag(false));
        result.set("error", Json::text(error));
        return result;
    };

    if (action == "luna") {
        const std::string uri = requestPayload["uri"].string();
        const Json payload = requestPayload["payload"];
        if (!requestPayload["uri"].isString() || !payload.isObject()) {
            sendSetupError(exchange, 400, "Home screen Luna operation expected");
            return;
        }
        const bool readOnly = uri == "com.webos.service.eim/getAllInputStatus" ||
            uri == "com.webos.applicationManager/listLaunchPoints" ||
            uri == "com.webos.applicationManager/getForegroundAppInfo";
        bool valid = readOnly;
        std::string policyAction;
        std::string resource = "home-screen-launch-points";
        if (readOnly) {
            policyAction = "home-screen.luna.read";
            if (uri == "com.webos.service.eim/getAllInputStatus")
                resource = "home-screen-input-status";
        } else if (uri == "com.webos.applicationManager/addLaunchPoint") {
            const std::string id = payload["id"].string();
            valid = validInputApp(id) && validText(payload["title"], 256) &&
                    payload["params"].isObject() &&
                    (!payload.has("titleVisible") || payload["titleVisible"].isBoolean());
            policyAction = "home-screen.luna.add-input";
        } else if (uri == "com.webos.applicationManager/updateLaunchPoint") {
            valid = validText(payload["launchPointId"], 256) &&
                    validText(payload["title"], 256) && payload["titleVisible"].isBoolean();
            policyAction = "home-screen.luna.update-input";
        } else if (uri == "com.webos.applicationManager/removeLaunchPoint") {
            valid = validText(payload["launchPointId"], 256);
            policyAction = "home-screen.luna.remove-input";
        } else if (uri == "com.webos.applicationManager/moveLaunchPoint") {
            const double position = payload["position"].number(-1);
            valid = validText(payload["launchPointId"], 256) && position >= 0 &&
                    position <= 3 && std::floor(position) == position;
            policyAction = "home-screen.luna.move-input";
        } else if (uri == "com.webos.applicationManager/launch") {
            valid = payload["id"].isString() && payload["id"].string() == "com.webos.app.home";
            policyAction = "home-screen.luna.launch-home";
            resource = "home-screen-app";
        }
        if (!valid) {
            sendSetupError(exchange, 400, "unsupported Home screen Luna operation");
            return;
        }
        LunaReply reply;
        if (!waitForCommandPolicyLuna(
                luna_, commandPolicy_, controlPolicyContext(config_), policyAction,
                uri, payload, resource, false, reply, std::string(), false,
                std::chrono::milliseconds(5000))) {
            sendSetupError(exchange, 502,
                           reply.error.empty() ? "Home screen Luna request failed" : reply.error);
            return;
        }
        Json envelope = Json::object();
        envelope.set("ok", Json::flag(true));
        envelope.set("result", reply.response);
        (void)exchange.respond(200,
                               {{"Content-Type", "application/json; charset=utf-8"},
                                {"Cache-Control", "no-store"},
                                {"X-Content-Type-Options", "nosniff"}},
                               envelope.dump());
        return;
    }

    if (action == "state") {
        Json envelope = Json::object();
        envelope.set("ok", Json::flag(true));
        envelope.set("result", homeScreenManager_->state());
        (void)exchange.respond(200,
                               {{"Content-Type", "application/json; charset=utf-8"},
                                {"Cache-Control", "no-store"},
                                {"X-Content-Type-Options", "nosniff"}},
                               envelope.dump());
        return;
    }

    if (action != "setPart" && action != "restore" && action != "allowChanged" &&
        action != "reapply" && action != "restart" && action != "inputTracking" &&
        action != "stopLgHome") {
        sendSetupError(exchange, 400, "unknown Home screen action");
        return;
    }
    if ((action == "setPart" &&
         (!requestPayload["id"].isString() || !requestPayload["shown"].isBoolean())) ||
        (action == "allowChanged" && !requestPayload["allow"].isBoolean()) ||
        (action == "inputTracking" && !requestPayload["inputs"].isObject())) {
        sendSetupError(exchange, 400, "Home screen action parameters expected");
        return;
    }

    Json operation = Json::object();
    CommandPolicySpec localCommand;
    localCommand.action = "home-screen." + action;
    localCommand.uri = "native/home-screen/" + action;
    localCommand.payload = requestPayload;
    localCommand.timeout = std::chrono::milliseconds(30000);
    localCommand.requiresControl = false;
    localCommand.resource = "home-screen-layout";
    LunaReply localReply;
    const CommandDecision decision = commandPolicy_->executeLocal(
        localCommand, controlPolicyContext(config_), false,
        [this, &operation, &requestPayload, &action, failedOperation]() {
            if (action == "setPart")
                operation = homeScreenManager_->setPart(
                    requestPayload["id"].string(), requestPayload["shown"].boolean());
            else if (action == "restore") {
                const Json tracked = homeScreenManager_->state()["addedInputs"];
                for (const std::string &id : tracked.keys()) {
                    Json payload = Json::object();
                    payload.set("launchPointId", tracked[id]);
                    LunaReply removed;
                    (void)waitForCommandPolicyLuna(
                        luna_, commandPolicy_, controlPolicyContext(config_),
                        "home-screen.luna.restore-input",
                        "com.webos.applicationManager/removeLaunchPoint", payload,
                        "home-screen-launch-points", false, removed,
                        std::string(), false, std::chrono::milliseconds(5000),
                        false);
                }
                operation = homeScreenManager_->setAddedInputs(Json::object());
                if (operation["ok"].boolean()) operation = homeScreenManager_->restore();
            }
            else if (action == "allowChanged")
                operation = homeScreenManager_->setAllowed(requestPayload["allow"].boolean());
            else if (action == "reapply") operation = homeScreenManager_->reapply();
            else if (action == "inputTracking")
                operation = homeScreenManager_->setAddedInputs(requestPayload["inputs"]);
            else if (action == "stopLgHome") {
                bool stopped = false;
                std::string error;
                operation = Json::object();
                if (!homeScreenManager_->stopHome(stopped, &error)) {
                    operation = failedOperation(error.empty()
                        ? "could not stop LG's Home" : error);
                } else {
                    operation.set("ok", Json::flag(true));
                    operation.set("stopped", Json::flag(stopped));
                }
            }
            else {
                operation = Json::object();
                operation.set("ok", Json::flag(true));
                operation.set("changed", Json::flag(true));
                operation.set("restart", Json::flag(true));
            }
            if (!operation["ok"].isBoolean() || !operation["ok"].boolean()) return false;

            const bool shouldRestart = action == "restart" ||
                (action != "inputTracking" && action != "stopLgHome" &&
                 (operation["restart"].boolean() || operation["changed"].boolean()));
            if (!shouldRestart) return true;
            bool wasForeground = false;
            LunaReply foreground;
            if (waitForCommandPolicyLuna(
                    luna_, commandPolicy_, controlPolicyContext(config_),
                    "home-screen.foreground", "com.webos.applicationManager/getForegroundAppInfo",
                    Json::object(), "home-screen-app", false, foreground,
                    std::string(), false, std::chrono::milliseconds(5000)))
                wasForeground = foreground.response["appId"].string() == "com.webos.app.home";
            std::string restartError;
            if (!homeScreenManager_->restartHome(&restartError)) {
                operation = failedOperation(restartError.empty()
                    ? "could not restart the Home screen" : restartError);
                return false;
            }
            if (wasForeground) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2000));
                LunaReply launched;
                (void)waitForCommandPolicyLuna(
                    luna_, commandPolicy_, controlPolicyContext(config_),
                    "home-screen.launch", "com.webos.applicationManager/launch",
                    Json::parse("{\"id\":\"com.webos.app.home\"}"),
                    "home-screen-app", false, launched, std::string(), false,
                    std::chrono::milliseconds(5000));
            }
            return true;
        }, localReply);
    const bool succeeded = decision.kind == CommandDecisionKind::Allowed &&
        localReply.response["returnValue"].isBoolean() &&
        localReply.response["returnValue"].boolean();
    if (!succeeded && (!operation["ok"].isBoolean() || operation["ok"].boolean()))
        operation = failedOperation(!localReply.error.empty() ? localReply.error
            : !decision.reason.empty() ? decision.reason : "Home screen operation failed");
    Json envelope = Json::object();
    envelope.set("ok", Json::flag(true));
    envelope.set("result", operation);
    (void)exchange.respond(200,
                           {{"Content-Type", "application/json; charset=utf-8"},
                            {"Cache-Control", "no-store"},
                            {"X-Content-Type-Options", "nosniff"}},
                           envelope.dump());
}

bool HttpFrontDoor::setServiceStubs(const service::BackgroundServiceInfo &info,
                                    const std::string &id, bool disabled,
                                    std::string &operationError) const {
    const std::vector<std::string> programs =
        service::backgroundServicePrograms(id, systemRoot_);
    const std::string shellStub =
        service::backgroundServiceStubPath(dataRoot_, false);
    const std::string jsStub =
        service::backgroundServiceStubPath(dataRoot_, true);
    if (disabled && !programs.empty() &&
        (!writeServiceStub(shellStub, false) || !writeServiceStub(jsStub, true))) {
        operationError = "could not prepare service stubs";
        return false;
    }

    const auto run = [this](const std::string &executable,
                            const std::vector<std::string> &arguments,
                            std::chrono::milliseconds timeout) {
        if (executable.empty()) return false;
        ChildOptions options;
        options.arguments.push_back(executable);
        options.arguments.insert(options.arguments.end(), arguments.begin(),
                                 arguments.end());
        options.timeout = timeout;
        options.maximumOutputBytes = 16 * 1024;
        return children_.run(options).succeeded();
    };
    const std::string mount = findSystemExecutable(
        systemRoot_, {"/bin/mount", "/usr/bin/mount"});
    const std::string umount = findSystemExecutable(
        systemRoot_, {"/bin/umount", "/usr/bin/umount"});
    for (const std::string &program : programs) {
        const std::string target = rootedSystemPath(systemRoot_, program);
        const bool mounted = service::backgroundServiceStubbed(procRoot_, target);
        if (disabled && !mounted) {
            const std::string stub = containsJavascriptSuffix(program)
                ? jsStub : shellStub;
            if (!run(mount, {"--bind", stub, target},
                     std::chrono::milliseconds(8000))) {
                operationError = "could not bind service stub over " + program;
                return false;
            }
        } else if (!disabled && mounted &&
                   !run(umount, {target}, std::chrono::milliseconds(8000))) {
            operationError = "could not remove service stub from " + program;
            return false;
        }
    }

    if (!info.jail.empty() && !info.exe.empty()) {
        const std::string jailRoot = rootedSystemPath(
            systemRoot_, "/var/palm/jail/" + info.bin);
        const std::string jailUsr = jailRoot + "/usr";
        if (disabled && !service::backgroundServiceStubbed(procRoot_, jailUsr)) {
            const std::string jailer = findSystemExecutable(
                systemRoot_, {"/usr/bin/jailer"});
            if (!run(jailer, {"-t", info.jail, "-i", info.bin,
                              "-p", "/usr", "/bin/true"},
                     std::chrono::milliseconds(30000))) {
                operationError = "could not prepare ThinQ AI service jail";
                return false;
            }
        }
        const std::string jailedProgram = jailRoot + info.exe;
        const bool mounted = service::backgroundServiceStubbed(
            procRoot_, jailedProgram);
        if (disabled && !mounted) {
            if (!writeServiceStub(shellStub, false) ||
                !run(findSystemExecutable(systemRoot_, {"/bin/mount", "/usr/bin/mount"}),
                     {"--bind", shellStub, jailedProgram},
                     std::chrono::milliseconds(8000))) {
                operationError = "could not bind the ThinQ AI jail stub";
                return false;
            }
        } else if (!disabled && mounted &&
                   !run(findSystemExecutable(systemRoot_, {"/bin/umount", "/usr/bin/umount"}),
                        {jailedProgram}, std::chrono::milliseconds(8000))) {
            operationError = "could not remove the ThinQ AI jail stub";
            return false;
        }
    }
    return true;
}

bool HttpFrontDoor::applyBackgroundServiceState(
    const service::BackgroundServiceInfo &info, const std::string &id,
    bool disabled, Json &result, std::string &operationError) const {
    std::vector<std::string> disabledServices;
    if (!service::readDisabledBackgroundServices(dataRoot_, disabledServices,
                                                  &operationError))
        return false;
    if (!service::setBackgroundServiceDisabled(dataRoot_, id, disabled,
                                                disabledServices,
                                                &operationError))
        return false;
    if (!service::syncBackgroundServiceBootScript(systemRoot_, dataRoot_,
                                                   disabledServices,
                                                   &operationError))
        return false;

    if (!setServiceStubs(info, id, disabled, operationError)) return false;
    const auto run = [this](const std::string &executable,
                            const std::vector<std::string> &arguments,
                            std::chrono::milliseconds timeout) {
        if (executable.empty()) return false;
        ChildOptions options;
        options.arguments.push_back(executable);
        options.arguments.insert(options.arguments.end(), arguments.begin(),
                                 arguments.end());
        options.timeout = timeout;
        options.maximumOutputBytes = 16 * 1024;
        return children_.run(options).succeeded();
    };

    bool unitFile = false;
    if (!info.unit.empty()) {
        const std::string unitPaths[] = {
            "/etc/systemd/system/" + info.unit,
            "/lib/systemd/system/" + info.unit,
            "/usr/lib/systemd/system/" + info.unit
        };
        for (const std::string &path : unitPaths)
            if (access(rootedSystemPath(systemRoot_, path).c_str(), F_OK) == 0)
                unitFile = true;
    }
    const std::string systemctl =
        service::backgroundSystemctlExecutable(systemRoot_);
    const std::string initctl =
        service::backgroundInitctlExecutable(systemRoot_);
    if (!systemctl.empty() && unitFile) {
        if (!disabled) {
            const std::string mask = rootedSystemPath(
                systemRoot_, "/run/systemd/transient/" + info.unit);
            const bool masked = unlink(mask.c_str()) == 0;
            if (masked)
                (void)run(systemctl, {"daemon-reload"},
                          std::chrono::milliseconds(12000));
        }
        (void)run(systemctl, {disabled ? "stop" : "start", info.unit},
                  std::chrono::milliseconds(12000));
        result.set("running", Json::flag(!disabled));
    } else if (!initctl.empty() && !info.upstart.empty()) {
        (void)run(initctl, {disabled ? "stop" : "start", info.upstart},
                  std::chrono::milliseconds(12000));
        result.set("running", Json::flag(!disabled));
    } else {
        if (disabled) {
            for (const int pid : service::backgroundServiceProcessIds(id, procRoot_))
                (void)kill(pid, SIGTERM);
        }
        result.set("running", Json::flag(false));
    }

    const std::string script = assetsRoot_ + "/early-service/install.sh";
    const std::string manifest = rootedSystemPath(
        systemRoot_, "/var/luna-service2-dev/manifests.d/com.webos.glasshouse.boot.json");
    const std::string homeFlag = rootedSystemPath(
        systemRoot_, "/var/lib/tvweb/early-home");
    if (access(script.c_str(), R_OK) == 0) {
        const std::string shell = findSystemExecutable(
            systemRoot_, {"/bin/sh", "/usr/bin/sh"});
        const size_t slash = script.find_last_of('/');
        const std::string assetsDirectory = slash == std::string::npos
            ? std::string(".") : script.substr(0, slash);
        if (!disabledServices.empty() && access(manifest.c_str(), F_OK) != 0)
            (void)run(shell, {script, "install", assetsDirectory},
                      std::chrono::milliseconds(30000));
        else if (disabledServices.empty() && access(homeFlag.c_str(), F_OK) != 0 &&
                 access(manifest.c_str(), F_OK) == 0)
            (void)run(shell, {script, "remove"},
                      std::chrono::milliseconds(30000));
    }
    result.set("ok", Json::flag(true));
    result.set("id", Json::text(id));
    result.set("disabled", Json::flag(disabled));
    return true;
}

bool HttpFrontDoor::reconcileNativeRemoteKeys(bool &wrote,
                                              std::string &error) const {
    wrote = false;
    error.clear();
    std::lock_guard<std::mutex> lock(remoteKeysMutex_);
    if (service::hasJavaScriptRemoteKeys(systemRoot_))
        return true;

    JsonFile mapStore(dataFilePath(dataRoot_, "remotekeys.json"), 64 * 1024);
    const JsonFileResult mapFile = mapStore.load();
    if (!mapFile.error.empty()) {
        error = mapFile.error;
        return false;
    }
    Json map = mapFile.value["keys"];
    if (!map.isObject()) map = Json::object();

    const std::string lgPath = dataFilePath(dataRoot_, "remotekeys-lg.json");
    JsonFile lgStore(lgPath, 256 * 1024);
    const JsonFileResult lgFile = lgStore.load();
    if (!lgFile.error.empty()) {
        error = lgFile.error;
        return false;
    }
    Json oldLgRecord = lgFile.value.isObject()
        ? Json::parse(lgFile.value.dump()) : Json::object();
    if (map.keys().empty() &&
        !service::nativeRemoteKeysLgRecordHasValues(oldLgRecord))
        return true;

    Json configNames = Json::array();
    for (const char *name : {
            "com.webos.surfacemanager.keyFiltersCpp",
            "com.webos.keyaction.tvGuide",
            "com.webos.keyaction.inputHub"})
        configNames.add(Json::text(name));
    Json configRequest = Json::object();
    configRequest.set("configNames", configNames);
    Json mappingRequest = Json::object();
    mappingRequest.set("category", Json::text("other"));
    Json mappingKeys = Json::array();
    mappingKeys.add(Json::text("mapping_info"));
    mappingRequest.set("keys", mappingKeys);
    Json buttonRequest = Json::object();
    buttonRequest.set("category", Json::text("other"));
    Json buttonKeys = Json::array();
    buttonKeys.add(Json::text("customShortPress"));
    buttonKeys.add(Json::text("customLongPress"));
    buttonRequest.set("keys", buttonKeys);
    Json alexaRequest = Json::object();
    alexaRequest.set("id", Json::text("amazon.alexa.view"));
    std::vector<std::pair<std::string, Json>> calls = {
        {"com.webos.service.config/getConfigs", configRequest},
        {"com.webos.settingsservice/getSystemSettings", mappingRequest},
        {"com.webos.settingsservice/getSystemSettings", buttonRequest},
        {"com.webos.applicationManager/getAppInfo", alexaRequest}
    };
    std::vector<LunaReply> replies;
    if (!waitForLunaBatch(luna_, calls, replies) || replies.size() != calls.size()) {
        error = "timed out reading native remote-key settings";
        return false;
    }
    const Json filters = replies[0].response["configs"][
        "com.webos.surfacemanager.keyFiltersCpp"];
    if (!replies[0].error.empty() ||
        !replies[0].response["returnValue"].boolean() ||
        !filters.isArray() || filters.size() == 0)
        return true;

    service::NativeRemoteKeysWriteState state;
    if (replies[1].error.empty() &&
        replies[1].response["settings"]["mapping_info"].isArray())
        state.mappingInfo = replies[1].response["settings"]["mapping_info"];
    else
        state.mappingInfo = Json::array();
    state.actions = Json::object();
    for (const char *name : {"tvGuide", "inputHub"}) {
        const Json value = replies[0].response["configs"][
            std::string("com.webos.keyaction.") + name];
        if (value.isObject()) state.actions.set(name, value);
    }
    state.buttonSettings = Json::object();
    if (replies[2].error.empty()) {
        for (const char *name : {"customShortPress", "customLongPress"}) {
            const Json value = replies[2].response["settings"][name];
            if (value.isObject() && value["id"].isString())
                state.buttonSettings.set(name, value);
        }
    }
    state.lgRecord = oldLgRecord;
    service::NativeRemoteKeysWriteResult writes =
        service::nativeRemoteKeysWriteRequests(map, state);

    Json settings = Json::object();
    if (writes.mappingChanged)
        settings.set("mapping_info", writes.mappingInfo);
    for (const std::string &name : writes.buttonSettings.keys())
        settings.set(name, writes.buttonSettings[name]);

    Json record = writes.lgRecord;
    const auto saveRecord = [&lgStore, &record, &error]() {
        if (service::nativeRemoteKeysLgRecordHasValues(record))
            return lgStore.save(record, &error);
        if (unlinkJsonFile(lgStore.path()) == 0 || errno == ENOENT)
            return true;
        error = "could not clear saved LG remote-key state";
        return false;
    };
    const auto persistRecordIfChanged = [&]() {
        if (record.dump() == oldLgRecord.dump() &&
            service::nativeRemoteKeysLgRecordHasValues(record) ==
                service::nativeRemoteKeysLgRecordHasValues(oldLgRecord))
            return true;
        if (!saveRecord()) return false;
        oldLgRecord = Json::parse(record.dump());
        return true;
    };
    // Persist original LG values before the first write so a daemon restart
    // can still restore them if the following Luna call is interrupted.
    if (!persistRecordIfChanged()) return false;

    const std::string voiceService = "voiceconductor";
    Json serviceOwners = record["services"];
    if (!serviceOwners.isObject()) serviceOwners = Json::object();
    const bool ownsVoiceService = serviceOwners[voiceService].boolean();
    const bool wantsVoiceServiceOff = map.has("ai");
    bool voiceServiceDisabled = false;
    if (wantsVoiceServiceOff || ownsVoiceService) {
        std::vector<std::string> disabledServices;
        if (!service::readDisabledBackgroundServices(dataRoot_, disabledServices,
                                                       &error))
            return false;
        voiceServiceDisabled = std::find(disabledServices.begin(),
            disabledServices.end(), voiceService) != disabledServices.end();
    }
    const bool disableVoiceService = wantsVoiceServiceOff &&
        (!ownsVoiceService || !voiceServiceDisabled);
    const bool enableVoiceService = !wantsVoiceServiceOff && ownsVoiceService;
    if (disableVoiceService || enableVoiceService) {
        service::BackgroundServiceInfo info;
        if (!service::backgroundHiddenServiceDefinition(voiceService, info)) {
            error = "the AI voice service is unavailable";
            return false;
        }
        if (disableVoiceService) {
            serviceOwners.set(voiceService, Json::flag(true));
            record.set("services", serviceOwners);
            if (!persistRecordIfChanged()) return false;
        }
        CommandPolicySpec command;
        command.action = disableVoiceService
            ? "remoteKeys.voice-service-disable"
            : "remoteKeys.voice-service-restore";
        command.uri = "native/remote-keys/voiceconductor";
        command.payload = Json::object();
        command.payload.set("id", Json::text(voiceService));
        command.payload.set("disabled", Json::flag(disableVoiceService));
        command.timeout = std::chrono::milliseconds(60000);
        command.requiresControl = false;
        command.resource = "background-services";
        Json result = Json::object();
        std::string serviceError;
        LunaReply serviceReply;
        const CommandDecision decision = commandPolicy_
            ? commandPolicy_->executeLocal(command, controlPolicyContext(config_), false,
                [this, &info, &voiceService, disableVoiceService,
                 &result, &serviceError]() {
                    return applyBackgroundServiceState(info, voiceService,
                        disableVoiceService, result, serviceError);
                }, serviceReply)
            : CommandDecision{CommandDecisionKind::TransportUnavailable,
                              "service policy is unavailable"};
        if (decision.kind != CommandDecisionKind::Allowed ||
            !serviceReply.response["returnValue"].boolean()) {
            error = !serviceError.empty() ? serviceError
                : !decision.reason.empty() ? decision.reason
                : "the TV did not take the voice-service change";
            return false;
        }
        wrote = true;
        if (enableVoiceService) {
            Json restoredOwners = Json::object();
            for (const std::string &name : serviceOwners.keys())
                if (name != voiceService)
                    restoredOwners.set(name, serviceOwners[name]);
            record.set("services", restoredOwners);
            if (!persistRecordIfChanged()) return false;
        }
    }

    const std::string alexaApp = "amazon.alexa.view";
    Json blockRecord = record["blocks"];
    if (!blockRecord.isObject()) blockRecord = Json::object();
    const bool recordedBlocked = blockRecord[alexaApp].boolean();
    const bool wantsBlocked = map.has("alexa");
    const bool alexaAvailable = replies[3].error.empty() &&
        replies[3].response["returnValue"].boolean();
    const bool blockAlexa = wantsBlocked && (!recordedBlocked || alexaAvailable);
    const bool releaseAlexa = !wantsBlocked && recordedBlocked;
    if (blockAlexa || releaseAlexa) {
        Json nextBlocks = Json::object();
        for (const std::string &name : blockRecord.keys())
            if (name != alexaApp) nextBlocks.set(name, blockRecord[name]);
        if (blockAlexa) nextBlocks.set(alexaApp, Json::flag(true));
        if (blockAlexa) {
            record.set("blocks", nextBlocks);
            if (!persistRecordIfChanged()) return false;
        }
        Json blockPayload = Json::object();
        blockPayload.set("id", Json::text(alexaApp));
        if (blockAlexa)
            blockPayload.set("forceBlockSystemApp", Json::flag(false));
        LunaReply blockReply;
        const bool answered = waitForCommandPolicyLuna(
            luna_, commandPolicy_, controlPolicyContext(config_),
            blockAlexa ? "remoteKeys.alexa-block" : "remoteKeys.alexa-release",
            blockAlexa ? "com.webos.applicationManager/blockSystemApp"
                       : "com.webos.applicationManager/releaseBlockSystemApp",
            blockPayload, "remote-keys", false, blockReply, alexaApp, false,
            std::chrono::milliseconds(5000), false);
        std::string blockError = blockReply.response["errorText"].string();
        std::transform(blockError.begin(), blockError.end(), blockError.begin(),
            [](unsigned char character) {
                return static_cast<char>(std::tolower(character));
            });
        const bool alreadyBlocked = blockAlexa &&
            blockError.find("already blocked") != std::string::npos;
        if (!answered || (!blockReply.response["returnValue"].boolean() &&
                          !alreadyBlocked)) {
            error = "the TV did not take the Alexa key change";
            return false;
        }
        wrote = true;
        if (releaseAlexa) {
            record.set("blocks", nextBlocks);
            if (!persistRecordIfChanged()) return false;
        }
    }

    const CommandPolicyContext policyContext = controlPolicyContext(config_);
    const auto writeNativeValue = [this, &policyContext](
        const std::string &action, const std::string &uri,
        const Json &payload) {
        LunaReply reply;
        return waitForCommandPolicyLuna(
            luna_, commandPolicy_, policyContext, action, uri, payload,
            "remote-keys", false, reply, std::string(), false,
            std::chrono::milliseconds(5000), false) &&
            reply.response["returnValue"].boolean();
    };
    if (!settings.keys().empty()) {
        Json write = Json::object();
        write.set("category", Json::text("other"));
        write.set("settings", settings);
        if (!writeNativeValue("remoteKeys.native-settings",
                "com.webos.settingsservice/setSystemSettings", write)) {
            error = "the TV did not take the native remote-key settings";
            return false;
        }
        wrote = true;
    }
    if (!writes.actionValues.keys().empty()) {
        Json write = Json::object();
        write.set("configs", writes.actionValues);
        if (!writeNativeValue("remoteKeys.native-actions",
                "com.webos.service.config/setConfigs", write)) {
            error = "the TV did not take the native remote-key actions";
            return false;
        }
        wrote = true;
    }
    if (!persistRecordIfChanged()) return false;
    if (wrote && logger_)
        logger_->info("remotekeys: restored configured native key settings");
    return true;
}

void HttpFrontDoor::setBackgroundService(HttpExchange &exchange) const {
    const HttpRequest &request = exchange.request();
    if (!config_.allowControl) {
        sendSetupError(exchange, 403, say::msg("srv.controlsOff", "controls disabled in config"));
        return;
    }
    if (!setupJsonContentType(request)) {
        sendSetupError(exchange, 415, "Content-Type must be application/json");
        return;
    }
    if (rejectCrossSitePost(exchange)) return;
    if (!exchange.setBodyLimit(8192)) {
        sendSetupError(exchange, 413, "request body too large", true);
        return;
    }
    std::string body;
    char buffer[1024];
    for (;;) {
        const int count = exchange.readBody(buffer, sizeof(buffer));
        if (count == 0) break;
        if (count < 0) {
            sendSetupError(exchange, exchange.bodyLimitExceeded() ? 413 : 400,
                           exchange.bodyLimitExceeded() ? "request body too large"
                                                        : "invalid request body",
                           true);
            return;
        }
        body.append(buffer, static_cast<size_t>(count));
    }
    if (exceedsUtf16CodeUnitLimit(body, 8192)) {
        sendSetupError(exchange, 413, "request body too large", true);
        return;
    }
    const Json payload = Json::parse(body);
    if (!payload.isObject() || !payload["id"].isString() ||
        !payload["disabled"].isBoolean()) {
        sendSetupError(exchange, 400, "service id and disabled flag expected");
        return;
    }
    const std::string id = payload["id"].string();
    const bool disabled = payload["disabled"].boolean();
    service::BackgroundServiceInfo info;
    if (!service::backgroundServiceInfo(id, systemRoot_, info)) {
        Json failure = Json::object();
        failure.set("ok", Json::flag(false));
        failure.set("error", Json::text("Unknown service ID: " + id));
        (void)exchange.respond(400,
                              {{"Content-Type", "application/json; charset=utf-8"},
                               {"Cache-Control", "no-store"},
                               {"X-Content-Type-Options", "nosniff"}},
                              failure.dump());
        return;
    }
    if (!service::backgroundServicesSupported(systemRoot_)) {
        sendSetupError(exchange, 400,
                       "Service management is not supported on this platform");
        return;
    }
    if (!commandPolicy_) {
        sendSetupError(exchange, 503, "service policy is unavailable");
        return;
    }

    CommandPolicySpec localCommand;
    localCommand.action = "background-service-toggle";
    localCommand.uri = "native/services/toggle";
    localCommand.payload = Json::object();
    localCommand.payload.set("id", Json::text(id));
    localCommand.payload.set("disabled", Json::flag(disabled));
    localCommand.timeout = std::chrono::milliseconds(60000);
    localCommand.requiresControl = false;
    localCommand.resource = "background-services";
    const CommandPolicyContext context = controlPolicyContext(config_);
    Json result = Json::object();
    std::string operationError;
    LunaReply localReply;
    const CommandDecision decision = commandPolicy_->executeLocal(
        localCommand, context, false,
        [this, &info, &id, disabled, &result, &operationError]() {
            return applyBackgroundServiceState(info, id, disabled, result,
                                               operationError);
        }, localReply);
    const bool succeeded = decision.kind == CommandDecisionKind::Allowed &&
        localReply.response["returnValue"].isBoolean() &&
        localReply.response["returnValue"].boolean();
    if (!succeeded) {
        if (!result.isObject()) result = Json::object();
        result.set("ok", Json::flag(false));
        const std::string error = !operationError.empty() ? operationError
            : !localReply.error.empty() ? localReply.error
            : !decision.reason.empty() ? decision.reason
            : "could not change the background service";
        result.set("error", Json::text(error));
    }
    const int status = succeeded ? 200
        : decision.kind == CommandDecisionKind::Allowed ? 400 : 503;
    (void)exchange.respond(status,
                           {{"Content-Type", "application/json; charset=utf-8"},
                            {"Cache-Control", "no-store"},
                            {"X-Content-Type-Options", "nosniff"}},
                           result.dump());
}

Json HttpFrontDoor::collectLgSettingsData(
    const std::vector<std::string> &sections) const {
    Json settingsByCategory = Json::object();
    Json descriptionsByCategory = Json::object();
    Json dimensionsByCategory = Json::object();
    const std::vector<std::string> categories = service::lgSettingsCategories(sections);
    if (categories.empty()) {
        Json result = Json::object();
        result.set("settings", settingsByCategory);
        result.set("descriptions", descriptionsByCategory);
        result.set("dimensions", dimensionsByCategory);
        return result;
    }

    std::lock_guard<std::mutex> cacheLock(lgSettingsCacheMutex_);
    std::lock_guard<std::mutex> settingsCommandLock(systemSettingsControlMutex_);
    for (const std::string &category : categories) {
        const auto cachedAt = lgSettingsCategoryCachedAt_.find(category);
        const bool fresh = cachedAt != lgSettingsCategoryCachedAt_.end() &&
            std::chrono::steady_clock::now() - cachedAt->second <
                std::chrono::milliseconds(120000);
        if (!fresh) {
            Json request = Json::object();
            request.set("category", Json::text(category));
            LunaReply reply;
            const bool answered = waitForLuna(
                luna_, "com.webos.service.settings/getSystemSettings", request, reply);
            const Json returnValue = reply.response["returnValue"];
            Json settings = Json::object();
            if (answered && !(returnValue.isBoolean() && !returnValue.boolean()) &&
                reply.response["settings"].isObject())
                settings = reply.response["settings"];

            const Json dimension = reply.response["dimension"];
            const bool dimensionTruthy = dimension.isObject() || dimension.isArray() ||
                (dimension.isString() && !dimension.string().empty()) ||
                (dimension.isNumber() && dimension.number() != 0) ||
                (dimension.isBoolean() && dimension.boolean());
            lgSettingsCacheSettings_.set(category, settings);
            lgSettingsCacheDescriptions_.set(category, Json::array());
            lgSettingsCacheDescriptionCoverage_.set(category, Json::object());
            lgSettingsCacheDimensions_.set(
                category, answered && dimensionTruthy ? dimension : Json());
            if (answered)
                lgSettingsCategoryCachedAt_[category] =
                    std::chrono::steady_clock::now();
            else
                lgSettingsCategoryCachedAt_.erase(category);
        }

        const Json currentSettings = lgSettingsCacheSettings_[category];
        Json coveredKeys = lgSettingsCacheDescriptionCoverage_[category];
        if (!coveredKeys.isObject())
            coveredKeys = Json::object();
        Json missingKeys = Json::array();
        for (const std::string &key : service::lgSettingsKeys(category, sections))
            if (currentSettings.has(key) && !coveredKeys.has(key))
                missingKeys.add(Json::text(key));
        if (missingKeys.size()) {
            Json descriptionRequest = Json::object();
            descriptionRequest.set("category", Json::text(category));
            descriptionRequest.set("keys", missingKeys);
            LunaReply descriptionReply;
            if (waitForLuna(luna_, "com.webos.service.settings/getSystemSettingDesc",
                            descriptionRequest, descriptionReply)) {
                const Json oldRows = lgSettingsCacheDescriptions_[category];
                Json mergedRows = Json::array();
                if (oldRows.isArray()) {
                    for (const Json &item : oldRows.items()) {
                        bool replaced = false;
                        for (const Json &key : missingKeys.items())
                            if (item["key"].string() == key.string()) {
                                replaced = true;
                                break;
                            }
                        if (!replaced)
                            mergedRows.add(item);
                    }
                }
                if (descriptionReply.response["results"].isArray())
                    for (const Json &item : descriptionReply.response["results"].items())
                        mergedRows.add(item);
                lgSettingsCacheDescriptions_.set(category, mergedRows);

                Json updatedCoverage = Json::object();
                for (const std::string &key : coveredKeys.keys())
                    updatedCoverage.set(key, Json::flag(true));
                for (const Json &key : missingKeys.items())
                    updatedCoverage.set(key.string(), Json::flag(true));
                lgSettingsCacheDescriptionCoverage_.set(category, updatedCoverage);
            }
        }

        settingsByCategory.set(category, lgSettingsCacheSettings_[category]);
        descriptionsByCategory.set(category, lgSettingsCacheDescriptions_[category]);
        const Json dimension = lgSettingsCacheDimensions_[category];
        const bool dimensionTruthy = dimension.isObject() || dimension.isArray() ||
            (dimension.isString() && !dimension.string().empty()) ||
            (dimension.isNumber() && dimension.number() != 0) ||
            (dimension.isBoolean() && dimension.boolean());
        if (dimensionTruthy)
            dimensionsByCategory.set(category, dimension);
    }

    Json result = Json::object();
    result.set("settings", settingsByCategory);
    result.set("descriptions", descriptionsByCategory);
    result.set("dimensions", dimensionsByCategory);
    return result;
}

Json HttpFrontDoor::lgSettingsInputNames() const {
    {
        std::lock_guard<std::mutex> lock(inputNameCacheMutex_);
        if (inputNameCacheValid_ && inputNameCache_.keys().size() > 0 &&
            std::chrono::steady_clock::now() - inputNameCachedAt_ <
                std::chrono::seconds(60))
            return inputNameCache_;
    }

    LunaReply reply;
    if (waitForLuna(luna_, "com.webos.service.eim/getAllInputStatus",
                    Json::object(), reply) && reply.response["devices"].isArray() &&
        reply.response["devices"].size() > 0) {
        const Json scanned = service::lgSettingsInputNames(reply.response["devices"]);
        std::lock_guard<std::mutex> lock(inputNameCacheMutex_);
        Json merged = Json::object();
        for (const std::string &key : inputNameCache_.keys())
            merged.set(key, inputNameCache_[key]);
        for (const std::string &key : scanned.keys())
            merged.set(key, scanned[key]);
        inputNameCache_ = merged;
        inputNameCachedAt_ = std::chrono::steady_clock::now();
        inputNameCacheValid_ = true;
        return inputNameCache_;
    }
    std::lock_guard<std::mutex> lock(inputNameCacheMutex_);
    return inputNameCache_.isObject() ? inputNameCache_ : Json::object();
}

void HttpFrontDoor::sendGameSettings(HttpExchange &exchange) const {
    const Json data = collectLgSettingsData({"game"});
    const Json settings = data["settings"];
    const Json descriptions = data["descriptions"];
    const Json dimensions = data["dimensions"];
    Json response = service::lgGameResponse(settings, descriptions, dimensions,
                                             Json::object());
    if (response["available"].boolean()) {
        Json pictureRequest = Json::object();
        pictureRequest.set("category", Json::text("picture"));
        Json pictureKeys = Json::array();
        pictureKeys.add(Json::text("pictureMode"));
        pictureRequest.set("keys", pictureKeys);
        LunaReply pictureReply;
        (void)waitForLuna(luna_, "com.webos.service.settings/getSystemSettings",
                          pictureRequest, pictureReply);
        response = service::lgGameResponse(settings, descriptions, dimensions,
                                            pictureReply.response["settings"]);
    }
    sendMetricsJson(exchange, response.dump());
}

void HttpFrontDoor::sendGameFrameRate(HttpExchange &exchange) const {
    const Json latest = gameFrameRate_ ? gameFrameRate_->frameRate() : Json();
    LunaReply foreground;
    (void)waitForLuna(luna_, "com.webos.applicationManager/getForegroundAppInfo",
                      Json::object(), foreground);

    Json aliases = Json::object();
    for (const Json &device : hdmiInputDevices().items()) {
        const Json key = names::hdmiKeyForPortId(device["id"].string());
        const std::string appId = device["appId"].string();
        if (!key.isString() || appId.empty())
            continue;
        const std::string shortId = names::shortAppId(appId);
        if (shortId != key.string())
            aliases.set(shortId, key);
    }
    const std::string onScreenInput = names::shortAppId(
        foreground.response["appId"].string(), aliases);
    const std::string ratePort =
        names::hdmiKeyForPortId(latest["port"].string()).string();
    const bool onScreen = !ratePort.empty() && onScreenInput == ratePort;

    Json fps = Json::object();
    if (onScreen) {
        fps.set("frameRate", latest["frameRate"]);
        fps.set("vrrType", latest["vrrType"]);
        fps.set("port", latest["port"]);
    } else {
        fps.set("frameRate", Json::numeric(0));
        fps.set("vrrType", Json::text("off"));
        fps.set("port", Json());
    }
    Json response = Json::object();
    response.set("ok", Json::flag(true));
    response.set("fps", fps);
    sendMetricsJson(exchange, response.dump());
}

void HttpFrontDoor::sendLgSettings(HttpExchange &exchange) const {
    std::string selected;
    std::vector<std::string> sections;
    if (queryParameter(exchange.request().query, "section", selected))
        sections = splitCommaSeparated(selected);
    sections.erase(std::remove(sections.begin(), sections.end(), std::string()),
                   sections.end());
    const Json data = collectLgSettingsData(sections);
    const Json response = service::lgSettingsResponse(
        sections, data["settings"], data["descriptions"], data["dimensions"],
        lgSettingsInputNames());
    sendMetricsJson(exchange, response.dump());
}

Json HttpFrontDoor::collectMqttLgSettings() const {
    const std::vector<std::string> sections = {
        "sound", "devices", "game", "promotions"
    };
    const Json data = collectLgSettingsData(sections);
    return service::lgSettingsResponse(
        sections, data["settings"], data["descriptions"], data["dimensions"],
        lgSettingsInputNames());
}

Json HttpFrontDoor::executeMqttControl(const Json &payload) const {
    const auto failure = [](const std::string &message) {
        Json result = Json::object();
        result.set("ok", Json::flag(false));
        result.set("error", Json::text(message));
        return result;
    };
    if (!config_.allowControl)
        return failure("controls disabled in config");
    if (!payload.isObject() || !payload["action"].isString() ||
        payload["action"].string().empty())
        return failure("control action is invalid");

    const std::string body = payload.dump();
    HttpRequest request;
    request.method = "POST";
    request.uri = "/api/control";
    request.version = "1.1";
    request.remoteAddress = "127.0.0.1";
    request.contentLength = static_cast<long long>(body.size());
    request.headers.push_back({"Content-Type", "application/json"});
    HttpExchange exchange(std::move(request), body);
    setControl(exchange);
    const Json result = Json::parse(exchange.responseBody());
    return result.isObject() ? result : failure(
        exchange.responseStarted() ? "control returned an invalid response"
                                   : "control did not return a response");
}

void HttpFrontDoor::invalidateLgSettingsCache() const {
    std::lock_guard<std::mutex> lock(lgSettingsCacheMutex_);
    lgSettingsCategoryCachedAt_.clear();
}

Json HttpFrontDoor::hdmiInputDevices() const {
    const auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(quickAccessInputMutex_);
        if (quickAccessInputScanComplete_ &&
            now - quickAccessInputScannedAt_ < std::chrono::seconds(60))
            return quickAccessInputDevices_;
    }

    LunaReply reply;
    if (waitForLuna(luna_, "com.webos.service.eim/getAllInputStatus",
                    Json::object(), reply) && reply.response["devices"].isArray()) {
        const Json devices = reply.response["devices"];
        bool hasHdmi = false;
        for (const Json &device : devices.items())
            if (names::hdmiKeyForPortId(device["id"].string()).isString())
                hasHdmi = true;
        if (hasHdmi) {
            std::lock_guard<std::mutex> lock(quickAccessInputMutex_);
            quickAccessInputDevices_ = devices;
            quickAccessInputScannedAt_ = now;
            quickAccessInputScanComplete_ = true;
            return quickAccessInputDevices_;
        }
    }

    std::lock_guard<std::mutex> lock(quickAccessInputMutex_);
    return quickAccessInputDevices_;
}

void HttpFrontDoor::setQuickAccess(HttpExchange &exchange) const {
    const HttpRequest &request = exchange.request();
    if (request.method != "POST") {
        sendSetupError(exchange, 405, "method not allowed");
        return;
    }
    if (!setupJsonContentType(request)) {
        sendSetupError(exchange, 415, "Content-Type must be application/json");
        return;
    }
    if (!config_.allowControl) {
        sendSetupError(exchange, 403, "controls disabled in config");
        return;
    }
    if (!exchange.setBodyLimit(8192 * 6 + 256)) {
        sendSetupError(exchange, 413, "request body too large", true);
        return;
    }
    std::string body;
    char buffer[1024];
    for (;;) {
        const int count = exchange.readBody(buffer, sizeof(buffer));
        if (count == 0)
            break;
        if (count < 0) {
            sendSetupError(exchange, exchange.bodyLimitExceeded() ? 413 : 400,
                           exchange.bodyLimitExceeded() ? "request body too large"
                                                        : "invalid request body",
                           true);
            return;
        }
        body.append(buffer, static_cast<size_t>(count));
    }
    if (exceedsUtf16CodeUnitLimit(body, 8192)) {
        sendSetupError(exchange, 413, "request body too large", true);
        return;
    }
    const Json payload = Json::parse(body);
    if (!payload.isObject()) {
        sendSetupError(exchange, 400, "malformed JSON");
        return;
    }

    long keyValue = 0;
    if (payload["key"].isString()) {
        const std::string raw = payload["key"].string();
        char *end = nullptr;
        keyValue = std::strtol(raw.c_str(), &end, 10);
        if (end == raw.c_str())
            keyValue = 0;
    } else if (payload["key"].isNumber()) {
        keyValue = static_cast<long>(payload["key"].number());
    }
    if (keyValue < 1 || keyValue > 9) {
        sendSetupError(exchange, 400, "Quick Access keys are 1 to 9");
        return;
    }

    std::string id;
    const Json requestedId = payload["id"];
    const bool falsyId = !requestedId.valid() ||
                         (requestedId.isBoolean() && !requestedId.boolean()) ||
                         (requestedId.isNumber() && requestedId.number() == 0);
    if (!falsyId) {
        id = quickAccessStringValue(requestedId);
        const size_t first = id.find_first_not_of(" \t\r\n\f\v");
        const size_t last = id.find_last_not_of(" \t\r\n\f\v");
        id = first == std::string::npos ? "" : id.substr(first, last - first + 1);
    }

    std::lock_guard<std::mutex> commandLock(quickAccessMutex_);
    // Keep LG's clear-delay-replace sequence atomic with respect to every
    // other native system-settings write.
    std::unique_lock<std::mutex> settingsCommandLock(systemSettingsControlMutex_);
    Json settingsRequest = Json::object();
    settingsRequest.set("category", Json::text("option"));
    settingsRequest.set("keys", service::quickAccessSettingKeys());
    LunaReply settingsReply;
    if (!waitForLuna(luna_, "com.webos.service.settings/getSystemSettings",
                     settingsRequest, settingsReply) ||
        !settingsReply.response["returnValue"].boolean(true) ||
        !service::quickAccessSettingsSupported(settingsReply.response["settings"])) {
        sendSetupError(exchange, 400,
                       "This TV's Quick Access cannot be changed from here");
        return;
    }

    const std::string current =
        settingsReply.response["settings"]["magicNum" + std::to_string(keyValue)]["id"].string();
    if (!id.empty()) {
        LunaReply appsReply;
        if (!waitForLuna(luna_, "com.webos.applicationManager/listApps",
                         Json::object(), appsReply) ||
            !appsReply.response["apps"].isArray() ||
            appsReply.response["apps"].size() == 0) {
            sendSetupError(exchange, 400,
                           "Could not read the TV's apps. Try again in a moment.");
            return;
        }
        const Json devices = hdmiInputDevices();
        if (!service::quickAccessTargetAllowed(id, devices,
                                              appsReply.response["apps"])) {
            sendSetupError(exchange, 400, "That app is not on this TV");
            return;
        }
    }

    const bool clearFirst = !id.empty() && !current.empty() && current != id;
    const auto write = [this, keyValue](const std::string &target,
                                        LunaReply &reply) {
        const Json writeRequest = service::quickAccessWriteRequest(
            static_cast<unsigned>(keyValue), target);
        const CommandPolicyContext context = controlPolicyContext(config_);
        return waitForCommandPolicyLuna(
                   luna_, commandPolicy_, context, "quickaccess.write",
                   "com.webos.service.settings/setSystemSettings", writeRequest,
                   "system-settings", false, reply) &&
               reply.response["returnValue"].boolean();
    };
    LunaReply writeReply;
    if (clearFirst && !write("", writeReply)) {
        sendSetupError(exchange, 400, "The TV did not save the change");
        return;
    }
    if (clearFirst) {
        if (logger_)
            logger_->info("quickaccess: key " + std::to_string(keyValue) +
                          " cleared before replacement");
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    if (!write(id, writeReply)) {
        sendSetupError(exchange, 400, "The TV did not save the change");
        return;
    }
    if (logger_)
        logger_->info("quickaccess: key " + std::to_string(keyValue) +
                      (id.empty() ? " cleared" : " opens " + id));
    settingsCommandLock.unlock();
    invalidateLgSettingsCache();
    (void)exchange.respond(200,
                           {{"Content-Type", "application/json"},
                            {"Cache-Control", "no-store"},
                            {"X-Content-Type-Options", "nosniff"}},
                           "{\"ok\":true}");
}

void HttpFrontDoor::setRemoteKey(HttpExchange &exchange) const {
    const HttpRequest &request = exchange.request();
    if (request.method != "POST") {
        sendSetupError(exchange, 405, "method not allowed");
        return;
    }
    if (!setupJsonContentType(request)) {
        sendSetupError(exchange, 415, "Content-Type must be application/json");
        return;
    }
    if (!config_.allowControl) {
        sendSetupError(exchange, 403, "controls disabled in config");
        return;
    }
    if (rejectCrossSitePost(exchange)) return;
    if (!exchange.setBodyLimit(8192 * 6 + 256)) {
        sendSetupError(exchange, 413, "request body too large", true);
        return;
    }
    std::string body;
    char buffer[1024];
    for (;;) {
        const int count = exchange.readBody(buffer, sizeof(buffer));
        if (count == 0)
            break;
        if (count < 0) {
            sendSetupError(exchange, exchange.bodyLimitExceeded() ? 413 : 400,
                           exchange.bodyLimitExceeded() ? "request body too large"
                                                        : "invalid request body",
                           true);
            return;
        }
        body.append(buffer, static_cast<size_t>(count));
    }
    if (exceedsUtf16CodeUnitLimit(body, 8192)) {
        sendSetupError(exchange, 413, "request body too large", true);
        return;
    }
    const Json payload = Json::parse(body);
    if (!payload.isObject()) {
        sendSetupError(exchange, 400, "malformed JSON");
        return;
    }

    const std::string key = payload["key"].string();
    const std::string action = payload["action"].string();
    std::string id = payload["id"].valid()
        ? service::javascriptString(payload["id"]) : std::string();
    const size_t first = id.find_first_not_of(" \t\r\n\f\v");
    const size_t last = id.find_last_not_of(" \t\r\n\f\v");
    id = first == std::string::npos ? "" : id.substr(first, last - first + 1);

    if (!service::hasJavaScriptRemoteKeys(systemRoot_)) {
        std::lock_guard<std::mutex> lock(remoteKeysMutex_);
        Json configNames = Json::array();
        configNames.add(Json::text("com.webos.surfacemanager.keyFiltersCpp"));
        configNames.add(Json::text("com.webos.keyaction.tvGuide"));
        configNames.add(Json::text("com.webos.keyaction.inputHub"));
        Json configQuery = Json::object();
        configQuery.set("configNames", configNames);
        Json mappingRequest = Json::object();
        mappingRequest.set("category", Json::text("other"));
        Json mappingKeys = Json::array();
        mappingKeys.add(Json::text("mapping_info"));
        mappingRequest.set("keys", mappingKeys);
        Json buttonRequest = Json::object();
        buttonRequest.set("category", Json::text("other"));
        Json buttonKeys = Json::array();
        buttonKeys.add(Json::text("customShortPress"));
        buttonKeys.add(Json::text("customLongPress"));
        buttonRequest.set("keys", buttonKeys);
        std::vector<std::pair<std::string, Json>> nativeCalls = {
            {"com.webos.service.config/getConfigs", configQuery},
            {"com.webos.settingsservice/getSystemSettings", mappingRequest},
            {"com.webos.settingsservice/getSystemSettings", buttonRequest},
            {"com.webos.applicationManager/listApps", Json::object()},
            {"com.webos.service.eim/getAllInputStatus", Json::object()},
            {"com.webos.applicationManager/getAppInfo",
             Json::parse("{\"id\":\"com.webos.app.quickinputpicker\"}")},
            {"com.webos.applicationManager/getAppInfo",
             Json::parse("{\"id\":\"amazon.alexa.view\"}")}
        };
        std::vector<LunaReply> nativeReplies;
        (void)waitForLunaBatch(luna_, nativeCalls, nativeReplies);
        if (nativeReplies.size() != nativeCalls.size() ||
            !nativeReplies[0].error.empty() ||
            !nativeReplies[0].response["returnValue"].boolean() ||
            !nativeReplies[0].response["configs"][
                "com.webos.surfacemanager.keyFiltersCpp"].isArray() ||
            nativeReplies[0].response["configs"][
                "com.webos.surfacemanager.keyFiltersCpp"].size() == 0) {
            sendSetupError(exchange, 400, say::msg("srv.remoteKeys.badKey",
                                               "This TV does not have that key"));
            return;
        }

        service::NativeRemoteKeysReadState nativeState;
        nativeState.dataRoot = dataRoot_;
        nativeState.filterChain = nativeReplies[0].response["configs"][
            "com.webos.surfacemanager.keyFiltersCpp"];
        nativeState.mappingInfo = Json::array();
        if (nativeReplies[1].error.empty() &&
            nativeReplies[1].response["settings"]["mapping_info"].isArray())
            nativeState.mappingInfo =
                nativeReplies[1].response["settings"]["mapping_info"];
        nativeState.actions = Json::object();
        for (const char *name : {"tvGuide", "inputHub"}) {
            const Json value = nativeReplies[0].response["configs"][
                std::string("com.webos.keyaction.") + name];
            if (value.isObject()) nativeState.actions.set(name, value);
        }
        nativeState.buttonSettings = Json::object();
        if (nativeReplies[2].error.empty()) {
            for (const char *name : {"customShortPress", "customLongPress"}) {
                const Json value = nativeReplies[2].response["settings"][name];
                if (value.isObject() && value["id"].isString())
                    nativeState.buttonSettings.set(name, value);
            }
        }
        nativeState.appsReply = nativeReplies[3].error.empty()
            ? nativeReplies[3].response : Json::object();
        nativeState.inputDevices = nativeReplies[4].error.empty()
            ? nativeReplies[4].response["devices"] : Json::array();
        nativeState.inputSwitcherAvailable =
            nativeReplies[5].error.empty() &&
            nativeReplies[5].response["returnValue"].boolean();
        nativeState.voiceConductorAvailable =
            service::backgroundHiddenServiceAvailable("voiceconductor", systemRoot_);
        nativeState.alexaViewAvailable = nativeReplies[6].error.empty() &&
            nativeReplies[6].response["returnValue"].boolean();
        const Json availableKeys = service::nativeRemoteKeysReadResponse(nativeState);
        if (!availableKeys["supported"].boolean()) {
            sendSetupError(exchange, 400, say::msg("srv.remoteKeys.badKey",
                                               "This TV does not have that key"));
            return;
        }

        Json keyRow;
        for (const Json &row : availableKeys["keys"].items())
            if (row["key"].string() == key) keyRow = row;
        if (!keyRow.valid()) {
            sendSetupError(exchange, 400, "This TV does not have that key");
            return;
        }
        if (action != "lg" && action != "nothing" && action != "open") {
            sendSetupError(exchange, 400, "lg, nothing or open expected");
            return;
        }
        if (keyRow["noNothing"].boolean() && action == "nothing") {
            sendSetupError(exchange, 400, "This button cannot be set to do nothing");
            return;
        }
        if (action == "open" && id.empty()) {
            sendSetupError(exchange, 400, "lg, nothing or open expected");
            return;
        }
        if (action == "open") {
            bool validTarget = false;
            for (const Json &choice : availableKeys["choices"].items())
                if (choice["id"].string() == id) validTarget = true;
            if (!validTarget) {
                sendSetupError(exchange, 400, "That app is not on this TV");
                return;
            }
        }

        const std::string mapPath = dataFilePath(dataRoot_, "remotekeys.json");
        JsonFile mapStore(mapPath, 64 * 1024);
        const Json loadedDocument = mapStore.load().value;
        const Json previousDocument = loadedDocument.valid()
            ? Json::parse(loadedDocument.dump()) : Json();
        Json document = loadedDocument;
        if (!document.isObject()) document = Json::object();
        const Json previousKeys = document["keys"];
        Json updatedKeys = Json::object();
        for (const std::string &name : previousKeys.keys()) {
            if (name != key && previousKeys[name].isString())
                updatedKeys.set(name, previousKeys[name]);
        }
        if (action == "nothing") updatedKeys.set(key, Json::text(""));
        else if (action == "open") updatedKeys.set(key, Json::text(id));
        document.set("keys", updatedKeys);
        document.set("updated", Json::text(utcIsoTimestamp()));

        service::NativeRemoteKeysWriteState writeState;
        writeState.mappingInfo = nativeState.mappingInfo;
        writeState.actions = nativeState.actions;
        writeState.buttonSettings = nativeState.buttonSettings;
        JsonFile lgStore(dataFilePath(dataRoot_, "remotekeys-lg.json"), 256 * 1024);
        const JsonFileResult previousLgFile = lgStore.load();
        const Json previousLgRecord = previousLgFile.value.isObject()
            ? Json::parse(previousLgFile.value.dump()) : Json();
        writeState.lgRecord = previousLgFile.value;
        service::NativeRemoteKeysWriteResult writes =
            service::nativeRemoteKeysWriteRequests(updatedKeys, writeState);
        Json settings = Json::object();
        if (writes.mappingChanged)
            settings.set("mapping_info", writes.mappingInfo);
        for (const std::string &name : writes.buttonSettings.keys())
            settings.set(name, writes.buttonSettings[name]);

        std::string fileError;
        const auto restoreMapFile = [this, &mapStore, &mapPath, &previousDocument]() {
            std::string ignored;
            if (previousDocument.isObject()) {
                if (!mapStore.save(previousDocument, &ignored) && logger_)
                    logger_->error("remotekeys: could not roll back the key map: " +
                                   ignored);
            } else if (unlink(mapPath.c_str()) != 0 && errno != ENOENT && logger_) {
                logger_->error("remotekeys: could not remove the failed key map");
            }
        };
        const auto restoreLgFile = [this, &lgStore, &previousLgRecord]() {
            std::string ignored;
            if (previousLgRecord.isObject()) {
                if (!lgStore.save(previousLgRecord, &ignored) && logger_)
                    logger_->error("remotekeys: could not roll back LG key settings: " +
                                   ignored);
            } else if (unlinkJsonFile(lgStore.path()) != 0 && errno != ENOENT && logger_) {
                logger_->error("remotekeys: could not remove failed LG key settings");
            }
        };
        if (!mapStore.save(document, &fileError)) {
            sendSetupError(exchange, 500, "Could not save the change" +
                (fileError.empty() ? std::string() : ": " + fileError));
            return;
        }
        bool hasLgValues =
            service::nativeRemoteKeysLgRecordHasValues(writes.lgRecord);
        if (hasLgValues && !lgStore.save(writes.lgRecord, &fileError)) {
            restoreMapFile();
            sendSetupError(exchange, 500, "Could not save LG key settings" +
                (fileError.empty() ? std::string() : ": " + fileError));
            return;
        }

        const CommandPolicyContext policyContext = controlPolicyContext(config_);
        const auto writeNativeValue = [this, &policyContext](
            const std::string &actionName, const std::string &uri,
            const Json &write) {
            LunaReply reply;
            return waitForCommandPolicyLuna(
                luna_, commandPolicy_, policyContext, actionName, uri, write,
                "remote-keys", false, reply) &&
                reply.response["returnValue"].boolean();
        };

        // The AI button is consumed before evdev. Own voiceconductor's
        // disabled-list entry and service state alongside its app mapping,
        // restoring it only when this record says nativeTv switched it off.
        const std::string voiceService = "voiceconductor";
        Json serviceOwners = writes.lgRecord["services"];
        if (!serviceOwners.isObject()) serviceOwners = Json::object();
        const bool ownsVoiceService = serviceOwners[voiceService].boolean();
        const bool wantsVoiceServiceOff = updatedKeys.has("ai");
        bool voiceServiceDisabled = false;
        if (wantsVoiceServiceOff || ownsVoiceService) {
            std::vector<std::string> disabledServices;
            if (!service::readDisabledBackgroundServices(dataRoot_, disabledServices,
                                                          &fileError)) {
                restoreMapFile();
                restoreLgFile();
                sendSetupError(exchange, 500, fileError.empty()
                    ? "Could not read disabled services" : fileError);
                return;
            }
            voiceServiceDisabled = std::find(disabledServices.begin(),
                disabledServices.end(), voiceService) != disabledServices.end();
        }
        const bool disableVoiceService = wantsVoiceServiceOff &&
            (!ownsVoiceService || !voiceServiceDisabled);
        const bool enableVoiceService = !wantsVoiceServiceOff && ownsVoiceService;
        if (disableVoiceService || enableVoiceService) {
            service::BackgroundServiceInfo voiceInfo;
            if (!service::backgroundHiddenServiceDefinition(voiceService, voiceInfo)) {
                restoreMapFile();
                restoreLgFile();
                sendSetupError(exchange, 400, "The AI voice service is unavailable");
                return;
            }
            if (disableVoiceService) {
                serviceOwners.set(voiceService, Json::flag(true));
                writes.lgRecord.set("services", serviceOwners);
                fileError.clear();
                if (!lgStore.save(writes.lgRecord, &fileError)) {
                    restoreMapFile();
                    restoreLgFile();
                    sendSetupError(exchange, 500, "Could not save LG service state" +
                        (fileError.empty() ? std::string() : ": " + fileError));
                    return;
                }
                hasLgValues = true;
            }

            CommandPolicySpec serviceCommand;
            serviceCommand.action = disableVoiceService
                ? "remoteKeys.voice-service-disable"
                : "remoteKeys.voice-service-restore";
            serviceCommand.uri = "native/remote-keys/voiceconductor";
            serviceCommand.payload = Json::object();
            serviceCommand.payload.set("id", Json::text(voiceService));
            serviceCommand.payload.set("disabled",
                                       Json::flag(disableVoiceService));
            serviceCommand.timeout = std::chrono::milliseconds(60000);
            serviceCommand.requiresControl = false;
            serviceCommand.resource = "background-services";
            Json serviceResult = Json::object();
            std::string serviceError;
            LunaReply serviceReply;
            const CommandDecision serviceDecision = commandPolicy_
                ? commandPolicy_->executeLocal(serviceCommand, policyContext, false,
                    [this, &voiceInfo, &voiceService, disableVoiceService,
                     &serviceResult, &serviceError]() {
                        return applyBackgroundServiceState(voiceInfo, voiceService,
                            disableVoiceService, serviceResult, serviceError);
                    }, serviceReply)
                : CommandDecision{CommandDecisionKind::TransportUnavailable,
                                  "service policy is unavailable"};
            if (serviceDecision.kind != CommandDecisionKind::Allowed ||
                !serviceReply.response["returnValue"].boolean()) {
                restoreMapFile();
                restoreLgFile();
                const std::string reason = !serviceError.empty() ? serviceError
                    : !serviceDecision.reason.empty() ? serviceDecision.reason
                    : "the TV did not take the service change";
                sendSetupError(exchange,
                    serviceDecision.kind == CommandDecisionKind::Allowed ? 400 : 503,
                    reason);
                return;
            }

            if (enableVoiceService) {
                Json restoredOwners = Json::object();
                for (const std::string &name : serviceOwners.keys())
                    if (name != voiceService)
                        restoredOwners.set(name, serviceOwners[name]);
                writes.lgRecord.set("services", restoredOwners);
                fileError.clear();
                if (service::nativeRemoteKeysLgRecordHasValues(writes.lgRecord)) {
                    if (!lgStore.save(writes.lgRecord, &fileError)) {
                        restoreMapFile();
                        restoreLgFile();
                        sendSetupError(exchange, 500,
                            "Could not clear saved LG service state" +
                            (fileError.empty() ? std::string() : ": " + fileError));
                        return;
                    }
                } else if (unlinkJsonFile(lgStore.path()) != 0 && errno != ENOENT) {
                    restoreMapFile();
                    restoreLgFile();
                    sendSetupError(exchange, 500,
                                   "Could not clear saved LG service state");
                    return;
                }
                hasLgValues = service::nativeRemoteKeysLgRecordHasValues(
                    writes.lgRecord);
            }
        }

        // Alexa is opened by a system adapter rather than the native key map.
        // Keep the adapter blocked while nativeTv owns that key, and restore
        // it only when this daemon's LG record says nativeTv blocked it.
        const std::string alexaApp = "amazon.alexa.view";
        Json blockRecord = writes.lgRecord["blocks"];
        if (!blockRecord.isObject()) blockRecord = Json::object();
        const bool recordedBlocked = blockRecord[alexaApp].boolean();
        const bool wantsBlocked = updatedKeys.has("alexa");
        const bool alexaAppAvailable = nativeReplies[6].error.empty() &&
            nativeReplies[6].response["returnValue"].boolean();
        const bool blockAlexa = wantsBlocked &&
            (!recordedBlocked || alexaAppAvailable);
        const bool releaseAlexa = !wantsBlocked && recordedBlocked;
        if (blockAlexa || releaseAlexa) {
            Json nextBlocks = Json::object();
            for (const std::string &name : blockRecord.keys())
                if (name != alexaApp) nextBlocks.set(name, blockRecord[name]);
            if (blockAlexa) nextBlocks.set(alexaApp, Json::flag(true));

            if (blockAlexa) {
                writes.lgRecord.set("blocks", nextBlocks);
                fileError.clear();
                if (!lgStore.save(writes.lgRecord, &fileError)) {
                    restoreMapFile();
                    restoreLgFile();
                    sendSetupError(exchange, 500, "Could not save LG key settings" +
                        (fileError.empty() ? std::string() : ": " + fileError));
                    return;
                }
                hasLgValues = true;
            }

            Json blockPayload = Json::object();
            blockPayload.set("id", Json::text(alexaApp));
            const std::string uri = blockAlexa
                ? "com.webos.applicationManager/blockSystemApp"
                : "com.webos.applicationManager/releaseBlockSystemApp";
            if (blockAlexa)
                blockPayload.set("forceBlockSystemApp", Json::flag(false));
            LunaReply blockReply;
            const bool answered = waitForCommandPolicyLuna(
                luna_, commandPolicy_, policyContext,
                blockAlexa ? "remoteKeys.alexa-block" : "remoteKeys.alexa-release",
                uri, blockPayload, "remote-keys", false, blockReply, alexaApp);
            std::string blockError = blockReply.response["errorText"].string();
            std::transform(blockError.begin(), blockError.end(), blockError.begin(),
                [](unsigned char character) {
                    return static_cast<char>(std::tolower(character));
                });
            const bool alreadyBlocked = blockAlexa &&
                blockError.find("already blocked") != std::string::npos;
            if (!answered || (!blockReply.response["returnValue"].boolean() &&
                              !alreadyBlocked)) {
                restoreMapFile();
                restoreLgFile();
                sendSetupError(exchange, 400, "The TV did not take the change");
                return;
            }

            if (releaseAlexa) {
                writes.lgRecord.set("blocks", nextBlocks);
                fileError.clear();
                if (service::nativeRemoteKeysLgRecordHasValues(writes.lgRecord)) {
                    if (!lgStore.save(writes.lgRecord, &fileError)) {
                        restoreLgFile();
                        sendSetupError(exchange, 500,
                            "Could not clear saved LG key settings" +
                            (fileError.empty() ? std::string() : ": " + fileError));
                        return;
                    }
                } else if (unlinkJsonFile(lgStore.path()) != 0 && errno != ENOENT) {
                    restoreLgFile();
                    sendSetupError(exchange, 500, "Could not clear saved LG key settings");
                    return;
                }
                hasLgValues = service::nativeRemoteKeysLgRecordHasValues(writes.lgRecord);
            }
        }
        if (!settings.keys().empty()) {
            Json write = Json::object();
            write.set("category", Json::text("other"));
            write.set("settings", settings);
            if (!writeNativeValue("remoteKeys.native-settings",
                    "com.webos.settingsservice/setSystemSettings", write)) {
                sendSetupError(exchange, 400, "The TV did not take the change");
                return;
            }
        }
        if (!writes.actionValues.keys().empty()) {
            Json write = Json::object();
            write.set("configs", writes.actionValues);
            if (!writeNativeValue("remoteKeys.native-actions",
                    "com.webos.service.config/setConfigs", write)) {
                sendSetupError(exchange, 400, "The TV did not take the change");
                return;
            }
        }

        if (!hasLgValues &&
            unlinkJsonFile(dataFilePath(dataRoot_, "remotekeys-lg.json")) != 0 &&
            errno != ENOENT) {
            sendSetupError(exchange, 500, "Could not clear saved LG key settings");
            return;
        }
        if (logger_)
            logger_->info("remotekeys: " + key +
                (action == "lg" ? " restored to LG" : action == "nothing"
                    ? " set to do nothing" : " opens " + id));
        (void)exchange.respond(200,
            {{"Content-Type", "application/json"}, {"Cache-Control", "no-store"}},
            "{\"ok\":true}");
        return;
    }

    // The JavaScript filter takes Alexa like any other key it names
    // (WebOS.Key_webOS_ALEXA); the log follower and the app block are webOS 11's.
    std::lock_guard<std::mutex> lock(remoteKeysMutex_);

    Json configNames = Json::array();
    configNames.add(Json::text("com.webos.surfacemanager.keyFilters"));
    Json configQuery = Json::object();
    configQuery.set("configNames", configNames);
    std::vector<std::pair<std::string, Json>> calls = {
        {"com.webos.service.config/getConfigs", configQuery},
        {"com.webos.applicationManager/listApps", Json::object()},
        {"com.webos.service.eim/getAllInputStatus", Json::object()},
        {"com.webos.applicationManager/getAppInfo",
         Json::parse("{\"id\":\"com.webos.app.quickinputpicker\"}")}
    };
    std::vector<LunaReply> replies;
    (void)waitForLunaBatch(luna_, calls, replies);
    if (replies.size() != calls.size() || !replies[0].error.empty() ||
        !replies[0].response.isObject() ||
        !replies[0].response["returnValue"].boolean() ||
        !replies[0].response["configs"]["com.webos.surfacemanager.keyFilters"].isArray()) {
        sendSetupError(exchange, 400, say::msg("srv.remoteKeys.badKey",
                                               "This TV does not have that key"));
        return;
    }

    service::RemoteKeysReadState state;
    state.systemRoot = systemRoot_;
    state.dataRoot = dataRoot_;
    state.filterChain = replies[0].response["configs"][
        "com.webos.surfacemanager.keyFilters"];
    state.appsReply = replies[1].error.empty() && replies[1].response.isObject()
        ? replies[1].response : Json::object();
    state.inputDevices = replies[2].error.empty() && replies[2].response.isObject()
        ? replies[2].response["devices"] : Json::array();
    state.inputSwitcherAvailable = replies[3].error.empty() &&
        replies[3].response.isObject() && replies[3].response["returnValue"].boolean();
    const Json remoteKeys = service::remoteKeysReadResponse(state);
    if (!remoteKeys["supported"].boolean()) {
        sendSetupError(exchange, 400, say::msg("srv.remoteKeys.badKey",
                                               "This TV does not have that key"));
        return;
    }

    bool offered = false;
    for (const Json &row : remoteKeys["keys"].items())
        if (row["key"].string() == key) offered = true;
    if (!offered) {
        sendSetupError(exchange, 400, "This TV does not have that key");
        return;
    }
    if (action != "lg" && action != "nothing" && action != "open") {
        sendSetupError(exchange, 400, "lg, nothing or open expected");
        return;
    }
    if (action == "open" && id.empty()) {
        sendSetupError(exchange, 400, "lg, nothing or open expected");
        return;
    }
    bool validTarget = action != "open";
    if (action == "open") {
        for (const Json &choice : remoteKeys["choices"].items())
            if (choice["id"].string() == id) validTarget = true;
        if (!validTarget) {
            sendSetupError(exchange, 400, "That app is not on this TV");
            return;
        }
    }

    const std::string mapPath = dataFilePath(dataRoot_, "remotekeys.json");
    JsonFile mapStore(mapPath, 64 * 1024);
    Json document = mapStore.load().value;
    if (!document.isObject()) document = Json::object();
    const Json previousKeys = document["keys"];
    Json updatedKeys = Json::object();
    for (const std::string &name : previousKeys.keys()) {
        const Json value = previousKeys[name];
        if (name != key && value.isString()) updatedKeys.set(name, value);
    }
    if (action == "nothing")
        updatedKeys.set(key, Json::text(""));
    else if (action == "open")
        updatedKeys.set(key, Json::text(id));
    document.set("keys", updatedKeys);
    document.set("updated", Json::text(utcIsoTimestamp()));
    std::string fileError;
    if (!mapStore.save(document, &fileError)) {
        sendSetupError(exchange, 500, "Could not save the change" +
                       (fileError.empty() ? std::string() : ": " + fileError));
        return;
    }

    bool inChain = false;
    Json rest = Json::array();
    const std::string filterPath = dataFilePath(dataRoot_, "remotekeys-filter.js");
    for (const Json &entry : state.filterChain.items()) {
        if (entry["file"].string() == filterPath) {
            inChain = true;
            continue;
        }
        rest.add(entry);
    }
    const bool hasHome = document["home"].isString() &&
                         !document["home"].string().empty();
    const bool hasStarting = document["starting"].isString() &&
                             !document["starting"].string().empty();
    if (updatedKeys.keys().empty() && !hasHome && !hasStarting && !inChain) {
        (void)exchange.respond(200,
                               {{"Content-Type", "application/json"},
                                {"Cache-Control", "no-store"}},
                               "{\"ok\":true}");
        return;
    }

    bool filterChanged = false;
    std::string filterError;
    if (!service::installRemoteKeysFilterAsset(
            dataFilePath(dataRoot_, "native/remotekeys-filter.js"), filterPath,
            filterChanged, &filterError)) {
        sendSetupError(exchange, 503, filterError.empty()
            ? "remote key filter asset is unavailable" : filterError);
        return;
    }

    Json configValue = Json::object();
    configValue.set("keys", updatedKeys);
    if (hasHome) configValue.set("home", document["home"]);
    if (hasStarting) configValue.set("starting", document["starting"]);
    if (hasStarting && document["splash"].boolean())
        configValue.set("splash", Json::flag(true));
    Json mapConfigs = Json::object();
    mapConfigs.set("glasshouse.remoteKeys", configValue);
    const CommandPolicyContext policyContext = controlPolicyContext(config_);
    const auto writeConfigs = [this, &policyContext](const Json &configs,
                                                     const std::string &policyAction) {
        Json write = Json::object();
        write.set("configs", configs);
        LunaReply reply;
        return waitForCommandPolicyLuna(
                   luna_, commandPolicy_, policyContext, policyAction,
                   "com.webos.service.config/setConfigs", write, "remote-keys",
                   false, reply) && reply.response["returnValue"].boolean();
    };
    if (!writeConfigs(mapConfigs, "remoteKeys.map")) {
        sendSetupError(exchange, 400, "The TV did not take the change");
        return;
    }

    Json filterEntry = Json::object();
    filterEntry.set("file", Json::text(filterPath));
    filterEntry.set("handler", Json::text("handleNativeTvKeys"));
    Json full = Json::array();
    full.add(filterEntry);
    for (const Json &entry : rest.items()) full.add(entry);
    if (!inChain) {
        Json chainConfigs = Json::object();
        chainConfigs.set("com.webos.surfacemanager.keyFilters", full);
        if (!writeConfigs(chainConfigs, "remoteKeys.chain")) {
            sendSetupError(exchange, 400, "The TV did not take the change");
            return;
        }
    } else if (filterChanged) {
        Json removeConfigs = Json::object();
        removeConfigs.set("com.webos.surfacemanager.keyFilters", rest);
        if (!writeConfigs(removeConfigs, "remoteKeys.chain-remove")) {
            sendSetupError(exchange, 400, "The TV did not take the change");
            return;
        }
        Json addConfigs = Json::object();
        addConfigs.set("com.webos.surfacemanager.keyFilters", full);
        if (!writeConfigs(addConfigs, "remoteKeys.chain-add")) {
            sendSetupError(exchange, 400, "The TV did not take the change");
            return;
        }
    }

    if (logger_)
        logger_->info("remotekeys: " + key +
            (action == "lg" ? " restored to LG" : action == "nothing"
                ? " set to do nothing" : " opens " + id));
    (void)exchange.respond(200,
                           {{"Content-Type", "application/json"},
                            {"Cache-Control", "no-store"}},
                           "{\"ok\":true}");
}

void HttpFrontDoor::setPreloadToggle(HttpExchange &exchange) const {
    const auto respond = [&exchange](int status, const Json &payload) {
        (void)exchange.respond(status,
                               {{"Content-Type", "application/json"},
                                {"Cache-Control", "no-store"},
                                {"X-Content-Type-Options", "nosniff"}},
                               payload.dump());
    };
    const auto fail = [&respond](int status, const std::string &error) {
        Json response = Json::object();
        response.set("ok", Json::flag(false));
        response.set("error", Json::text(error));
        respond(status, response);
    };
    const HttpRequest &request = exchange.request();
    if (!setupJsonContentType(request)) {
        fail(415, "Content-Type must be application/json");
        return;
    }
    if (!sameOriginRequest(request)) {
        fail(403, "cross-origin request refused");
        return;
    }
    if (!exchange.setBodyLimit(32768)) {
        sendSetupError(exchange, 413, "request body too large", true);
        return;
    }
    std::string body;
    char buffer[1024];
    for (;;) {
        const int count = exchange.readBody(buffer, sizeof(buffer));
        if (count == 0)
            break;
        if (count < 0) {
            sendSetupError(exchange, exchange.bodyLimitExceeded() ? 413 : 400,
                           exchange.bodyLimitExceeded() ? "request body too large"
                                                        : "invalid request body",
                           true);
            return;
        }
        body.append(buffer, static_cast<size_t>(count));
    }
    if (exceedsUtf16CodeUnitLimit(body, 8192)) {
        sendSetupError(exchange, 413, "request body too large", true);
        return;
    }
    const Json payload = Json::parse(body);
    if (!payload.isObject() && !payload.isArray()) {
        fail(400, "malformed JSON");
        return;
    }
    const Json requestedId = payload["id"];
    const std::string id = service::javascriptTruthy(requestedId)
                               ? service::javascriptString(requestedId)
                               : std::string();
    if (id.empty()) {
        fail(400, "This app is not offered here");
        return;
    }
    const bool off = service::javascriptTruthy(payload["off"]);

    std::lock_guard<std::mutex> commandLock(preloadMutex_);
    Json configNames = Json::array();
    configNames.add(Json::text(service::preloadConfigKey()));
    Json getRequest = Json::object();
    getRequest.set("configNames", configNames);
    LunaReply listReply;
    if (!waitForLuna(luna_, "com.webos.service.config/getConfigs", getRequest,
                     listReply) ||
        !listReply.response["configs"][service::preloadConfigKey()].isArray()) {
        fail(400, "This TV has no list of preloaded apps");
        return;
    }
    const Json currentList =
        listReply.response["configs"][service::preloadConfigKey()];
    const std::string statePath =
        dataRoot_.empty() || dataRoot_[dataRoot_.size() - 1] == '/'
            ? dataRoot_ + "preload.json" : dataRoot_ + "/preload.json";
    JsonFile stateFile(statePath);
    const JsonFileResult stateResult = stateFile.load();
    Json updatedState;
    Json updatedList;
    bool enabled = false;
    if (!service::preloadChange(stateResult.found ? stateResult.value : Json::object(),
                                currentList, id, off, updatedState, updatedList,
                                enabled)) {
        fail(400, "This app is not offered here");
        return;
    }

    const CommandPolicyContext context = controlPolicyContext(config_);
    const Json writeRequest = service::preloadConfigsRequest(updatedList);
    LunaReply writeReply;
    if (!waitForCommandPolicyLuna(
            luna_, commandPolicy_, context, "preload.persist",
            "com.webos.service.config/setConfigs", writeRequest, "preload-manager",
            false, writeReply) ||
        !writeReply.response["returnValue"].boolean()) {
        fail(400, "The TV did not save the change");
        return;
    }
    std::string saveError;
    if (!stateFile.save(updatedState, &saveError)) {
        fail(400, "could not save the setting: " + saveError);
        return;
    }

    const Json liveRequest = service::preloadPolicyRequest(id, enabled);
    LunaReply liveReply;
    // Legacy behavior reports success after the live-policy callback even if
    // this firmware refuses the transient policy; the persisted configd value
    // remains the durable setting.
    (void)waitForCommandPolicyLuna(
        luna_, commandPolicy_, context, "preload.apply",
        "com.webos.service.preloadmanager/setPreloadPolicy", liveRequest,
        "preload-manager", false, liveReply);
    if (logger_)
        logger_->info("preload: " + id +
                      (off ? " switched off" : " back to LG's setting"));
    Json response = Json::object();
    response.set("ok", Json::flag(true));
    respond(200, response);
}

void HttpFrontDoor::handleSavedPage(HttpExchange &exchange) const {
    const HttpRequest &request = exchange.request();
    if (!config_.allowControl) {
        sendSetupError(exchange, 400, say::msg("srv.controlsOff.apps", "Control is disabled in server configuration"));
        return;
    }
    if (!setupJsonContentType(request)) {
        sendSetupError(exchange, 415, "Content-Type must be application/json");
        return;
    }
    if (!exchange.setBodyLimit(32768)) {
        sendSetupError(exchange, 413, "request body too large", true);
        return;
    }
    std::string body;
    char buffer[1024];
    for (;;) {
        const int count = exchange.readBody(buffer, sizeof(buffer));
        if (count == 0) break;
        if (count < 0) {
            sendSetupError(exchange, exchange.bodyLimitExceeded() ? 413 : 400,
                           exchange.bodyLimitExceeded() ? "request body too large"
                                                        : "invalid request body",
                           true);
            return;
        }
        body.append(buffer, static_cast<size_t>(count));
    }
    if (exceedsUtf16CodeUnitLimit(body, 8192)) {
        sendSetupError(exchange, 413, "request body too large", true);
        return;
    }
    const Json payload = Json::parse(body);
    if (!payload.isObject() && !payload.isArray()) {
        sendSetupError(exchange, 400, "malformed JSON");
        return;
    }

    const bool add = request.uri == "/api/apps/add-page";
    const bool remove = request.uri == "/api/apps/remove-page";
    const Json rawTitle = payload["title"];
    std::string title = service::normalizeSavedPageTitle(
        rawTitle.valid() ? service::javascriptString(rawTitle) : std::string());
    if ((add || request.uri == "/api/apps/edit-page" ||
         request.uri == "/api/apps/rename") &&
        service::savedPageTitleLength(title) > 60) {
        sendSetupError(exchange, 400,
                       say::msg("srv.apps.nameTooLong", "names are limited to 60 characters"));
        return;
    }

    service::SavedPageTarget target;
    bool hasTarget = false;
    const Json rawAddress = payload["address"];
    if (add) {
        const std::string address = service::javascriptTruthy(rawAddress)
            ? service::javascriptString(rawAddress) : std::string();
        if (!service::savedPageTarget(address, target)) {
            sendSetupError(exchange, 400,
                           say::msg("srv.apps.badAddress",
                                    "enter a web address, such as bbc.co.uk"));
            return;
        }
        hasTarget = true;
        if (title.empty()) title = target.host;
        if (service::savedPageTitleLength(title) > 60) {
            sendSetupError(exchange, 400,
                           say::msg("srv.apps.nameTooLong",
                                    "names are limited to 60 characters"));
            return;
        }
    } else if (!remove && payload.has("address") && rawAddress.valid()) {
        const std::string address = service::javascriptString(rawAddress);
        if (!service::savedPageAddressEmpty(address)) {
            if (!service::savedPageTarget(address, target)) {
                sendSetupError(exchange, 400,
                               say::msg("srv.apps.badAddress",
                                        "enter a web address, such as bbc.co.uk"));
                return;
            }
            hasTarget = true;
        }
    }

    const Json rawLaunchPointId = payload["launchPointId"];
    const std::string launchPointId = service::javascriptTruthy(rawLaunchPointId)
        ? service::javascriptString(rawLaunchPointId) : std::string();
    if (!add && launchPointId.empty()) {
        sendSetupError(exchange, 400,
                       say::msg("srv.apps.pageMissing", "missing page"));
        return;
    }
    if (!commandPolicy_) {
        sendSetupError(exchange, 503, "native saved page controls are unavailable");
        return;
    }

    const CommandPolicyContext context = controlPolicyContext(config_);
    const auto savedTitlesPath = [this]() {
        return dataRoot_.empty() || dataRoot_[dataRoot_.size() - 1] == '/'
            ? dataRoot_ + "saved_page_titles.json"
            : dataRoot_ + "/saved_page_titles.json";
    };
    const auto loadTitles = [&savedTitlesPath]() {
        Json titles = Json::object();
        const JsonFileResult saved = JsonFile(savedTitlesPath()).load();
        if (saved.found && saved.value.isObject()) titles = saved.value;
        return titles;
    };
    const auto saveTitles = [this, &savedTitlesPath](const Json &titles) {
        std::string error;
        if (!JsonFile(savedTitlesPath()).save(titles, &error) && logger_)
            logger_->warning("apps: could not save saved page titles: " + error);
    };
    const auto callLuna = [this, &context](const std::string &action,
                                           const std::string &uri,
                                           const Json &callPayload,
                                           LunaReply &reply) {
        return waitForCommandPolicyLuna(
            luna_, commandPolicy_, context, action, uri, callPayload, std::string(),
            false, reply, std::string(), false, std::chrono::milliseconds(7000));
    };

    Json result = Json::object();
    CommandPolicySpec localCommand;
    localCommand.action = add ? "apps.saved-page.add"
        : remove ? "apps.saved-page.remove" : "apps.saved-page.edit";
    localCommand.uri = add ? "native/apps/saved-page/add"
        : remove ? "native/apps/saved-page/remove" : "native/apps/saved-page/edit";
    localCommand.timeout = std::chrono::milliseconds(25000);
    localCommand.resource = "saved-pages";
    LunaReply localReply;
    const CommandDecision decision = commandPolicy_->executeLocal(
        localCommand, context, false,
        [&result, &title, &target, hasTarget, add, remove,
         &launchPointId, &loadTitles, &saveTitles, &callLuna]() {
            const auto failure = [&result](const std::string &message) {
                result = Json::object();
                result.set("ok", Json::flag(false));
                result.set("error", Json::text(message));
                return false;
            };
            if (add) {
                Json params = Json::object();
                params.set("target", Json::text(target.url));
                Json call = Json::object();
                call.set("id", Json::text("com.webos.app.browser"));
                call.set("title", Json::text(title));
                call.set("params", params);
                LunaReply reply;
                if (!callLuna("apps.saved-page.add-launch-point",
                              "com.webos.applicationManager/addLaunchPoint", call, reply) ||
                    !reply.response["returnValue"].boolean())
                    return failure(say::msg("srv.apps.addRefused", "the TV would not add it"));
                result = Json::object();
                result.set("ok", Json::flag(true));
                if (reply.response["launchPointId"].valid())
                    result.set("launchPointId", reply.response["launchPointId"]);
                result.set("title", Json::text(title));
                return true;
            }

            LunaReply listReply;
            if (!callLuna("apps.saved-page.list-launch-points",
                          "com.webos.applicationManager/listLaunchPoints",
                          Json::object(), listReply))
                return failure(say::msg("srv.apps.pageGone",
                                        "that saved page is no longer on the TV"));
            Json point;
            const Json launchPoints = listReply.response["launchPoints"];
            if (launchPoints.isArray()) {
                for (const Json &candidate : launchPoints.items()) {
                    if (candidate["launchPointId"].string() == launchPointId &&
                        service::savedPageBookmark(candidate["id"].string(),
                                                   candidate["lptype"].string(),
                                                   "com.webos.app.browser")) {
                        point = candidate;
                        break;
                    }
                }
            }
            if (!point.isObject())
                return failure(say::msg("srv.apps.pageGone",
                                        "that saved page is no longer on the TV"));

            if (remove) {
                Json call = Json::object();
                call.set("launchPointId", Json::text(launchPointId));
                LunaReply reply;
                if (!callLuna("apps.saved-page.remove-launch-point",
                              "com.webos.applicationManager/removeLaunchPoint",
                              call, reply) || !reply.response["returnValue"].boolean())
                    return failure(say::msg("srv.apps.removeRefused", "the TV would not remove it"));
                Json originals = loadTitles();
                if (originals.has(launchPointId)) {
                    Json retained = Json::object();
                    for (const std::string &key : originals.keys())
                        if (key != launchPointId) retained.set(key, originals[key]);
                    saveTitles(retained);
                }
                result = Json::object();
                result.set("ok", Json::flag(true));
                return true;
            }

            Json originals = loadTitles();
            if (title.empty()) {
                title = originals.has(launchPointId)
                    ? originals[launchPointId].string() : point["title"].string();
                Json retained = Json::object();
                for (const std::string &key : originals.keys())
                    if (key != launchPointId) retained.set(key, originals[key]);
                originals = retained;
            } else if (title != point["title"].string() &&
                       !originals.has(launchPointId) && point["title"].valid()) {
                originals.set(launchPointId, point["title"]);
            }

            Json call = Json::object();
            call.set("launchPointId", Json::text(launchPointId));
            call.set("title", Json::text(title));
            if (hasTarget) {
                Json params = Json::object();
                const Json oldParams = point["params"];
                if (oldParams.isObject())
                    for (const std::string &key : oldParams.keys())
                        params.set(key, oldParams[key]);
                params.set("target", Json::text(target.url));
                call.set("params", params);
            }
            LunaReply reply;
            if (!callLuna("apps.saved-page.update-launch-point",
                          "com.webos.applicationManager/updateLaunchPoint", call, reply) ||
                !reply.response["returnValue"].boolean())
                return failure(say::msg("srv.tvRefused", "the TV would not change it"));
            saveTitles(originals);
            result = Json::object();
            result.set("ok", Json::flag(true));
            result.set("title", Json::text(title));
            if (hasTarget) result.set("address", Json::text(target.url));
            return true;
        }, localReply);

    const bool succeeded = decision.kind == CommandDecisionKind::Allowed &&
        localReply.response["returnValue"].isBoolean() &&
        localReply.response["returnValue"].boolean() && result["ok"].boolean();
    if (!succeeded && !result["error"].valid()) {
        result.set("ok", Json::flag(false));
        result.set("error", Json::text(!decision.reason.empty() ? decision.reason
            : !localReply.error.empty() ? localReply.error : "saved page operation failed"));
    }
    (void)exchange.respond(succeeded ? 200 : 400,
                           {{"Content-Type", "application/json; charset=utf-8"},
                            {"Cache-Control", "no-store"},
                            {"X-Content-Type-Options", "nosniff"}},
                           result.dump());
}

void HttpFrontDoor::sendDashboardSettings(HttpExchange &exchange) const {
    // The MQTT connection status remains Node-owned. Entity and category
    // metadata are served from the native catalogue so the public settings
    // response no longer depends on Node for those definitions.
    Json context;
    std::string bridgeError;
    if (!collectSettingsContext(context, bridgeError)) {
        (void)proxy_.forward(exchange);
        return;
    }

    const Json mqtt = config_.document["mqtt"];
    const Json entities = mqtt["entities"];
    // These fields follow the legacy payload's JavaScript `||` defaults, so an
    // explicit 0/false/empty value falls back exactly as the dashboard expects.
    const auto jsOr = [](const Json &value, const Json &fallback) {
        return setupTruthy(value) ? value : fallback;
    };
    const auto entityEnabled = [&entities](const char *key) {
        if (!entities.isObject())
            return true;
        const Json value = entities[key];
        return !(value.isBoolean() && !value.boolean());
    };

    const Json rejectUnauthorized = mqtt["tlsRejectUnauthorized"];
    Json disabled = Json::array();
    if (entities.isObject() && entities["disabled"].isArray())
        disabled = entities["disabled"];

    Json settings = Json::object();
    settings.set("enabled", Json::flag(setupTruthy(mqtt["enabled"])));
    settings.set("host", jsOr(mqtt["host"], Json::text("")));
    settings.set("port", mqtt.isObject() && mqtt.has("port") ? mqtt["port"] : Json());
    settings.set("tls", Json::flag(setupTruthy(mqtt["tls"])));
    settings.set("tlsRejectUnauthorized",
                 Json::flag(!(rejectUnauthorized.isBoolean() &&
                              !rejectUnauthorized.boolean())));
    settings.set("username", jsOr(mqtt["username"], Json::text("")));
    // As in Node, only whether a password is set is exposed.
    settings.set("passwordSet", Json::flag(setupTruthy(mqtt["password"])));
    settings.set("topicPrefix", jsOr(mqtt["topicPrefix"], Json::text("lgtv")));
    settings.set("discoveryPrefix",
                 jsOr(mqtt["discoveryPrefix"], Json::text("homeassistant")));
    settings.set("telemetryIntervalMs",
                 jsOr(mqtt["telemetryIntervalMs"], Json::numeric(30000)));
    Json entitySettings = Json::object();
    entitySettings.set("controls", Json::flag(entityEnabled("controls")));
    entitySettings.set("oled", Json::flag(entityEnabled("oled")));
    entitySettings.set("video", Json::flag(entityEnabled("video")));
    entitySettings.set("system", Json::flag(entityEnabled("system")));
    entitySettings.set("diagnostics", Json::flag(entityEnabled("diagnostics")));
    entitySettings.set("disabled", disabled);
    settings.set("entities", entitySettings);
    settings.set("categories", haEntityCategories());
    settings.set("entityCatalogue", haEntityCatalogue());

    // What the server calls the TV: the detected model once known, else config.json.
    const Json device = statsCollector()->deviceIdentity();

    Json payload = Json::object();
    payload.set("ok", Json::flag(true));
    payload.set("writable", Json::flag(config_.allowControl));
    payload.set("configFile", Json::text(configPath_));
    payload.set("iconServer", Json::flag(config_.document["iconServer"].isBoolean() &&
                                         config_.document["iconServer"].boolean()));
    payload.set("mqtt", settings);
    payload.set("device", device);
    payload.set("status", context["status"]);
    (void)exchange.respond(200,
                           {{"Content-Type", "application/json"},
                            {"Cache-Control", "no-store"},
                            {"X-Content-Type-Options", "nosniff"},
                            {"Referrer-Policy", "no-referrer"}},
                           payload.dump());
}

bool HttpFrontDoor::createBackup(Json &backup, const Json *settingsContext) const {
    Json context;
    if (settingsContext && settingsContext->isObject()) {
        context = *settingsContext;
    } else {
        std::string bridgeError;
        if (!collectSettingsContext(context, bridgeError, false)) return false;
    }

    const std::vector<std::string> sections = {
        "promotions", "sound", "devices", "hdmi"
    };
    const Json data = collectLgSettingsData(sections);
    const Json lg = service::lgSettingsResponse(
        sections, data["settings"], data["descriptions"], data["dimensions"],
        Json::object());

    std::map<std::string, Json> developers;
    LunaReply appReply;
    if (waitForLuna(luna_, "com.webos.applicationManager/listApps",
                    Json::object(), appReply) && appReply.response["apps"].isArray()) {
        for (const Json &app : appReply.response["apps"].items()) {
            const std::string id = app["id"].string();
            const std::string folder = app["folderPath"].string();
            if (id.empty() || folder.find("/media/developer/apps/") != 0 ||
                !service::backupAppAllowed(id))
                continue;
            Json item = Json::object();
            const Json title = app["title"];
            item.set("id", Json::text(id));
            item.set("title", setupTruthy(title) ? title : Json::text(id));
            const Json version = app["version"];
            item.set("version", Json::text(setupTruthy(version)
                ? service::javascriptString(version) : std::string()));
            developers[id] = item;
        }
    }
    Json apps = Json::array();
    for (const auto &entry : developers) apps.add(entry.second);

    const Json device = config_.document["device"];
    const std::string detectedModel = tvModelName();
    Json model = detectedModel.empty() ? Json() : Json::text(detectedModel);
    Json version = setupTruthy(context["displayVersion"])
        ? context["displayVersion"]
        : (version_.empty() ? Json() : Json::text(version_));
    Json backupConfig = config_.document;
    {   // The settings a module has not been given are saved at their defaults, as
        // the server's modules fill them in at start.
        const auto fill = [&backupConfig](const char *section, const char *defaults) {
            Json merged = Json::parse(defaults);
            const Json given = backupConfig[section];
            if (given.isObject())
                for (const std::string &key : given.keys()) merged.set(key, given[key]);
            backupConfig.set(section, merged);
        };
        fill("history", "{\"enabled\":false}");
        fill("logging", "{\"level\":\"info\"}");
        fill("ntp", "{\"port\":123,\"server\":\"\"}");
        fill("screenTime", "{\"action\":\"off\",\"appAction\":\"home\",\"apps\":{},"
                           "\"bedtimeEnd\":\"\",\"bedtimeStart\":\"\",\"dailyMinutes\":0,"
                           "\"enabled\":false}");
        fill("syslog", "{\"port\":514,\"redact\":true,\"server\":\"\","
                       "\"sources\":[\"system\",\"nativetv\"]}");
    }
    if (context["backupDaily"].isBoolean()) {
        Json backupSettings = backupConfig["backup"];
        if (!backupSettings.isObject()) backupSettings = Json::object();
        backupSettings.set("daily", context["backupDaily"]);
        backupConfig.set("backup", backupSettings);
    }
    backup = service::createSettingsBackup(
        utcIsoTimestamp(), model, version, backupConfig, lg["rows"],
        context["homeLayout"], apps);
    return backup.isObject();
}

bool HttpFrontDoor::createDailyBackup(const std::string &name,
                                     std::string &error) const {
    std::lock_guard<std::mutex> lock(dailyBackupMutex_);
    error.clear();
    if (!service::validSavedBackupName(name)) {
        error = "daily backup name is invalid";
        return false;
    }
    bool exists = false;
    if (!service::savedBackupExists(dataFilePath(dataRoot_, "backups"), name,
                                    exists, &error))
        return false;
    if (exists)
        return true;
    Json backup;
    if (!createBackup(backup)) {
        error = "backup settings context is unavailable";
        return false;
    }
    return service::saveDailyBackup(dataFilePath(dataRoot_, "backups"), name,
                                    backup, &error);
}

bool HttpFrontDoor::createDailyBackupIfDue(std::string &error) const {
    std::lock_guard<std::mutex> lock(dailyBackupMutex_);
    error.clear();
    Json context;
    if (!collectSettingsContext(context, error, false))
        return false;
    const bool enabled = context["backupDaily"].isBoolean() && context["backupDaily"].boolean();
    const std::string timestamp = utcIsoTimestamp();
    if (enabled && timestamp.size() < 10) {
        error = "could not read the UTC clock for daily backup";
        return false;
    }
    // Empty while off or before the clock is set, as Node's dailyCheck.
    const std::string name = service::dailyBackupNameDue(enabled, timestamp);
    if (name.empty())
        return true;
    const std::string directory = dataFilePath(dataRoot_, "backups");
    bool exists = false;
    if (!service::savedBackupExists(directory, name, exists, &error))
        return false;
    if (exists)
        return true;

    Json backup;
    if (!createBackup(backup, &context)) {
        error = "backup settings context is unavailable";
        return false;
    }
    return service::saveDailyBackup(directory, name, backup, &error);
}

void HttpFrontDoor::sendBackup(HttpExchange &exchange) const {
    Json backup;
    if (!createBackup(backup)) {
        (void)proxy_.forward(exchange);
        return;
    }
    sendMetricsJson(exchange, backup.dump());
}

void HttpFrontDoor::setDashboardSettings(HttpExchange &exchange) const {
    const auto respond = [&exchange](int status, const Json &payload) {
        (void)exchange.respond(status,
                               {{"Content-Type", "application/json; charset=utf-8"},
                                {"Cache-Control", "no-store"},
                                {"X-Content-Type-Options", "nosniff"}},
                               payload.dump());
    };
    const auto fail = [&respond](int status, const std::string &error) {
        Json payload = Json::object();
        payload.set("ok", Json::flag(false));
        payload.set("error", Json::text(error));
        respond(status, payload);
    };
    const HttpRequest &request = exchange.request();
    if (request.method != "POST") {
        fail(405, "method not allowed");
        return;
    }
    if (!config_.allowControl) {
        fail(403, "controls disabled in config");
        return;
    }
    // Node checks the body type before the origin; the shared guard keeps that
    // order and the manifest's declared CSRF rule in one place.
    if (rejectCrossSitePost(exchange))
        return;
    // JSON containing 8,192 UTF-16 code units can use up to 32 KiB in UTF-8.
    if (!exchange.setBodyLimit(32768)) {
        sendSetupError(exchange, 413, "request body too large", true);
        return;
    }
    std::string body;
    char buffer[1024];
    for (;;) {
        const int count = exchange.readBody(buffer, sizeof(buffer));
        if (count == 0) break;
        if (count < 0) {
            sendSetupError(exchange, exchange.bodyLimitExceeded() ? 413 : 400,
                           exchange.bodyLimitExceeded() ? "request body too large"
                                                        : "invalid request body",
                           true);
            return;
        }
        body.append(buffer, static_cast<size_t>(count));
    }
    if (exceedsUtf16CodeUnitLimit(body, 8192)) {
        sendSetupError(exchange, 413, "request body too large", true);
        return;
    }
    const Json input = Json::parse(body);
    if (!input.valid()) {
        fail(400, "malformed JSON");
        return;
    }
    Json patch;
    std::vector<std::string> errors;
    if (!service::validateDashboardSettings(input, patch, errors)) {
        std::string combined;
        for (const std::string &error : errors) {
            if (!combined.empty()) combined += "; ";
            combined += error;
        }
        fail(400, combined);
        return;
    }

    int restartStatus = 0;
    std::string restartBody;
    std::string restartError;
    if (!proxy_.requestNodeRestart(true, restartStatus, restartBody, restartError)) {
        fail(502, restartError.empty() ? "could not check restart readiness" : restartError);
        return;
    }
    if (restartStatus != 200) {
        const Json response = Json::parse(restartBody);
        if (response.isObject()) {
            respond(restartStatus, response);
        } else {
            fail(502, "could not check restart readiness");
        }
        return;
    }

    std::string saveError;
    if (!ConfigStore(configPath_).writeDashboardSettings(patch, &saveError)) {
        fail(500, "could not write " + configPath_ + ": " + saveError);
        return;
    }
    if (logger_)
        logger_->info("config: dashboard settings saved to " + configPath_ +
                      "; restarting to apply");

    restartStatus = 0;
    restartBody.clear();
    restartError.clear();
    if (!proxy_.requestNodeRestart(false, restartStatus, restartBody, restartError) ||
        restartStatus < 200 || restartStatus >= 300 ||
        !Json::parse(restartBody)["ok"].boolean()) {
        fail(502, "settings saved but server restart failed" +
             (restartError.empty() ? std::string() : ": " + restartError));
        return;
    }
    Json response = Json::object();
    response.set("ok", Json::flag(true));
    response.set("restarting", Json::flag(true));
    respond(200, response);
}

void HttpFrontDoor::setControl(HttpExchange &exchange) const {
    if (!exchange.setBodyLimit(16384)) {
        sendSetupError(exchange, 413, "request body too large", true);
        return;
    }

    std::string body;
    char buffer[1024];
    for (;;) {
        const int count = exchange.readBody(buffer, sizeof(buffer));
        if (count == 0)
            break;
        if (count < 0) {
            sendSetupError(exchange, exchange.bodyLimitExceeded() ? 413 : 400,
                           exchange.bodyLimitExceeded() ? "request body too large"
                                                        : "invalid request body",
                           true);
            return;
        }
        body.append(buffer, static_cast<size_t>(count));
    }
    if (exceedsUtf16CodeUnitLimit(body, 4096)) {
        sendSetupError(exchange, 413, "request body too large", true);
        return;
    }

    const Json payload = Json::parse(body);
    const auto answerControl = [&exchange](const Json &answer) {
        (void)exchange.respond(200,
                               {{"Content-Type", "application/json; charset=utf-8"},
                                {"Cache-Control", "no-store"},
                                {"X-Content-Type-Options", "nosniff"}},
                               answer.dump());
    };
    const auto refuseControl = [&answerControl](const std::string &error) {
        Json refusal = Json::object();
        refusal.set("ok", Json::flag(false));
        refusal.set("error", Json::text(error));
        answerControl(refusal);
    };
    if (!payload.isObject() && !payload.isArray()) {
        sendSetupError(exchange, 400, "malformed JSON");
        return;
    }
    if (!config_.allowControl) {
        refuseControl(say::msg("srv.controlsOff", "controls disabled in config"));
        return;
    }
    const std::string action = payload.isObject() ? payload["action"].string() : "";
    {
        Json settingResponse;
        if (serverSettingControl(action, payload["value"], settingResponse)) {
            sendMetricsJson(exchange, settingResponse.dump());
            return;
        }
    }
    if (action == "screensaverMode" || action == "screensaver") {
        ScreensaverControl *const saver = screensaverControl();
        if (!saver) {
            sendMetricsJson(exchange, "{\"ok\":false,\"error\":\"native screen saver manager is unavailable\"}");
            return;
        }
        if (action == "screensaver") {
            sendMetricsJson(exchange, saver->trigger().dump());
            return;
        }
        // The mode and how brightly to draw it are staged together: both go in
        // the same file, so setting one alone would reset the other.
        const Json value = payload["value"];
        std::string mode;
        std::string level = service::screensaverLevel(screensaverManager_->state(
            config_.allowControl, false));
        if (value.isObject()) {
            mode = value["mode"].string();
            if (value["level"].isString() && !value["level"].string().empty())
                level = value["level"].string();
        } else if (value.isString()) {
            mode = value.string();
        }
        sendMetricsJson(exchange, saver->setMode(trimmedText(mode), level).dump());
        return;
    }
    if (action == "tvAppInstall" || action == "tvAppRemove") {
        sendMetricsJson(exchange, runTvApp(action == "tvAppInstall" ? "install" : "remove").dump());
        return;
    }
    const bool consentAction = action == "consent";
    const bool acrAction = action == "acr";
    const bool limitAdTrackingAction = action == "limitAdTracking";
    const bool clearAdCookiesAction = action == "clearAdCookies";
    const bool resetAdIdAction = action == "resetAdId";
    const bool adBlockAction = action == "adblock" || action == "setAdBlock" ||
                               action == "toggleAdBlock";
    const bool blockTvUpdatesAction = action == "blockTvUpdates";
    const service::LgSettingWrite lgSetting = action == "lgSetting"
        ? service::lgSettingWrite(payload["value"])
        : service::LgSettingWrite();
    const bool lgSettingAction = action == "lgSetting" && lgSetting.handled;
    const bool volumeAction = action == "volume";
    const bool volumeStepAction = action == "volumeStep";
    const bool muteAction = action == "mute";
    const bool powerOffAction = action == "powerOff";
    const bool powerOnAction = action == "powerOn";
    const bool soundOutputAction = action == "sound_output" || action == "soundOutput";
    const bool screenOffAction = action == "screenOff";
    const bool screenOnAction = action == "screenOn";
    const bool inputAction = action == "input";
    const bool pictureModeAction = action == "picture_mode" || action == "pictureMode";
    const bool energySavingAction = action == "energySaving";
    const bool backlightAction = action == "backlight";
    const bool standbyLightAction = action == "standbyLight";
    const bool logoLightAction = action == "logoLight";
    const bool screenShiftAction = action == "screenShift";
    const bool logoDimmingAction = action == "logoDimming";
    const bool alwaysReadyAction = action == "alwaysReady";
    const bool alwaysReadyScreenAction = action == "alwaysReadyScreen";
    const bool alwaysReadyOffStartAction = action == "alwaysReadyOffStart";
    const bool wakeOnLanAction = action == "wakeOnLan";
    const bool lgLogoAction = action == "lgLogo";
    const bool sleepTimerAction = action == "sleepTimer";
    const bool quickBootAction = action == "quickBoot";
    const bool deviceDetectionAction = action == "deviceDetection";
    const bool powerTimerAction = action == "powerTimer";
    const bool launchUrlAction = action == "launch_url" || action == "launchUrl";
    const bool launchAppAction = action == "launch_app" || action == "launchApp";
    const bool closeAppAction = action == "close_app" || action == "closeApp";
    const bool piccapAction = action == "piccap";
    const bool playbackAction = action == "playback" || action == "media";
    const bool toastAction = action == "toast";
    const bool oledProtectionAction = action == "oledProtection";
    const bool refresherScheduleAction = action == "refresherSchedule";
    const bool refresherCancelAction = action == "refresherCancel";
    const bool refresherAction = refresherScheduleAction || refresherCancelAction;
    const bool serviceMenuOpenAction = action == "serviceMenuOpen";
    const bool serviceMenuLockAction = action == "serviceMenuLock";
    const Json oledProtectionKeyValue = payload["value"]["key"];
    const std::string oledProtectionKey =
        service::javascriptTruthy(oledProtectionKeyValue)
            ? service::javascriptString(oledProtectionKeyValue) : std::string();
    const bool oledProtectionEnabled = service::javascriptTruthy(
        payload["value"]["enabled"]);
    const bool homeKeyAction = action == "rcu" &&
        service::controlRemoteHome(payload["value"]);
    const bool mediaToggle = payload["value"].isString() &&
        (payload["value"].string() == "playPause" ||
         payload["value"].string() == "playpause");
    int playbackKeyCode = 0;
    const int remoteKeyCode = action == "rcu"
        ? service::controlRemoteKeyCode(payload["value"]) : 0;
    const bool remoteKeyAction = remoteKeyCode > 0;
    if (!volumeAction && !volumeStepAction && !muteAction && !powerOffAction &&
        !powerOnAction &&
        !soundOutputAction && !screenOffAction &&
        !screenOnAction && !inputAction && !pictureModeAction && !energySavingAction &&
        !backlightAction && !standbyLightAction && !logoLightAction &&
        !screenShiftAction && !logoDimmingAction &&
        !alwaysReadyAction && !alwaysReadyScreenAction &&
        !alwaysReadyOffStartAction && !wakeOnLanAction && !lgLogoAction &&
        !lgSettingAction &&
        !sleepTimerAction && !quickBootAction && !deviceDetectionAction &&
        !powerTimerAction &&
        !launchUrlAction && !launchAppAction && !closeAppAction &&
        !piccapAction && !remoteKeyAction && !homeKeyAction && !playbackAction &&
        !toastAction && !oledProtectionAction && !refresherAction &&
        !serviceMenuOpenAction &&
        !serviceMenuLockAction && !consentAction && !acrAction &&
        !limitAdTrackingAction &&
        !clearAdCookiesAction && !resetAdIdAction && !adBlockAction &&
        !blockTvUpdatesAction) {
        invalidateLgSettingsCache();
        if (action == "lgSetting") {
            refuseControl(payload["value"].isObject() && payload["value"]["id"].isString()
                ? say::msg("srv.lgsettings.unknown", "Unknown setting")
                : std::string("lgSetting needs an id"));
        } else if (action == "rcu") {
            const Json key = payload["value"];
            std::string name = key.isString() ? key.string()
                : key.isNumber() ? compactNumber(key.number()) : std::string();
            name = trimmedText(name);
            std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            refuseControl("unknown key: " + name);
        } else {
            refuseControl("unknown action");
        }
        return;
    }
    if ((powerOffAction || powerOnAction) && !config_.allowPower) {
        // Never start a power transition against the file-only policy.
        refuseControl(say::msg("srv.powerOff", "power actions disabled (set allowPower)"));
        return;
    }
    if (consentAction) {
        setConsentControl(exchange, payload["value"]);
        return;
    }
    if (acrAction) {
        setAcrControl(exchange, payload["value"]);
        return;
    }
    if (limitAdTrackingAction) {
        setLimitAdTrackingControl(exchange, payload["value"]);
        return;
    }
    if (clearAdCookiesAction) {
        clearAdCookiesControl(exchange);
        return;
    }
    if (resetAdIdAction) {
        resetAdIdControl(exchange);
        return;
    }
    if (adBlockAction || blockTvUpdatesAction) {
        const auto sendResult = [&exchange](const Json &result) {
            (void)exchange.respond(200,
                                   {{"Content-Type", "application/json; charset=utf-8"},
                                    {"Cache-Control", "no-store"},
                                    {"X-Content-Type-Options", "nosniff"}},
                                   result.dump());
        };
        if (!adBlockManager_ || !commandPolicy_) {
            Json failure = Json::object();
            failure.set("ok", Json::flag(false));
            failure.set("error", Json::text("native ad-block service is unavailable"));
            sendResult(failure);
            return;
        }
        CommandPolicySpec localCommand;
        localCommand.action = adBlockAction ? "control.adblock" : "control.block-tv-updates";
        localCommand.uri = "native/hosts-table/apply";
        localCommand.timeout = std::chrono::milliseconds(3000);
        localCommand.resource = "hosts-table";
        const CommandPolicyContext context = controlPolicyContext(config_);
        Json operationResult;
        LunaReply localReply;
        const CommandDecision decision = commandPolicy_->executeLocal(
            localCommand, context, false,
            [this, &payload, action, blockTvUpdatesAction,
             &operationResult]() {
                if (blockTvUpdatesAction) {
                    const Json value = payload["value"];
                    const bool blocked = value.isBoolean() ? value.boolean()
                        : value.isString() &&
                            (value.string() == "on" || value.string() == "true");
                    operationResult = adBlockManager_->setTvUpdatesBlocked(blocked);
                    return operationResult["ok"].isBoolean() &&
                           operationResult["ok"].boolean();
                }
                std::string requested = payload["value"].valid()
                    ? service::javascriptString(payload["value"]) : std::string();
                std::transform(requested.begin(), requested.end(), requested.begin(),
                               [](unsigned char character) {
                                   return static_cast<char>(std::tolower(character));
                               });
                if (action == "toggleAdBlock" || requested == "toggle") {
                    const service::AdBlockStore &store = adBlockManager_->store();
                    requested = store.mode() != "off" && store.tableMounted()
                        ? "off" : "full";
                } else if (requested != "off" && requested != "ads" &&
                           requested != "full") {
                    const Json value = payload["value"];
                    const bool enabled = (value.isBoolean() && value.boolean()) ||
                        requested == "on" || requested == "true" || requested == "1";
                    requested = enabled ? "full" : "off";
                }
                operationResult = adBlockManager_->setMode(requested);
                return operationResult["ok"].isBoolean() &&
                       operationResult["ok"].boolean();
            }, localReply);
        const bool succeeded = decision.kind == CommandDecisionKind::Allowed &&
            localReply.response["returnValue"].isBoolean() &&
            localReply.response["returnValue"].boolean();
        if (succeeded) {
            invalidateNodeControlCaches();
            sendResult(operationResult);
            return;
        }
        if (!operationResult.isObject() || operationResult["ok"].boolean()) {
            operationResult = Json::object();
            operationResult.set("ok", Json::flag(false));
            const std::string error = !localReply.error.empty() ? localReply.error
                : !localReply.response["errorText"].string().empty()
                    ? localReply.response["errorText"].string()
                    : !decision.reason.empty() ? decision.reason
                    : "could not update the hosts table";
            operationResult.set("error", Json::text(error));
        }
        sendResult(operationResult);
        return;
    }

    const std::string inputAppId =
        inputAction ? service::controlInputAppId(payload["value"]) : std::string();
    if (inputAction && inputAppId.empty()) {
        Json response = Json::object();
        response.set("ok", Json::flag(false));
        response.set("error", Json::text("unknown input"));
        (void)exchange.respond(200,
                               {{"Content-Type", "application/json; charset=utf-8"},
                                {"Cache-Control", "no-store"},
                                {"X-Content-Type-Options", "nosniff"}},
                               response.dump());
        return;
    }

    const std::string pictureMode =
        pictureModeAction ? service::controlPictureModeTarget(payload["value"]) : std::string();
    std::string energySaving;
    const bool validEnergySaving = !energySavingAction ||
        service::controlEnergySavingTarget(payload["value"], energySaving);
    int backlight = 0;
    const bool validBacklight = !backlightAction ||
        service::controlBacklightTarget(payload["value"], backlight);
    std::string logoDimming;
    const bool validLogoDimming = !logoDimmingAction ||
        service::controlLogoDimmingTarget(payload["value"], logoDimming);
    int alwaysReadyOffStartHour = 0;
    const bool validAlwaysReadyOffStart = !alwaysReadyOffStartAction ||
        service::controlAlwaysReadyOffStartHour(payload["value"],
                                                alwaysReadyOffStartHour);
    std::string sleepTimer;
    const bool validSleepTimer = !sleepTimerAction ||
        service::controlSleepTimerTarget(payload["value"], sleepTimer);
    std::string launchUrl;
    const bool validLaunchUrl = !launchUrlAction ||
        service::controlLaunchUrlTarget(payload["value"], launchUrl);
    const std::string closeAppId =
        closeAppAction ? service::controlCloseAppId(payload["value"]) : std::string();
    const std::string soundOutput = soundOutputAction
        ? service::controlSoundOutputTarget(payload["value"]) : std::string();
    const std::string requestedAppId =
        launchAppAction ? service::controlCloseAppId(payload["value"]) : std::string();
    const auto sendControlFailure = [&exchange](const std::string &error) {
        Json response = Json::object();
        response.set("ok", Json::flag(false));
        response.set("error", Json::text(error));
        (void)exchange.respond(200,
                               {{"Content-Type", "application/json; charset=utf-8"},
                                {"Cache-Control", "no-store"},
                                {"X-Content-Type-Options", "nosniff"}},
                               response.dump());
    };
    if (action == "piccap") {
        setPiccapControl(exchange, payload["value"]);
        return;
    }
    if (lgSettingAction && !lgSetting.valid) {
        sendControlFailure(lgSetting.error);
        return;
    }
    service::PowerTimerChange powerTimer;
    std::string powerTimerError;
    const bool validPowerTimer = !powerTimerAction ||
        service::controlPowerTimerChange(payload["value"], powerTimer,
                                         powerTimerError);
    const CommandPolicyContext policyContext = controlPolicyContext(config_);
    const auto command = [this, &policyContext](const std::string &commandAction,
                                                const std::string &commandUri,
                                                const Json &commandPayload,
                                                const std::string &resource,
                                                bool requiresPower,
                                                LunaReply &commandReply) {
        return waitForCommandPolicyLuna(
            luna_, commandPolicy_, policyContext, commandAction, commandUri,
            commandPayload, resource, requiresPower, commandReply);
    };
    if (pictureModeAction && pictureMode.empty()) {
        sendControlFailure("missing picture mode");
        return;
    }
    if (energySavingAction && !validEnergySaving) {
        sendControlFailure("energy saving must be one of auto, off, min, med, max, screen_off");
        return;
    }
    if (backlightAction && !validBacklight) {
        sendControlFailure("backlight must be between 0 and 100");
        return;
    }
    if (logoDimmingAction && !validLogoDimming) {
        sendControlFailure("logo dimming takes off, light, strong");
        return;
    }
    if (alwaysReadyOffStartAction && !validAlwaysReadyOffStart) {
        sendControlFailure("the start must be an hour from 0 to 23");
        return;
    }
    if (sleepTimerAction && !validSleepTimer) {
        sendControlFailure("sleep timer must be one of off, 10, 30, 60, 90, 120");
        return;
    }
    if (powerTimerAction && !validPowerTimer) {
        sendControlFailure(powerTimerError);
        return;
    }
    if (launchUrlAction && !validLaunchUrl) {
        sendControlFailure("url must start with http:// or https://");
        return;
    }
    if (closeAppAction && closeAppId.empty()) {
        sendControlFailure("missing app id");
        return;
    }
    if (launchAppAction && requestedAppId.empty()) {
        sendControlFailure("missing app id");
        return;
    }
    if (soundOutputAction && soundOutput.empty()) {
        sendControlFailure("missing sound output");
        return;
    }
    if (oledProtectionAction && oledProtectionKey != "gsr" &&
        oledProtectionKey != "tpc") {
        sendControlFailure("unknown protection: " + oledProtectionKey);
        return;
    }

    Json requestPayload = Json::object();
    const int step = volumeStepAction ? service::controlVolumeStep(payload["value"]) : 0;
    const int target = volumeAction ? service::controlVolumeTarget(payload["value"]) : 0;
    const bool mute = muteAction && service::controlMuteTarget(payload["value"]);
    std::string uri = volumeAction ? "com.webos.audio/setVolume"
        : muteAction ? "com.webos.audio/setMuted"
        : powerOffAction ? "com.webos.service.tvpower/power/powerOff"
        : powerOnAction ? "com.webos.service.tvpower/power/powerOn"
        : soundOutputAction ? "com.webos.service.settings/setSystemSettings"
        : sleepTimerAction || quickBootAction || deviceDetectionAction || powerTimerAction
            ? "com.webos.service.settings/setSystemSettings"
        : lgSettingAction ? "com.webos.service.settings/setSystemSettings"
        : inputAction ? "com.webos.applicationManager/launch"
        : launchUrlAction ? "com.webos.applicationManager/launch"
        : launchAppAction ? "com.webos.applicationManager/launch"
        : closeAppAction ? "com.webos.applicationManager/closeByAppId"
        : homeKeyAction ? "com.webos.applicationManager/launch"
        : playbackAction ? "native/input/writeKey"
        : toastAction ? "com.webos.notification/createToast"
        : serviceMenuOpenAction ? "com.webos.applicationManager/launch"
        : serviceMenuLockAction ? "com.webos.settingsservice/setSystemSettings"
        : oledProtectionAction ? "com.webos.service.oledepl/getGlobalStressReduction"
        : refresherAction ? "com.webos.service.tv.display/requestClearPanelNoise"
        : remoteKeyAction ? "com.webos.service.networkinput/test/sendKeyCode"
        : pictureModeAction || energySavingAction || backlightAction ||
                standbyLightAction || logoLightAction || screenShiftAction ||
                logoDimmingAction || alwaysReadyAction || alwaysReadyScreenAction ||
                alwaysReadyOffStartAction || wakeOnLanAction || lgLogoAction
            ? "com.webos.service.settings/setSystemSettings"
        : screenOffAction ? "com.webos.service.tvpower/power/turnOffScreen"
                          : "com.webos.service.tvpower/power/turnOnScreen";
    if (volumeAction)
        requestPayload.set("volume", Json::numeric(target));
    else if (muteAction)
        requestPayload.set("muted", Json::flag(mute));
    else if (powerOffAction || powerOnAction)
        requestPayload.set("reason", Json::text("remoteKey"));
    else if (inputAction)
        requestPayload.set("id", Json::text(inputAppId));
    else if (launchUrlAction) {
        requestPayload.set("id", Json::text("com.webos.app.browser"));
        Json params = Json::object();
        params.set("target", Json::text(launchUrl));
        requestPayload.set("params", params);
    } else if (launchAppAction) {
        requestPayload.set("id", Json::text(requestedAppId));
    } else if (closeAppAction) {
        requestPayload.set("id", Json::text(closeAppId));
    } else if (homeKeyAction) {
        requestPayload.set("id", Json::text("com.webos.app.home"));
    } else if (toastAction) {
        requestPayload.set("sourceId", Json::text("com.webos.app.home"));
        requestPayload.set("message", Json::text(
            service::controlToastMessage(payload["value"])));
    } else if (refresherAction) {
        requestPayload.set("mode", Json::text(
            refresherScheduleAction ? "schedule" : "cancel_schedule"));
    } else if (serviceMenuOpenAction) {
        requestPayload.set("id", Json::text("com.webos.app.factorywin"));
        Json params = Json::object();
        params.set("irKey", Json::text(
            service::controlServiceMenuTarget(payload["value"])));
        requestPayload.set("params", params);
    } else if (serviceMenuLockAction) {
        requestPayload.set("category", Json::text("other"));
        Json settings = Json::object();
        settings.set("svcMenuFlag", Json::flag(service::javascriptTruthy(
            payload["value"]["locked"])));
        requestPayload.set("settings", settings);
    } else if (remoteKeyAction) {
        requestPayload.set("keyCode", Json::numeric(remoteKeyCode));
    } else if (soundOutputAction) {
        requestPayload.set("category", Json::text("sound"));
        Json settings = Json::object();
        settings.set("soundOutput", Json::text(soundOutput));
        requestPayload.set("settings", settings);
    } else if (standbyLightAction || logoLightAction) {
        requestPayload.set("category", Json::text("option"));
        Json settings = Json::object();
        const std::string key = standbyLightAction ? "standByLight" : "logoLight";
        settings.set(key, Json::text(service::controlIndicatorLightTarget(payload["value"])
                                         ? "on" : "off"));
        requestPayload.set("settings", settings);
    } else if (screenShiftAction || logoDimmingAction) {
        requestPayload.set("category", Json::text("picture"));
        Json settings = Json::object();
        if (screenShiftAction)
            settings.set("screenShift", Json::text(
                service::controlIndicatorLightTarget(payload["value"]) ? "on" : "off"));
        else
            settings.set("logoLuminanceAdjust", Json::text(logoDimming));
        requestPayload.set("settings", settings);
    } else if (alwaysReadyAction || alwaysReadyScreenAction ||
               alwaysReadyOffStartAction) {
        requestPayload.set("category", Json::text("general"));
        Json settings = Json::object();
        if (alwaysReadyAction) {
            settings.set("alwaysOn", Json::text(
                service::controlIndicatorLightTarget(payload["value"]) ? "on" : "off"));
        } else if (alwaysReadyScreenAction) {
            settings.set("lifeOnScreenMode", Json::text(
                service::controlIndicatorLightTarget(payload["value"])
                    ? "allEnabled" : "off"));
        } else {
            settings.set("alwaysOnDisableStartHour",
                         Json::text(std::to_string(alwaysReadyOffStartHour)));
            settings.set("alwaysOnDisableStartMinute", Json::text("0"));
            settings.set("alwaysOnDisableEndHour", Json::text(
                std::to_string((alwaysReadyOffStartHour + 5) % 24)));
            settings.set("alwaysOnDisableEndMinute", Json::text("0"));
        }
        requestPayload.set("settings", settings);
    } else if (wakeOnLanAction) {
        requestPayload.set("category", Json::text("network"));
        Json settings = Json::object();
        settings.set("wolwowlOnOff", Json::text(
            service::controlIndicatorLightTarget(payload["value"]) ? "true" : "false"));
        requestPayload.set("settings", settings);
    } else if (lgLogoAction) {
        requestPayload.set("category", Json::text("other"));
        Json settings = Json::object();
        settings.set("lgLogoDisplay", Json::text(
            service::controlIndicatorLightTarget(payload["value"]) ? "on" : "off"));
        requestPayload.set("settings", settings);
    } else if (lgSettingAction) {
        requestPayload.set("category", Json::text(lgSetting.category));
        Json settings = Json::object();
        settings.set(lgSetting.key, lgSetting.storedValue);
        requestPayload.set("settings", settings);
    } else if (sleepTimerAction) {
        requestPayload.set("category", Json::text("time"));
        Json settings = Json::object();
        settings.set("sleepTimer", Json::text(sleepTimer));
        requestPayload.set("settings", settings);
    } else if (quickBootAction || deviceDetectionAction) {
        requestPayload.set("category", Json::text(quickBootAction ? "option" : "other"));
        Json settings = Json::object();
        const std::string key = quickBootAction ? "quickStartMode" : "ueiEnable";
        settings.set(key, Json::text(service::controlIndicatorLightTarget(payload["value"])
                                         ? "on" : "off"));
        requestPayload.set("settings", settings);
    } else if (powerTimerAction) {
        requestPayload.set("category", Json::text("time"));
        requestPayload.set("settings", powerTimer.settings);
    } else if (pictureModeAction || energySavingAction || backlightAction) {
        requestPayload.set("category", Json::text("picture"));
        Json settings = Json::object();
        if (pictureModeAction)
            settings.set("pictureMode", Json::text(pictureMode));
        else if (backlightAction)
            settings.set("backlight", Json::text(std::to_string(backlight)));
        else {
            settings.set("energySaving", Json::text(energySaving));
            settings.set("energySavingModified", Json::text("true"));
        }
        requestPayload.set("settings", settings);
    }
    LunaReply reply;
    bool answered = false;
    bool succeeded = false;
    std::string actionError;
    bool serviceMenuLockWriteSucceeded = false;
    bool serviceMenuLockStateAvailable = false;
    Json serviceMenuLockFlag;
    const bool serviceMenuLockTarget = service::javascriptTruthy(
        payload["value"]["locked"]);
    bool oledProtectionWriteAttempted = false;
    bool refresherWriteAttempted = false;
    Json oledProtectionState;
    if (volumeStepAction) {
        // A step reads the current level before writing its target, so it must
        // serialize with direct volume changes for the whole read-modify-write.
        std::lock_guard<std::mutex> commandLock(volumeControlMutex_);
        if (step == 1 || step == -1) {
            Json keyRequest = Json::object();
            keyRequest.set("keyCode", Json::numeric(step == 1 ? 115 : 114));
            LunaReply keyReply;
            const bool keyAnswered = command(
                "control.volume-step", "com.webos.service.networkinput/test/sendKeyCode",
                keyRequest, "audio-volume", false, keyReply);
            if (keyAnswered && keyReply.response["returnValue"].boolean()) {
                reply = keyReply;
                answered = true;
                succeeded = true;
            } else {
                const std::string fallbackUri = step == 1
                    ? "com.webos.audio/volumeUp" : "com.webos.audio/volumeDown";
                answered = command("control.volume-step", fallbackUri, Json::object(),
                                   "audio-volume", false, reply);
                succeeded = answered && reply.response["returnValue"].boolean();
            }
        } else {
            double currentVolume = 10;
            LunaReply masterReply;
            const bool masterAnswered = waitForLuna(
                luna_, "com.webos.service.audio/master/getVolume", Json::object(),
                masterReply);
            const Json masterVolume = masterReply.response["volumeStatus"]["volume"];
            if (masterAnswered && masterVolume.isNumber()) {
                currentVolume = masterVolume.number();
            } else {
                LunaReply legacyReply;
                if (waitForLuna(luna_, "com.webos.audio/getVolume", Json::object(),
                                legacyReply) && legacyReply.response["volume"].isNumber())
                    currentVolume = legacyReply.response["volume"].number();
            }
            const double targetVolume =
                std::max(0.0, std::min(100.0, currentVolume + step));
            Json setRequest = Json::object();
            setRequest.set("volume", Json::numeric(targetVolume));
            answered = command("control.volume-step", "com.webos.audio/setVolume",
                               setRequest, "audio-volume", false, reply);
            succeeded = answered && reply.response["returnValue"].boolean();
        }
    } else {
        std::mutex &commandMutex = volumeAction ? volumeControlMutex_
            : muteAction ? muteControlMutex_
            : toastAction ? notificationControlMutex_
            : oledProtectionAction || refresherAction ? oledControlMutex_
            : serviceMenuOpenAction ? applicationControlMutex_
            : serviceMenuLockAction ? systemSettingsControlMutex_
            : remoteKeyAction || homeKeyAction || playbackAction ? remoteInputControlMutex_
            : powerOffAction || powerOnAction ? powerControlMutex_
            : inputAction ? inputControlMutex_
            : soundOutputAction ? systemSettingsControlMutex_
            : sleepTimerAction || quickBootAction || deviceDetectionAction ||
                    powerTimerAction || alwaysReadyAction || alwaysReadyScreenAction ||
                    alwaysReadyOffStartAction || wakeOnLanAction || lgLogoAction ||
                    lgSettingAction || serviceMenuLockAction
                ? systemSettingsControlMutex_
            : launchUrlAction || launchAppAction || closeAppAction ? applicationControlMutex_
            : pictureModeAction || energySavingAction || backlightAction ||
                    standbyLightAction || logoLightAction || screenShiftAction ||
                    logoDimmingAction
                ? systemSettingsControlMutex_
            : screenOffAction ? screenOffControlMutex_ : screenOnControlMutex_;
        std::lock_guard<std::mutex> commandLock(commandMutex);
        if (oledProtectionAction) {
            const auto readSyspropState = [this]() {
                Json request = Json::object();
                Json keys = Json::array();
                keys.add(Json::text("OledTPC"));
                keys.add(Json::text("OledGSR"));
                request.set("keys", keys);
                LunaReply stateReply;
                if (!waitForLuna(
                        luna_, "com.webos.service.tv.systemproperty/getProperties",
                        request, stateReply) ||
                    !stateReply.response["returnValue"].boolean() ||
                    !stateReply.response["OledTPC"].valid())
                    return Json();
                Json state = Json::object();
                state.set("gsr", Json::flag(
                    service::javascriptString(stateReply.response["OledGSR"]) ==
                    "true"));
                state.set("tpc", Json::flag(
                    service::javascriptString(stateReply.response["OledTPC"]) ==
                    "true"));
                state.set("gsrStressCount", Json());
                return state;
            };
            const auto readEplState = [this, &readSyspropState](bool &usingEpl) {
                LunaReply gsrReply;
                if (!waitForLuna(
                        luna_, "com.webos.service.oledepl/getGlobalStressReduction",
                        Json::object(), gsrReply) ||
                    !gsrReply.response["returnValue"].boolean()) {
                    usingEpl = false;
                    return readSyspropState();
                }
                usingEpl = true;
                Json state = Json::object();
                state.set("gsr", Json::flag(gsrReply.response["enable"].boolean()));
                state.set("gsrStressCount",
                    gsrReply.response["stressCount"].isNumber()
                        ? gsrReply.response["stressCount"] : Json());
                LunaReply tpcReply;
                if (waitForLuna(
                        luna_, "com.webos.service.oledepl/getTemporalPeakControl",
                        Json::object(), tpcReply) &&
                    tpcReply.response["returnValue"].boolean())
                    state.set("tpc", Json::flag(tpcReply.response["enable"].boolean()));
                else
                    state.set("tpc", Json());
                return state;
            };
            bool usingEpl = false;
            const Json initialState = readEplState(usingEpl);
            if (!usingEpl && !initialState.isObject()) {
                actionError = "this TV does not offer the control";
            } else {
                if (usingEpl) {
                    uri = oledProtectionKey == "gsr"
                        ? "com.webos.service.oledepl/setGlobalStressReduction"
                        : "com.webos.service.oledepl/setTemporalPeakControl";
                    requestPayload.set("enable", Json::flag(oledProtectionEnabled));
                } else {
                    uri = "com.webos.service.tv.systemproperty/setProperties";
                    requestPayload.set(
                        oledProtectionKey == "gsr" ? "OledGSR" : "OledTPC",
                        Json::text(oledProtectionEnabled ? "true" : "false"));
                }
                oledProtectionWriteAttempted = true;
                answered = waitForCommandPolicyLuna(
                    luna_, commandPolicy_, policyContext,
                    "control.oled-protection", uri, requestPayload,
                    "oled-panel", false, reply);
                succeeded = answered && reply.response["returnValue"].boolean();
                if (!succeeded) {
                    actionError = "the TV would not change it";
                } else {
                    bool readbackUsesEpl = usingEpl;
                    oledProtectionState = usingEpl
                        ? readEplState(readbackUsesEpl) : readSyspropState();
                    const Json current = oledProtectionState[oledProtectionKey];
                    succeeded = current.isBoolean() &&
                        current.boolean() == oledProtectionEnabled;
                    if (!succeeded)
                        actionError = "the setting did not take";
                }
            }
        } else if (refresherAction) {
            refresherWriteAttempted = true;
            answered = waitForCommandPolicyLuna(
                luna_, commandPolicy_, policyContext,
                refresherScheduleAction ? "control.refresher-schedule"
                                        : "control.refresher-cancel",
                uri, requestPayload, "oled-panel", false, reply);
            succeeded = answered && reply.response["returnValue"].boolean();
        } else if (serviceMenuLockAction) {
            answered = waitForCommandPolicyLuna(
                luna_, commandPolicy_, policyContext,
                "control.service-menu-lock", uri, requestPayload,
                "system-settings", false, reply);
            succeeded = answered && reply.response["returnValue"].boolean();
            serviceMenuLockWriteSucceeded = succeeded;
            if (serviceMenuLockWriteSucceeded) {
                Json stateRequest = Json::object();
                stateRequest.set("category", Json::text("other"));
                Json keys = Json::array();
                keys.add(Json::text("svcMenuFlag"));
                stateRequest.set("keys", keys);
                LunaReply stateReply;
                (void)waitForLuna(
                    luna_, "com.webos.settingsservice/getSystemSettings",
                    stateRequest, stateReply);
                serviceMenuLockFlag =
                    stateReply.response["settings"]["svcMenuFlag"];
                serviceMenuLockStateAvailable =
                    stateReply.response["returnValue"].boolean() &&
                    serviceMenuLockFlag.valid();
                succeeded = serviceMenuLockStateAvailable &&
                    (serviceMenuLockFlag.isBoolean() &&
                     serviceMenuLockFlag.boolean()) == serviceMenuLockTarget;
                if (!succeeded)
                    actionError = "the setting did not take";
            } else {
                actionError = "the TV would not change it";
            }
        } else if (toastAction) {
            answered = waitForCommandPolicyLuna(
                luna_, commandPolicy_, policyContext, "control.toast", uri,
                requestPayload, "notifications", false, reply,
                "com.webos.app.home");
            succeeded = answered && reply.response["returnValue"].boolean();
        } else if (homeKeyAction) {
            answered = waitForCommandPolicyLuna(
                luna_, commandPolicy_, policyContext, "control.remote-home-app", uri,
                requestPayload, "remote-input", false, reply,
                "com.webos.app.home", true);
            succeeded = answered && reply.response["returnValue"].boolean();
            if (!succeeded && commandPolicy_) {
                CommandPolicySpec localCommand;
                localCommand.action = "control.remote-home-key";
                localCommand.uri = "native/input/writeKey";
                localCommand.timeout = std::chrono::milliseconds(2500);
                localCommand.resource = "remote-input";
                const CommandDecision decision = commandPolicy_->executeLocal(
                    localCommand, policyContext, false,
                    [this]() {
                        return service::injectRemoteHomeKey(procRoot_, systemRoot_);
                    }, reply);
                answered = decision.kind == CommandDecisionKind::Allowed &&
                           reply.response["returnValue"].boolean();
                succeeded = answered;
            }
        } else if (remoteKeyAction) {
            answered = waitForCommandPolicyLuna(
                luna_, commandPolicy_, policyContext, "control.remote-key", uri,
                requestPayload, "remote-input", false, reply, std::string(), true);
            succeeded = answered && reply.response["returnValue"].boolean();
        } else if (playbackAction) {
            std::string playState;
            std::string playerType;
            if (mediaToggle) {
                if (stateSubscriptions_ && stateSubscriptions_->isLive("media")) {
                    const Json encoded =
                        stateSubscriptions_->snapshot()["media"]["snapshot"];
                    if (encoded.isString()) {
                        const Json media = Json::parse(encoded.string());
                        playState = media["state"].string();
                        playerType = media["playerType"].string();
                    }
                } else {
                    readMediaStateForControl(luna_, mediaAcbAbsent_, playState,
                                             playerType);
                }
            }
            playbackKeyCode = service::controlMediaKeyCode(
                payload["value"], playState, playerType);
            if (playbackKeyCode > 0 && commandPolicy_) {
                CommandPolicySpec localCommand;
                localCommand.action = "control.media-key";
                localCommand.uri = uri;
                localCommand.timeout = std::chrono::milliseconds(3500);
                localCommand.resource = "remote-input";
                const CommandDecision decision = commandPolicy_->executeLocal(
                    localCommand, policyContext, false,
                    [this, playbackKeyCode]() {
                        return service::injectRemoteMediaKey(
                            procRoot_, systemRoot_,
                            static_cast<unsigned>(playbackKeyCode));
                    }, reply);
                answered = decision.kind == CommandDecisionKind::Allowed &&
                           reply.response["returnValue"].boolean();
                succeeded = answered;
            }
        } else if (powerOffAction) {
            Json remoteRequest = Json::object();
            remoteRequest.set("reason", Json::text("remoteKey"));
            LunaReply remoteReply;
            answered = command("control.power-off",
                               "com.webos.service.tvpower/power/powerOff",
                               remoteRequest, "tv-power", true, remoteReply);
            reply = remoteReply;
            succeeded = answered && remoteReply.response["returnValue"].boolean();
            if (!succeeded) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1500));
                LunaReply stateReply;
                const bool stateAnswered = waitForLuna(
                    luna_, "com.webos.service.tvpower/power/getPowerState",
                    Json::object(), stateReply);
                const Json power = names::powerState(
                    Json::text(stateReply.response["state"].string()));
                if (stateAnswered && power["display"].string() != "Unknown" &&
                    !power["systemOn"].boolean()) {
                    succeeded = true;
                } else {
                    Json retryRequest = Json::object();
                    retryRequest.set("reason", Json::text("localKey"));
                    LunaReply retryReply;
                    answered = command("control.power-off",
                                       "com.webos.service.tvpower/power/powerOff",
                                       retryRequest, "tv-power", true, retryReply);
                    reply = retryReply;
                    std::string errorText = retryReply.response["errorText"].string();
                    std::string lowerError = errorText;
                    std::transform(lowerError.begin(), lowerError.end(), lowerError.begin(),
                                   [](unsigned char character) {
                                       return static_cast<char>(std::tolower(character));
                                   });
                    succeeded = (answered &&
                                 retryReply.response["returnValue"].boolean()) ||
                                lowerError.find("power is busy") != std::string::npos;
                    if (!succeeded) {
                        if (errorText.empty())
                            errorText = remoteReply.response["errorText"].string();
                        if (errorText.empty())
                            errorText = retryReply.error;
                        actionError = errorText;
                    }
                }
            }
        } else if (powerOnAction) {
            Json remoteRequest = Json::object();
            remoteRequest.set("reason", Json::text("remoteKey"));
            LunaReply remoteReply;
            answered = command("control.power-on",
                               "com.webos.service.tvpower/power/powerOn",
                               remoteRequest, "tv-power", true, remoteReply);
            reply = remoteReply;
            succeeded = answered && remoteReply.response["returnValue"].boolean();
            if (!succeeded) {
                (void)command("control.power-on.cancel",
                              "com.webos.service.tv.power/cancelPowerOff",
                              Json::object(), "tv-power", true, reply);
                std::this_thread::sleep_for(std::chrono::milliseconds(1500));
                LunaReply stateReply;
                const bool stateAnswered = waitForLuna(
                    luna_, "com.webos.service.tvpower/power/getPowerState",
                    Json::object(), stateReply);
                if (stateAnswered &&
                    stateReply.response["state"].string() == "Active") {
                    succeeded = true;
                } else {
                    answered = command("control.power-on.screen-fallback",
                                       "com.webos.service.tvpower/power/turnOnScreen",
                                       Json::object(), "tv-power", true, reply);
                    succeeded = answered && reply.response["returnValue"].boolean();
                    if (!succeeded)
                        actionError = "the TV stayed in standby";
                }
            }
        } else if (powerTimerAction) {
            const std::string readUri =
                "com.webos.service.settings/getSystemSettings";
            Json readRequest = Json::object();
            readRequest.set("category", Json::text("time"));
            Json keys = Json::array();
            keys.add(Json::text(powerTimer.enableKey));
            keys.add(Json::text("onTimerAppId"));
            keys.add(Json::text("onTimerChannel"));
            readRequest.set("keys", keys);
            LunaReply current;
            (void)waitForLuna(luna_, readUri, readRequest, current);
            const Json currentSettings = current.response["settings"];
            const bool currentlyEnabled =
                currentSettings[powerTimer.enableKey].string() == "on";

            Json timerSettings = Json::object();
            for (const std::string &key : powerTimer.settings.keys())
                timerSettings.set(key, powerTimer.settings[key]);

            const auto writeTimerSettings = [&command, &answered, &reply](
                                               const Json &settings) {
                Json request = Json::object();
                request.set("category", Json::text("time"));
                request.set("settings", settings);
                answered = command("control.power-timer",
                                   "com.webos.service.settings/setSystemSettings",
                                   request, "system-settings", false, reply);
                return answered && reply.response["returnValue"].boolean();
            };

            if (powerTimer.disable) {
                timerSettings.set(powerTimer.enableKey, Json::text("off"));
                succeeded = writeTimerSettings(timerSettings);
            } else if (!powerTimer.enable && !currentlyEnabled) {
                succeeded = writeTimerSettings(timerSettings);
            } else {
                const Json currentApp = currentSettings["onTimerAppId"];
                const Json currentChannel = currentSettings["onTimerChannel"];
                const bool channelTruthy = currentChannel.isBoolean()
                    ? currentChannel.boolean()
                    : currentChannel.isNumber()
                        ? currentChannel.number() != 0
                    : currentChannel.isString()
                        ? !currentChannel.string().empty()
                        : currentChannel.valid();
                const bool channelAlreadySelected = channelTruthy &&
                    !(currentChannel.isString() &&
                      currentChannel.string() == "noChannel");
                if (powerTimer.timer == "on" && currentApp.isString() &&
                    currentApp.string() == "com.webos.app.livetv" &&
                    !channelAlreadySelected) {
                    Json appRequest = Json::object();
                    appRequest.set("id", Json::text("com.webos.app.home"));
                    LunaReply homeApp;
                    if (waitForLuna(luna_,
                                    "com.webos.applicationManager/getAppInfo",
                                    appRequest, homeApp) &&
                        homeApp.response["returnValue"].boolean()) {
                        timerSettings.set("onTimerAppId",
                                          Json::text("com.webos.app.home"));
                    }
                }

                Json disabled = Json::object();
                disabled.set(powerTimer.enableKey, Json::text("off"));
                Json enabled = Json::object();
                enabled.set(powerTimer.enableKey, Json::text("on"));
                if (writeTimerSettings(disabled)) {
                    if (timerSettings.keys().empty() ||
                        writeTimerSettings(timerSettings))
                        succeeded = writeTimerSettings(enabled);
                }
            }
        } else if (soundOutputAction) {
            const auto field = [](const Json &value) {
                if (value.isString()) return value.string();
                if (value.isNumber()) return value.dump();
                if (value.isBoolean()) return value.boolean() ? std::string("true")
                                                              : std::string("false");
                return std::string();
            };
            const auto route = [this, &field](std::string &value) {
                LunaReply current;
                if (waitForLuna(luna_, "com.webos.service.audio/master/getVolume",
                                Json::object(), current) &&
                    current.response["volumeStatus"].isObject()) {
                    const Json status = current.response["volumeStatus"];
                    value = field(status["soundOutput"]) + "," +
                            field(status["adjustVolume"]) + "," +
                            field(status["externalDeviceControl"]);
                    return true;
                }
                LunaReply legacy;
                if (waitForLuna(luna_, "com.webos.audio/getVolume", Json::object(), legacy)) {
                    value = field(legacy.response["scenario"]) + "," +
                            field(legacy.response["volume"]);
                    return true;
                }
                value.clear();
                return false;
            };
            std::string before;
            (void)route(before);
            const auto deadline = std::chrono::steady_clock::now() +
                                  std::chrono::seconds(4);
            const auto writeOutput = [&command](const std::string &target,
                                                LunaReply &result) {
                Json write = Json::object();
                write.set("category", Json::text("sound"));
                Json settings = Json::object();
                settings.set("soundOutput", Json::text(target));
                write.set("settings", settings);
                return command("control.sound-output",
                               "com.webos.service.settings/setSystemSettings", write,
                               "system-settings", false, result) &&
                       result.response["returnValue"].boolean();
            };
            succeeded = writeOutput(soundOutput, reply);
            const std::string alias = service::controlSoundOutputAlias(soundOutput);
            if (!succeeded && !alias.empty())
                succeeded = writeOutput(alias, reply);
            if (succeeded) {
                bool routed = false;
                do {
                    std::string current;
                    if (route(current) && current != before) {
                        routed = true;
                        break;
                    }
                    if (std::chrono::steady_clock::now() >= deadline)
                        break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(400));
                } while (true);
                // The legacy route reports success once the setting is accepted,
                // even when this firmware never reports the new output route.
                (void)routed;
                answered = true;
            }
        } else if (launchAppAction) {
            LunaReply appList;
            if (!waitForLuna(luna_, "com.webos.applicationManager/listApps",
                             Json::object(), appList) ||
                !appList.response["returnValue"].boolean() ||
                !appList.response["apps"].isArray()) {
                (void)proxy_.forwardBufferedBody(exchange, body);
                return;
            }
            bool isInstalledId = false;
            const Json apps = appList.response["apps"];
            const std::vector<Json> installed = apps.items();
            for (const Json &app : installed)
                if (app["id"].string() == requestedAppId) {
                    isInstalledId = true;
                    break;
                }
            std::string resolvedId = requestedAppId;
            if (!isInstalledId) {
                std::string wanted = requestedAppId;
                std::transform(wanted.begin(), wanted.end(), wanted.begin(),
                               [](unsigned char character) {
                                   return static_cast<char>(std::tolower(character));
                               });
                for (const Json &app : installed) {
                    std::string title = app["title"].string();
                    std::transform(title.begin(), title.end(), title.begin(),
                                   [](unsigned char character) {
                                       return static_cast<char>(std::tolower(character));
                                   });
                    if (title == wanted) {
                        resolvedId = app["id"].string();
                        break;
                    }
                }
            }
            requestPayload.set("id", Json::text(resolvedId));
        }
        if (!soundOutputAction && !powerOffAction && !powerOnAction &&
            !powerTimerAction && !remoteKeyAction && !homeKeyAction &&
            !playbackAction && !toastAction && !oledProtectionAction &&
            !refresherAction &&
            !serviceMenuLockAction) {
            if (pictureModeAction) {
                Json readRequest = Json::object();
                readRequest.set("category", Json::text("picture"));
                Json keys = Json::array();
                keys.add(Json::text("pictureMode"));
                readRequest.set("keys", keys);
                LunaReply current;
                (void)waitForLuna(luna_, "com.webos.service.settings/getSystemSettings",
                                  readRequest, current);
                const Json dimension = current.response["dimension"];
                if (dimension["input"].string() == "gallery") {
                    if (logger_)
                        logger_->info("control: picture mode refused while in Gallery");
                    sendControlFailure("picture mode is locked while in Gallery");
                    return;
                }
                if (dimension.isObject())
                    requestPayload.set("dimension", dimension);
            }
            if (lgSettingAction) {
                Json descriptionRequest = Json::object();
                descriptionRequest.set("category", Json::text(lgSetting.category));
                Json keys = Json::array();
                keys.add(Json::text(lgSetting.key));
                descriptionRequest.set("keys", keys);
                LunaReply description;
                // Game Optimizer rows are not input-greyable, but Node still
                // refreshes LG's description before every supported write.
                const bool described = waitForLuna(
                    luna_, "com.webos.service.settings/getSystemSettingDesc",
                    descriptionRequest, description);
                // LG's menu greys an HDMI input's settings out while another input
                // is on screen, but its service stores a write regardless, so the
                // greying is checked here, fresh, and honoured.
                if (described && lgSetting.greyable &&
                    description.response["results"][0]["ui"]["active"].isBoolean() &&
                    !description.response["results"][0]["ui"]["active"].boolean()) {
                    sendControlFailure(say::msg("lgs.inactive",
                        "Not available with the TV’s current input or sound output."));
                    return;
                }
            }
            const std::string policyAction = volumeAction ? "control.volume"
                : muteAction ? "control.mute"
                : inputAction ? "control.input"
                : launchUrlAction ? "control.launch-url"
                : launchAppAction ? "control.launch-app"
                : closeAppAction ? "control.close-app"
                : serviceMenuOpenAction ? "control.service-menu-open"
                : serviceMenuLockAction ? "control.service-menu-lock"
                : oledProtectionAction ? "control.oled-protection"
                : pictureModeAction ? "control.picture-mode"
                : energySavingAction ? "control.energy-saving"
                : backlightAction ? "control.backlight"
                : standbyLightAction ? "control.standby-light"
                : logoLightAction ? "control.logo-light"
                : screenShiftAction ? "control.screen-shift"
                : logoDimmingAction ? "control.logo-dimming"
                : alwaysReadyAction ? "control.always-on"
                : alwaysReadyScreenAction ? "control.always-ready-screen"
                : alwaysReadyOffStartAction ? "control.always-on-off-start"
                : wakeOnLanAction ? "control.wake-on-lan"
                : lgLogoAction ? "control.lg-logo"
                : lgSettingAction ? "control.lg-setting"
                : sleepTimerAction ? "control.sleep-timer"
                : quickBootAction ? "control.quick-boot"
                : deviceDetectionAction ? "control.device-detection"
                : powerTimerAction ? "control.power-timer"
                : screenOffAction ? "control.screen-off" : "control.screen-on";
            const std::string resource = volumeAction ? "audio-volume"
                : muteAction ? "audio-mute"
            : inputAction || launchUrlAction || launchAppAction || closeAppAction ||
                    serviceMenuOpenAction
                    ? "application-manager"
                : pictureModeAction || energySavingAction || backlightAction ||
                          standbyLightAction || logoLightAction || screenShiftAction ||
                          logoDimmingAction || alwaysReadyAction ||
                          alwaysReadyScreenAction || alwaysReadyOffStartAction ||
                          wakeOnLanAction || lgLogoAction || lgSettingAction ||
                          soundOutputAction || serviceMenuLockAction ||
                          oledProtectionAction ||
                          sleepTimerAction || quickBootAction || deviceDetectionAction ||
                          powerTimerAction
                    ? "system-settings"
                : screenOffAction || screenOnAction ? "tv-screen" : "control";
            answered = command(policyAction, uri, requestPayload, resource,
                               powerOffAction || powerOnAction, reply);
            succeeded = answered && reply.response["returnValue"].boolean();
        }
        if ((succeeded && (screenShiftAction || logoDimmingAction)) ||
            (oledProtectionAction && oledProtectionWriteAttempted) ||
            (refresherAction && refresherWriteAttempted))
            invalidateOledCareCache();
    }
    const bool usesSetVolume = volumeAction ||
                               (volumeStepAction && step != 1 && step != -1);
    if ((usesSetVolume || muteAction) && answered &&
        service::volumeScenarioUnsupported(reply.response)) {
        statsCollector()->noteVolumeRefused();
        if (nodeFreeStart()) {
            // Node's answer to a refused volume change: not ok, nothing more.
            (void)exchange.respond(200,
                                   {{"Content-Type", "application/json"},
                                    {"Cache-Control", "no-store"},
                                    {"X-Content-Type-Options", "nosniff"}},
                                   "{\"ok\":false}");
            return;
        }
        if (logger_)
            logger_->info("control: audio request delegated for unsupported output scenario");
        (void)proxy_.forwardBufferedBody(exchange, body);
        return;
    }

    if (logger_) {
        const std::string description = volumeAction ? "volume " + std::to_string(target)
            : volumeStepAction ? "volume step " + std::to_string(step)
            : muteAction ? std::string("mute ") + (mute ? "enabled" : "disabled")
            : powerOffAction ? "power off"
            : powerOnAction ? "power on"
            : soundOutputAction ? "sound output " + soundOutput
            : inputAction ? "input " + inputAppId
            : launchUrlAction ? "launch URL " + launchUrl
            : launchAppAction ? "launch app " + requestPayload["id"].string()
            : closeAppAction ? "close app " + closeAppId
            : serviceMenuOpenAction ? "service menu " +
                    service::controlServiceMenuTarget(payload["value"])
            : serviceMenuLockAction ? std::string("service menu lock ") +
                    (serviceMenuLockTarget ? "on" : "off")
            : oledProtectionAction ? std::string("OLED protection ") +
                    oledProtectionKey + (oledProtectionEnabled ? " on" : " off")
            : refresherScheduleAction ? "OLED pixel refresher schedule"
            : refresherCancelAction ? "OLED pixel refresher cancel"
            : toastAction ? "toast"
            : homeKeyAction ? "remote Home"
            : remoteKeyAction ? "remote key code " + std::to_string(remoteKeyCode)
            : playbackAction ? "media key code " +
                    std::to_string(playbackKeyCode)
            : pictureModeAction ? "picture mode " + pictureMode
            : energySavingAction ? "energy saving " + energySaving
            : backlightAction ? "backlight " + std::to_string(backlight)
            : standbyLightAction ? std::string("standby light ") +
                    (service::controlIndicatorLightTarget(payload["value"]) ? "on" : "off")
            : logoLightAction ? std::string("logo light ") +
                    (service::controlIndicatorLightTarget(payload["value"]) ? "on" : "off")
            : screenShiftAction ? std::string("screen shift ") +
                    (service::controlIndicatorLightTarget(payload["value"]) ? "on" : "off")
            : logoDimmingAction ? "logo dimming " + logoDimming
            : alwaysReadyAction ? std::string("always-on ") +
                    (service::controlIndicatorLightTarget(payload["value"]) ? "on" : "off")
            : alwaysReadyScreenAction ? std::string("always-ready screen ") +
                    (service::controlIndicatorLightTarget(payload["value"]) ? "on" : "off")
            : alwaysReadyOffStartAction ? "always-on nightly start " +
                    std::to_string(alwaysReadyOffStartHour)
            : wakeOnLanAction ? std::string("wake-on-LAN ") +
                    (service::controlIndicatorLightTarget(payload["value"]) ? "on" : "off")
            : lgLogoAction ? std::string("LG logo display ") +
                    (service::controlIndicatorLightTarget(payload["value"]) ? "on" : "off")
            : lgSettingAction ? "LG setting " + lgSetting.id
            : sleepTimerAction ? "sleep timer " + sleepTimer
            : quickBootAction ? std::string("quick boot ") +
                    (service::controlIndicatorLightTarget(payload["value"]) ? "on" : "off")
            : deviceDetectionAction ? std::string("device detection ") +
                    (service::controlIndicatorLightTarget(payload["value"]) ? "on" : "off")
            : powerTimerAction ? "power timer " + powerTimer.timer
            : screenOffAction ? "screen off" : "screen on";
        logger_->info("control: " + description +
                      (succeeded ? " applied" : " refused or unavailable"));
    }
    if (succeeded || (oledProtectionAction && oledProtectionWriteAttempted))
        invalidateLgSettingsCache();
    if (succeeded || ((remoteKeyAction || homeKeyAction || sleepTimerAction || quickBootAction || deviceDetectionAction ||
                       lgSettingAction) && answered) ||
        (oledProtectionAction && oledProtectionWriteAttempted) ||
        (refresherAction && refresherWriteAttempted)) {
        if (succeeded) invalidatePrivacyCache();
        Json invalidation = Json::object();
        invalidation.set("type", Json::text("invalidate"));
        invalidation.set("group", Json::text("control"));
        std::string error;
        if (!proxy_.requestNodeState(invalidation, error, 1000) && logger_)
            logger_->warning("control: could not invalidate Node's caches" +
                             (error.empty() ? std::string() : ": " + error));
    }

    Json response = Json::object();
    response.set("ok", Json::flag(succeeded));
    if ((powerOffAction || powerOnAction) && !succeeded && !actionError.empty())
        response.set("error", Json::text(actionError));
    if (launchUrlAction && !succeeded && !reply.response["errorText"].string().empty())
        response.set("error", Json::text(reply.response["errorText"].string()));
    if (serviceMenuLockAction && !succeeded)
        response.set("error", Json::text(actionError));
    if (oledProtectionAction && !succeeded)
        response.set("error", Json::text(actionError));
    if (toastAction && !succeeded) {
        const std::string error = reply.response["errorText"].string().empty()
            ? reply.error : reply.response["errorText"].string();
        if (!error.empty())
            response.set("error", Json::text(error));
    }
    if (serviceMenuOpenAction)
        response.set("menu", Json::text(
            service::controlServiceMenuTarget(payload["value"])));
    if (serviceMenuLockAction && serviceMenuLockWriteSucceeded) {
        Json state = Json::object();
        state.set("ok", Json::flag(true));
        state.set("app", Json::flag(serviceMenuAppPresent(systemRoot_)));
        state.set("lockable", Json::flag(serviceMenuLockStateAvailable));
        state.set("locked", serviceMenuLockStateAvailable
            ? Json::flag(serviceMenuLockFlag.isBoolean() &&
                         serviceMenuLockFlag.boolean())
            : Json());
        state.set("writable", Json::flag(config_.allowControl));
        response.set("state", state);
    }
    if (oledProtectionWriteAttempted && answered &&
        reply.response["returnValue"].boolean())
        response.set("state", oledProtectionState);
    (void)exchange.respond(200,
                           {{"Content-Type", "application/json; charset=utf-8"},
                            {"Cache-Control", "no-store"},
                            {"X-Content-Type-Options", "nosniff"}},
                           response.dump());
}

Json HttpFrontDoor::recordHomeRecent(const std::string &row,
                                     const std::string &id) const {
    if (!homeRecentHistoryState_) return Json::object();
    return stampHomeRecentHistoryFile(*homeRecentHistoryState_, row, id,
                                      homeNowMilliseconds());
}

void HttpFrontDoor::clearAdCookiesControl(HttpExchange &exchange) const {
    const auto respond = [&exchange](const Json &response) {
        (void)exchange.respond(200,
                               {{"Content-Type", "application/json; charset=utf-8"},
                                {"Cache-Control", "no-store"},
                                {"X-Content-Type-Options", "nosniff"}},
                               response.dump());
    };
    const auto invalidateNodeCaches = [this]() {
        invalidatePrivacyCache();
        Json invalidation = Json::object();
        invalidation.set("type", Json::text("invalidate"));
        invalidation.set("group", Json::text("control"));
        std::string error;
        if (!proxy_.requestNodeState(invalidation, error, 1000) && logger_)
            logger_->warning("control: could not invalidate Node's caches" +
                             (error.empty() ? std::string() : ": " + error));
    };

    if (service::privacyServiceWouldStart(systemRoot_, procRoot_,
                                          "com.webos.service.admanager")) {
        Json response = Json::object();
        response.set("ok", Json::flag(false));
        response.set("error", Json::text(
            "LG’s advertising service is not running, and this would start it."));
        respond(response);
        return;
    }

    LunaReply reply;
    const CommandPolicyContext context = controlPolicyContext(config_);
    const bool answered = waitForCommandPolicyLuna(
        luna_, commandPolicy_, context, "control.clear-ad-cookies",
        "com.webos.service.admanager/inactivateCookies", Json::object(),
        "ad-manager", false, reply);
    const bool succeeded = answered &&
        !(reply.response["returnValue"].isBoolean() &&
          !reply.response["returnValue"].boolean());
    invalidateNodeCaches();
    if (logger_)
        logger_->info(std::string("control: clear ad cookies ") +
                      (succeeded ? "applied" : "refused or unavailable"));
    Json response = Json::object();
    response.set("ok", Json::flag(succeeded));
    respond(response);
}

void HttpFrontDoor::resetAdIdControl(HttpExchange &exchange) const {
    const auto respond = [&exchange](const Json &response) {
        (void)exchange.respond(200,
                               {{"Content-Type", "application/json; charset=utf-8"},
                                {"Cache-Control", "no-store"},
                                {"X-Content-Type-Options", "nosniff"}},
                               response.dump());
    };
    if (service::privacyServiceWouldStart(systemRoot_, procRoot_,
                                          "com.webos.service.admanager")) {
        Json response = Json::object();
        response.set("ok", Json::flag(false));
        response.set("error", Json::text(
            "LG’s advertising service is not running, and this would start it."));
        respond(response);
        return;
    }

    LunaReply before;
    const bool readBefore = waitForLuna(
        luna_, "com.webos.service.admanager/getAdid", Json::object(), before);
    const Json previousValue = before.response["IFA"];
    const std::string previousId = readBefore &&
        service::javascriptTruthy(previousValue)
        ? service::javascriptString(previousValue) : std::string();

    LunaReply reset;
    const CommandPolicyContext context = controlPolicyContext(config_);
    const bool resetAnswered = waitForCommandPolicyLuna(
        luna_, commandPolicy_, context, "control.reset-ad-id",
        "com.webos.service.admanager/resetIFA", Json::object(),
        "ad-manager", false, reset);

    LunaReply after;
    const bool readAfter = waitForLuna(
        luna_, "com.webos.service.admanager/getAdid", Json::object(), after);
    const Json currentValue = after.response["IFA"];
    const std::string currentId = readAfter &&
        service::javascriptTruthy(currentValue)
        ? service::javascriptString(currentValue) : std::string();
    invalidateNodeControlCaches();

    const bool succeeded = resetAnswered &&
        !(reset.response["returnValue"].isBoolean() &&
          !reset.response["returnValue"].boolean());
    Json response = Json::object();
    response.set("ok", Json::flag(succeeded));
    response.set("changed", Json::flag(!previousId.empty() && !currentId.empty() &&
                                        previousId != currentId));
    if (logger_)
        logger_->info(std::string("control: reset advertising ID ") +
                      (succeeded ? "applied" : "refused or unavailable"));
    respond(response);
}

void HttpFrontDoor::setLimitAdTrackingControl(HttpExchange &exchange,
                                              const Json &value) const {
    const bool enabled = (value.isBoolean() && value.boolean()) ||
        (value.isString() && (value.string() == "on" || value.string() == "true"));
    const auto respond = [&exchange](const Json &response) {
        (void)exchange.respond(200,
                               {{"Content-Type", "application/json; charset=utf-8"},
                                {"Cache-Control", "no-store"},
                                {"X-Content-Type-Options", "nosniff"}},
                               response.dump());
    };
    const auto fail = [&respond](const std::string &error) {
        Json response = Json::object();
        response.set("ok", Json::flag(false));
        response.set("error", Json::text(error));
        respond(response);
    };
    const auto invalidateNodeCaches = [this]() {
        invalidatePrivacyCache();
        Json invalidation = Json::object();
        invalidation.set("type", Json::text("invalidate"));
        invalidation.set("group", Json::text("control"));
        std::string error;
        if (!proxy_.requestNodeState(invalidation, error, 1000) && logger_)
            logger_->warning("control: could not invalidate Node's caches" +
                             (error.empty() ? std::string() : ": " + error));
    };
    const auto readLimitTracking = [this](Json &settings) {
        Json request = Json::object();
        request.set("category", Json::text("general"));
        Json keys = Json::array();
        keys.add(Json::text("lmt"));
        request.set("keys", keys);
        LunaReply reply;
        if (!waitForLuna(luna_, "com.webos.settingsservice/getSystemSettings",
                         request, reply) || !reply.response["settings"].isObject())
            return false;
        settings = reply.response["settings"];
        return true;
    };

    std::unique_lock<std::mutex> settingsLock(systemSettingsControlMutex_);
    Json settings;
    if (!readLimitTracking(settings) || !settings["lmt"].valid()) {
        fail("This TV has no Limit ad tracking setting.");
        return;
    }

    Json writeRequest = Json::object();
    writeRequest.set("category", Json::text("general"));
    Json changedSettings = Json::object();
    changedSettings.set("lmt", Json::text(enabled ? "on" : "off"));
    writeRequest.set("settings", changedSettings);
    LunaReply writeReply;
    const CommandPolicyContext context = controlPolicyContext(config_);
    const bool answered = waitForCommandPolicyLuna(
        luna_, commandPolicy_, context, "control.limit-ad-tracking",
        "com.webos.settingsservice/setSystemSettings", writeRequest,
        "system-settings", false, writeReply);
    const bool wrote = answered && writeReply.response["returnValue"].isBoolean() &&
        writeReply.response["returnValue"].boolean();
    if (!wrote) {
        settingsLock.unlock();
        const std::string error = writeReply.response["errorText"].string();
        fail(error.empty() ? "the TV would not change it" : error);
        if (logger_)
            logger_->info(std::string("control: limit ad tracking ") +
                          (enabled ? "on" : "off") + " refused or unavailable");
        return;
    }

    bool now = false;
    if (service::privacyServiceWouldStart(systemRoot_, procRoot_,
                                          "com.webos.service.admanager")) {
        Json verifiedSettings;
        if (readLimitTracking(verifiedSettings)) {
            std::string current = service::javascriptString(verifiedSettings["lmt"]);
            std::transform(current.begin(), current.end(), current.begin(),
                           [](unsigned char character) {
                               return static_cast<char>(std::tolower(character));
                           });
            now = current == "on";
        }
    } else {
        LunaReply adManagerReply;
        if (waitForLuna(luna_, "com.webos.service.admanager/getAdid",
                        Json::object(), adManagerReply)) {
            std::string current = service::javascriptString(adManagerReply.response["LMT"]);
            std::transform(current.begin(), current.end(), current.begin(),
                           [](unsigned char character) {
                               return static_cast<char>(std::tolower(character));
                           });
            now = current == "on";
        }
    }
    settingsLock.unlock();
    invalidateNodeCaches();
    const bool applied = now == enabled;
    if (logger_)
        logger_->info(std::string("control: limit ad tracking ") +
                      (enabled ? "on" : "off") +
                      (applied ? " applied" : " refused or unavailable"));

    Json response = Json::object();
    response.set("ok", Json::flag(applied));
    if (applied) {
        response.set("limitTracking", Json::flag(now));
    } else {
        response.set("error", Json::text("the setting did not take"));
    }
    respond(response);
}

void HttpFrontDoor::invalidateNodeControlCaches() const {
    invalidatePrivacyCache();
    Json invalidation = Json::object();
    invalidation.set("type", Json::text("invalidate"));
    invalidation.set("group", Json::text("control"));
    std::string error;
    if (!proxy_.requestNodeState(invalidation, error, 1000) && logger_)
        logger_->warning("control: could not invalidate Node's caches" +
                         (error.empty() ? std::string() : ": " + error));
}

void HttpFrontDoor::setAdBlockCustom(HttpExchange &exchange) const {
    const HttpRequest &request = exchange.request();
    if (request.method != "POST") {
        sendSetupError(exchange, 405, "method not allowed");
        return;
    }
    if (!setupJsonContentType(request)) {
        sendSetupError(exchange, 415, "Content-Type must be application/json");
        return;
    }
    if (!config_.allowControl) {
        sendSetupError(exchange, 403, "controls disabled in config");
        return;
    }
    if (!adBlockManager_ || !commandPolicy_) {
        sendSetupError(exchange, 502, "native ad-block service is unavailable");
        return;
    }
    if (!exchange.setBodyLimit(4 * 1024 * 1024)) {
        sendSetupError(exchange, 413, "request body too large", true);
        return;
    }
    std::string body;
    char buffer[4096];
    for (;;) {
        const int count = exchange.readBody(buffer, sizeof(buffer));
        if (count == 0)
            break;
        if (count < 0) {
            sendSetupError(exchange, exchange.bodyLimitExceeded() ? 413 : 400,
                           exchange.bodyLimitExceeded() ? "request body too large"
                                                        : "invalid request body",
                           true);
            return;
        }
        body.append(buffer, static_cast<size_t>(count));
    }
    if (exceedsUtf16CodeUnitLimit(body, 1048576)) {
        sendSetupError(exchange, 413, "request body too large", true);
        return;
    }
    const Json payload = Json::parse(body);
    if (!payload.isObject()) {
        sendSetupError(exchange, 400, "malformed JSON");
        return;
    }

    CommandPolicySpec localCommand;
    localCommand.action = "privacy.adblock-custom";
    localCommand.uri = "native/hosts-table/custom";
    localCommand.timeout = std::chrono::milliseconds(3000);
    localCommand.resource = "hosts-table";
    const CommandPolicyContext context = controlPolicyContext(config_);
    Json operationResult;
    LunaReply localReply;
    const CommandDecision decision = commandPolicy_->executeLocal(
        localCommand, context, false,
        [this, &payload, &operationResult]() {
            operationResult = adBlockManager_->setCustomHosts(payload);
            return operationResult["ok"].isBoolean() &&
                   operationResult["ok"].boolean();
        }, localReply);
    const bool succeeded = decision.kind == CommandDecisionKind::Allowed &&
        localReply.response["returnValue"].isBoolean() &&
        localReply.response["returnValue"].boolean();
    if (succeeded) {
        invalidateNodeControlCaches();
    } else if (!operationResult.isObject() || operationResult["ok"].boolean()) {
        operationResult = Json::object();
        operationResult.set("ok", Json::flag(false));
        const std::string error = !localReply.error.empty() ? localReply.error
            : !decision.reason.empty() ? decision.reason
            : "could not update the custom hosts list";
        operationResult.set("error", Json::text(error));
    }
    const int status = succeeded ? 200
        : decision.kind == CommandDecisionKind::Allowed ? 400 : 200;
    (void)exchange.respond(status,
                           {{"Content-Type", "application/json; charset=utf-8"},
                            {"Cache-Control", "no-store"},
                            {"X-Content-Type-Options", "nosniff"}},
                           operationResult.dump());
}

Json HttpFrontDoor::applyConsentChange(const std::string &key,
                                       bool enabled) const {
    const auto failure = [](const std::string &error) {
        Json response = Json::object();
        response.set("ok", Json::flag(false));
        response.set("error", Json::text(error));
        return response;
    };
    if (key.empty()) return failure("no consent flag named");

    std::string root = systemRoot_.empty() ? "/" : systemRoot_;
    if (root[root.size() - 1] != '/') root.push_back('/');
    const std::string mappingPath = root + "var/palm/license/eulaInfoNetwork.json";
    std::ifstream mappingFile(mappingPath.c_str(), std::ios::in | std::ios::binary);
    std::string mappingText;
    if (mappingFile)
        mappingText.assign(std::istreambuf_iterator<char>(mappingFile),
                           std::istreambuf_iterator<char>());
    const Json groups = service::privacyConsentGroups(Json::parse(mappingText));
    if (!service::privacyConsentSettable(key, groups))
        return failure(key + " is not changeable from here");

    Json readFlagsRequest = Json::object();
    Json flagKeys = Json::array();
    flagKeys.add(Json::text("eulaStatus"));
    readFlagsRequest.set("keys", flagKeys);
    LunaReply flagsReply;
    if (!waitForLuna(luna_, "com.webos.settingsservice/getSystemSettings",
                     readFlagsRequest, flagsReply) ||
        !flagsReply.response["settings"]["eulaStatus"].isObject())
        return failure("could not read the consent flags");
    const Json flags = flagsReply.response["settings"]["eulaStatus"];

    Json readDocumentsRequest = Json::object();
    Json documentKeys = Json::array();
    documentKeys.add(Json::text("eulaInfoNetwork"));
    readDocumentsRequest.set("keys", documentKeys);
    LunaReply documentsReply;
    if (!waitForLuna(luna_, "com.webos.settingsservice/getSystemSettings",
                     readDocumentsRequest, documentsReply) ||
        !documentsReply.response["settings"]["eulaInfoNetwork"]["eulaList"].isArray())
        return failure("could not read the agreement documents");
    const Json documents = documentsReply.response["settings"]["eulaInfoNetwork"];

    const service::PrivacyConsentPlan plan = service::planPrivacyConsent(
        key, enabled, flags, documents, groups);
    if (!plan.valid) return failure(plan.error);
    if (!plan.changed) {
        invalidateNodeControlCaches();
        Json response = Json::object();
        response.set("ok", Json::flag(true));
        response.set("key", Json::text(key));
        response.set("enabled", Json::flag(enabled));
        response.set("changed", Json::flag(false));
        return response;
    }

    Json update = Json::object();
    update.set("eulaInfoNetwork", plan.documents);
    update.set("eulaStatus", plan.flags);
    Json writeRequest = Json::object();
    writeRequest.set("settings", update);
    LunaReply writeReply;
    const CommandPolicyContext context = controlPolicyContext(config_);
    const bool answered = waitForCommandPolicyLuna(
        luna_, commandPolicy_, context, "control.consent",
        "com.webos.settingsservice/setSystemSettings", writeRequest,
        "system-settings", false, writeReply);
    invalidateNodeControlCaches();
    if (!answered || !service::javascriptTruthy(writeReply.response["returnValue"])) {
        const std::string error = writeReply.response["errorText"].string();
        return failure(error.empty() ? "the TV refused the change" : error);
    }

    Json verifyRequest = Json::object();
    verifyRequest.set("keys", flagKeys);
    LunaReply verifyReply;
    (void)waitForLuna(luna_, "com.webos.settingsservice/getSystemSettings",
                      verifyRequest, verifyReply);
    const Json current = verifyReply.response["settings"]["eulaStatus"][key];
    const bool applied = current.isBoolean() && current.boolean() == enabled;
    if (logger_)
        logger_->info("privacy: consent " + key + (enabled ? " enabled" : " disabled") +
                      (applied ? "" : " (accepted but not applied)"));

    Json response = Json::object();
    response.set("ok", Json::flag(applied));
    if (!applied) {
        response.set("error", Json::text("the TV accepted the change without applying it"));
    } else {
        response.set("key", Json::text(key));
        response.set("enabled", Json::flag(enabled));
        response.set("changed", Json::flag(true));
        response.set("alsoChanged", Json::numeric(plan.alsoChanged));
    }
    return response;
}

void HttpFrontDoor::setConsentControl(HttpExchange &exchange, const Json &value) const {
    const Json requestedKey = value["key"];
    const std::string key = service::javascriptTruthy(requestedKey)
        ? service::javascriptString(requestedKey) : std::string();
    const Json requestedEnabled = value["enabled"];
    const bool enabled = (requestedEnabled.isBoolean() && requestedEnabled.boolean()) ||
        (requestedEnabled.isString() && requestedEnabled.string() == "true");
    std::unique_lock<std::mutex> settingsLock(systemSettingsControlMutex_);
    const Json response = applyConsentChange(key, enabled);
    settingsLock.unlock();
    (void)exchange.respond(200,
                           {{"Content-Type", "application/json; charset=utf-8"},
                            {"Cache-Control", "no-store"},
                            {"X-Content-Type-Options", "nosniff"}},
                           response.dump());
}

void HttpFrontDoor::setAcrControl(HttpExchange &exchange, const Json &value) const {
    const bool enabled = (value.isBoolean() && value.boolean()) ||
        (value.isString() && (value.string() == "on" || value.string() == "ON" ||
                              value.string() == "true"));
    std::unique_lock<std::mutex> settingsLock(systemSettingsControlMutex_);

    Json liveReadRequest = Json::object();
    liveReadRequest.set("category", Json::text("option"));
    Json liveKeys = Json::array();
    liveKeys.add(Json::text("livePlus"));
    liveReadRequest.set("keys", liveKeys);
    LunaReply liveReadReply;
    const bool liveRead = waitForLuna(
        luna_, "com.webos.service.settings/getSystemSettings",
        liveReadRequest, liveReadReply);
    const bool hasLivePlus = liveRead &&
        liveReadReply.response["settings"].has("livePlus");
    bool livePlusWritten = false;
    if (hasLivePlus) {
        Json liveSettings = Json::object();
        liveSettings.set("livePlus", Json::text(enabled ? "on" : "off"));
        Json liveWriteRequest = Json::object();
        liveWriteRequest.set("category", Json::text("option"));
        liveWriteRequest.set("settings", liveSettings);
        LunaReply liveWriteReply;
        const CommandPolicyContext context = controlPolicyContext(config_);
        livePlusWritten = waitForCommandPolicyLuna(
            luna_, commandPolicy_, context, "control.acr-live-plus",
            "com.webos.service.settings/setSystemSettings", liveWriteRequest,
            "system-settings", false, liveWriteReply) &&
            service::javascriptTruthy(liveWriteReply.response["returnValue"]);
    }

    std::string root = systemRoot_.empty() ? "/" : systemRoot_;
    if (root[root.size() - 1] != '/') root.push_back('/');
    std::ifstream acrBinary(root + "usr/sbin/acr2", std::ios::in | std::ios::binary);
    std::string acrText;
    if (acrBinary)
        acrText.assign(std::istreambuf_iterator<char>(acrBinary),
                       std::istreambuf_iterator<char>());
    std::vector<std::string> consentKeys;
    const char *const candidates[] = {"acrAllowed", "acrOnAllowed"};
    for (const char *candidate : candidates)
        if (acrText.find(candidate) != std::string::npos)
            consentKeys.push_back(candidate);
    if (consentKeys.empty()) consentKeys.push_back("acrAllowed");

    bool flagSucceeded = false;
    unsigned alsoChanged = 0;
    for (const std::string &key : consentKeys) {
        const Json result = applyConsentChange(key, enabled);
        if (!result["ok"].boolean()) continue;
        flagSucceeded = true;
        if (result["alsoChanged"].isNumber())
            alsoChanged += static_cast<unsigned>(result["alsoChanged"].number());
    }
    settingsLock.unlock();

    Json response = Json::object();
    response.set("ok", Json::flag(hasLivePlus ? livePlusWritten : flagSucceeded));
    response.set("alsoChanged", Json::numeric(alsoChanged));
    if (logger_)
        logger_->info(std::string("control: ACR ") + (enabled ? "on" : "off") +
                      (response["ok"].boolean() ? " applied" : " refused or unavailable"));
    (void)exchange.respond(200,
                           {{"Content-Type", "application/json; charset=utf-8"},
                            {"Cache-Control", "no-store"},
                            {"X-Content-Type-Options", "nosniff"}},
                           response.dump());
}

void HttpFrontDoor::setPiccapControl(HttpExchange &exchange,
                                     const Json &value) const {
    const auto respond = [&exchange](bool succeeded, const std::string &error) {
        Json response = Json::object();
        response.set("ok", Json::flag(succeeded));
        if (!succeeded && !error.empty())
            response.set("error", Json::text(error));
        (void)exchange.respond(200,
                               {{"Content-Type", "application/json; charset=utf-8"},
                                {"Cache-Control", "no-store"},
                                {"X-Content-Type-Options", "nosniff"}},
                               response.dump());
    };

    if (!service::piccapInstalled(systemRoot_) ||
        !service::piccapSupportedWebos(systemRoot_)) {
        respond(false, "PicCap is not available");
        return;
    }

    std::lock_guard<std::mutex> commandLock(piccapControlMutex_);
    LunaReply status;
    const bool statusAnswered = waitForLuna(
        luna_, "org.webosbrew.piccap.service/status", Json::object(), status);
    const Json statusResult = status.response["returnValue"];
    const bool available = statusAnswered &&
        !(statusResult.isBoolean() && !statusResult.boolean()) &&
        status.response["isRunning"].isBoolean();
    if (!available) {
        if (logger_)
            logger_->info("control: PicCap capture unavailable");
        respond(false, "PicCap is not available");
        return;
    }

    const bool turnOn = service::controlIndicatorLightTarget(value);
    const std::string verb = turnOn ? "start" : "stop";
    const std::string uri = "org.webosbrew.piccap.service/" + verb;
    LunaReply commandReply;
    const CommandPolicyContext policyContext = controlPolicyContext(config_);
    const bool answered = waitForCommandPolicyLuna(
        luna_, commandPolicy_, policyContext, "control.piccap." + verb, uri,
        Json::object(), "piccap", false, commandReply);
    const bool succeeded = answered &&
        commandReply.response["returnValue"].isBoolean() &&
        commandReply.response["returnValue"].boolean();

    // PicCap changes its capture state asynchronously; preserve the legacy
    // settle delay and status refresh after either an accepted or refused call.
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    LunaReply refreshedStatus;
    (void)waitForLuna(luna_, "org.webosbrew.piccap.service/status",
                      Json::object(), refreshedStatus);

    std::string error;
    if (!succeeded) {
        error = commandReply.response["errorText"].string();
        if (error.empty())
            error = commandReply.response["errorCode"].string();
        if (error.empty())
            error = commandReply.error;
        if (error.empty())
            error = "PicCap refused the request";
    }
    if (logger_)
        logger_->info("control: PicCap capture " + verb +
                      (succeeded ? " applied" : " refused or unavailable"));
    respond(succeeded, error);
}

std::string HttpFrontDoor::activeHandoffUrl() const {
    std::lock_guard<std::mutex> lock(handoffMutex_);
    if (!handoffCode_.empty() && std::chrono::steady_clock::now() >= handoffExpires_) {
        retiredHandoffCode_ = handoffCode_;
        handoffCode_.clear();
        handoffExpires_ = std::chrono::steady_clock::time_point();
    }
    if (handoffCode_.empty())
        return std::string();
    const std::string address = localIpv4Address();
    if (address.empty())
        return std::string();
    return "http://" + address + ":" + std::to_string(config_.port + 1) + "/?c=" +
           handoffCode_;
}

bool HttpFrontDoor::setupHandoffAction(HttpExchange &exchange,
                                       const std::string &expectedAction,
                                       const Json *providedPayload) const {
    if (!isLoopbackPeer(exchange.request().remoteAddress)) {
        sendSetupError(exchange, 403, "TV-local setup action required", true);
        return true;
    }
    if (exchange.request().method != "POST") {
        sendSetupError(exchange, 405, "method not allowed", true);
        return true;
    }
    if (!setupJsonContentType(exchange.request())) {
        sendSetupError(exchange, 415, "Content-Type must be application/json");
        return true;
    }
    Json payload;
    if (!getSetupPayload(exchange, providedPayload, payload))
        return true;
    if (payload["action"].string() != expectedAction) {
        sendSetupError(exchange, 400, "invalid setup action");
        return true;
    }
    if (setupDemo(config_, exchange.request(), payload)) {
        std::string body = "{\"ok\":true,\"demo\":true}";
        if (expectedAction == "handoff") {
            std::string address = localIpv4Address();
            if (address.empty())
                address = "192.168.1.131";
            body = "{\"ok\":true,\"demo\":true,\"url\":\"http://" + address + ":" +
                   std::to_string(config_.port) + "/?c=demo\"}";
        }
        (void)exchange.respond(200,
                               {{"Content-Type", "application/json"},
                                {"Cache-Control", "no-store"},
                                {"X-Content-Type-Options", "nosniff"}},
                               body);
        return true;
    }
    if (!config_.allowControl) {
        sendSetupError(exchange, 403, "controls disabled in config");
        return true;
    }

    std::string body = "{\"ok\":true}";
    if (expectedAction == "handoff") {
        const std::string address = localIpv4Address();
        if (address.empty()) {
            sendSetupError(exchange, 500, "this TV has no network address");
            return true;
        }
        std::lock_guard<std::mutex> lock(handoffMutex_);
        if (handoffCode_.empty() || std::chrono::steady_clock::now() >= handoffExpires_) {
            if (!handoffCode_.empty())
                retiredHandoffCode_ = handoffCode_;
            std::string code;
            if (!createHandoffCode(code)) {
                sendSetupError(exchange, 500, "could not create setup link");
                return true;
            }
            handoffCode_ = code;
            handoffExpires_ = std::chrono::steady_clock::now() + std::chrono::minutes(10);
        }
        body = "{\"ok\":true,\"url\":\"http://" + address + ":" +
               std::to_string(config_.port + 1) + "/?c=" + handoffCode_ + "\"}";
    } else {
        std::lock_guard<std::mutex> lock(handoffMutex_);
        if (!handoffCode_.empty())
            retiredHandoffCode_ = handoffCode_;
        handoffCode_.clear();
        handoffExpires_ = std::chrono::steady_clock::time_point();
    }
    (void)exchange.respond(200,
                           {{"Content-Type", "application/json"},
                            {"Cache-Control", "no-store"},
                            {"X-Content-Type-Options", "nosniff"}},
                           body);
    return true;
}

void HttpFrontDoor::handleHdmiSeen(HttpExchange &exchange) const {
    const HttpRequest &request = exchange.request();
    if (!isLoopbackPeer(request.remoteAddress) || proxySecret_.empty() ||
        !tokenMatchesConstantTime(request.header("x-nativetv-proxy-secret"), proxySecret_) ||
        !request.header("x-nativetv-original-remote").empty()) {
        sendSetupError(exchange, 403, "native HDMI observation required", true);
        return;
    }
    if (request.method != "POST") {
        sendSetupError(exchange, 405, "method not allowed");
        return;
    }
    if (!setupJsonContentType(request)) {
        sendSetupError(exchange, 415, "Content-Type must be application/json");
        return;
    }
    Json payload;
    if (!readSetupPayload(exchange, payload))
        return;
    const Json diagnostics = payload["diag"];
    if (!diagnostics.isObject()) {
        sendSetupError(exchange, 400, "invalid HDMI diagnostics");
        return;
    }
    std::string error;
    bool recorded = false;
    {
        std::lock_guard<std::mutex> lock(hdmiSeenMutex_);
        recorded = recordHdmiSeen(hdmiSeenPath_, diagnostics, &error);
    }
    if (!recorded) {
        if (logger_)
            logger_->warning("hdmi_seen: " + error);
        sendSetupError(exchange, 500, "could not persist HDMI diagnostics");
        return;
    }
    (void)exchange.respond(204,
                           {{"Cache-Control", "no-store"},
                            {"X-Content-Type-Options", "nosniff"}},
                           "");
}

// Node's remote-key apply and restore, for the Home key and the starting message
// it sets and for its uninstall: the one reconcile the daemon already runs at
// start, on settings changes and on power changes, so two copies do not take
// turns writing LG's values and remotekeys-lg.json.
void HttpFrontDoor::handleRemoteKeysApply(HttpExchange &exchange) const {
    const HttpRequest &request = exchange.request();
    if (!isLoopbackPeer(request.remoteAddress) || proxySecret_.empty() ||
        !tokenMatchesConstantTime(request.header("x-nativetv-proxy-secret"), proxySecret_) ||
        !request.header("x-nativetv-original-remote").empty()) {
        sendSetupError(exchange, 403, "native remote keys apply required", true);
        return;
    }
    if (request.method != "POST") {
        sendSetupError(exchange, 405, "method not allowed");
        return;
    }
    Json payload;
    if (!readSetupPayload(exchange, payload)) return;
    std::string error;
    bool wrote = false;
    const bool ok = payload["restore"].boolean()
        ? restoreNativeRemoteKeys(error) : reconcileNativeRemoteKeys(wrote, error);
    Json body = Json::object();
    body.set("ok", Json::flag(ok));
    body.set("wrote", Json::flag(wrote));
    if (!ok) body.set("error", Json::text(error));
    (void)exchange.respond(200,
                           {{"Content-Type", "application/json; charset=utf-8"},
                            {"Cache-Control", "no-store"},
                            {"X-Content-Type-Options", "nosniff"}},
                           body.dump());
}

bool HttpFrontDoor::restoreNativeRemoteKeys(std::string &error) const {
    std::string root = dataRoot_;
    while (root.size() > 1 && root.back() == '/') root.pop_back();
    const std::string mapPath = root + "/remotekeys.json";
    const std::string lgPath = root + "/remotekeys-lg.json";
    const auto exists = [](const std::string &path) {
        return access(path.c_str(), F_OK) == 0;
    };
    if (!exists(mapPath) && !exists(lgPath))
        return true;

    JsonFile lgStore(lgPath, 256 * 1024);
    const JsonFileResult lgBefore = lgStore.load();
    if (!lgBefore.error.empty()) {
        error = lgBefore.error;
        return false;
    }

    JsonFile mapStore(mapPath, 64 * 1024);
    const JsonFileResult previous = mapStore.load();
    if (!previous.error.empty()) {
        error = previous.error;
        return false;
    }
    Json emptyMap = previous.value.isObject()
        ? Json::parse(previous.value.dump()) : Json::object();
    emptyMap.set("keys", Json::object());
    if (!mapStore.save(emptyMap, &error))
        return false;
    if (unlink((mapPath + ".last-good").c_str()) != 0 && errno != ENOENT) {
        error = "could not clear the previous native remote-key map";
        return false;
    }

    const auto preserveLgRecord = [&]() {
        if (!lgBefore.found)
            return true;
        std::string preserveError;
        if (lgStore.save(lgBefore.value, &preserveError))
            return true;
        if (error.empty()) error = preserveError;
        else error += "; could not preserve LG remote-key state: " + preserveError;
        return false;
    };
    bool wrote = false;
    bool reconciled = false;
    try {
        reconciled = reconcileNativeRemoteKeys(wrote, error);
    } catch (...) {
        (void)preserveLgRecord();
        throw;
    }
    if (!reconciled) {
        (void)preserveLgRecord();
        return false;
    }

    if (unlink(mapPath.c_str()) != 0 && errno != ENOENT) {
        error = "could not clear restored native remote-key state";
        return false;
    }
    return true;
}

void HttpFrontDoor::handleMqttForget(HttpExchange &exchange) const {
    const HttpRequest &request = exchange.request();
    if (!isLoopbackPeer(request.remoteAddress) || proxySecret_.empty() ||
        !tokenMatchesConstantTime(request.header("x-nativetv-proxy-secret"), proxySecret_) ||
        !request.header("x-nativetv-original-remote").empty()) {
        sendSetupError(exchange, 403, "native MQTT forget required", true);
        return;
    }
    if (request.method != "POST") {
        sendSetupError(exchange, 405, "method not allowed");
        return;
    }
    // Nothing to clear without a native client; Node then has nothing to wait for.
    const size_t cleared = mqttForget_ ? mqttForget_() : 0;
    Json body = Json::object();
    body.set("ok", Json::flag(true));
    body.set("cleared", Json::numeric(static_cast<double>(cleared)));
    (void)exchange.respond(200,
                           {{"Content-Type", "application/json; charset=utf-8"},
                            {"Cache-Control", "no-store"},
                            {"X-Content-Type-Options", "nosniff"}},
                           body.dump());
}

void HttpFrontDoor::handleLastApp(HttpExchange &exchange) const {
    const HttpRequest &request = exchange.request();
    if (!isLoopbackPeer(request.remoteAddress) || proxySecret_.empty() ||
        !tokenMatchesConstantTime(request.header("x-nativetv-proxy-secret"), proxySecret_) ||
        !request.header("x-nativetv-original-remote").empty()) {
        sendSetupError(exchange, 403, "native last app observation required", true);
        return;
    }
    if (request.method != "POST") {
        sendSetupError(exchange, 405, "method not allowed");
        return;
    }
    if (!setupJsonContentType(request)) {
        sendSetupError(exchange, 415, "Content-Type must be application/json");
        return;
    }
    Json payload;
    if (!readSetupPayload(exchange, payload))
        return;
    std::string error;
    if (!validateLastAppSnapshot(payload, &error)) {
        sendSetupError(exchange, 400, error.empty() ? "invalid last app snapshot" : error);
        return;
    }
    bool recorded = false;
    {
        std::lock_guard<std::mutex> lock(lastAppMutex_);
        recorded = recordLastApp(lastAppPath_, payload, &error);
    }
    if (!recorded) {
        if (logger_)
            logger_->warning("last_app: " + error);
        sendSetupError(exchange, 500, "could not persist last app snapshot");
        return;
    }
    (void)exchange.respond(204,
                           {{"Cache-Control", "no-store"},
                            {"X-Content-Type-Options", "nosniff"}},
                           "");
}

void HttpFrontDoor::handleAppsState(HttpExchange &exchange) const {
    const HttpRequest &request = exchange.request();
    if (!isLoopbackPeer(request.remoteAddress) || proxySecret_.empty() ||
        !tokenMatchesConstantTime(request.header("x-nativetv-proxy-secret"), proxySecret_) ||
        !request.header("x-nativetv-original-remote").empty()) {
        sendSetupError(exchange, 403, "native apps state update required", true);
        return;
    }
    if (request.method != "POST") {
        sendSetupError(exchange, 405, "method not allowed");
        return;
    }
    if (!setupJsonContentType(request)) {
        sendSetupError(exchange, 415, "Content-Type must be application/json");
        return;
    }
    Json payload;
    if (!readSetupPayload(exchange, payload)) return;
    std::string error;
    bool saved = false;
    {
        std::lock_guard<std::mutex> lock(appsStateMutex_);
        saved = persistAppTileState(hiddenAppsPath_, tileHidingFlagPath_, payload, &error);
    }
    if (!saved) {
        if (logger_) logger_->warning("apps state: " + error);
        sendSetupError(exchange, 400, error.empty() ? "invalid apps state" : error);
        return;
    }
    (void)exchange.respond(204,
                           {{"Cache-Control", "no-store"},
                            {"X-Content-Type-Options", "nosniff"}},
                           "");
}

void HttpFrontDoor::handleAppElevation(HttpExchange &exchange) const {
    const HttpRequest &request = exchange.request();
    if (!isLoopbackPeer(request.remoteAddress) || proxySecret_.empty() ||
        !tokenMatchesConstantTime(request.header("x-nativetv-proxy-secret"), proxySecret_) ||
        !request.header("x-nativetv-original-remote").empty()) {
        sendSetupError(exchange, 403, "native app elevation state access required", true);
        return;
    }
    if (request.method != "POST") {
        sendSetupError(exchange, 405, "method not allowed");
        return;
    }
    if (!setupJsonContentType(request)) {
        sendSetupError(exchange, 415, "Content-Type must be application/json");
        return;
    }
    Json payload;
    if (!readSetupPayload(exchange, payload)) return;
    const std::string action = payload["action"].string();
    const std::string appId = payload["package"].string();
    Json response = Json::object();
    std::string error;
    bool ok = false;
    {
        std::lock_guard<std::mutex> lock(elevatedAppsMutex_);
        if (action == "read") {
            Json services;
            ok = readElevatedServices(elevatedAppsPath_, appId, services, &error);
            if (ok) {
                if (!appInstallElevationAvailable(systemRoot_))
                    services = Json::array();
                response.set("services", services);
            }
        } else if (action == "write") {
            ok = recordElevatedServices(elevatedAppsPath_, appId,
                                        payload["services"], &error);
        } else {
            error = "unsupported app elevation state action";
        }
    }
    if (!ok) {
        if (logger_) logger_->warning("app elevation state: " + error);
        response.set("ok", Json::flag(false));
        response.set("error", Json::text(error.empty()
                                               ? "invalid app elevation state"
                                               : error));
        (void)exchange.respond(400,
                               {{"Content-Type", "application/json; charset=utf-8"},
                                {"Cache-Control", "no-store"}},
                               response.dump());
        return;
    }
    response.set("ok", Json::flag(true));
    (void)exchange.respond(200,
                           {{"Content-Type", "application/json; charset=utf-8"},
                            {"Cache-Control", "no-store"},
                            {"X-Content-Type-Options", "nosniff"}},
                           response.dump());
}

void HttpFrontDoor::handleInstallerJob(HttpExchange &exchange) const {
    const HttpRequest &request = exchange.request();
    if (!isLoopbackPeer(request.remoteAddress) || proxySecret_.empty() ||
        !tokenMatchesConstantTime(request.header("x-nativetv-proxy-secret"), proxySecret_) ||
        !request.header("x-nativetv-original-remote").empty()) {
        sendSetupError(exchange, 403, "native installer job state access required", true);
        return;
    }
    if (request.method != "POST") {
        sendSetupError(exchange, 405, "method not allowed");
        return;
    }
    if (!setupJsonContentType(request)) {
        sendSetupError(exchange, 415, "Content-Type must be application/json");
        return;
    }
    Json payload;
    if (!readSetupPayload(exchange, payload, 1024 * 1024)) return;
    const std::string action = payload["action"].string();
    const bool hasSnapshot = payload.has("snapshot");
    const Json snapshot = payload["snapshot"];
    Json response = Json::object();
    response.set("ok", Json::flag(true));
    std::string error;
    bool ok = false;
    if (hasSnapshot && !validateAppInstallSnapshot(snapshot, &error)) {
        sendSetupError(exchange, 400, error);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(installJobMutex_);
        if (action == "read") {
            Json job;
            bool found = false;
            ok = readInstallerJob(installJobPath_, job, found, &error);
            if (ok) {
                response.set("found", Json::flag(found));
                response.set("job", job);
            }
        } else if (action == "recover") {
            Json job;
            bool found = false;
            ok = recoverInstallerJob(installJobPath_, job, found, &error);
            if (ok) {
                response.set("found", Json::flag(found));
                response.set("job", job);
            }
        } else if (action == "write") {
            ok = transitionInstallerJob(installJobPath_, payload["job"], &error);
            if (ok && hasSnapshot) {
                appInstallSnapshot_ = snapshot;
                appInstallSnapshotReady_ = true;
            }
        } else if (action == "snapshot") {
            if (!hasSnapshot) {
                error = "installer snapshot is required";
            } else {
                appInstallSnapshot_ = snapshot;
                appInstallSnapshotReady_ = true;
                ok = true;
            }
        } else if (action == "clear") {
            ok = clearInstallerJob(installJobPath_, &error);
            if (ok) {
                appInstallSnapshot_ = idleAppInstallSnapshot();
                appInstallSnapshotReady_ = true;
            }
        } else {
            error = "unsupported installer job action";
        }
    }
    if (!ok) {
        if (logger_) logger_->warning("installer job state: " + error);
        response.set("ok", Json::flag(false));
        response.set("error", Json::text(error.empty()
                                               ? "invalid installer job state"
                                               : error));
        (void)exchange.respond(400,
                               {{"Content-Type", "application/json; charset=utf-8"},
                                {"Cache-Control", "no-store"}},
                               response.dump());
        return;
    }
    (void)exchange.respond(200,
                           {{"Content-Type", "application/json; charset=utf-8"},
                            {"Cache-Control", "no-store"},
                            {"X-Content-Type-Options", "nosniff"}},
                           response.dump());
}

void HttpFrontDoor::handleAppElevationRun(HttpExchange &exchange) const {
    const HttpRequest &request = exchange.request();
    if (!isLoopbackPeer(request.remoteAddress) || proxySecret_.empty() ||
        !tokenMatchesConstantTime(request.header("x-nativetv-proxy-secret"), proxySecret_) ||
        !request.header("x-nativetv-original-remote").empty()) {
        sendSetupError(exchange, 403, "native app elevation access required", true);
        return;
    }
    if (request.method != "POST") {
        sendSetupError(exchange, 405, "method not allowed");
        return;
    }
    if (!setupJsonContentType(request)) {
        sendSetupError(exchange, 415, "Content-Type must be application/json");
        return;
    }
    Json payload;
    if (!readSetupPayload(exchange, payload)) return;
    const Json requested = payload["services"];
    if (!requested.isArray()) {
        sendSetupError(exchange, 400, "elevated services must be an array");
        return;
    }
    std::vector<std::string> services;
    services.reserve(requested.size());
    for (const Json &service : requested.items()) {
        if (!service.isString()) {
            sendSetupError(exchange, 400, "elevated service name is invalid");
            return;
        }
        services.push_back(service.string());
    }

    AppElevationResult result;
    std::string error;
    bool ok = false;
    {
        std::lock_guard<std::mutex> lock(appElevationRunMutex_);
        AppElevationExecutor executor(children_, systemRoot_);
        ok = executor.run(payload["package"].string(), services, result, &error);
    }
    if (!ok) {
        if (logger_) logger_->warning("app elevation: " + error);
        sendSetupError(exchange, 400, error.empty() ? "invalid app elevation request" : error);
        return;
    }

    Json done = Json::array();
    for (const std::string &service : result.done) done.add(Json::text(service));
    Json failed = Json::array();
    for (const AppElevationFailure &failure : result.failed) {
        Json item = Json::object();
        item.set("service", Json::text(failure.service));
        item.set("error", Json::text(failure.error));
        failed.add(item);
        if (logger_) logger_->warning("app elevation: " + failure.service + ": " +
                                      failure.error);
    }
    Json response = Json::object();
    response.set("ok", Json::flag(true));
    response.set("done", done);
    response.set("failed", failed);
    (void)exchange.respond(200,
                           {{"Content-Type", "application/json; charset=utf-8"},
                            {"Cache-Control", "no-store"},
                            {"X-Content-Type-Options", "nosniff"}},
                           response.dump());
}

void HttpFrontDoor::handleAppUpload(HttpExchange &exchange) const {
    const HttpRequest &request = exchange.request();
    if (request.method != "POST") {
        (void)proxy_.forward(exchange);
        return;
    }
    std::string contentType = request.header("content-type");
    std::transform(contentType.begin(), contentType.end(), contentType.begin(),
                   [](unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });
    if (contentType != "application/octet-stream") {
        sendInstallError(exchange, 415,
                         "Content-Type must be application/octet-stream");
        return;
    }
    if (rejectCrossSitePost(exchange)) return;
    if (!config_.allowControl) {
        sendInstallError(exchange, 403,
                         say::msg("srv.controlsOff", "controls disabled in config"));
        return;
    }
    if (!appInstallHostAllowed(request, config_.document)) {
        Json failure = Json::object();
        failure.set("ok", Json::flag(false));
        failure.set("error", Json::text(
            "this address is not allowed to install apps; add the name to apps.hosts in config.json, or use the TV's IP address"));
        (void)exchange.respond(403,
                               {{"Content-Type", "application/json; charset=utf-8"},
                                {"Cache-Control", "no-store"},
                                {"X-Content-Type-Options", "nosniff"}},
                               failure.dump());
        return;
    }
    const bool sideloadEnabled = config_.document["apps"]["sideload"].isBoolean() &&
                                 config_.document["apps"]["sideload"].boolean();
    const bool sideloadAllowed = sideloadEnabled || !config_.token.empty() ||
                                 isLoopbackPeer(request.remoteAddress);
    if (!sideloadAllowed) {
        sendInstallError(exchange, 403,
                         say::msg("srv.install.sideloadOff",
                                  "installing from a URL or a file is off; set \"apps\": {\"sideload\": true} in config.json, set a token, or use the TV's web browser"));
        return;
    }
    const std::string transferEncoding = request.header("transfer-encoding");
    if (!transferEncoding.empty() || request.contentLength < 0) {
        sendInstallError(exchange, 411,
                         say::msg("srv.install.upload.length",
                                  "the upload must state its size in advance"), true);
        return;
    }
    const unsigned long long length = static_cast<unsigned long long>(request.contentLength);
    if (length == 0) {
        sendInstallError(exchange, 400,
                         say::msg("srv.install.upload.empty",
                                  "the upload is empty or its size is not valid"), true);
        return;
    }
    if (length > 536870912) {
        sendInstallError(exchange, 413, appUploadTooBig(), true);
        return;
    }

    const char *configuredStaging = std::getenv("TVWEB_INSTALL_STAGING");
    const std::string stagingDirectory = configuredStaging && configuredStaging[0]
        ? configuredStaging : "/media/developer/temp/nativetv-install";
    std::string file;
    std::string reservationError;
    uint64_t availableBytes = 0;
    AppInstallUploadReservation::ReserveResult reserveResult;
    {
        std::lock_guard<std::mutex> operationLock(appInstallActionMutex_);
        bool installBusy = true;
        {
            std::lock_guard<std::mutex> snapshotLock(installJobMutex_);
            installBusy = !appInstallSnapshotReady_ ||
                          appInstallSnapshotBusy(appInstallSnapshot_);
        }
        reserveResult = appInstallUploadReservation_.reserve(
            stagingDirectory, static_cast<uint64_t>(length), 536870912ULL,
            installBusy, file, availableBytes, &reservationError);
    }
    if (reserveResult == AppInstallUploadReservation::ReserveResult::tooLarge) {
        sendInstallError(exchange, 413, appUploadTooBig(), true);
        return;
    }
    if (reserveResult == AppInstallUploadReservation::ReserveResult::busy) {
        sendInstallError(exchange, 409,
                         say::msg("srv.install.busy",
                                  "an install is already in progress"));
        return;
    }
    if (reserveResult == AppInstallUploadReservation::ReserveResult::noSpace) {
        Json variables = Json::object();
        variables.set("need", Json::text(appInstallFormatMb(length * 3)));
        variables.set("free", Json::text(appInstallFormatMb(availableBytes)));
        sendInstallError(exchange, 507,
                         say::msg("srv.install.noSpace",
                                  "not enough free space: {need} needed, {free} free",
                                  variables));
        return;
    }
    if (reserveResult != AppInstallUploadReservation::ReserveResult::reserved) {
        if (logger_)
            logger_->warning("app upload reservation: " + reservationError);
        sendInstallError(exchange, 500,
                         say::msg("srv.install.upload.failed",
                                  "the upload could not be saved"));
        return;
    }
    auto releaseReservation = [&]() {
        std::string error;
        if (!appInstallUploadReservation_.release(stagingDirectory, file, true, &error) &&
            logger_)
            logger_->warning("app upload reservation cleanup: " + error);
    };
    if (!exchange.setBodyLimit(static_cast<size_t>(length))) {
        releaseReservation();
        sendInstallError(exchange, 413, appUploadTooBig(), true);
        return;
    }

    const int directory = open(stagingDirectory.c_str(), O_RDONLY | O_DIRECTORY |
                                O_CLOEXEC | O_NOFOLLOW);
    if (directory < 0) {
        releaseReservation();
        sendInstallError(exchange, 500,
                         say::msg("srv.install.upload.failed",
                                  "the upload could not be saved"));
        return;
    }
    const int output = openat(directory, file.c_str(), O_WRONLY | O_CREAT | O_EXCL |
                              O_CLOEXEC | O_NOFOLLOW, 0600);
    if (output < 0) {
        close(directory);
        releaseReservation();
        sendInstallError(exchange, 500,
                         say::msg("srv.install.upload.failed",
                                  "the upload could not be saved"));
        return;
    }

    bool saved = true;
    bool bodyMismatch = false;
    unsigned long long received = 0;
    auto lastTouch = std::chrono::steady_clock::now();
    char buffer[65536];
    while (received < length) {
        const size_t capacity = static_cast<size_t>(std::min<unsigned long long>(
            sizeof(buffer), length - received));
        const int count = exchange.readBody(buffer, capacity);
        if (count <= 0) {
            saved = false;
            bodyMismatch = count == 0;
            break;
        }
        size_t offset = 0;
        while (offset < static_cast<size_t>(count)) {
            const ssize_t written = write(output, buffer + offset,
                                          static_cast<size_t>(count) - offset);
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) {
                saved = false;
                break;
            }
            offset += static_cast<size_t>(written);
        }
        if (!saved) break;
        received += static_cast<unsigned long long>(count);
        const auto now = std::chrono::steady_clock::now();
        if (now - lastTouch >= std::chrono::seconds(15)) {
            if (!appInstallUploadReservation_.touch(file)) {
                saved = false;
                break;
            }
            lastTouch = now;
        }
    }
    if (saved && received != length) {
        saved = false;
        bodyMismatch = true;
    }
    if (saved && fsync(output) != 0)
        saved = false;
    close(output);
    close(directory);
    if (!saved) {
        releaseReservation();
        if (bodyMismatch) {
            sendInstallError(exchange, 400,
                             say::msg("srv.install.upload.size",
                                      "the upload does not match its stated size"), true);
        } else {
            sendInstallError(exchange, 500,
                             say::msg("srv.install.upload.failed",
                                      "the upload could not be saved"));
        }
        return;
    }

    std::unique_lock<std::mutex> operationLock(appInstallActionMutex_);
    if (!appInstallUploadReservation_.owns(file)) {
        operationLock.unlock();
        releaseReservation();
        sendInstallError(exchange, 409,
                         say::msg("srv.install.busy",
                                  "an install is already in progress"));
        return;
    }
    int nodeStatus = 0;
    Json nodeResponse;
    std::string bridgeError;
    if (!startAppUpload(file, nodeStatus, nodeResponse, bridgeError)) {
        // Node may have accepted the file before the response was lost. Keep
        // the staged bytes in that ambiguous case so its verifier cannot race
        // cleanup; normal boot recovery removes abandoned staging files.
        if (logger_) logger_->warning("app upload start: " + bridgeError);
        sendInstallError(exchange, 502, "native service unavailable");
        return;
    }
    if (nodeStatus < 200 || nodeStatus >= 300 ||
        !nodeResponse["ok"].isBoolean() || !nodeResponse["ok"].boolean()) {
        operationLock.unlock();
        releaseReservation();
        const int status = nodeStatus >= 400 && nodeStatus <= 599 ? nodeStatus : 502;
        (void)exchange.respond(status,
                               {{"Content-Type", "application/json; charset=utf-8"},
                                {"Cache-Control", "no-store"},
                                {"X-Content-Type-Options", "nosniff"}},
                               nodeResponse.dump());
        return;
    }
    const Json installSnapshot = nodeResponse["install"];
    std::string snapshotError;
    if (!installSnapshot.isObject() ||
        !validateAppInstallStartedSnapshot(installSnapshot, &snapshotError)) {
        if (logger_)
            logger_->warning("app upload start returned an invalid install snapshot: " +
                             snapshotError);
        sendInstallError(exchange, 502, "native service unavailable");
        return;
    }
    cacheAppInstallStartSnapshot(nodeResponse);
    (void)appInstallUploadReservation_.complete(file);
    (void)exchange.respond(200,
                           {{"Content-Type", "application/json; charset=utf-8"},
                            {"Cache-Control", "no-store"},
                            {"X-Content-Type-Options", "nosniff"}},
                           nodeResponse.dump());
}

void HttpFrontDoor::handleAppInstallFetch(HttpExchange &exchange) const {
    const HttpRequest &request = exchange.request();
    if (request.method != "POST") {
        (void)proxy_.forward(exchange);
        return;
    }
    if (!config_.allowControl) {
        sendSetupError(exchange, 403, say::msg("srv.controlsOff", "controls disabled in config"));
        return;
    }
    Json payload;
    if (!readAppInstallJson(exchange, payload)) return;
    if (!payload["id"].isString() || payload["id"].string().empty()) {
        sendInstallError(exchange, 400,
                         say::msg("srv.install.idRequired", "an app id is required"));
        return;
    }

    std::string repo;
    if (payload["repo"].isString() && !payload["repo"].string().empty())
        repo = payload["repo"].string();
    LunaReply appReply;
    if (!waitForLuna(luna_, "com.webos.applicationManager/listApps",
                     Json::object(), appReply))
        appReply.response = Json::object();
    Json configuredRepos;
    {
        std::lock_guard<std::mutex> lock(appCatalogReposMutex_);
        configuredRepos = appCatalogRepos_;
    }
    const int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::system_clock::now().time_since_epoch())
                              .count();
    const Json selection = appCatalog_.findPackage(configuredRepos, appReply.response,
                                                    payload["id"].string(), repo, nowMs);
    if (!selection["ok"].isBoolean() || !selection["ok"].boolean()) {
        sendInstallError(exchange, 400,
                         selection["error"].string("native app catalog lookup failed"));
        return;
    }

    int nodeStatus = 0;
    Json nodeResponse;
    std::string bridgeError;
    Json startPayload = Json::object();
    startPayload.set("source", Json::text("catalog"));
    startPayload.set("pkg", selection["pkg"]);
    startPayload.set("auto", Json::flag(payload["auto"].isBoolean() &&
                                         payload["auto"].boolean()));
    startPayload.set("nativeValidated", Json::flag(true));
    std::unique_lock<std::mutex> operationLock(appInstallActionMutex_);
    bool installBusy = appInstallUploadReservation_.held();
    {
        std::lock_guard<std::mutex> snapshotLock(installJobMutex_);
        installBusy = installBusy || (appInstallSnapshotReady_ &&
                                      appInstallSnapshotBusy(appInstallSnapshot_));
    }
    if (installBusy) {
        sendInstallError(exchange, 409,
                         say::msg("srv.install.busy",
                                  "an install is already in progress"));
        return;
    }
    if (!startAppInstall(startPayload,
                         nodeStatus, nodeResponse, bridgeError)) {
        if (logger_) logger_->warning("app install start: " + bridgeError);
        sendInstallError(exchange, 502, "native service unavailable");
        return;
    }
    if (nodeStatus >= 200 && nodeStatus < 300) {
        std::string snapshotError;
        if (!nodeResponse["ok"].isBoolean() || !nodeResponse["ok"].boolean() ||
            !nodeResponse["install"].isObject() ||
            !validateAppInstallStartedSnapshot(nodeResponse["install"], &snapshotError)) {
            if (logger_)
                logger_->warning("app install start returned an invalid snapshot: " +
                                 snapshotError);
            sendInstallError(exchange, 502, "native service unavailable");
            return;
        }
        cacheAppInstallStartSnapshot(nodeResponse);
    }
    (void)exchange.respond(nodeStatus,
                           {{"Content-Type", "application/json; charset=utf-8"},
                            {"Cache-Control", "no-store"},
                            {"X-Content-Type-Options", "nosniff"}},
                           nodeResponse.dump());
}

void HttpFrontDoor::handleAppInstallUrl(HttpExchange &exchange) const {
    const HttpRequest &request = exchange.request();
    if (request.method != "POST") {
        (void)proxy_.forward(exchange);
        return;
    }
    if (!config_.allowControl) {
        sendSetupError(exchange, 403, say::msg("srv.controlsOff", "controls disabled in config"));
        return;
    }
    const bool sideloadEnabled = config_.document["apps"]["sideload"].isBoolean() &&
                                 config_.document["apps"]["sideload"].boolean();
    if (!sideloadEnabled && config_.token.empty() &&
        !isLoopbackPeer(request.remoteAddress)) {
        sendInstallError(exchange, 403,
                         say::msg("srv.install.sideloadOff",
                                  "installing from a URL or a file is off; set \"apps\": {\"sideload\": true} in config.json, set a token, or use the TV's web browser"));
        return;
    }
    Json payload;
    if (!readAppInstallJson(exchange, payload)) return;

    const std::string url = payload["url"].string();
    std::string validationError;
    if (!service::AppPackageFetcher::validUrl(url, validationError)) {
        sendInstallError(exchange, 400, validationError);
        return;
    }

    Json sha256;
    const Json rawSha256 = payload["sha256"];
    if (rawSha256.valid() && !(rawSha256.isString() && rawSha256.string().empty())) {
        const std::string hash = quickAccessStringValue(rawSha256);
        const bool validHash = hash.size() == 64 &&
            std::all_of(hash.begin(), hash.end(), [](unsigned char character) {
                return std::isxdigit(character) != 0;
            });
        if (!validHash) {
            sendInstallError(exchange, 400,
                             say::msg("srv.install.badHash",
                                      "the sha256 hash must be 64 hexadecimal digits"));
            return;
        }
        sha256 = Json::text(hash);
    }

    Json startPayload = Json::object();
    startPayload.set("source", Json::text("url"));
    startPayload.set("url", Json::text(url));
    startPayload.set("sha256", sha256);
    startPayload.set("nativeValidated", Json::flag(true));
    int nodeStatus = 0;
    Json nodeResponse;
    std::string bridgeError;
    std::unique_lock<std::mutex> operationLock(appInstallActionMutex_);
    bool installBusy = appInstallUploadReservation_.held();
    {
        std::lock_guard<std::mutex> snapshotLock(installJobMutex_);
        installBusy = installBusy || (appInstallSnapshotReady_ &&
                                      appInstallSnapshotBusy(appInstallSnapshot_));
    }
    if (installBusy) {
        sendInstallError(exchange, 409,
                         say::msg("srv.install.busy",
                                  "an install is already in progress"));
        return;
    }
    if (!startAppInstall(startPayload, nodeStatus,
                         nodeResponse, bridgeError)) {
        if (logger_) logger_->warning("app URL install start: " + bridgeError);
        sendInstallError(exchange, 502, "native service unavailable");
        return;
    }
    if (nodeStatus >= 200 && nodeStatus < 300) {
        std::string snapshotError;
        if (!nodeResponse["ok"].isBoolean() || !nodeResponse["ok"].boolean() ||
            !nodeResponse["install"].isObject() ||
            !validateAppInstallStartedSnapshot(nodeResponse["install"], &snapshotError)) {
            if (logger_)
                logger_->warning("app URL install start returned an invalid snapshot: " +
                                 snapshotError);
            sendInstallError(exchange, 502, "native service unavailable");
            return;
        }
        cacheAppInstallStartSnapshot(nodeResponse);
    }
    (void)exchange.respond(nodeStatus,
                           {{"Content-Type", "application/json; charset=utf-8"},
                            {"Cache-Control", "no-store"},
                            {"X-Content-Type-Options", "nosniff"}},
                           nodeResponse.dump());
}

void HttpFrontDoor::handleAppInstallAction(HttpExchange &exchange,
                                           const std::string &action) const {
    const HttpRequest &request = exchange.request();
    if (!config_.allowControl) {
        sendSetupError(exchange, 403, say::msg("srv.controlsOff", "controls disabled in config"));
        return;
    }
    if (request.method != "POST") {
        sendSetupError(exchange, 404, "not found");
        return;
    }
    Json payload;
    if (!readAppInstallJson(exchange, payload)) return;
    Json actionPayload = payload;
    std::shared_ptr<std::atomic<bool>> nativeDownloadCancellation;
    std::string nativeDownloadJobId;

    Json snapshot;
    bool snapshotReady = false;
    {
        std::lock_guard<std::mutex> lock(installJobMutex_);
        snapshot = appInstallSnapshot_;
        snapshotReady = appInstallSnapshotReady_;
    }
    bool nativeConfirmationValidated = false;
    if (snapshotReady) {
        const Json currentJobId = snapshot["jobId"];
        const Json requestedJobId = payload["jobId"];
        const std::string state = snapshot["state"].string();
        const bool matchesCurrentJob = requestedJobId.isString() &&
            currentJobId.isString() &&
            requestedJobId.string() == currentJobId.string();
        if (!matchesCurrentJob ||
            (action == "confirm" && state != "awaiting-confirm")) {
            sendInstallError(exchange, 400,
                say::msg("srv.install.noJob",
                         "there is no install waiting for confirmation"));
            return;
        }
        if (action == "cancel" && state != "downloading" &&
            state != "verifying" && state != "awaiting-confirm") {
            sendInstallError(exchange, 400,
                say::msg("srv.install.cannotCancel",
                         "the install is already running and cannot be cancelled"));
            return;
        }
        if (action == "cancel" && state == "downloading" &&
            currentJobId.isString()) {
            std::lock_guard<std::mutex> downloadLock(appInstallDownloadsMutex_);
            const auto active = appInstallDownloads_.find(currentJobId.string());
            if (active != appInstallDownloads_.end()) {
                nativeDownloadCancellation = active->second;
                nativeDownloadJobId = currentJobId.string();
                actionPayload.set("nativeDownloadCancelled", Json::flag(true));
            }
        }
        const Json preview = snapshot["preview"];
        if (action == "confirm" && preview.isObject()) {
            if (appInstallJsTruthy(preview["storeInstalled"]) &&
                !(payload["replaceStore"].isBoolean() &&
                  payload["replaceStore"].boolean())) {
                sendInstallError(exchange, 400,
                    say::msg("srv.install.storeReplace",
                             "this app came from the LG store; replacing it stops store updates and needs separate confirmation"));
                return;
            }

            const Json elevate = payload["elevate"];
            const bool wantRoot = elevate.isBoolean() && elevate.boolean();
            if (wantRoot && !appInstallElevationAvailable(systemRoot_)) {
                sendInstallError(exchange, 400,
                    say::msg("srv.install.noElevate",
                             "root access needs the Homebrew Channel, which is not installed"));
                return;
            }
            if (wantRoot && !appInstallJsTruthy(preview["vetted"]) &&
                !appInstallSameStringNames(payload["confirmRoot"],
                                           preview["services"])) {
                sendInstallError(exchange, 400,
                    say::msg("srv.install.rootConfirm",
                             "root access for a package from outside the catalog must be confirmed for the services listed in its preview"));
                return;
            }
            const Json nativeServices = wantRoot ? preview["services"]
                                                 : preview["reelevate"];
            if (nativeServices.isArray() && nativeServices.size() > 256) {
                sendInstallError(exchange, 502, "native service unavailable");
                return;
            }
            if (nativeServices.isArray())
                actionPayload.set("nativeServices", nativeServices);
            nativeConfirmationValidated = true;
        }
    }

    int nodeStatus = 0;
    Json nodeResponse;
    std::string bridgeError;
    const bool nodeActionDispatched = runAppInstallAction(
        action, actionPayload, nativeConfirmationValidated, nodeStatus,
        nodeResponse, bridgeError);
    const bool nativeCancelAccepted = nativeDownloadCancellation &&
        (!nodeActionDispatched || (nodeStatus >= 200 && nodeStatus < 300));
    if (nativeCancelAccepted) {
        nativeDownloadCancellation->store(true);
        const char *configuredDirectory = std::getenv("TVWEB_INSTALL_STAGING");
        const std::string stagingDirectory = configuredDirectory && configuredDirectory[0]
            ? configuredDirectory : "/media/developer/temp/nativetv-install";
        const int directory = open(stagingDirectory.c_str(), O_RDONLY | O_DIRECTORY |
                                   O_CLOEXEC | O_NOFOLLOW);
        if (directory >= 0) {
            const std::string fileName = nativeDownloadJobId + ".ipk";
            (void)unlinkat(directory, fileName.c_str(), 0);
            (void)fsync(directory);
            close(directory);
        }
    }
    if (!nodeActionDispatched) {
        if (logger_) logger_->warning("app install " + action + ": " + bridgeError);
        sendInstallError(exchange, 502, "native service unavailable");
        return;
    }
    (void)exchange.respond(nodeStatus,
                           {{"Content-Type", "application/json; charset=utf-8"},
                            {"Cache-Control", "no-store"},
                            {"X-Content-Type-Options", "nosniff"}},
                           nodeResponse.dump());
}

void HttpFrontDoor::sendAppInstallStatus(HttpExchange &exchange) const {
    const HttpRequest &request = exchange.request();
    if (!appInstallHostAllowed(request, config_.document)) {
        sendSetupError(exchange, 403, say::msg("srv.install.hostRefused", "this address is not allowed to install apps; add the name to apps.hosts in config.json, or use the TV's IP address"));
        return;
    }
    if (!config_.allowControl) {
        sendSetupError(exchange, 403, say::msg("srv.controlsOff", "controls disabled in config"));
        return;
    }
    if (request.method != "GET") {
        sendSetupError(exchange, 404, "not found");
        return;
    }

    Json status;
    bool ready = false;
    {
        std::lock_guard<std::mutex> lock(installJobMutex_);
        status = appInstallSnapshot_;
        ready = appInstallSnapshotReady_;
    }
    if (!ready) {
        (void)proxy_.forward(exchange);
        return;
    }

    std::string sideloadVia;
    if (config_.document["apps"]["sideload"].isBoolean() &&
        config_.document["apps"]["sideload"].boolean()) {
        sideloadVia = "config";
    } else if (!config_.token.empty()) {
        sideloadVia = "token";
    } else if (isLoopbackPeer(request.remoteAddress)) {
        sideloadVia = "tv";
    }
    status.set("ok", Json::flag(true));
    status.set("writable", Json::flag(config_.allowControl));
    status.set("sideload", Json::flag(!sideloadVia.empty()));
    status.set("sideloadVia", sideloadVia.empty() ? Json() : Json::text(sideloadVia));
    (void)exchange.respond(200,
                           {{"Content-Type", "application/json; charset=utf-8"},
                            {"Cache-Control", "no-store"},
                            {"X-Content-Type-Options", "nosniff"}},
                           status.dump());
}

void HttpFrontDoor::handleAppInstallRun(HttpExchange &exchange) const {
    const HttpRequest &request = exchange.request();
    if (!isLoopbackPeer(request.remoteAddress) || proxySecret_.empty() ||
        !tokenMatchesConstantTime(request.header("x-nativetv-proxy-secret"), proxySecret_) ||
        !request.header("x-nativetv-original-remote").empty()) {
        sendSetupError(exchange, 403, "native app install access required", true);
        return;
    }
    if (request.method != "POST") {
        sendSetupError(exchange, 405, "method not allowed");
        return;
    }
    if (!setupJsonContentType(request)) {
        sendSetupError(exchange, 415, "Content-Type must be application/json");
        return;
    }
    if (!config_.allowControl) {
        sendSetupError(exchange, 403, "controls disabled in config");
        return;
    }
    if (!exchange.setBodyLimit(8192)) {
        sendSetupError(exchange, 413, "request body too large", true);
        return;
    }
    Json payload;
    if (!readSetupPayload(exchange, payload)) return;
    if (!payload.isObject() || payload.keys().size() != 4 ||
        !payload["package"].isString() || !payload["file"].isString() ||
        !payload["sha256"].isString() || payload["sha256"].string().size() != 64 ||
        !payload["timeoutMs"].isNumber() ||
        payload["timeoutMs"].number() < 1 ||
        payload["timeoutMs"].number() > 120000 ||
        std::floor(payload["timeoutMs"].number()) != payload["timeoutMs"].number()) {
        sendSetupError(exchange, 400, "app install parameters expected");
        return;
    }

    const char *configuredStaging = std::getenv("TVWEB_INSTALL_STAGING");
    const std::string stagingDirectory = configuredStaging && configuredStaging[0]
        ? configuredStaging : "/media/developer/temp/nativetv-install";
    const char *configuredLunaSend = std::getenv("TVWEB_LUNA_SEND");
    const std::string lunaSendPath = configuredLunaSend && configuredLunaSend[0]
        ? configuredLunaSend : "/usr/bin/luna-send";

    AppInstallResult result;
    std::string error;
    bool accepted = false;
    {
        std::lock_guard<std::mutex> lock(appInstallRunMutex_);
        AppInstallExecutor executor(children_, stagingDirectory, lunaSendPath);
        accepted = executor.run(payload["package"].string(), payload["file"].string(),
            payload["sha256"].string(),
            std::chrono::milliseconds(static_cast<long long>(payload["timeoutMs"].number())),
            result, &error);
    }
    if (!accepted) {
        if (logger_) logger_->warning("app install: " + error);
        Json failure = Json::object();
        failure.set("ok", Json::flag(false));
        failure.set("error", Json::text(error.empty()
            ? "invalid app install request" : error));
        (void)exchange.respond(400,
                               {{"Content-Type", "application/json; charset=utf-8"},
                                {"Cache-Control", "no-store"},
                                {"X-Content-Type-Options", "nosniff"}},
                               failure.dump());
        return;
    }

    if (result.outcome == AppInstallOutcome::Closed ||
        result.outcome == AppInstallOutcome::TimedOut) {
        LunaReply appList;
        if (waitForLuna(luna_, "com.webos.applicationManager/listApps",
                        Json::object(), appList)) {
            const Json apps = appList.response["apps"].isArray()
                ? appList.response["apps"] : appList.response["launchPoints"];
            if (apps.isArray()) {
                for (const Json &installed : apps.items()) {
                    if (!installed["version"].isString()) continue;
                    const std::string id = installed["id"].string();
                    const std::string version = installed["version"].string();
                    bool matches = id == payload["package"].string() &&
                                   version == result.packageVersion;
                    for (const AppInstallAppVersion &app : result.apps) {
                        if (id == app.id &&
                            (version == result.packageVersion ||
                             (!app.version.empty() && version == app.version))) {
                            matches = true;
                            break;
                        }
                    }
                    if (matches) {
                        result.outcome = AppInstallOutcome::Installed;
                        result.error.clear();
                        break;
                    }
                }
            }
        }
    }

    const char *outcome = "closed";
    switch (result.outcome) {
    case AppInstallOutcome::Installed: outcome = "installed"; break;
    case AppInstallOutcome::Failed: outcome = "failed"; break;
    case AppInstallOutcome::Closed: outcome = "closed"; break;
    case AppInstallOutcome::TimedOut: outcome = "timeout"; break;
    }
    Json response = Json::object();
    response.set("ok", Json::flag(true));
    response.set("outcome", Json::text(outcome));
    if (!result.error.empty()) response.set("error", Json::text(result.error));
    (void)exchange.respond(200,
                           {{"Content-Type", "application/json; charset=utf-8"},
                            {"Cache-Control", "no-store"},
                            {"X-Content-Type-Options", "nosniff"}},
                           response.dump());
}

void HttpFrontDoor::handleInstalledApps(HttpExchange &exchange) const {
    const HttpRequest &request = exchange.request();
    if (!isLoopbackPeer(request.remoteAddress) || proxySecret_.empty() ||
        !tokenMatchesConstantTime(request.header("x-nativetv-proxy-secret"), proxySecret_) ||
        !request.header("x-nativetv-original-remote").empty()) {
        sendSetupError(exchange, 403, "native installed-app access required", true);
        return;
    }
    if (request.method != "POST") {
        sendSetupError(exchange, 405, "method not allowed");
        return;
    }
    if (!setupJsonContentType(request)) {
        sendSetupError(exchange, 415, "Content-Type must be application/json");
        return;
    }
    if (!exchange.setBodyLimit(1024)) {
        sendSetupError(exchange, 413, "request body too large", true);
        return;
    }
    Json payload;
    if (!readSetupPayload(exchange, payload)) return;
    if (!payload.isObject() || !payload.keys().empty()) {
        sendSetupError(exchange, 400, "empty installed-app request expected");
        return;
    }

    LunaReply reply;
    if (!waitForLuna(luna_, "com.webos.applicationManager/listApps",
                     Json::object(), reply)) {
        sendSetupError(exchange, 502, "installed-app list is unavailable");
        return;
    }
    const Json rows = reply.response["apps"].isArray()
        ? reply.response["apps"] : reply.response["launchPoints"];
    if (!rows.isArray()) {
        sendSetupError(exchange, 502, "installed-app list is unavailable");
        return;
    }

    Json apps = Json::array();
    for (const Json &item : rows.items()) {
        if (!item["id"].isString() || item["id"].string().empty()) continue;
        Json app = Json::object();
        app.set("id", item["id"]);
        app.set("version", item["version"].isString()
            ? item["version"] : Json::text(""));
        apps.add(app);
    }
    Json response = Json::object();
    response.set("ok", Json::flag(true));
    response.set("apps", apps);
    (void)exchange.respond(200,
                           {{"Content-Type", "application/json; charset=utf-8"},
                            {"Cache-Control", "no-store"},
                            {"X-Content-Type-Options", "nosniff"}},
                           response.dump());
}

void HttpFrontDoor::setNativeBackupConfig(HttpExchange &exchange) const {
    const HttpRequest &request = exchange.request();
    if (!isLoopbackPeer(request.remoteAddress) || proxySecret_.empty() ||
        !tokenMatchesConstantTime(request.header("x-nativetv-proxy-secret"), proxySecret_) ||
        !request.header("x-nativetv-original-remote").empty()) {
        sendSetupError(exchange, 403, "native backup config update required", true);
        return;
    }
    if (request.method != "POST") {
        sendSetupError(exchange, 405, "method not allowed");
        return;
    }
    if (!setupJsonContentType(request)) {
        sendSetupError(exchange, 415, "Content-Type must be application/json");
        return;
    }
    if (!config_.allowControl) {
        sendSetupError(exchange, 403, "controls disabled in config");
        return;
    }
    if (!exchange.setBodyLimit(65536)) {
        sendSetupError(exchange, 413, "request body too large", true);
        return;
    }
    std::string body;
    char buffer[4096];
    for (;;) {
        const int count = exchange.readBody(buffer, sizeof(buffer));
        if (count == 0)
            break;
        if (count < 0) {
            sendSetupError(exchange, exchange.bodyLimitExceeded() ? 413 : 400,
                           exchange.bodyLimitExceeded() ? "request body too large"
                                                        : "invalid request body",
                           true);
            return;
        }
        body.append(buffer, static_cast<size_t>(count));
    }
    if (exceedsUtf16CodeUnitLimit(body, 65536)) {
        sendSetupError(exchange, 413, "request body too large", true);
        return;
    }
    const Json payload = Json::parse(body);
    if (!payload.isObject() || !payload["patch"].isObject()) {
        sendSetupError(exchange, 400, "backup config patch expected");
        return;
    }
    const Json patch = service::safeBackupConfig(payload["patch"]);
    std::string error;
    Json changed = Json::array();
    if (!ConfigStore(configPath_).patchBackupSettings(patch, &error, &changed)) {
        if (logger_)
            logger_->warning("backup config: " + error);
        sendSetupError(exchange, 500, "could not save nativeTv settings");
        return;
    }
    Json result = Json::object();
    result.set("ok", Json::flag(true));
    result.set("changed", changed);
    (void)exchange.respond(200,
                           {{"Content-Type", "application/json"},
                            {"Cache-Control", "no-store"},
                            {"X-Content-Type-Options", "nosniff"}},
                           result.dump());
}

void HttpFrontDoor::validateNativeBackupRestore(HttpExchange &exchange) const {
    const HttpRequest &request = exchange.request();
    if (!isLoopbackPeer(request.remoteAddress) || proxySecret_.empty() ||
        !tokenMatchesConstantTime(request.header("x-nativetv-proxy-secret"), proxySecret_) ||
        !request.header("x-nativetv-original-remote").empty()) {
        sendSetupError(exchange, 403, "native backup validator required", true);
        return;
    }
    if (request.method != "POST") {
        sendSetupError(exchange, 405, "method not allowed");
        return;
    }
    if (!setupJsonContentType(request)) {
        sendSetupError(exchange, 415, "Content-Type must be application/json");
        return;
    }
    if (!config_.allowControl) {
        sendSetupError(exchange, 403, "controls disabled in config");
        return;
    }
    if (!exchange.setBodyLimit(1024 * 1024)) {
        sendSetupError(exchange, 413, "request body too large", true);
        return;
    }
    std::string body;
    char buffer[4096];
    for (;;) {
        const int count = exchange.readBody(buffer, sizeof(buffer));
        if (count == 0)
            break;
        if (count < 0) {
            sendSetupError(exchange, exchange.bodyLimitExceeded() ? 413 : 400,
                           exchange.bodyLimitExceeded() ? "request body too large"
                                                        : "invalid request body",
                           true);
            return;
        }
        body.append(buffer, static_cast<size_t>(count));
    }
    const Json backup = Json::parse(body);
    const size_t nullStart = body.find_first_not_of(" \t\r\n");
    const size_t nullEnd = body.find_last_not_of(" \t\r\n");
    const bool isNull = nullStart != std::string::npos && nullEnd >= nullStart &&
                        body.compare(nullStart, nullEnd - nullStart + 1, "null") == 0;
    if (!backup.valid() && !isNull) {
        sendSetupError(exchange, 400, "malformed JSON");
        return;
    }
    Json result = Json::object();
    result.set("ok", Json::flag(true));
    result.set("result", Json::text(service::backupRestoreEnvelopeResult(backup)));
    (void)exchange.respond(200,
                           {{"Content-Type", "application/json; charset=utf-8"},
                            {"Cache-Control", "no-store"},
                            {"X-Content-Type-Options", "nosniff"}},
                           result.dump());
}

void HttpFrontDoor::setNativePendingBackupApps(HttpExchange &exchange) const {
    const HttpRequest &request = exchange.request();
    if (!isLoopbackPeer(request.remoteAddress) || proxySecret_.empty() ||
        !tokenMatchesConstantTime(request.header("x-nativetv-proxy-secret"), proxySecret_) ||
        !request.header("x-nativetv-original-remote").empty()) {
        sendSetupError(exchange, 403, "native pending backup app update required", true);
        return;
    }
    if (request.method != "POST") {
        sendSetupError(exchange, 405, "method not allowed");
        return;
    }
    if (!setupJsonContentType(request)) {
        sendSetupError(exchange, 415, "Content-Type must be application/json");
        return;
    }
    if (!config_.allowControl) {
        sendSetupError(exchange, 403, "controls disabled in config");
        return;
    }
    if (!exchange.setBodyLimit(1024 * 1024)) {
        sendSetupError(exchange, 413, "request body too large", true);
        return;
    }
    std::string body;
    char buffer[4096];
    for (;;) {
        const int count = exchange.readBody(buffer, sizeof(buffer));
        if (count == 0)
            break;
        if (count < 0) {
            sendSetupError(exchange, exchange.bodyLimitExceeded() ? 413 : 400,
                           exchange.bodyLimitExceeded() ? "request body too large"
                                                        : "invalid request body",
                           true);
            return;
        }
        body.append(buffer, static_cast<size_t>(count));
    }
    if (exceedsUtf16CodeUnitLimit(body, 1024 * 1024)) {
        sendSetupError(exchange, 413, "request body too large", true);
        return;
    }
    const Json payload = Json::parse(body);
    if (!payload.isObject() || !payload["action"].isString()) {
        sendSetupError(exchange, 400, "pending backup app action expected");
        return;
    }
    const std::string action = payload["action"].string();
    const std::string path = dataFilePath(dataRoot_, "restore-apps.json");
    std::string error;
    bool succeeded = false;
    {
        std::lock_guard<std::mutex> lock(pendingBackupAppsMutex_);
        if (action == "clear") {
            succeeded = service::clearPendingBackupApps(path, &error);
        } else if (action == "save") {
            succeeded = service::savePendingBackupApps(
                path, payload["saved"], payload["apps"], &error);
        } else {
            error = "pending backup app action is invalid";
        }
    }
    if (!succeeded) {
        if (logger_)
            logger_->warning("pending backup apps: " + error);
        sendSetupError(exchange, 400,
                       error.empty() ? "could not update pending backup apps" : error);
        return;
    }
    (void)exchange.respond(204,
                           {{"Cache-Control", "no-store"},
                            {"X-Content-Type-Options", "nosniff"}},
                           "");
}

void HttpFrontDoor::planNativeBackupApps(HttpExchange &exchange) const {
    const HttpRequest &request = exchange.request();
    if (!exchange.inProcess() &&
        (!isLoopbackPeer(request.remoteAddress) || proxySecret_.empty() ||
         !tokenMatchesConstantTime(request.header("x-nativetv-proxy-secret"), proxySecret_) ||
         !request.header("x-nativetv-original-remote").empty())) {
        sendSetupError(exchange, 403, "native backup app planner required", true);
        return;
    }
    if (request.method != "POST") {
        sendSetupError(exchange, 405, "method not allowed");
        return;
    }
    if (!setupJsonContentType(request)) {
        sendSetupError(exchange, 415, "Content-Type must be application/json");
        return;
    }
    if (!exchange.setBodyLimit(1024 * 1024)) {
        sendSetupError(exchange, 413, "request body too large", true);
        return;
    }
    std::string body;
    char buffer[4096];
    for (;;) {
        const int count = exchange.readBody(buffer, sizeof(buffer));
        if (count == 0)
            break;
        if (count < 0) {
            sendSetupError(exchange, exchange.bodyLimitExceeded() ? 413 : 400,
                           exchange.bodyLimitExceeded() ? "request body too large"
                                                        : "invalid request body",
                           true);
            return;
        }
        body.append(buffer, static_cast<size_t>(count));
    }
    if (exceedsUtf16CodeUnitLimit(body, 1024 * 1024)) {
        sendSetupError(exchange, 413, "request body too large", true);
        return;
    }
    const Json payload = Json::parse(body);
    if (!payload.isObject() || !payload["apps"].isArray() ||
        payload["apps"].size() > 4096) {
        sendSetupError(exchange, 400, "invalid backup restore app planning request");
        return;
    }

    // Older internal callers may still provide a complete planning context.
    // Managed Node now sends only the backup's app list so Luna and the
    // Homebrew catalog remain owned by the native daemon.
    const bool hasPlanningContext = payload.has("installedIds") ||
                                    payload.has("catalogIds") ||
                                    payload.has("catalogAvailable") ||
                                    payload.has("catalogError");
    Json plan;
    if (hasPlanningContext) {
        if (!payload["installedIds"].isArray() ||
            payload["installedIds"].size() > 8192 ||
            !payload["catalogIds"].isArray() || payload["catalogIds"].size() > 8192 ||
            !payload["catalogAvailable"].isBoolean() ||
            (payload.has("catalogError") && !payload["catalogError"].isString()) ||
            exceedsUtf16CodeUnitLimit(payload["catalogError"].string(), 512)) {
            sendSetupError(exchange, 400, "invalid backup restore app planning request");
            return;
        }
        plan = service::planBackupRestoreApps(
            payload["apps"], payload["installedIds"], payload["catalogIds"],
            payload["catalogAvailable"].boolean(), payload["catalogError"].string());
    } else {
        Json installedIds = Json::array();
        LunaReply appReply;
        if (!waitForLuna(luna_, "com.webos.applicationManager/listApps",
                         Json::object(), appReply) ||
            !appReply.response["apps"].isArray()) {
            appReply.response = Json::object();
        } else {
            for (const Json &app : appReply.response["apps"].items()) {
                const std::string id = app["id"].string();
                const std::string folder = app["folderPath"].string();
                if (id.empty() || folder.find("/media/developer/apps/") != 0 ||
                    !service::backupAppAllowed(id))
                    continue;
                installedIds.add(Json::text(id));
            }
        }

        Json noCatalogIds = Json::array();
        Json initialPlan = service::planBackupRestoreApps(
            payload["apps"], installedIds, noCatalogIds, false, "no catalog");
        if (!initialPlan["toInstall"].isArray() ||
            initialPlan["toInstall"].size() == 0) {
            plan = initialPlan;
        } else {
            Json configuredRepos;
            {
                std::lock_guard<std::mutex> lock(appCatalogReposMutex_);
                configuredRepos = appCatalogRepos_;
            }
            const int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                      std::chrono::system_clock::now().time_since_epoch())
                                      .count();
            const Json catalog = appCatalog_.get(
                configuredRepos, appReply.response, false, nowMs);
            Json catalogIds = Json::array();
            if (catalog["apps"].isArray()) {
                for (const Json &app : catalog["apps"].items()) {
                    const Json id = app["id"];
                    if (id.isString()) catalogIds.add(id);
                }
            }
            const bool catalogAvailable = catalog["ok"].isBoolean() &&
                                          catalog["ok"].boolean();
            const std::string catalogError = catalog["error"].string().empty()
                ? "catalog unavailable" : catalog["error"].string();
            plan = service::planBackupRestoreApps(
                payload["apps"], installedIds, catalogIds,
                catalogAvailable, catalogError);
        }
    }
    (void)exchange.respond(200,
                           {{"Content-Type", "application/json; charset=utf-8"},
                            {"Cache-Control", "no-store"},
                            {"X-Content-Type-Options", "nosniff"}},
                           plan.dump());
}

void HttpFrontDoor::restoreNativeBackupLg(HttpExchange &exchange) const {
    const HttpRequest &request = exchange.request();
    if (!exchange.inProcess() &&
        (!isLoopbackPeer(request.remoteAddress) || proxySecret_.empty() ||
         !tokenMatchesConstantTime(request.header("x-nativetv-proxy-secret"), proxySecret_) ||
         !request.header("x-nativetv-original-remote").empty())) {
        sendSetupError(exchange, 403, "native backup LG restore required", true);
        return;
    }
    if (request.method != "POST") {
        sendSetupError(exchange, 405, "method not allowed");
        return;
    }
    if (!setupJsonContentType(request)) {
        sendSetupError(exchange, 415, "Content-Type must be application/json");
        return;
    }
    if (!config_.allowControl) {
        sendSetupError(exchange, 403, "controls disabled in config");
        return;
    }
    if (!exchange.setBodyLimit(1024 * 1024)) {
        sendSetupError(exchange, 413, "request body too large", true);
        return;
    }
    std::string body;
    char buffer[4096];
    for (;;) {
        const int count = exchange.readBody(buffer, sizeof(buffer));
        if (count == 0)
            break;
        if (count < 0) {
            sendSetupError(exchange, exchange.bodyLimitExceeded() ? 413 : 400,
                           exchange.bodyLimitExceeded() ? "request body too large"
                                                        : "invalid request body",
                           true);
            return;
        }
        body.append(buffer, static_cast<size_t>(count));
    }
    if (exceedsUtf16CodeUnitLimit(body, 1024 * 1024)) {
        sendSetupError(exchange, 413, "request body too large", true);
        return;
    }
    const Json payload = Json::parse(body);
    const Json wanted = payload["lg"];
    if (!payload.isObject() || !wanted.isObject() || wanted.keys().size() > 512) {
        sendSetupError(exchange, 400, "backup LG settings object expected");
        return;
    }

    const std::vector<std::string> sections = {
        "promotions", "sound", "devices", "hdmi"
    };
    const Json data = collectLgSettingsData(sections);
    const Json current = service::lgSettingsResponse(
        sections, data["settings"], data["descriptions"], data["dimensions"],
        Json::object());
    std::map<std::string, Json> rows;
    if (current["rows"].isArray()) {
        for (const Json &row : current["rows"].items())
            if (row["id"].isString())
                rows[row["id"].string()] = row;
    }

    Json applied = Json::array();
    Json skipped = Json::array();
    Json failed = Json::array();
    size_t unchanged = 0;
    bool wrote = false;
    const CommandPolicyContext context = controlPolicyContext(config_);
    const auto appendSkipped = [&skipped](const std::string &id,
                                          const std::string &reason) {
        Json issue = Json::object();
        issue.set("id", Json::text(id));
        issue.set("reason", Json::text(reason));
        skipped.add(issue);
    };
    for (const std::string &id : wanted.keys()) {
        const auto found = rows.find(id);
        if (found == rows.end()) {
            appendSkipped(id, "absent");
            continue;
        }
        const Json &row = found->second;
        const Json value = wanted[id];
        const Json currentValue = row["type"].string() == "switch"
            ? row["on"] : row["value"];
        const bool same = (currentValue.isBoolean() && value.isBoolean() &&
                           currentValue.boolean() == value.boolean()) ||
                          (currentValue.isNumber() && value.isNumber() &&
                           currentValue.number() == value.number()) ||
                          (currentValue.isString() && value.isString() &&
                           currentValue.string() == value.string());
        if (same) {
            ++unchanged;
            continue;
        }

        bool offered = false;
        const std::string type = row["type"].string();
        if (type == "switch") {
            offered = value.isBoolean();
        } else if (type == "choice" && value.isString() &&
                   row["choices"].isArray()) {
            for (const Json &choice : row["choices"].items())
                if (choice["value"].isString() &&
                    choice["value"].string() == value.string()) {
                    offered = true;
                    break;
                }
        } else if (type == "number" && value.isNumber()) {
            const double minimum = row["min"].isNumber()
                ? row["min"].number() : -std::numeric_limits<double>::infinity();
            const double maximum = row["max"].isNumber()
                ? row["max"].number() : std::numeric_limits<double>::infinity();
            offered = std::isfinite(value.number()) && value.number() >= minimum &&
                      value.number() <= maximum;
        }
        if (!offered) {
            appendSkipped(id, "value");
            continue;
        }
        if (row["active"].isBoolean() && !row["active"].boolean()) {
            appendSkipped(id, "inactive");
            continue;
        }

        const service::LgSettingWrite setting =
            service::lgSettingsRestoreWrite(id, value, row);
        if (!setting.handled || !setting.valid) {
            Json issue = Json::object();
            issue.set("id", Json::text(id));
            issue.set("error", Json::text(setting.error.empty()
                ? "That value is not one this setting takes" : setting.error));
            failed.add(issue);
            continue;
        }
        Json settings = Json::object();
        settings.set(setting.key, setting.storedValue);
        Json command = Json::object();
        command.set("category", Json::text(setting.category));
        command.set("settings", settings);
        LunaReply reply;
        const bool answered = waitForCommandPolicyLuna(
            luna_, commandPolicy_, context, "backup.restore-lg." + id,
            "com.webos.service.settings/setSystemSettings", command,
            "system-settings", false, reply);
        if (answered && service::javascriptTruthy(reply.response["returnValue"])) {
            applied.add(Json::text(id));
            wrote = true;
        } else {
            Json issue = Json::object();
            issue.set("id", Json::text(id));
            issue.set("error", Json::text(reply.error.empty() ? "refused" : reply.error));
            failed.add(issue);
        }
    }
    if (wrote)
        invalidateLgSettingsCache();

    Json response = Json::object();
    response.set("ok", Json::flag(true));
    response.set("applied", applied);
    response.set("unchanged", Json::numeric(static_cast<double>(unchanged)));
    response.set("skipped", skipped);
    response.set("failed", failed);
    (void)exchange.respond(200,
                           {{"Content-Type", "application/json; charset=utf-8"},
                            {"Cache-Control", "no-store"},
                            {"X-Content-Type-Options", "nosniff"}},
                           response.dump());
}

void HttpFrontDoor::handleBackupRestore(HttpExchange &exchange, const std::string &body) const {
    // The routes the restore reuses answer a request this process makes for itself.
    const auto call = [this](void (HttpFrontDoor::*route)(HttpExchange &) const,
                             const char *uri, const std::string &requestBody, int &status) {
        HttpRequest request;
        request.method = "POST";
        request.uri = uri;
        request.version = "1.1";
        request.remoteAddress = "127.0.0.1";
        request.contentLength = static_cast<long long>(requestBody.size());
        request.headers.push_back({"Content-Type", "application/json"});
        HttpExchange inner(std::move(request), requestBody);
        (this->*route)(inner);
        status = inner.responseStatus();
        return Json::parse(inner.responseBody());
    };
    BackupRestoreDeps deps;
    deps.restoreLg = [&call](const Json &wanted, Json &result, std::string &error) {
        Json request = Json::object();
        request.set("lg", wanted);
        int status = 0;
        result = call(&HttpFrontDoor::restoreNativeBackupLg, "/_internal/backup/restore-lg",
                      request.dump(), status);
        if (status != 200 || !result["ok"].boolean() || !result["applied"].isArray() ||
            !result["unchanged"].isNumber() || !result["skipped"].isArray() ||
            !result["failed"].isArray()) {
            error = status != 200 ? "native backup LG restore returned HTTP " + std::to_string(status)
                                  : "native backup LG restore returned an invalid result";
            return false;
        }
        return true;
    };
    deps.planApps = [&call](const Json &apps, Json &plan, std::string &error) {
        Json request = Json::object();
        request.set("apps", apps);
        int status = 0;
        plan = call(&HttpFrontDoor::planNativeBackupApps, "/_internal/backup/restore-plan",
                    request.dump(), status);
        if (status != 200) {
            error = "native backup app planner returned HTTP " + std::to_string(status);
            return false;
        }
        return true;
    };
    deps.savePendingApps = [this](const Json &toInstall, long long savedMs, std::string &error) {
        std::lock_guard<std::mutex> lock(pendingBackupAppsMutex_);
        return service::savePendingBackupApps(
            dataFilePath(dataRoot_, "restore-apps.json"),
            Json::numeric(static_cast<double>(savedMs)), toInstall, &error);
    };
    deps.clearPendingApps = [this](std::string &error) {
        std::lock_guard<std::mutex> lock(pendingBackupAppsMutex_);
        return service::clearPendingBackupApps(dataFilePath(dataRoot_, "restore-apps.json"), &error);
    };
    deps.restoreHome = [&call](const Json &layout) {
        int status = 0;
        const Json answer = call(&HttpFrontDoor::handleHomeLayoutPost, "/api/home/layout",
                                 layout.dump(), status);
        if (answer.isObject())
            return answer;
        Json failed = Json::object();
        failed.set("ok", Json::flag(false));
        failed.set("error", Json::text("layout update failed"));
        return failed;
    };
    deps.writeConfig = [this](const Json &section, Json &changed, std::string &error) {
        return ConfigStore(configPath_).patchBackupSettings(
            service::safeBackupConfig(section), &error, &changed);
    };
    deps.warn = [this](const std::string &warning) {
        if (logger_) logger_->warning(warning);
    };
    deps.nowMs = []() {
        return static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    };

    const BackupRestoreOutcome outcome = restoreBackup(deps, Json::parse(body));
    (void)exchange.respond(outcome.status,
                           {{"Content-Type", "application/json; charset=utf-8"},
                            {"Cache-Control", "no-store"},
                            {"X-Content-Type-Options", "nosniff"}},
                           outcome.body.dump());
    if (!outcome.restart)
        return;
    std::string error;
    if (!restartServer(250, error) && logger_)
        logger_->warning("backup: could not restart - restart manually to apply: " + error);
}

void HttpFrontDoor::setNativeDailyBackup(HttpExchange &exchange) const {
    const HttpRequest &request = exchange.request();
    if (!isLoopbackPeer(request.remoteAddress) || proxySecret_.empty() ||
        !tokenMatchesConstantTime(request.header("x-nativetv-proxy-secret"), proxySecret_) ||
        !request.header("x-nativetv-original-remote").empty()) {
        sendSetupError(exchange, 403, "native daily backup creator required", true);
        return;
    }
    if (request.method != "POST") {
        sendSetupError(exchange, 405, "method not allowed");
        return;
    }
    if (!setupJsonContentType(request)) {
        sendSetupError(exchange, 415, "Content-Type must be application/json");
        return;
    }
    if (!exchange.setBodyLimit(4096)) {
        sendSetupError(exchange, 413, "request body too large", true);
        return;
    }
    std::string body;
    char buffer[1024];
    for (;;) {
        const int count = exchange.readBody(buffer, sizeof(buffer));
        if (count == 0)
            break;
        if (count < 0) {
            sendSetupError(exchange, exchange.bodyLimitExceeded() ? 413 : 400,
                           exchange.bodyLimitExceeded() ? "request body too large"
                                                        : "invalid request body",
                           true);
            return;
        }
        body.append(buffer, static_cast<size_t>(count));
    }
    if (exceedsUtf16CodeUnitLimit(body, 4096)) {
        sendSetupError(exchange, 413, "request body too large", true);
        return;
    }
    const Json payload = Json::parse(body);
    const std::string name = payload["name"].string();
    if (!payload.isObject() || !service::validSavedBackupName(name)) {
        sendSetupError(exchange, 400, "daily backup name is invalid");
        return;
    }
    std::string error;
    if (!createDailyBackup(name, error)) {
        if (logger_) logger_->warning("daily backup: " + error);
        sendSetupError(exchange,
                       error == "backup settings context is unavailable" ? 503 : 400,
                       error.empty() ? "could not save daily backup" : error);
        return;
    }
    (void)exchange.respond(204,
                           {{"Cache-Control", "no-store"},
                            {"X-Content-Type-Options", "nosniff"}},
                           "");
}

void HttpFrontDoor::sendAppIcon(HttpExchange &exchange) const {
    const HttpRequest &request = exchange.request();
    std::string appId;
    if (!uniqueQueryParameter(request.query, "id", appId)) {
        (void)exchange.respond(404, {{"Content-Type", "application/json"}},
                              "{\"ok\":false,\"error\":\"icon not found\"}");
        return;
    }
    const service::AppIconFile icon = service::readAppIcon(appId, systemRoot_);
    if (!icon.found) {
        (void)exchange.respond(404, {{"Content-Type", "application/json"}},
                              "{\"ok\":false,\"error\":\"icon not found\"}");
        return;
    }

    const std::string cacheControl = "public, max-age=86400";
    if (request.header("if-none-match") == icon.etag) {
        (void)exchange.respond(304, {{"ETag", icon.etag},
                                     {"Cache-Control", cacheControl}}, "");
        return;
    }
    const std::vector<HttpHeader> headers = {
        {"Content-Type", "image/png"},
        {"Cache-Control", cacheControl},
        {"ETag", icon.etag}
    };
    if (!exchange.beginResponse(200, headers, static_cast<long long>(icon.bytes.size())))
        return;
    if (request.method == "GET" && !icon.bytes.empty())
        (void)exchange.writeBody(icon.bytes.data(), icon.bytes.size());
    (void)exchange.finishResponse();
}

void HttpFrontDoor::sendHomeChannels(HttpExchange &exchange) const {
    bool tunerAvailable = false;
    const Json channels = homeChannelSnapshot(tunerAvailable);
    Json response = Json::object();
    response.set("ok", Json::flag(true));
    response.set("channels", channels);
    response.set("tuner", Json::flag(tunerAvailable));
    (void)exchange.respond(200,
                           {{"Content-Type", "application/json; charset=utf-8"},
                            {"Cache-Control", "no-store"},
                            {"X-Content-Type-Options", "nosniff"}},
                           response.dump());
}

Json HttpFrontDoor::homeChannelSnapshot(bool &tunerAvailable) const {
    Json channels = Json::parse("null");
    tunerAvailable = false;
    bool haveCachedChannels = false;
    const std::chrono::steady_clock::time_point now =
        std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(homeChannelsMutex_);
        if (homeChannelsCacheValid_ &&
            now - homeChannelsCachedAt_ < std::chrono::seconds(30)) {
            channels = homeChannelsCache_;
            tunerAvailable = true;
            haveCachedChannels = true;
        }
    }
    if (!haveCachedChannels) {
        LunaReply reply;
        const bool answered = waitForLuna(
            luna_, "com.webos.service.apiadapter/tv/getChannelList",
            Json::object(), reply);
        const service::HomeChannels result = service::homeChannels(
            answered ? reply.response : Json());
        channels = result.channels;
        tunerAvailable = result.tunerAvailable;
        if (tunerAvailable) {
            std::lock_guard<std::mutex> lock(homeChannelsMutex_);
            homeChannelsCache_ = channels;
            homeChannelsCacheValid_ = true;
            homeChannelsCachedAt_ = std::chrono::steady_clock::now();
        }
    }

    return channels;
}

void HttpFrontDoor::sendHomeStatus(HttpExchange &exchange) const {
    const HttpRequest &request = exchange.request();
    std::string with;
    const bool hasWith = queryParameter(request.query, "with", with);
    bool includeCatalogue = false;
    bool includeChannels = false;
    if (hasWith) {
        size_t start = 0;
        while (start <= with.size()) {
            const size_t end = with.find(',', start);
            const std::string item = with.substr(
                start, end == std::string::npos ? std::string::npos : end - start);
            if (item == "catalogue") includeCatalogue = true;
            if (item == "channels") includeChannels = true;
            if (end == std::string::npos) break;
            start = end + 1;
        }
    }

    Json rawHome;
    std::ifstream homeFile(dataFilePath(dataRoot_, "home.json").c_str(),
                           std::ios::in | std::ios::binary);
    if (homeFile) {
        const size_t maximumBytes = 8 * 1024 * 1024;
        std::string contents;
        char buffer[4096];
        while (homeFile) {
            homeFile.read(buffer, sizeof(buffer));
            const std::streamsize count = homeFile.gcount();
            if (count <= 0) break;
            if (contents.size() + static_cast<size_t>(count) > maximumBytes) {
                contents.clear();
                break;
            }
            contents.append(buffer, static_cast<size_t>(count));
        }
        if (homeFile.eof() && !contents.empty())
            rawHome = Json::parse(contents);
    }
    const Json document = service::normalizeHomeDocument(rawHome);

    const bool mounted = homeTakeoverMounted(systemRoot_, dataRoot_);
    bool active = false;
    if (mounted) {
        Json payload = Json::object();
        payload.set("id", Json::text("com.webos.app.home"));
        LunaReply app;
        if (waitForLuna(luna_, "com.webos.applicationManager/getAppInfo",
                        payload, app))
            active = app.response["appInfo"]["type"].string() == "web";
    }

    std::string foreground = stateSubscriptions_
        ? stateSubscriptions_->snapshot()["application"]["app_id"].string()
        : std::string();
    if (foreground.empty()) {
        LunaReply current;
        if (waitForLuna(luna_, "com.webos.applicationManager/getForegroundAppInfo",
                        Json::object(), current))
            foreground = current.response["appId"].string();
    }
    const bool lgHomeOpen = document["enabled"].boolean() &&
                            foreground == "com.webos.app.home" && !active;

    Json wallpapers = service::homeWallpaperStatus(dataRoot_);
    if (homeWallpaperUploadActive_.load())
        wallpapers.set("uploading", Json::flag(true));
    bool historyEnabled = config_.document["history"]["enabled"].boolean();
    if (historyRecorder_)
        historyEnabled = historyRecorder_->status()["enabled"].boolean();
    Json response = service::homeStatusProjection(
        rawHome, homeStatusWebosVersion(systemRoot_),
        config_.document["allowExperimentalHome"].boolean(), active,
        lgHomeOpen, historyEnabled, wallpapers);
    response.set("writable", Json::flag(config_.allowControl ||
                                         isLoopbackPeer(request.remoteAddress)));
    // The native client's probe as it last ran (never run from here), and the
    // watch that gives LG's Home back when the client keeps failing.
    const Json probe = homeProbeGate()->known();
    const std::string probeClass = probe["class"].string();
    // Only a refusal a rerun would not change hides the switch: after one that
    // may be timing (no display yet), switching on probes again and says why.
    if (probe.isObject() && !probe["ok"].boolean() && response["supported"]["ok"].boolean() &&
        (probeClass == "missing" || probeClass == "gl" || probeClass == "readback")) {
        response.set("supported", homeSupportedWithProbe(response["supported"], probe));
        response.set("available", Json::flag(response["enabled"].boolean() ||
                                             response["restorePending"].boolean()));
    }
    if (probe.isObject()) response.set("nativeClient", probe);
    if (const HomeOrchestrator *orchestrator = homeOrchestrator())
        response.set("nativeHome", orchestrator->nativeStatus());
    if (includeCatalogue) {
        response.set("catalogue", homeCatalogueResponse(false));
        if (includeChannels) {
            bool tunerAvailable = false;
            response.set("channels", homeChannelSnapshot(tunerAvailable));
        }
    }
    (void)exchange.respond(200,
                           {{"Content-Type", "application/json; charset=utf-8"},
                            {"Cache-Control", "no-store"},
                            {"X-Content-Type-Options", "nosniff"}},
                           response.dump());
}

void HttpFrontDoor::sendHomeCatalogue(HttpExchange &exchange, bool view) const {
    const Json response = homeCatalogueResponse(view);
    (void)exchange.respond(200,
                           {{"Content-Type", "application/json; charset=utf-8"},
                            {"Cache-Control", "no-store"},
                            {"X-Content-Type-Options", "nosniff"}},
                           response.dump());
}

Json HttpFrontDoor::homeCatalogueAppsSnapshot() const {
    Json apps;
    bool refresh = false;
    const std::chrono::steady_clock::time_point now =
        std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(homeCatalogueMutex_);
        if (homeCatalogueAppsCacheValid_ &&
            now - homeCatalogueAppsCachedAt_ < std::chrono::seconds(30))
            apps = homeCatalogueAppsCache_;
        else
            refresh = true;
    }
    if (!refresh) return apps;

    std::vector<std::pair<std::string, Json>> launchCalls;
    launchCalls.emplace_back("com.webos.applicationManager/listLaunchPoints",
                             Json::object());
    std::vector<LunaReply> launchReplies;
    (void)waitForLunaBatch(luna_, launchCalls, launchReplies);
    if (launchReplies.empty() ||
        !launchReplies[0].response["launchPoints"].isArray())
        return apps;

    std::vector<std::pair<std::string, Json>> sidebarCalls;
    static const char *sidebarIds[] = {
        "com.webos.app.notificationcenter", "com.webos.app.voice"};
    for (const char *id : sidebarIds) {
        Json payload = Json::object();
        payload.set("id", Json::text(id));
        sidebarCalls.emplace_back("com.webos.applicationManager/getAppInfo", payload);
    }
    std::vector<LunaReply> sidebarReplies;
    (void)waitForLunaBatch(luna_, sidebarCalls, sidebarReplies);
    Json sidebarInfo = Json::array();
    for (const LunaReply &reply : sidebarReplies)
        sidebarInfo.add(reply.response);
    apps = service::homeCatalogueApps(launchReplies[0].response, sidebarInfo);
    if (apps.isArray()) {
        std::lock_guard<std::mutex> lock(homeCatalogueMutex_);
        homeCatalogueAppsCache_ = apps;
        homeCatalogueAppsCacheValid_ = true;
        homeCatalogueAppsCachedAt_ = std::chrono::steady_clock::now();
    }
    return apps;
}

Json HttpFrontDoor::homeCatalogueDevicesSnapshot() const {
    Json devices;
    bool refresh = false;
    const std::chrono::steady_clock::time_point now =
        std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(homeCatalogueMutex_);
        if (homeCatalogueDevicesCacheValid_ &&
            now - homeCatalogueDevicesCachedAt_ < std::chrono::seconds(30))
            devices = homeCatalogueDevicesCache_;
        else
            refresh = true;
    }
    if (!refresh) return devices;

    std::vector<std::pair<std::string, Json>> sourceCalls;
    sourceCalls.emplace_back("com.webos.service.eim/getAllInputStatus",
                             Json::object());
    std::vector<LunaReply> sourceReplies;
    (void)waitForLunaBatch(luna_, sourceCalls, sourceReplies);
    if (sourceReplies.empty() ||
        !sourceReplies[0].response["devices"].isArray())
        return devices;
    devices = service::homeCatalogueDevices(sourceReplies[0].response);
    if (devices.isArray()) {
        std::lock_guard<std::mutex> lock(homeCatalogueMutex_);
        homeCatalogueDevicesCache_ = devices;
        homeCatalogueDevicesCacheValid_ = true;
        homeCatalogueDevicesCachedAt_ = std::chrono::steady_clock::now();
    }
    return devices;
}

Json HttpFrontDoor::homeCatalogueResponse(bool view) const {
    std::string liveChannelToRecord;
    Json rawHome;
    std::ifstream homeFile(dataFilePath(dataRoot_, "home.json").c_str(),
                           std::ios::in | std::ios::binary);
    if (homeFile) {
        const size_t maximumBytes = 8 * 1024 * 1024;
        std::string contents;
        char buffer[4096];
        while (homeFile) {
            homeFile.read(buffer, sizeof(buffer));
            const std::streamsize count = homeFile.gcount();
            if (count <= 0) break;
            if (contents.size() + static_cast<size_t>(count) > maximumBytes) {
                contents.clear();
                break;
            }
            contents.append(buffer, static_cast<size_t>(count));
        }
        if (homeFile.eof() && !contents.empty())
            rawHome = Json::parse(contents);
    }
    const Json document = service::normalizeHomeDocument(rawHome);
    const bool channelsRequested = !view || document["showLive"].boolean();
    if (channelsRequested && document["enabled"].boolean() &&
        document["sort"]["ch"].string() == "recent") {
        std::string foreground = stateSubscriptions_
            ? stateSubscriptions_->snapshot()["application"]["app_id"].string()
            : std::string();
        if (foreground.empty()) {
            LunaReply current;
            if (waitForLuna(luna_, "com.webos.applicationManager/getForegroundAppInfo",
                            Json::object(), current))
                foreground = current.response["appId"].string();
        }
        if (foreground == "com.webos.app.livetv") {
            LunaReply lastChannel;
            if (waitForLuna(luna_, "com.webos.service.utp.boot/getLastChannelId",
                            Json::object(), lastChannel))
                liveChannelToRecord = lastChannel.response["channelId"].string();
        }
    }

    const Json apps = homeCatalogueAppsSnapshot();
    const Json devices = homeCatalogueDevicesSnapshot();

    Json historyTotals = Json::object();
    bool historyEnabled = false;
    if (historyRecorder_) {
        historyEnabled = historyRecorder_->status()["enabled"].boolean();
        if (historyEnabled) {
            const int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                      std::chrono::system_clock::now().time_since_epoch())
                                      .count();
            historyTotals = historyRecorder_->totals(0, nowMs);
        }
    }
    bool tunerAvailable = false;
    const Json channels = channelsRequested
        ? homeChannelSnapshot(tunerAvailable) : Json::parse("null");
    const Json recent = liveChannelToRecord.empty()
        ? service::readHomeRecent(dataRoot_)
        : recordHomeRecent("ch", liveChannelToRecord);
    const Json catalogue = service::homeCatalogue(
        document, apps, devices, channels, channelsRequested, recent,
        historyTotals, historyEnabled);
    Json response = catalogue;
    response.set("ok", Json::flag(true));
    response.set("layout", service::homeLayout(document));
    response.set("profiles", service::homeProfileList(document));
    response.set("profile", document["profile"]);
    response.set("enabled", document["enabled"]);
    Json wallpaperStatus = service::homeWallpaperStatus(dataRoot_);
    if (homeWallpaperUploadActive_.load())
        wallpaperStatus.set("uploading", Json::flag(true));
    response.set("wallpapers", wallpaperStatus);

    struct stat homeInfo = {};
    int64_t layoutVersion = 0;
    if (stat(dataFilePath(dataRoot_, "home.json").c_str(), &homeInfo) == 0) {
#if defined(__APPLE__)
        layoutVersion = static_cast<int64_t>(homeInfo.st_mtimespec.tv_sec) * 1000 +
                        homeInfo.st_mtimespec.tv_nsec / 1000000;
#else
        layoutVersion = static_cast<int64_t>(homeInfo.st_mtim.tv_sec) * 1000 +
                        homeInfo.st_mtim.tv_nsec / 1000000;
#endif
    }
    response.set("layoutVersion", Json::text(std::to_string(layoutVersion)));
    return response;
}

void HttpFrontDoor::sendHomeGuide(HttpExchange &exchange) const {
    Json guide;
    const std::chrono::steady_clock::time_point now =
        std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(homeGuideMutex_);
        if (homeGuideCacheValid_ &&
            now - homeGuideCachedAt_ < std::chrono::seconds(60))
            guide = homeGuideCache_;
    }
    if (!guide.valid()) {
        Json rawHome;
        std::ifstream homeFile(dataFilePath(dataRoot_, "home.json").c_str(),
                               std::ios::in | std::ios::binary);
        if (homeFile) {
            const size_t maximumBytes = 8 * 1024 * 1024;
            std::string contents;
            char buffer[4096];
            while (homeFile) {
                homeFile.read(buffer, sizeof(buffer));
                const std::streamsize count = homeFile.gcount();
                if (count <= 0) break;
                if (contents.size() + static_cast<size_t>(count) > maximumBytes) {
                    contents.clear();
                    break;
                }
                contents.append(buffer, static_cast<size_t>(count));
            }
            if (homeFile.eof() && !contents.empty())
                rawHome = Json::parse(contents);
        }
        const Json document = service::normalizeHomeDocument(rawHome);
        guide = Json::object();
        const Json selected = document["channels"];
        if (selected.isArray()) {
            for (const Json &id : selected.items()) {
                Json request = Json::object();
                request.set("channelId", id);
                LunaReply reply;
                if (!waitForLuna(luna_,
                                 "com.webos.service.apiadapter/tv/getChannelCurrentProgramInfo",
                                 request, reply))
                    continue;
                const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                const Json programme = service::homeCurrentProgramme(reply.response, nowMs);
                if (!programme.valid()) continue;
                Json programmes = Json::array();
                programmes.add(programme);
                guide.set(id.string(), programmes);
            }
        }
        {
            std::lock_guard<std::mutex> lock(homeGuideMutex_);
            homeGuideCache_ = guide;
            homeGuideCacheValid_ = true;
            homeGuideCachedAt_ = std::chrono::steady_clock::now();
        }
    }

    Json response = Json::object();
    response.set("ok", Json::flag(true));
    response.set("guide", guide);
    (void)exchange.respond(200,
                           {{"Content-Type", "application/json; charset=utf-8"},
                            {"Cache-Control", "no-store"},
                            {"X-Content-Type-Options", "nosniff"}},
                           response.dump());
}

void HttpFrontDoor::sendHomeNow(HttpExchange &exchange) const {
    std::string appId;
    int64_t since = 0;
    {
        std::lock_guard<std::mutex> lock(homeNowState_->mutex);
        appId = homeNowState_->appId;
        since = homeNowState_->since;
    }
    if (appId.empty()) {
        // Nothing has been in front since the daemon started.
        (void)exchange.respond(200,
                               {{"Content-Type", "application/json; charset=utf-8"},
                                {"Cache-Control", "no-store"},
                                {"X-Content-Type-Options", "nosniff"}},
                               "{\"kind\":null}");
        return;
    }
    Json response;
    unsigned port = 0;
    if (service::homeNowInputPort(appId, port)) {
        LunaReply deviceReply;
        (void)waitForLuna(luna_, "com.webos.service.eim/getAllInputStatus",
                          Json::object(), deviceReply);
        response = service::homeNowInput(
            appId, since, service::homeCatalogueDevices(deviceReply.response));
    } else if (service::homeNowIsLiveTv(appId)) {
        LunaReply lastChannel;
        (void)waitForLuna(luna_, "com.webos.service.utp.boot/getLastChannelId",
                          Json::object(), lastChannel);
        bool tunerAvailable = false;
        const Json channels = homeChannelSnapshot(tunerAvailable);
        Json programme;
        const std::string channelId = lastChannel.response["channelId"].string();
        if (!channelId.empty() && channels.isArray()) {
            for (const Json &channel : channels.items()) {
                if (channel["id"].string() != channelId) continue;
                Json payload = Json::object();
                payload.set("channelId", Json::text(channelId));
                LunaReply current;
                (void)waitForLuna(
                    luna_, "com.webos.service.apiadapter/tv/getChannelCurrentProgramInfo",
                    payload, current);
                const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                                        std::chrono::system_clock::now().time_since_epoch())
                                        .count();
                programme = service::homeCurrentProgramme(current.response, now);
                break;
            }
        }
        response = service::homeNowChannel(appId, since, channels,
                                            lastChannel.response, programme);
    } else {
        Json apps;
        bool haveApps = false;
        const auto appCacheNow = std::chrono::steady_clock::now();
        {
            std::lock_guard<std::mutex> lock(homeCatalogueMutex_);
            if (homeCatalogueAppsCacheValid_ &&
                appCacheNow - homeCatalogueAppsCachedAt_ < std::chrono::seconds(30)) {
                apps = homeCatalogueAppsCache_;
                haveApps = true;
            }
        }
        if (!haveApps) {
            LunaReply launchPoints;
            (void)waitForLuna(luna_, "com.webos.applicationManager/listLaunchPoints",
                              Json::object(), launchPoints);
            apps = service::homeCatalogueApps(launchPoints.response, Json::array());
            if (apps.isArray()) {
                std::lock_guard<std::mutex> lock(homeCatalogueMutex_);
                homeCatalogueAppsCache_ = apps;
                homeCatalogueAppsCacheValid_ = true;
                homeCatalogueAppsCachedAt_ = std::chrono::steady_clock::now();
            }
        }
        response = service::homeNowApp(appId, since, apps);
        if (!response["title"].valid() &&
            (appId == "com.webos.app.notificationcenter" ||
             appId == "com.webos.app.voice")) {
            Json payload = Json::object();
            payload.set("id", Json::text(appId));
            LunaReply appInfo;
            if (waitForLuna(luna_, "com.webos.applicationManager/getAppInfo",
                            payload, appInfo) &&
                appInfo.response["returnValue"].boolean() &&
                appInfo.response["appInfo"]["id"].string() == appId) {
                const Json title = appInfo.response["appInfo"]["title"];
                response.set("title", title.isString() && !title.string().empty()
                                           ? title : Json::text(appId));
            }
        }
    }
    (void)exchange.respond(200,
                           {{"Content-Type", "application/json; charset=utf-8"},
                            {"Cache-Control", "no-store"},
                            {"X-Content-Type-Options", "nosniff"}},
                           response.dump());
}

void HttpFrontDoor::sendHomePicture(HttpExchange &exchange) const {
    ensureHomePictureTracking();
    const auto refuse = [&exchange](const std::string &message) {
        Json error = Json::object();
        error.set("ok", Json::flag(false));
        error.set("error", Json::text(message));
        (void)exchange.respond(400,
                               {{"Content-Type", "application/json; charset=utf-8"},
                                {"Cache-Control", "no-store"},
                                {"X-Content-Type-Options", "nosniff"}},
                               error.dump());
    };
    std::string kind;
    if (!uniqueQueryParameter(exchange.request().query, "kind", kind) ||
        (kind != "channel" && kind != "input" && kind != "app")) {
        refuse("kind input, channel or app expected");
        return;
    }

    std::string input;
    std::string cacheKey;
    std::string trackedAppId;
    uint64_t appGeneration = 0;
    std::string range;
    Json signal;
    if (kind == "channel") {
        input = "dtv";
        cacheKey = "channel";
        range = "sdr";
    } else if (kind == "app") {
        std::string appId;
        (void)uniqueQueryParameter(exchange.request().query, "app", appId);
        trackedAppId = appId;
        input = "default";
        cacheKey = "app:" + appId;
        if (!appId.empty()) {
            std::lock_guard<std::mutex> lock(homePictureState_->mutex);
            appGeneration = homePictureState_->generations[appId];
            const auto played = homePictureState_->played.find(appId);
            if (played != homePictureState_->played.end())
                signal = played->second;
        }
        if (signal.valid())
            range = service::homePictureRange(signal["hdr"].string());
    } else {
        std::string rawPort;
        (void)uniqueQueryParameter(exchange.request().query, "port", rawPort);
        char *end = nullptr;
        const double portNumber = std::strtod(rawPort.c_str(), &end);
        while (end && *end && std::isspace(static_cast<unsigned char>(*end))) ++end;
        // A missing, non-numeric or fractional port is outside 1 to 4 as well.
        if (!end || end == rawPort.c_str() || *end || !std::isfinite(portNumber) ||
            std::floor(portNumber) != portNumber || portNumber < 1 || portNumber > 4) {
            refuse("port 1 to 4 expected");
            return;
        }
        const unsigned port = static_cast<unsigned>(portNumber);
        input = "hdmi" + std::to_string(port);
        cacheKey = "input:" + std::to_string(port);
        const Json receiver = hdmiReceiverMap()[std::to_string(port)];
        if (receiver.isNumber() && receiver.number() >= 0 && receiver.number() < 4) {
            const std::string prefix = systemRoot_ == "/" ? "" : systemRoot_;
            const std::string receiverPath = prefix + "/proc/lg/hdmi20/port" +
                                             std::to_string(static_cast<unsigned>(receiver.number())) + "/";
            std::string status, packet;
            if (service::readKernelFile(receiverPath + "status", status)) {
                const Json facts = service::hdmiPortFactFromStatus(status);
                const bool packetAvailable = service::readKernelFile(receiverPath + "pkt", packet);
                signal = service::homePictureHdmiSignal(status, packet,
                                                         packetAvailable, facts);
                if (signal.valid())
                    range = service::homePictureRange(signal["hdr"].string());
            }
        }
    }

    Json response;
    const auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(homePictureState_->mutex);
        const auto cached = homePictureState_->cache.find(cacheKey);
        if (cached != homePictureState_->cache.end() &&
            now - cached->second.first < std::chrono::seconds(10))
            response = cached->second.second;
    }
    if (response.valid()) {
        (void)exchange.respond(200,
                               {{"Content-Type", "application/json; charset=utf-8"},
                                {"Cache-Control", "no-store"},
                                {"X-Content-Type-Options", "nosniff"}},
                               response.dump());
        return;
    }

    bool oled = false;
    const Json configuredPanel = service::javascriptTruthy(config_.document["panel"])
        ? config_.document["panel"] : config_.document["device"]["panel"];
    if (service::javascriptTruthy(configuredPanel)) {
        oled = service::oledModel(service::javascriptString(configuredPanel));
    } else if (service::hasOledPanelTypeMarker(systemRoot_)) {
        oled = true;
    } else {
        Json modelRequest = Json::object();
        Json modelKeys = Json::array();
        modelKeys.add(Json::text("modelName"));
        modelRequest.set("keys", modelKeys);
        LunaReply modelReply;
        const bool modelAnswered = waitForLuna(
            luna_, "com.webos.service.tv.systemproperty/getSystemProperties",
            modelRequest, modelReply);
        std::string model = service::javascriptString(modelReply.response["modelName"]);
        if (!service::javascriptTruthy(modelReply.response["modelName"]))
            model.clear();
        if (model == "webOS TV")
            model.clear();
        const Json configuredModel = config_.document["device"]["model"];
        if (model.empty() && service::javascriptTruthy(configuredModel))
            model = service::javascriptString(configuredModel);
        if (model.empty() && (!modelAnswered ||
                              !modelReply.response["returnValue"].boolean())) {
            // Let the legacy detector retry an unavailable model response; with
            // no Node the records alone decide.
            if (!nodeFreeStart()) {
                (void)proxy_.forward(exchange);
                return;
            }
        }
        oled = service::oledModel(model) || service::hasOledPanelRecords(systemRoot_);
    }

    std::string mode;
    if (!range.empty()) {
        Json modeRequest = Json::object();
        modeRequest.set("category", Json::text("picture"));
        Json modeKeys = Json::array();
        modeKeys.add(Json::text("pictureMode"));
        modeRequest.set("keys", modeKeys);
        Json modeDimension = Json::object();
        modeDimension.set("input", Json::text(input));
        modeDimension.set("dynamicRange", Json::text(range));
        modeRequest.set("dimension", modeDimension);
        LunaReply modeReply;
        (void)waitForLuna(luna_, "com.webos.service.settings/getSystemSettings",
                          modeRequest, modeReply);
        mode = modeReply.response["settings"]["pictureMode"].string();
    }

    Json light;
    if (!mode.empty()) {
        Json lightRequest = Json::object();
        lightRequest.set("category", Json::text("picture"));
        Json lightKeys = Json::array();
        lightKeys.add(Json::text("backlight"));
        lightRequest.set("keys", lightKeys);
        Json lightDimension = Json::object();
        lightDimension.set("input", Json::text(input));
        lightDimension.set("pictureMode", Json::text(mode));
        lightRequest.set("dimension", lightDimension);
        LunaReply lightReply;
        (void)waitForLuna(luna_, "com.webos.service.settings/getSystemSettings",
                          lightRequest, lightReply);
        const Json rawLight = lightReply.response["settings"]["backlight"];
        if (rawLight.isNumber()) {
            light = rawLight;
        } else if (rawLight.isString()) {
            const std::string text = rawLight.string();
            char *end = nullptr;
            const double value = std::strtod(text.c_str(), &end);
            while (end && *end && std::isspace(static_cast<unsigned char>(*end)))
                ++end;
            if (end && end != text.c_str() && !*end && std::isfinite(value))
                light = Json::numeric(value);
        }
    }

    response = service::homePictureResponse(input, range, signal, mode, light, oled);
    {
        std::lock_guard<std::mutex> lock(homePictureState_->mutex);
        if (trackedAppId.empty() ||
            homePictureState_->generations[trackedAppId] == appGeneration)
            homePictureState_->cache[cacheKey] = {
                std::chrono::steady_clock::now(), response};
    }
    (void)exchange.respond(200,
                           {{"Content-Type", "application/json; charset=utf-8"},
                            {"Cache-Control", "no-store"},
                            {"X-Content-Type-Options", "nosniff"}},
                           response.dump());
}

void HttpFrontDoor::sendHomeContent(HttpExchange &exchange,
                                    const std::string &kind) const {
    const HttpRequest &request = exchange.request();
    std::string tmdbType, id, query;
    const bool hasType = uniqueQueryParameter(request.query, "type", tmdbType);
    const bool hasId = uniqueQueryParameter(request.query, "id", id);
    Json result;
    const std::string key = config_.document["tmdbKey"].string();
    const bool netflixEnabled =
        config_.document["netflixDeepLinks"].boolean();
    if (kind == "search") {
        const bool hasQuery = uniqueQueryParameter(request.query, "q", query);
        result = homeContent_.search(key, query, hasQuery);
    } else if (kind == "netflix") {
        result = homeContent_.netflix(key, netflixEnabled,
                                      hasType ? tmdbType : "",
                                      hasId ? id : "");
    } else {
        result = homeContent_.details(key, netflixEnabled,
                                      hasType ? tmdbType : "",
                                      hasId ? id : "");
    }
    (void)exchange.respond(200,
                           {{"Content-Type", "application/json; charset=utf-8"},
                            {"Cache-Control", "no-store"},
                            {"X-Content-Type-Options", "nosniff"}},
                           result.dump());
}

void HttpFrontDoor::sendHomeDiscover(HttpExchange &exchange) const {
    const Json result = homeDiscover_.feed(
        config_.document["tmdbKey"].string(),
        config_.document["youtubeKey"].string());
    if (result["enabled"].boolean() && result["rows"].isArray() &&
        result["rows"].size())
        homeImageProxy_.retainAndWarm(result["rows"]);
    (void)exchange.respond(200,
                           {{"Content-Type", "application/json; charset=utf-8"},
                            {"Cache-Control", "no-store"},
                            {"X-Content-Type-Options", "nosniff"}},
                           result.dump());
}

void HttpFrontDoor::sendHomeImage(HttpExchange &exchange) const {
    std::string url, priority;
    if (!uniqueQueryParameter(exchange.request().query, "u", url)) {
        (void)exchange.respond(404,
                               {{"Content-Type", "application/json; charset=utf-8"}},
                               "{\"ok\":false,\"error\":\"no picture\"}");
        return;
    }
    (void)uniqueQueryParameter(exchange.request().query, "priority", priority);
    const service::HomeImage image = homeImageProxy_.image(url, priority);
    if (!image.found) {
        (void)exchange.respond(404,
                               {{"Content-Type", "application/json; charset=utf-8"}},
                               "{\"ok\":false,\"error\":\"no picture\"}");
        return;
    }
    const std::string cacheControl = "public, max-age=604800, immutable";
    if (exchange.request().header("if-none-match") == image.etag) {
        (void)exchange.respond(304,
                               {{"ETag", image.etag}, {"Cache-Control", cacheControl}},
                               "");
        return;
    }
    const std::vector<HttpHeader> headers = {
        {"Content-Type", "image/jpeg"}, {"Cache-Control", cacheControl},
        {"ETag", image.etag}
    };
    if (!exchange.beginResponse(200, headers,
                                static_cast<long long>(image.bytes.size())))
        return;
    if (!image.bytes.empty())
        (void)exchange.writeBody(image.bytes.data(), image.bytes.size());
    (void)exchange.finishResponse();
}

void HttpFrontDoor::sendHomeWallpaper(HttpExchange &exchange) const {
    const HttpRequest &request = exchange.request();
    std::string slot;
    if (!uniqueQueryParameter(request.query, "slot", slot)) {
        (void)exchange.respond(404, {{"Content-Type", "application/json"}},
                              "{\"ok\":false,\"error\":\"no wallpaper uploaded\"}");
        return;
    }
    const service::HomeWallpaperFile wallpaper =
        service::readHomeWallpaper(slot, dataRoot_);
    if (!wallpaper.found) {
        if (!wallpaper.validSlot) {
            (void)exchange.respond(404, {{"Content-Type", "application/json"}},
                                  "{\"ok\":false,\"error\":\"no wallpaper uploaded\"}");
            return;
        }
        const int status = wallpaper.readError ? 500 : 404;
        Json failure = Json::object();
        failure.set("ok", Json::flag(false));
        failure.set("error", Json::text("icon read failed"));
        (void)exchange.respond(status,
                               {{"Content-Type", "application/json; charset=utf-8"}},
                               failure.dump());
        return;
    }
    if (request.header("if-none-match") == wallpaper.etag) {
        (void)exchange.respond(304, {{"ETag", wallpaper.etag},
                                     {"Cache-Control", "public, max-age=86400"}}, "");
        return;
    }
    const std::vector<HttpHeader> headers = {
        {"Content-Type", "image/jpeg"},
        {"Cache-Control", "public, max-age=86400"},
        {"ETag", wallpaper.etag}
    };
    if (!exchange.beginResponse(200, headers,
                               static_cast<long long>(wallpaper.bytes.size())))
        return;
    if (request.method == "GET" && !wallpaper.bytes.empty())
        (void)exchange.writeBody(wallpaper.bytes.data(), wallpaper.bytes.size());
    (void)exchange.finishResponse();
}

void HttpFrontDoor::handleHomeWallpaperUpload(HttpExchange &exchange) const {
    const HttpRequest &request = exchange.request();
    auto respond = [&exchange](int status, const std::string &error,
                               bool closeConnection) {
        Json failure = Json::object();
        failure.set("ok", Json::flag(false));
        failure.set("error", Json::text(error));
        std::vector<HttpHeader> headers = {
            {"Content-Type", "application/json; charset=utf-8"},
            {"Cache-Control", "no-store"},
            {"X-Content-Type-Options", "nosniff"}
        };
        if (closeConnection)
            headers.push_back({"Connection", "close"});
        (void)exchange.respond(status, headers, failure.dump());
    };
    if (!config_.allowControl && !isLoopbackPeer(request.remoteAddress)) {
        respond(403, say::msg("srv.controlsOff", "controls disabled in config"), false);
        return;
    }

    std::string contentType = request.header("content-type");
    std::transform(contentType.begin(), contentType.end(), contentType.begin(),
                   [](unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });
    if (contentType != "image/jpeg") {
        respond(415, "Content-Type must be image/jpeg", false);
        return;
    }
    if (rejectCrossSitePost(exchange))
        return;

    const std::string slot = [&request]() {
        std::string value;
        if (!uniqueQueryParameter(request.query, "slot", value))
            return std::string();
        return value;
    }();
    if (slot != "morning" && slot != "afternoon" && slot != "evening" &&
        slot != "night") {
        respond(400, "unknown wallpaper slot", false);
        return;
    }

    const std::string transferEncoding = request.header("transfer-encoding");
    if (!transferEncoding.empty() || request.contentLength < 1 ||
        request.contentLength > 5 * 1024 * 1024) {
        respond(413, say::msg("srv.home.wallUploadSize",
                              "The uploaded JPEG must be at most 5 MB and state its size."),
                true);
        return;
    }

    bool expected = false;
    if (!homeWallpaperUploadActive_.compare_exchange_strong(expected, true)) {
        respond(409, say::msg("srv.home.wallUploading",
                              "Wait for the wallpaper upload to finish."), false);
        return;
    }
    struct UploadFlagReset {
        std::atomic<bool> &active;
        ~UploadFlagReset() { active.store(false); }
    } uploadFlagReset{homeWallpaperUploadActive_};

    const size_t maximumBytes = 5 * 1024 * 1024;
    if (!exchange.setBodyLimit(maximumBytes)) {
        respond(413, say::msg("srv.home.wallUploadSize",
                              "The uploaded JPEG must be at most 5 MB and state its size."),
                true);
        return;
    }
    std::string bytes;
    bytes.reserve(static_cast<size_t>(request.contentLength));
    char buffer[65536];
    while (bytes.size() < static_cast<size_t>(request.contentLength)) {
        const size_t capacity = std::min(
            sizeof(buffer), static_cast<size_t>(request.contentLength) - bytes.size());
        const int count = exchange.readBody(buffer, capacity);
        if (count <= 0) {
            const bool tooLarge = exchange.bodyLimitExceeded();
            homeWallpaperUploadActive_.store(false);
            respond(tooLarge ? 413 : 400,
                    tooLarge
                        ? say::msg("srv.home.wallUploadSize",
                                   "The uploaded JPEG must be at most 5 MB and state its size.")
                        : say::msg("srv.home.wallUploadImage",
                                   "Choose a valid JPEG up to 1920 × 1080 pixels."),
                    true);
            return;
        }
        bytes.append(buffer, static_cast<size_t>(count));
    }

    std::string error;
    const service::HomeWallpaperSaveResult result = service::saveHomeWallpaper(
        slot, dataRoot_, bytes, &error);
    homeWallpaperUploadActive_.store(false);
    if (result != service::HomeWallpaperSaveResult::saved) {
        int status = 500;
        switch (result) {
        case service::HomeWallpaperSaveResult::invalidSlot:
            status = 400;
            break;
        case service::HomeWallpaperSaveResult::invalidImage:
            status = 400;
            error = say::msg("srv.home.wallUploadImage",
                             "Choose a valid JPEG up to 1920 × 1080 pixels.");
            break;
        case service::HomeWallpaperSaveResult::insufficientSpace:
            status = 507;
            error = say::msg("srv.home.wallUploadSpace",
                             "Not enough confirmed free space for the wallpaper.");
            break;
        case service::HomeWallpaperSaveResult::ioError:
            status = 500;
            break;
        case service::HomeWallpaperSaveResult::saved:
            break;
        }
        respond(status, error, false);
        return;
    }

    Json response = Json::object();
    response.set("ok", Json::flag(true));
    Json wallpaperStatus = service::homeWallpaperStatus(dataRoot_);
    if (homeWallpaperUploadActive_.load())
        wallpaperStatus.set("uploading", Json::flag(true));
    response.set("wallpapers", wallpaperStatus);
    (void)exchange.respond(200,
                           {{"Content-Type", "application/json; charset=utf-8"},
                            {"Cache-Control", "no-store"},
                            {"X-Content-Type-Options", "nosniff"}},
                           response.dump());
}

void HttpFrontDoor::handleHomeWallpapersRemove(HttpExchange &exchange) const {
    Json payload;
    if (!readAppInstallJson(exchange, payload, true))
        return;
    if (!payload["removeUploads"].isBoolean() ||
        !payload["removeUploads"].boolean()) {
        (void)exchange.respond(400,
                              {{"Content-Type", "application/json; charset=utf-8"},
                               {"Cache-Control", "no-store"},
                               {"X-Content-Type-Options", "nosniff"}},
                              "{\"ok\":false,\"error\":\"removeUploads expected\"}");
        return;
    }

    const Json supported = service::homeStatusSupported(
        homeStatusWebosVersion(systemRoot_),
        config_.document["allowExperimentalHome"].boolean());
    if (!supported["ok"].boolean()) {
        Json failure = Json::object();
        failure.set("ok", Json::flag(false));
        failure.set("error", supported["reason"]);
        (void)exchange.respond(400,
                              {{"Content-Type", "application/json; charset=utf-8"},
                               {"Cache-Control", "no-store"},
                               {"X-Content-Type-Options", "nosniff"}},
                              failure.dump());
        return;
    }

    bool expected = false;
    if (!homeWallpaperUploadActive_.compare_exchange_strong(expected, true)) {
        Json failure = Json::object();
        failure.set("ok", Json::flag(false));
        failure.set("error", Json::text(say::msg(
            "srv.home.wallUploading", "Wait for the wallpaper upload to finish.")));
        (void)exchange.respond(400,
                              {{"Content-Type", "application/json; charset=utf-8"},
                               {"Cache-Control", "no-store"},
                               {"X-Content-Type-Options", "nosniff"}},
                              failure.dump());
        return;
    }
    struct UploadFlagReset {
        std::atomic<bool> &active;
        ~UploadFlagReset() { active.store(false); }
    } uploadFlagReset{homeWallpaperUploadActive_};

    std::string error;
    if (!service::removeHomeWallpapers(dataRoot_, &error)) {
        Json failure = Json::object();
        failure.set("ok", Json::flag(false));
        failure.set("error", Json::text(error));
        (void)exchange.respond(400,
                              {{"Content-Type", "application/json; charset=utf-8"},
                               {"Cache-Control", "no-store"},
                               {"X-Content-Type-Options", "nosniff"}},
                              failure.dump());
        return;
    }

    Json response = Json::object();
    response.set("ok", Json::flag(true));
    Json status = service::homeWallpaperStatus(dataRoot_);
    status.set("uploading", Json::flag(false));
    response.set("wallpapers", status);
    (void)exchange.respond(200,
                           {{"Content-Type", "application/json; charset=utf-8"},
                            {"Cache-Control", "no-store"},
                            {"X-Content-Type-Options", "nosniff"}},
                           response.dump());
}

service::HomeRecentApps HttpFrontDoor::queryHomeRecentApps() const {
    LunaReply reply;
    const bool answered = waitForLuna(
        luna_, "com.webos.service.recent/listRecents", Json::object(), reply);
    service::HomeRecentApps recent =
        service::homeRecentApps(answered ? reply.response : Json());
    if (recent.available) {
        std::lock_guard<std::mutex> lock(homeRecentAppsMutex_);
        homeRecentAppsCache_ = recent;
        homeRecentAppsCacheValid_ = true;
    }
    return recent;
}

void HttpFrontDoor::sendHomeRecentApps(HttpExchange &exchange) const {
    const service::HomeRecentApps recent = queryHomeRecentApps();
    if (!recent.available) {
        (void)exchange.respond(400,
                               {{"Content-Type", "application/json; charset=utf-8"},
                                {"Cache-Control", "no-store"},
                                {"X-Content-Type-Options", "nosniff"}},
                               "{\"ok\":false,\"error\":\"the TV did not list its recent apps\"}");
        return;
    }
    Json response = Json::object();
    response.set("ok", Json::flag(true));
    response.set("apps", recent.apps);
    (void)exchange.respond(200,
                           {{"Content-Type", "application/json; charset=utf-8"},
                            {"Cache-Control", "no-store"},
                            {"X-Content-Type-Options", "nosniff"}},
                           response.dump());
}

void HttpFrontDoor::sendHomeRecentAppImage(HttpExchange &exchange) const {
    std::string appId;
    if (!uniqueQueryParameter(exchange.request().query, "id", appId)) {
        (void)exchange.respond(404,
                               {{"Content-Type", "application/json; charset=utf-8"}},
                               "{\"ok\":false,\"error\":\"no picture\"}");
        return;
    }
    service::HomeRecentApps recent;
    {
        std::lock_guard<std::mutex> lock(homeRecentAppsMutex_);
        if (homeRecentAppsCacheValid_)
            recent = homeRecentAppsCache_;
    }
    const service::HomeRecentAppImage image =
        service::readHomeRecentAppImage(recent, appId);
    if (!image.found) {
        const int status = image.readError ? 500 : 404;
        Json failure = Json::object();
        failure.set("ok", Json::flag(false));
        failure.set("error", Json::text(image.readError ? "icon read failed" : "no picture"));
        (void)exchange.respond(status,
                               {{"Content-Type", "application/json; charset=utf-8"},
                                {"Cache-Control", "no-store"},
                                {"X-Content-Type-Options", "nosniff"}},
                               failure.dump());
        return;
    }
    const std::string cacheControl = "private, no-store";
    if (exchange.request().header("if-none-match") == image.etag) {
        (void)exchange.respond(304, {{"ETag", image.etag},
                                     {"Cache-Control", cacheControl}}, "");
        return;
    }
    const std::vector<HttpHeader> headers = {
        {"Content-Type", image.contentType},
        {"Cache-Control", cacheControl},
        {"ETag", image.etag}
    };
    if (!exchange.beginResponse(200, headers,
                                static_cast<long long>(image.bytes.size())))
        return;
    if (!image.bytes.empty())
        (void)exchange.writeBody(image.bytes.data(), image.bytes.size());
    (void)exchange.finishResponse();
}

void HttpFrontDoor::handleHomeRecentAppsPost(HttpExchange &exchange) const {
    const HttpRequest &request = exchange.request();
    const auto respond = [&exchange](int status, const Json &body) {
        (void)exchange.respond(status,
                               {{"Content-Type", "application/json; charset=utf-8"},
                                {"Cache-Control", "no-store"},
                                {"X-Content-Type-Options", "nosniff"}},
                               body.dump());
    };
    const auto fail = [&respond](const std::string &message, int status = 400) {
        Json body = Json::object();
        body.set("ok", Json::flag(false));
        body.set("error", Json::text(message));
        respond(status, body);
    };

    if (!config_.allowControl && !isLoopbackPeer(request.remoteAddress)) {
        fail(say::msg("srv.controlsOff", "controls disabled in config"), 403);
        return;
    }
    if (rejectCrossSitePost(exchange)) return;
    if (!exchange.setBodyLimit(8192)) {
        fail("request body too large", 413);
        return;
    }
    std::string body;
    char buffer[1024];
    for (;;) {
        const int count = exchange.readBody(buffer, sizeof(buffer));
        if (count == 0) break;
        if (count < 0) {
            fail(exchange.bodyLimitExceeded() ? "request body too large"
                                              : "malformed JSON",
                 exchange.bodyLimitExceeded() ? 413 : 400);
            return;
        }
        body.append(buffer, static_cast<size_t>(count));
    }
    if (exceedsUtf16CodeUnitLimit(body, 8192)) {
        fail("request body too large", 413);
        return;
    }
    const Json payload = Json::parse(body);
    if (!payload.isObject()) {
        fail("malformed JSON");
        return;
    }
    const std::string action = payload["action"].string();
    if (action != "open" && action != "close" && action != "closeAll") {
        fail("action must be open, close or closeAll");
        return;
    }

    service::HomeRecentApps recent = queryHomeRecentApps();
    if (!recent.available) {
        fail("the TV did not list its recent apps");
        return;
    }
    std::vector<std::string> ids;
    if (action == "closeAll") {
        for (const Json &app : recent.apps.items())
            if (app["id"].isString()) ids.push_back(app["id"].string());
    } else {
        const std::string id = payload["id"].isString() ? payload["id"].string() : "";
        const std::vector<Json> recentItems = recent.apps.items();
        const bool listed = std::any_of(recentItems.begin(), recentItems.end(),
                                        [&id](const Json &app) {
            return app["id"].string() == id;
        });
        if (!listed) {
            fail("that app is not in recent apps");
            return;
        }
        if (action == "open") {
            Json launch = Json::object();
            launch.set("id", Json::text(id));
            LunaReply launchReply;
            if (!waitForLuna(luna_, "com.webos.applicationManager/launch",
                             launch, launchReply) ||
                !service::javascriptTruthy(launchReply.response["returnValue"])) {
                fail("the TV would not open it");
                return;
            }
            Json success = Json::object();
            success.set("ok", Json::flag(true));
            respond(200, success);
            return;
        }
        ids.push_back(id);
    }

    if (ids.empty()) {
        recent = queryHomeRecentApps();
        if (!recent.available) {
            fail("the TV did not list its recent apps");
            return;
        }
        Json success = Json::object();
        success.set("ok", Json::flag(true));
        success.set("apps", recent.apps);
        respond(200, success);
        return;
    }

    const auto readRunning = [this](Json &running) {
        LunaReply runningReply;
        if (!waitForLuna(luna_, "com.webos.applicationManager/running",
                         Json::object(), runningReply) ||
            (runningReply.response["returnValue"].isBoolean() &&
             !runningReply.response["returnValue"].boolean()) ||
            !runningReply.response["running"].isArray())
            return false;
        running = runningReply.response["running"];
        return true;
    };
    Json running;
    if (!readRunning(running)) {
        fail("the TV would not close it");
        return;
    }
    std::vector<std::pair<std::string, Json>> stopCalls;
    for (const Json &app : running.items()) {
        const std::string id = app["id"].string();
        if (std::find(ids.begin(), ids.end(), id) == ids.end()) continue;
        const bool webApp = app["appType"].string() == "web";
        Json stop = Json::object();
        stop.set(webApp ? "appId" : "id", Json::text(id));
        stopCalls.emplace_back(webApp
            ? "com.palm.webappmanager/killApp"
            : "com.webos.applicationManager/closeByAppId", stop);
    }
    std::vector<LunaReply> stopReplies;
    if (!stopCalls.empty())
        (void)waitForLunaBatch(luna_, stopCalls, stopReplies);

    bool stopped = false;
    for (unsigned attempt = 0; attempt <= 10; ++attempt) {
        if (!readRunning(running)) {
            fail("the TV would not close it");
            return;
        }
        const std::vector<Json> runningItems = running.items();
        const bool anyRunning = std::any_of(
            runningItems.begin(), runningItems.end(), [&ids](const Json &app) {
                return std::find(ids.begin(), ids.end(), app["id"].string()) != ids.end();
            });
        if (!anyRunning) {
            stopped = true;
            break;
        }
        if (attempt < 10) std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    if (!stopped) {
        fail("the TV would not close it");
        return;
    }

    recent = queryHomeRecentApps();
    if (!recent.available) {
        fail("the TV did not list its recent apps");
        return;
    }
    std::vector<std::string> removeIds;
    for (const Json &app : recent.apps.items()) {
        const std::string id = app["id"].string();
        if (std::find(ids.begin(), ids.end(), id) != ids.end())
            removeIds.push_back(id);
    }
    for (const std::string &id : removeIds) {
        Json remove = Json::object();
        remove.set("appId", Json::text(id));
        LunaReply removed;
        if (!waitForLuna(luna_, "com.webos.service.recent/removeRecent",
                         remove, removed) ||
            !service::javascriptTruthy(removed.response["returnValue"])) {
            fail("the TV would not close it");
            return;
        }
    }
    recent = queryHomeRecentApps();
    if (!recent.available) {
        fail("the TV did not list its recent apps");
        return;
    }
    Json success = Json::object();
    success.set("ok", Json::flag(true));
    success.set("apps", recent.apps);
    respond(200, success);
}

Json HttpFrontDoor::openLgHomeEscape(int &status) const {
    const auto fail = [](const std::string &message) {
        Json result = Json::object();
        result.set("ok", Json::flag(false));
        result.set("error", Json::text(message));
        return result;
    };
    status = 400;
    homeEscapeActive_->store(true, std::memory_order_release);

    std::string bridgeError;
    if (!proxy_.requestNodeHomeEscape(true, bridgeError) && logger_)
        logger_->warning("home open: Node escape-state bridge unavailable" +
            (bridgeError.empty() ? std::string() : ": " + bridgeError));

    if (homeTakeoverMounted(systemRoot_, dataRoot_)) {
        if (!homeTakeoverLifecycleManager_) {
            status = 503;
            return fail("native Home takeover lifecycle is unavailable");
        }
        const Json released = homeTakeoverLifecycleManager_->release(
            controlPolicyContext(config_));
        if (!released["ok"].boolean())
            return fail(released["error"].isString()
                ? released["error"].string()
                : say::msg("srv.home.launchRefused", "the TV would not open it"));
    }

    Json launchPayload = Json::object();
    launchPayload.set("id", Json::text("com.webos.app.home"));
    LunaReply launchReply;
    if (!waitForLuna(luna_, "com.webos.applicationManager/launch",
                     launchPayload, launchReply) ||
        !service::javascriptTruthy(launchReply.response["returnValue"]))
        return fail(say::msg("srv.home.launchRefused", "the TV would not open it"));

    Json success = Json::object();
    success.set("ok", Json::flag(true));
    status = 200;
    return success;
}

Json HttpFrontDoor::openLgHomeFromRemoteInput() const {
    Json result = Json::object();
    result.set("ok", Json::flag(true));
    result.set("ignored", Json::flag(true));
    JsonFileResult stored;
    {
        std::lock_guard<std::mutex> lock(homeConfigMutex_);
        JsonFile file(dataFilePath(dataRoot_, "home.json"), 8 * 1024 * 1024);
        stored = file.load();
    }
    if (!stored.error.empty() && !stored.found) {
        result.set("ok", Json::flag(false));
        result.set("ignored", Json::flag(false));
        result.set("error", Json::text(stored.error));
        return result;
    }
    const Json document = service::normalizeHomeDocument(
        stored.found ? stored.value : Json::object());
    if (!document["enabled"].boolean() ||
        homeEscapeActive_->load(std::memory_order_acquire))
        return result;
    int ignoredStatus = 200;
    return openLgHomeEscape(ignoredStatus);
}

Json HttpFrontDoor::returnNativeTvHomeFromRemoteInput() const {
    const auto ignored = []() {
        Json result = Json::object();
        result.set("ok", Json::flag(true));
        result.set("ignored", Json::flag(true));
        return result;
    };
    const auto failed = [](const std::string &error) {
        Json result = Json::object();
        result.set("ok", Json::flag(false));
        result.set("ignored", Json::flag(false));
        result.set("error", Json::text(error));
        return result;
    };
    // Given back by the crash watch or the probe: not taken again until restart.
    if (!homeEscapeActive_->load(std::memory_order_acquire) || homeHalted())
        return ignored();

    LunaReply foreground;
    if (!waitForLuna(luna_, "com.webos.applicationManager/getForegroundAppInfo",
                     Json::object(), foreground) ||
        foreground.response["appId"].string() != "com.webos.app.home")
        return ignored();

    bool expected = true;
    if (!homeEscapeActive_->compare_exchange_strong(
            expected, false, std::memory_order_acq_rel))
        return ignored();

    JsonFileResult stored;
    {
        std::lock_guard<std::mutex> lock(homeConfigMutex_);
        JsonFile file(dataFilePath(dataRoot_, "home.json"), 8 * 1024 * 1024);
        stored = file.load();
    }
    if (!stored.error.empty() && !stored.found)
        return failed(stored.error);
    const Json home = service::normalizeHomeDocument(
        stored.found ? stored.value : Json::object());
    if (!home["enabled"].boolean())
        return ignored();
    if (!homeTakeoverLifecycleManager_)
        return failed("native Home takeover lifecycle is unavailable");

    const Json takeover = homeTakeoverLifecycleManager_->takeover(
        home["hideLgHomeAtStart"].boolean(), controlPolicyContext(config_));
    if (!takeover["ok"].boolean())
        return failed(takeover["error"].string(
            "could not take nativeTv Home's place again"));

    std::string bridgeError;
    const bool nodeSynchronized =
        proxy_.requestNodeHomeEscape(false, bridgeError);
    if (!nodeSynchronized && logger_)
        logger_->warning("home return: Node escape-state bridge unavailable" +
            (bridgeError.empty() ? std::string() : ": " + bridgeError));

    Json result = Json::object();
    result.set("ok", Json::flag(true));
    result.set("ignored", Json::flag(false));
    result.set("fallback", Json::flag(!nodeSynchronized));
    return result;
}

Json HttpFrontDoor::returnNativeTvHomeAfterForegroundChange(
    const std::string &observedAppId) const {
    const auto ignored = []() {
        Json result = Json::object();
        result.set("ok", Json::flag(true));
        result.set("ignored", Json::flag(true));
        return result;
    };
    const auto failed = [](const std::string &error) {
        Json result = Json::object();
        result.set("ok", Json::flag(false));
        result.set("ignored", Json::flag(false));
        result.set("fallback", Json::flag(true));
        result.set("error", Json::text(error));
        return result;
    };
    if (observedAppId.empty() || observedAppId == "com.webos.app.home" ||
        !homeEscapeActive_->load(std::memory_order_acquire) || homeHalted())
        return ignored();

    LunaReply foreground;
    if (!waitForLuna(luna_, "com.webos.applicationManager/getForegroundAppInfo",
                     Json::object(), foreground))
        return failed("could not confirm the foreground app after leaving LG Home");
    const std::string currentAppId = foreground.response["appId"].string();
    if (currentAppId.empty() || currentAppId == "com.webos.app.home")
        return ignored();

    JsonFileResult stored;
    {
        std::lock_guard<std::mutex> lock(homeConfigMutex_);
        JsonFile file(dataFilePath(dataRoot_, "home.json"), 8 * 1024 * 1024);
        stored = file.load();
    }
    if (!stored.error.empty() && !stored.found)
        return failed(stored.error);
    const Json home = service::normalizeHomeDocument(
        stored.found ? stored.value : Json::object());
    if (!home["enabled"].boolean()) {
        homeEscapeActive_->store(false, std::memory_order_release);
        return ignored();
    }
    if (!homeTakeoverLifecycleManager_)
        return failed("native Home takeover lifecycle is unavailable");

    bool expected = true;
    if (!homeEscapeActive_->compare_exchange_strong(
            expected, false, std::memory_order_acq_rel))
        return ignored();

    const Json takeover = homeTakeoverLifecycleManager_->takeover(
        home["hideLgHomeAtStart"].boolean(), controlPolicyContext(config_));
    if (!takeover["ok"].boolean()) {
        homeEscapeActive_->store(true, std::memory_order_release);
        return failed(takeover["error"].string(
            "could not take nativeTv Home's place after leaving LG Home"));
    }

    std::string bridgeError;
    if (!proxy_.requestNodeHomeEscape(false, bridgeError)) {
        homeEscapeActive_->store(true, std::memory_order_release);
        if (logger_)
            logger_->warning("home foreground return: Node escape-state bridge unavailable" +
                (bridgeError.empty() ? std::string() : ": " + bridgeError));
        Json result = Json::object();
        result.set("ok", Json::flag(true));
        result.set("ignored", Json::flag(false));
        result.set("fallback", Json::flag(true));
        return result;
    }

    Json result = Json::object();
    result.set("ok", Json::flag(true));
    result.set("ignored", Json::flag(false));
    return result;
}

void HttpFrontDoor::queueHomeForegroundReturn(const std::string &appId) const {
    if (!eventLoop_ || appId.empty() || appId == "com.webos.app.home" ||
        !homeEscapeActive_ || !homeEscapeReturnPending_)
        return;
    bool expected = false;
    if (!homeEscapeReturnPending_->compare_exchange_strong(
            expected, true, std::memory_order_acq_rel))
        return;

    const std::shared_ptr<std::atomic<bool>> pending = homeEscapeReturnPending_;
    const bool accepted = eventLoop_->submitBlocking(
        [this, appId, pending]() {
            try {
                const Json result = returnNativeTvHomeAfterForegroundChange(appId);
                if (result["ignored"].boolean()) {
                    pending->store(false, std::memory_order_release);
                    return;
                }
                if (!result["ok"].boolean() || result["fallback"].boolean()) {
                    std::string fallbackError;
                    if (proxy_.requestNodeHomeForegroundFallback(appId,
                                                                fallbackError)) {
                        homeEscapeActive_->store(false, std::memory_order_release);
                    } else if (logger_) {
                        logger_->warning("home foreground return fallback failed" +
                            (fallbackError.empty() ? std::string() :
                             ": " + fallbackError));
                    }
                }
            } catch (const std::exception &error) {
                if (logger_)
                    logger_->warning(std::string("home foreground return failed: ") +
                                     error.what());
            } catch (...) {
                if (logger_)
                    logger_->warning("home foreground return failed with an unknown error");
            }
            pending->store(false, std::memory_order_release);
        });
    if (accepted) return;

    if (eventLoop_->scheduleAfter(EventLoop::Milliseconds(100),
            [this, appId, pending]() {
                pending->store(false, std::memory_order_release);
                queueHomeForegroundReturn(appId);
            }))
        return;

    pending->store(false, std::memory_order_release);
    if (logger_)
        logger_->warning("home foreground return: worker queue is full; "
                         "will retry on the next foreground change");
}

/*
 * Opens a Home tile or a feed result using the native catalogue, metadata and
 * launch services. A device opens its HDMI input; nothing is sent to the
 * device or a receiver. Managed LG Home escape releases the native takeover
 * and launches LG Home; standalone mode remains with Node.
 */
void HttpFrontDoor::handleHomeOpenPost(HttpExchange &exchange) const {
    const HttpRequest &request = exchange.request();
    const auto respond = [&exchange](int status, const Json &body) {
        (void)exchange.respond(status,
                               {{"Content-Type", "application/json; charset=utf-8"},
                                {"Cache-Control", "no-store"},
                                {"X-Content-Type-Options", "nosniff"}},
                               body.dump());
    };
    const auto fail = [&respond](const std::string &message, int status = 400) {
        Json body = Json::object();
        body.set("ok", Json::flag(false));
        body.set("error", Json::text(message));
        respond(status, body);
    };

    if (!config_.allowControl && !isLoopbackPeer(request.remoteAddress)) {
        fail(say::msg("srv.controlsOff", "controls disabled in config"), 403);
        return;
    }
    if (rejectCrossSitePost(exchange)) return;
    if (!setupJsonContentType(request)) {
        fail("Content-Type must be application/json", 415);
        return;
    }
    if (!exchange.setBodyLimit(32768)) {
        fail("request body too large", 413);
        return;
    }
    std::string body;
    char buffer[1024];
    for (;;) {
        const int count = exchange.readBody(buffer, sizeof(buffer));
        if (count == 0) break;
        if (count < 0) {
            fail(exchange.bodyLimitExceeded() ? "request body too large"
                                              : "malformed JSON",
                 exchange.bodyLimitExceeded() ? 413 : 400);
            return;
        }
        body.append(buffer, static_cast<size_t>(count));
    }
    if (exceedsUtf16CodeUnitLimit(body, 8192)) {
        fail("request body too large", 413);
        return;
    }
    const Json payload = Json::parse(body);
    if (!payload.isObject()) {
        fail("malformed JSON");
        return;
    }
    const std::string kind = payload["kind"].string();
    std::string id = payload["id"].isString() ? payload["id"].string() : "";
    if ((kind == "title" || kind == "youtube" || kind == "netflix") &&
        payload["id"].isNumber())
        id = payload["id"].dump();
    const std::string type = payload["type"].isString()
                                 ? payload["type"].string() : "";

    const bool lgHomeEscape = kind == "lg" ||
        (kind == "app" && id == "lg-home");
    if (lgHomeEscape) {
        int status = 200;
        const Json result = openLgHomeEscape(status);
        respond(status, result);
        return;
    }

    Json apps;
    Json devices;
    Json channels;
    if (kind == "app" || kind == "title" || kind == "youtube" ||
        kind == "netflix") {
        apps = homeCatalogueAppsSnapshot();
    } else if (kind == "dev") {
        devices = homeCatalogueDevicesSnapshot();
    } else if (kind == "ch") {
        bool tunerAvailable = false;
        channels = homeChannelSnapshot(tunerAvailable);
    }

    const service::HomeOpenPlan plan =
        service::planHomeOpen(kind, id, apps, devices, channels, type);
    if (!plan.handled) {
        (void)proxy_.forwardBufferedBody(exchange, body);
        return;
    }
    service::HomeOpenPlan resolved = plan;
    if (resolved.ok && resolved.action == "netflix") {
        const Json lookup = homeContent_.netflix(
            config_.document["tmdbKey"].string(),
            config_.document["netflixDeepLinks"].boolean(), type, id);
        service::resolveHomeNetflixOpen(resolved, lookup);
    }
    if (!resolved.ok) {
        fail(resolved.error);
        return;
    }

    // A device tile marks its input first: the input arriving in front is the
    // tile, not the remote, and a refused tile must not consume the next
    // input switch from the remote.
    const bool deviceTile = resolved.action == "launch" && resolved.row == "dev";
    if (deviceTile) {
        std::lock_guard<std::mutex> lock(homeTileInputState_->mutex);
        homeTileInputState_->appId = resolved.appId;
        homeTileInputState_->at = homeNowMilliseconds();
    }
    if (resolved.action == "launch") {
        Json launchPayload = Json::object();
        launchPayload.set("id", Json::text(resolved.appId));
        if (resolved.launchParams.isObject())
            launchPayload.set("params", resolved.launchParams);
        LunaReply launchReply;
        if (!waitForLuna(luna_, "com.webos.applicationManager/launch",
                         launchPayload, launchReply) ||
            !service::javascriptTruthy(launchReply.response["returnValue"])) {
            if (deviceTile) {
                std::lock_guard<std::mutex> lock(homeTileInputState_->mutex);
                homeTileInputState_->appId.clear();
                homeTileInputState_->at = 0;
            }
            fail(say::msg("srv.home.launchRefused", "the TV would not open it"));
            return;
        }
    } else {
        Json tunePayload = Json::object();
        tunePayload.set("channelId", Json::text(resolved.channelId));
        LunaReply tuneReply;
        if (!waitForLuna(luna_, "com.webos.service.apiadapter/tv/openChannel",
                         tunePayload, tuneReply) ||
            !service::javascriptTruthy(tuneReply.response["returnValue"])) {
            fail(say::msg("srv.home.launchRefused", "the TV would not open it"));
            return;
        }
    }

    // Recorded only for a row set to Last opened, and only while nativeTv
    // Home is on.
    JsonFile homeFile(dataFilePath(dataRoot_, "home.json"));
    const Json document =
        service::normalizeHomeDocument(homeFile.load().value);
    if (service::homeOpenStampWanted(document, resolved.row))
        (void)recordHomeRecent(resolved.row, resolved.rowId);

    Json success = Json::object();
    success.set("ok", Json::flag(true));
    respond(200, success);
}

void HttpFrontDoor::sendApps(HttpExchange &exchange) const {
    service::AppListState state;
    state.dataRoot = dataRoot_;
    state.systemRoot = systemRoot_;
    std::string stateError;
    if (!readAppTileStateFiles(hiddenAppsPath_, tileHidingFlagPath_, state.hiddenIds,
                               state.tileHidingEnabled, &stateError)) {
        state.hiddenIds.clear();
        state.tileHidingEnabled = false;
        if (logger_)
            logger_->warning("apps: could not read hidden tile state: " + stateError);
    }

    const bool javascriptRemoteKeys =
        service::hasJavaScriptRemoteKeys(systemRoot_);
    std::vector<std::pair<std::string, Json>> calls = {
        {"com.webos.applicationManager/listApps", Json::object()},
        {"com.webos.applicationManager/listLaunchPoints", Json::object()}
    };
    size_t remoteKeysCall = calls.size();
    if (javascriptRemoteKeys) {
        Json configRequest = Json::object();
        Json configNames = Json::array();
        configNames.add(Json::text("com.webos.surfacemanager.keyFilters"));
        configRequest.set("configNames", configNames);
        calls.push_back({"com.webos.service.config/getConfigs", configRequest});
        calls.push_back({"com.webos.service.eim/getAllInputStatus", Json::object()});
        calls.push_back({"com.webos.applicationManager/getAppInfo",
                         Json::parse("{\"id\":\"com.webos.app.quickinputpicker\"}")});
    } else {
        Json configRequest = Json::object();
        Json configNames = Json::array();
        configNames.add(Json::text("com.webos.surfacemanager.keyFiltersCpp"));
        configNames.add(Json::text("com.webos.keyaction.tvGuide"));
        configNames.add(Json::text("com.webos.keyaction.inputHub"));
        configRequest.set("configNames", configNames);
        calls.push_back({"com.webos.service.config/getConfigs", configRequest});
        calls.push_back({"com.webos.service.eim/getAllInputStatus", Json::object()});
        calls.push_back({"com.webos.applicationManager/getAppInfo",
                         Json::parse("{\"id\":\"com.webos.app.quickinputpicker\"}")});
        Json mappingRequest = Json::object();
        mappingRequest.set("category", Json::text("other"));
        mappingRequest.set("keys", Json::parse("[\"mapping_info\"]"));
        calls.push_back({"com.webos.settingsservice/getSystemSettings", mappingRequest});
        Json buttonRequest = Json::object();
        buttonRequest.set("category", Json::text("other"));
        buttonRequest.set("keys", Json::parse(
            "[\"customShortPress\",\"customLongPress\"]"));
        calls.push_back({"com.webos.settingsservice/getSystemSettings", buttonRequest});
        calls.push_back({"com.webos.applicationManager/getAppInfo",
                         Json::parse("{\"id\":\"amazon.alexa.view\"}")});
    }
    std::vector<LunaReply> replies;
    (void)waitForLunaBatch(luna_, calls, replies);
    LunaReply appsReply;
    LunaReply launchPointsReply;
    if (!replies.empty() && replies[0].error.empty() && replies[0].response.isObject())
        appsReply = replies[0];
    else
        appsReply.response = Json::object();
    if (replies.size() > 1 && replies[1].error.empty() && replies[1].response.isObject())
        launchPointsReply = replies[1];
    else
        launchPointsReply.response = Json::object();

    const Json context = appsRuntimeContext(appsReply.response, launchPointsReply.response);
    Json result = service::appListResponse(appsReply.response, launchPointsReply.response,
                                           state, config_.allowControl);
    static const char *contextFields[] = {
        "tileHidingAvailable", "tileHidingHeld", "tileHidingOverridden",
        "tileHidingNudge", "nativeTvHome", "services", "preload",
        "quickAccess", "remoteKeys", "homeScreen"
    };
    for (const char *field : contextFields)
        if (context.has(field)) result.set(field, context[field]);

    if (javascriptRemoteKeys && replies.size() >= remoteKeysCall + 3 &&
        replies[remoteKeysCall].error.empty() &&
        replies[remoteKeysCall].response.isObject()) {
        service::RemoteKeysReadState remoteKeysState;
        remoteKeysState.systemRoot = systemRoot_;
        remoteKeysState.dataRoot = dataRoot_;
        remoteKeysState.filterChain = replies[remoteKeysCall].response["configs"]
            ["com.webos.surfacemanager.keyFilters"];
        remoteKeysState.appsReply = appsReply.response;
        remoteKeysState.inputDevices = replies[remoteKeysCall + 1].error.empty() &&
                replies[remoteKeysCall + 1].response.isObject()
            ? replies[remoteKeysCall + 1].response["devices"] : Json::array();
        remoteKeysState.inputSwitcherAvailable =
            replies[remoteKeysCall + 2].error.empty() &&
            replies[remoteKeysCall + 2].response.isObject() &&
            replies[remoteKeysCall + 2].response["returnValue"].boolean();
        const Json remoteKeys = service::remoteKeysReadResponse(remoteKeysState);
        if (remoteKeys["supported"].boolean())
            result.set("remoteKeys", remoteKeys);
    } else if (!javascriptRemoteKeys && replies.size() >= remoteKeysCall + 6 &&
               replies[remoteKeysCall].error.empty() &&
               replies[remoteKeysCall].response.isObject()) {
        service::NativeRemoteKeysReadState remoteKeysState;
        remoteKeysState.dataRoot = dataRoot_;
        remoteKeysState.filterChain = replies[remoteKeysCall].response["configs"]
            ["com.webos.surfacemanager.keyFiltersCpp"];
        remoteKeysState.actions = Json::object();
        for (const char *name : {"tvGuide", "inputHub"}) {
            const Json value = replies[remoteKeysCall].response["configs"][
                std::string("com.webos.keyaction.") + name];
            if (value.isObject()) remoteKeysState.actions.set(name, value);
        }
        remoteKeysState.inputDevices = replies[remoteKeysCall + 1].error.empty()
            ? replies[remoteKeysCall + 1].response["devices"] : Json::array();
        remoteKeysState.inputSwitcherAvailable =
            replies[remoteKeysCall + 2].error.empty() &&
            replies[remoteKeysCall + 2].response["returnValue"].boolean();
        remoteKeysState.mappingInfo = Json::array();
        if (replies[remoteKeysCall + 3].error.empty() &&
            replies[remoteKeysCall + 3].response["returnValue"].boolean() &&
            replies[remoteKeysCall + 3].response["settings"][
                "mapping_info"].isArray())
            remoteKeysState.mappingInfo = replies[remoteKeysCall + 3].response
                ["settings"]["mapping_info"];
        remoteKeysState.buttonSettings = Json::object();
        if (replies[remoteKeysCall + 4].error.empty() &&
            replies[remoteKeysCall + 4].response["returnValue"].boolean()) {
            for (const char *name : {"customShortPress", "customLongPress"}) {
                const Json value = replies[remoteKeysCall + 4].response
                    ["settings"][name];
                if (value.isObject() && value["id"].isString())
                    remoteKeysState.buttonSettings.set(name, value);
            }
        }
        remoteKeysState.appsReply = appsReply.response;
        remoteKeysState.voiceConductorAvailable =
            service::backgroundHiddenServiceAvailable("voiceconductor", systemRoot_);
        remoteKeysState.alexaViewAvailable = replies[remoteKeysCall + 5].error.empty() &&
            replies[remoteKeysCall + 5].response["returnValue"].boolean();
        const Json remoteKeys =
            service::nativeRemoteKeysReadResponse(remoteKeysState);
        if (remoteKeys["supported"].boolean())
            result.set("remoteKeys", remoteKeys);
    }

    // Unsupported firmware reads as null, not as a missing field.
    if (!result.has("remoteKeys")) result.set("remoteKeys", Json());

    if (context.has("tileHidingAvailable") &&
        !context["tileHidingAvailable"].boolean()) {
        result.set("systemTiles", Json::array());
        result.set("tileHidingEnabled", Json::flag(false));
        result.set("hiddenCount", Json::numeric(0));
    }
    sendMetricsJson(exchange, result.dump());
}

void HttpFrontDoor::sendAppCatalog(HttpExchange &exchange) const {
    const HttpRequest &request = exchange.request();
    std::string forceValue;
    const bool force = queryParameter(request.query, "force", forceValue) &&
                       !forceValue.empty();
    Json configuredRepos;
    {
        std::lock_guard<std::mutex> lock(appCatalogReposMutex_);
        configuredRepos = appCatalogRepos_;
    }

    LunaReply appReply;
    if (!waitForLuna(luna_, "com.webos.applicationManager/listApps",
                     Json::object(), appReply))
        appReply.response = Json::object();
    const int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::system_clock::now().time_since_epoch())
                              .count();
    const Json result = appCatalog_.get(configuredRepos, appReply.response, force, nowMs);
    sendMetricsJson(exchange, result.dump());
}

void HttpFrontDoor::handleLanIcon(HttpExchange &exchange) const {
    const HttpRequest &request = exchange.request();
    const std::string prefix = "/icons/";
    const std::string suffix = ".png";
    if ((request.method != "GET" && request.method != "HEAD") ||
        request.uri.compare(0, prefix.size(), prefix) != 0 ||
        request.uri.size() <= prefix.size() + suffix.size() ||
        request.uri.compare(request.uri.size() - suffix.size(), suffix.size(), suffix) != 0) {
        (void)exchange.respond(404, {{"Content-Type", "text/plain"}}, "not found");
        return;
    }
    const std::string appId = request.uri.substr(
        prefix.size(), request.uri.size() - prefix.size() - suffix.size());
    const service::AppIconFile icon = service::readAppIcon(appId, systemRoot_);
    if (!icon.found) {
        (void)exchange.respond(404, {{"Content-Type", "text/plain"}}, "not found");
        return;
    }
    const std::string cacheControl = "public, max-age=86400";
    if (request.header("if-none-match") == icon.etag) {
        (void)exchange.respond(304, {{"ETag", icon.etag},
                                     {"Cache-Control", cacheControl}}, "");
        return;
    }
    const std::vector<HttpHeader> headers = {
        {"Content-Type", "image/png"},
        {"Cache-Control", cacheControl},
        {"ETag", icon.etag}
    };
    if (!exchange.beginResponse(200, headers, static_cast<long long>(icon.bytes.size())))
        return;
    if (request.method == "GET" && !icon.bytes.empty())
        (void)exchange.writeBody(icon.bytes.data(), icon.bytes.size());
    (void)exchange.finishResponse();
}

void HttpFrontDoor::handlePhoneHandoff(HttpExchange &exchange) const {
    std::unique_lock<std::mutex> lock(handoffMutex_);
    if (!handoffCode_.empty() && std::chrono::steady_clock::now() >= handoffExpires_) {
        retiredHandoffCode_ = handoffCode_;
        handoffCode_.clear();
        handoffExpires_ = std::chrono::steady_clock::time_point();
    }
    if (handoffCode_.empty()) {
        const HttpRequest &request = exchange.request();
        if ((request.uri == "/" && request.method == "GET") ||
            (request.uri == "/mqtt" && request.method == "POST")) {
            std::string code;
            if (!handoffCodeFromQuery(request.query, code) || code == retiredHandoffCode_) {
                sendExpiredHandoff(exchange);
                return;
            }
        }
        lock.unlock();
        if (nodeFreeStart()) {   // no Node, so no legacy handoff listener to forward to
            sendExpiredHandoff(exchange);
            return;
        }
        (void)handoffProxy_.forward(exchange);
        return;
    }

    std::string code;
    if (!handoffCodeFromQuery(exchange.request().query, code) || code != handoffCode_) {
        sendExpiredHandoff(exchange);
        return;
    }
    if (exchange.request().uri == "/" && exchange.request().method == "GET") {
        lock.unlock();
        std::string page;
        std::string missing;
        if (!staticAssets_.assemblePage("google-tv/pages/setup-phone/index.html", page,
                                        missing)) {
            sendSetupError(exchange, 500,
                           "could not assemble the setup page: " + missing, true);
            return;
        }
        (void)exchange.respond(200,
                               {{"Content-Type", "text/html; charset=utf-8"},
                                {"Cache-Control", "no-store"},
                                {"X-Content-Type-Options", "nosniff"},
                                {"Referrer-Policy", "no-referrer"}},
                               page);
        return;
    }
    if (exchange.request().uri == "/mqtt" && exchange.request().method == "POST") {
        bool consumeCode = false;
        (void)savePhoneMqttSettings(exchange, configPath_, proxy_, consumeCode);
        if (consumeCode) {
            retiredHandoffCode_ = handoffCode_;
            handoffCode_.clear();
            handoffExpires_ = std::chrono::steady_clock::time_point();
        }
        return;
    }
    (void)exchange.respond(404, {{"Content-Type", "text/plain; charset=utf-8"}}, "not found");
}

void HttpFrontDoor::handle(HttpExchange &exchange) const {
    dispatch(exchange);
    // A write the TV accepted makes the next stats read fresh and drops every
    // cached Luna read, as controls.js does after each control; a forwarded
    // write counts too, as the Node side cannot reach this collector.
    const HttpRequest &request = exchange.request();
    const int status = exchange.responseStatus();
    if (request.method != "GET" && request.method != "HEAD" && request.method != "OPTIONS" &&
        status >= 200 && status < 300 && request.uri.compare(0, 10, "/_internal") != 0) {
        statsCollector()->clearCache();
        statsCollector()->forget({});
    }
}

void HttpFrontDoor::expireStats(const std::string &group) const {
    // The cached reads each live subscription makes stale (tvweb.js LIVE_STALE).
    static const std::map<std::string, std::vector<std::string>> stale = {
        {"audio", {"com.webos.audio/", "com.webos.service.audio/", "\"category\":\"sound\""}},
        {"application", {"getForegroundAppInfo", "\"category\":\"picture\"", "getVRRInfo",
                         "\"category\":\"other\"", "\"category\":\"general\"",
                         "\"category\":\"option\"", "getSystemSettingDesc"}},
        {"picture", {"\"category\":\"picture\""}},
        {"power", {"tvpower/power/getPowerState"}}};
    statsCollector()->expire();
    const auto found = stale.find(group);
    if (found != stale.end()) statsCollector()->forget(found->second);
}

void HttpFrontDoor::dispatch(HttpExchange &exchange) const {
    const HttpRequest &request = exchange.request();
    if (request.uri == "/_internal/devtools") {
        setNativeDevtools(exchange);
        return;
    }
    if (request.uri == "/_internal/screensaver") {
        setNativeScreensaver(exchange);
        return;
    }
    if (request.uri == "/_internal/sam-rescan") {
        setNativeSamRescan(exchange);
        return;
    }
    if (request.uri == "/_internal/home-screen") {
        setNativeHomeScreen(exchange);
        return;
    }
    if (request.uri == "/_internal/home-config") {
        persistHomeConfig(exchange);
        return;
    }
    if (request.uri == "/_internal/home-recent") {
        persistHomeRecent(exchange);
        return;
    }
    if (request.uri == "/_internal/home-auto-launch") {
        syncHomeAutoLaunch(exchange);
        return;
    }
    if (request.uri == "/_internal/hdmi-seen") {
        handleHdmiSeen(exchange);
        return;
    }
    if (request.uri == "/_internal/last-app") {
        handleLastApp(exchange);
        return;
    }
    if (request.uri == "/_internal/mqtt/forget") {
        handleMqttForget(exchange);
        return;
    }
    if (request.uri == "/_internal/remote-keys/apply") {
        handleRemoteKeysApply(exchange);
        return;
    }
    if (request.uri == "/_internal/apps-state") {
        handleAppsState(exchange);
        return;
    }
    if (request.uri == "/_internal/apps/elevated") {
        handleAppElevation(exchange);
        return;
    }
    if (request.uri == "/_internal/apps/elevate") {
        handleAppElevationRun(exchange);
        return;
    }
    if (request.uri == "/_internal/apps/install/run") {
        handleAppInstallRun(exchange);
        return;
    }
    if (request.uri == "/_internal/apps/install/apps") {
        handleInstalledApps(exchange);
        return;
    }
    if (request.uri == "/_internal/apps/install-job") {
        handleInstallerJob(exchange);
        return;
    }
    if (request.uri == "/_internal/apps/install/preview") {
        buildAppInstallPreview(exchange);
        return;
    }
    if (request.uri == "/_internal/apps/install/space") {
        readAppInstallSpace(exchange);
        return;
    }
    if (request.uri == "/_internal/backup/config") {
        setNativeBackupConfig(exchange);
        return;
    }
    if (request.uri == "/_internal/backup/validate") {
        validateNativeBackupRestore(exchange);
        return;
    }
    if (request.uri == "/_internal/backup/apps-state") {
        setNativePendingBackupApps(exchange);
        return;
    }
    if (request.uri == "/_internal/backup/restore-plan") {
        planNativeBackupApps(exchange);
        return;
    }
    if (request.uri == "/_internal/backup/restore-lg") {
        restoreNativeBackupLg(exchange);
        return;
    }
    if (request.uri == "/_internal/backup/daily") {
        setNativeDailyBackup(exchange);
        return;
    }
    if (request.uri == "/_internal/apps/tile-hiding") {
        setNativeAppTileOperation(exchange);
        return;
    }
    if (request.uri == "/_internal/apps/uninstall") {
        setNativeAppUninstall(exchange);
        return;
    }
    if (request.uri == "/_internal/apps/repos") {
        setNativeAppsRepos(exchange);
        return;
    }
    if (request.uri == "/_internal/apps/catalog/find") {
        findNativeAppPackage(exchange);
        return;
    }
    if (request.uri == "/_internal/apps/catalog/check") {
        checkNativeAppsRepository(exchange);
        return;
    }
    if (request.uri == "/_internal/apps/download") {
        downloadNativeAppPackage(exchange);
        return;
    }
    if (request.uri == "/_internal/apps/inspect") {
        inspectNativeAppPackage(exchange);
        return;
    }
    if ((request.uri == "/api/apps/install/sideload" ||
         request.uri == "/api/apps/install/repos" ||
         request.uri == "/api/apps/install/upload" ||
         request.uri == "/api/apps/install/fetch" ||
         request.uri == "/api/apps/install/url" ||
         request.uri == "/api/apps/install/confirm" ||
         request.uri == "/api/apps/install/cancel" ||
         request.uri == "/api/apps/install/status") &&
        !appInstallHostAllowed(request, config_.document)) {
        Json failure = Json::object();
        failure.set("ok", Json::flag(false));
        failure.set("error", Json::text(
            "this address is not allowed to install apps; add the name to apps.hosts in config.json, or use the TV's IP address"));
        (void)exchange.respond(403,
                               {{"Content-Type", "application/json; charset=utf-8"},
                                {"Cache-Control", "no-store"},
                                {"X-Content-Type-Options", "nosniff"}},
                               failure.dump());
        return;
    }
    if (request.uri.compare(0, 5, "/api/") == 0 && !requestIsAuthorized(request, config_.token)) {
        (void)exchange.respond(401,
                               {{"Content-Type", "application/json"},
                                {"Cache-Control", "no-store"},
                                {"X-Content-Type-Options", "nosniff"},
                                {"Referrer-Policy", "no-referrer"}},
                               "{\"ok\":false,\"error\":\"bad or missing token\"}");
        return;
    }
    if (request.uri == "/api/home/guide" && request.method == "GET") {
        sendHomeGuide(exchange);
        return;
    }
    if (request.uri == "/api/home" && request.method == "GET") {
        sendHomeStatus(exchange);
        return;
    }
    if (request.uri == "/api/home/profile" && request.method == "POST") {
        handleHomeProfilePost(exchange);
        return;
    }
    if (request.uri == "/api/home/layout" && request.method == "POST") {
        handleHomeLayoutPost(exchange);
        return;
    }
    if (request.uri == "/api/home/start-layout" && request.method == "POST") {
        handleHomeStartLayoutPost(exchange);
        return;
    }
    if (request.uri == "/api/home/startup" && request.method == "POST") {
        handleHomeStartupPost(exchange);
        return;
    }
    if (request.uri == "/api/home/enable" && request.method == "POST") {
        handleHomeEnablePost(exchange);
        return;
    }
    if (request.uri == "/api/home/disable" && request.method == "POST") {
        handleHomeDisablePost(exchange);
        return;
    }
    if (request.uri == "/api/home/now" && request.method == "GET") {
        sendHomeNow(exchange);
        return;
    }
    if (request.uri == "/api/home/picture" && request.method == "GET") {
        sendHomePicture(exchange);
        return;
    }
    if (request.uri == "/api/home/discover" && request.method == "GET") {
        sendHomeDiscover(exchange);
        return;
    }
    if (request.uri == "/api/home/image" && request.method == "GET") {
        sendHomeImage(exchange);
        return;
    }
    if (request.method == "GET" &&
        (request.uri == "/api/home/details" ||
         request.uri == "/api/home/search" ||
         request.uri == "/api/home/netflix")) {
        const std::string kind = request.uri == "/api/home/search" ? "search" :
                                 request.uri == "/api/home/netflix" ? "netflix" :
                                 "details";
        sendHomeContent(exchange, kind);
        return;
    }
    if (request.uri == "/api/home/catalogue" && request.method == "GET") {
        std::string view;
        const bool isView = uniqueQueryParameter(request.query, "view", view) &&
                            view == "1";
        sendHomeCatalogue(exchange, isView);
        return;
    }
    if (request.uri == "/api/home/wallpaper" && request.method == "GET") {
        sendHomeWallpaper(exchange);
        return;
    }
    if (request.uri == "/api/home/wallpapers" && request.method == "POST") {
        if (!config_.allowControl && !isLoopbackPeer(request.remoteAddress)) {
            sendSetupError(exchange, 403,
                           say::msg("srv.controlsOff", "controls disabled in config"));
            return;
        }
        handleHomeWallpapersRemove(exchange);
        return;
    }
    if (request.uri == "/api/home/wallpapers/upload" &&
        request.method == "POST") {
        handleHomeWallpaperUpload(exchange);
        return;
    }
    if (request.uri == "/api/home/icon" && request.method == "GET") {
        sendAppIcon(exchange);
        return;
    }
    if (request.uri == "/api/home/recents" && request.method == "GET") {
        sendHomeRecentApps(exchange);
        return;
    }
    if (request.uri == "/api/home/recents" && request.method == "POST") {
        handleHomeRecentAppsPost(exchange);
        return;
    }
    if (request.uri == "/api/home/open" && request.method == "POST") {
        handleHomeOpenPost(exchange);
        return;
    }
    if (request.uri == "/api/home/recents/image" && request.method == "GET") {
        sendHomeRecentAppImage(exchange);
        return;
    }
    if (request.uri == "/api/home/channels" && request.method == "GET") {
        sendHomeChannels(exchange);
        return;
    }
    if (request.uri == "/api/apps" && request.method == "GET") {
        sendApps(exchange);
        return;
    }
    if (request.uri == "/api/apps/catalog" && request.method == "GET") {
        if (!appInstallHostAllowed(request, config_.document)) {
            sendSetupError(exchange, 403, say::msg("srv.install.hostRefused", "this address is not allowed to install apps; add the name to apps.hosts in config.json, or use the TV's IP address"));
            return;
        }
        if (!config_.allowControl) {
            sendSetupError(exchange, 403, say::msg("srv.controlsOff", "controls disabled in config"));
            return;
        }
        sendAppCatalog(exchange);
        return;
    }
    if (request.uri == "/api/apps/install/sideload" && request.method == "POST") {
        setNativeAppSideload(exchange);
        return;
    }
    if (request.uri == "/api/apps/install/repos" && request.method == "POST") {
        setNativePublicAppsRepos(exchange);
        return;
    }
    if (request.uri == "/api/apps/install/upload") {
        handleAppUpload(exchange);
        return;
    }
    if (request.uri == "/api/apps/install/fetch") {
        handleAppInstallFetch(exchange);
        return;
    }
    if (request.uri == "/api/apps/install/url") {
        handleAppInstallUrl(exchange);
        return;
    }
    if (request.uri == "/api/apps/install/confirm") {
        handleAppInstallAction(exchange, "confirm");
        return;
    }
    if (request.uri == "/api/apps/install/cancel") {
        handleAppInstallAction(exchange, "cancel");
        return;
    }
    if (request.uri == "/api/apps/install/status") {
        sendAppInstallStatus(exchange);
        return;
    }
    if (request.uri == "/api/apps/icon" &&
        (request.method == "GET" || request.method == "HEAD")) {
        sendAppIcon(exchange);
        return;
    }
    if (request.uri == "/api/backup/apps" && request.method == "GET") {
        const Json pending = service::readPendingBackupApps(
            dataFilePath(dataRoot_, "restore-apps.json"));
        Json installedIds = Json::array();
        LunaReply appReply;
        if (waitForLuna(luna_, "com.webos.applicationManager/listApps",
                        Json::object(), appReply) && appReply.response["apps"].isArray()) {
            for (const Json &app : appReply.response["apps"].items()) {
                const std::string id = app["id"].string();
                const std::string folder = app["folderPath"].string();
                if (!id.empty() && folder.find("/media/developer/apps/") == 0 &&
                    service::backupAppAllowed(id))
                    installedIds.add(Json::text(id));
            }
        }
        Json result = Json::object();
        result.set("ok", Json::flag(true));
        result.set("apps", service::pendingBackupApps(pending, installedIds));
        sendMetricsJson(exchange, result.dump());
        return;
    }
    if (request.uri == "/api/backup/saved/list" && request.method == "GET") {
        Json backups = Json::array();
        std::string error;
        if (!service::listSavedBackups(dataFilePath(dataRoot_, "backups"),
                                       backups, &error)) {
            if (logger_)
                logger_->warning("saved backups: " + error);
            Json failure = Json::object();
            failure.set("ok", Json::flag(false));
            failure.set("error", Json::text("could not list saved backups"));
            (void)exchange.respond(500,
                                   {{"Content-Type", "application/json"},
                                    {"Cache-Control", "no-store"},
                                    {"X-Content-Type-Options", "nosniff"}},
                                   failure.dump());
            return;
        }
        Json result = Json::object();
        result.set("ok", Json::flag(true));
        result.set("saved", backups);
        sendMetricsJson(exchange, result.dump());
        return;
    }
    if (request.uri == "/api/backup/saved" && request.method == "GET") {
        std::string name;
        std::string contents;
        if (!uniqueQueryParameter(request.query, "name", name) ||
            !service::readSavedBackup(dataFilePath(dataRoot_, "backups"),
                                      name, contents)) {
            Json failure = Json::object();
            failure.set("ok", Json::flag(false));
            failure.set("error", Json::text("no such backup"));
            (void)exchange.respond(404,
                                   {{"Content-Type", "application/json"},
                                    {"Cache-Control", "no-store"},
                                    {"X-Content-Type-Options", "nosniff"}},
                                   failure.dump());
            return;
        }
        sendMetricsJson(exchange, contents);
        return;
    }
    if (request.uri == "/api/backup" && request.method == "GET") {
        sendBackup(exchange);
        return;
    }
    if (request.uri == "/api/backup" && request.method == "POST") {
        // Restoring changes LG's settings, so it is held to what saving the
        // dashboard's settings is: controls on, JSON, and from this origin.
        if (!config_.allowControl) {
            sendSetupError(exchange, 403, say::msg("srv.controlsOff", "controls disabled in config"));
            return;
        }
        if (rejectCrossSitePost(exchange))
            return;
        // The legacy route limits the decoded JavaScript string to 65,536
        // UTF-16 code units. Three UTF-8 bytes per unit covers the largest
        // valid raw UTF-8 representation within that limit.
        if (!exchange.setBodyLimit(65536 * 3)) {
            sendSetupError(exchange, 413, "request body too large", true);
            return;
        }
        std::string body;
        char buffer[4096];
        for (;;) {
            const int count = exchange.readBody(buffer, sizeof(buffer));
            if (count == 0)
                break;
            if (count < 0) {
                const bool tooLarge = exchange.bodyLimitExceeded();
                sendSetupError(exchange, tooLarge ? 413 : 400,
                               tooLarge ? "request body too large" : "invalid request body",
                               tooLarge);
                return;
            }
            body.append(buffer, static_cast<size_t>(count));
        }
        if (exceedsUtf16CodeUnitLimit(body, 65536)) {
            sendSetupError(exchange, 413, "request body too large", true);
            return;
        }
        // A body of `null` is JSON; it is not a backup, which the restore says.
        const size_t jsonStart = body.find_first_not_of(" \t\r\n");
        const size_t jsonEnd = body.find_last_not_of(" \t\r\n");
        const bool isNull = jsonStart != std::string::npos &&
                            body.compare(jsonStart, jsonEnd - jsonStart + 1, "null") == 0;
        if (!Json::parse(body).valid() && !isNull) {
            sendSetupError(exchange, 400, "malformed JSON");
            return;
        }
        Json installerSnapshot;
        bool installerSnapshotReady = false;
        {
            std::lock_guard<std::mutex> lock(installJobMutex_);
            installerSnapshot = appInstallSnapshot_;
            installerSnapshotReady = appInstallSnapshotReady_;
        }
        if (installerSnapshotReady && appInstallSnapshotWorking(installerSnapshot)) {
            sendInstallError(exchange, 409,
                             say::msg("srv.install.busy.restart",
                                      "an install is in progress; restart when it has finished"));
            return;
        }
        handleBackupRestore(exchange, body);
        return;
    }
    if (request.uri == "/api/caps") {
        const Json caps = nativeCapabilities(config_, version_, dataRoot_, request.remoteAddress,
                                             localIpv4Address());
        (void)exchange.respond(200,
                               {{"Content-Type", "application/json"},
                                {"Cache-Control", "no-store"},
                                {"X-Content-Type-Options", "nosniff"},
                                {"Referrer-Policy", "no-referrer"}},
                               caps.dump());
        return;
    }
    if (request.uri == "/api/servicemenu" &&
        (request.method == "GET" || request.method == "HEAD")) {
        sendServiceMenuState(exchange);
        return;
    }
    if (request.uri == "/api/update") {
        if (request.method == "GET" || request.method == "HEAD") {
            sendMetricsJson(exchange, serverStatus().dump());
        } else {
            sendSetupError(exchange, 405, "GET only");
        }
        return;
    }
    if (request.uri == "/api/tvapp") {
        if (request.method == "GET" || request.method == "HEAD") {
            Json status = runTvApp("status");
            if (status["ok"].boolean()) status.set("writable", Json::flag(config_.allowControl));
            sendMetricsJson(exchange, status.dump());
        } else {
            sendSetupError(exchange, 405, "GET only");
        }
        return;
    }
    if (request.uri == "/api/screensaver" && screensaverManager_) {
        if (request.method == "GET" || request.method == "HEAD") {
            sendScreensaverList(exchange);
        } else {
            sendSetupError(exchange, 405, "GET only");
        }
        return;
    }
    if (request.uri == "/api/oledcare" &&
        (request.method == "GET" || request.method == "HEAD")) {
        sendOledCare(exchange);
        return;
    }
    if (request.uri == "/api/privacy") {
        sendPrivacyProjection(exchange);
        return;
    }
    if (request.uri == "/api/services" &&
        (request.method == "GET" || request.method == "HEAD")) {
        sendBackgroundServices(exchange);
        return;
    }
    if (request.uri == "/api/services/toggle" && request.method == "POST") {
        setBackgroundService(exchange);
        return;
    }
    if (request.uri == "/api/ssh") {
        setSshKeys(exchange);
        return;
    }
    if (request.uri == "/api/prometheus/metrics" &&
        (request.method == "GET" || request.method == "HEAD")) {
        const Json prometheusConfig = config_.document["prometheus"];
        if (!prometheusConfig["enabled"].isBoolean() ||
            !prometheusConfig["enabled"].boolean()) {
            (void)exchange.respond(404,
                                   {{"Content-Type", "application/json"},
                                    {"Cache-Control", "no-store"},
                                    {"X-Content-Type-Options", "nosniff"}},
                                   "{\"ok\":false,\"error\":\"not found\"}");
            return;
        }
        const Json system = withStatsWindows(service::collectSystem(systemRoot_, dataRoot_), "stats");
        std::string response;
        std::string error;
        if (!proxy_.requestNodeStats(system, false, response, error, true)) {
            (void)exchange.respond(502,
                                   {{"Content-Type", "application/json; charset=utf-8"},
                                    {"Cache-Control", "no-store"},
                                    {"X-Content-Type-Options", "nosniff"}},
                                   "{\"ok\":false,\"error\":\"native service unavailable\"}");
            return;
        }
        const Json enriched = Json::parse(response);
        if (!enriched["stats"].isObject()) {
            (void)exchange.respond(502,
                                   {{"Content-Type", "application/json; charset=utf-8"},
                                    {"Cache-Control", "no-store"},
                                    {"X-Content-Type-Options", "nosniff"}},
                                   "{\"ok\":false,\"error\":\"invalid telemetry response\"}");
            return;
        }
        const std::string rendered = service::renderPrometheus(
            enriched["stats"], version_, enriched["syslog"], enriched["ntp"],
            enriched["watch"]);
        (void)exchange.respond(200,
                               {{"Content-Type", "text/plain; version=0.0.4; charset=utf-8"},
                                {"Cache-Control", "no-store"},
                                {"X-Content-Type-Options", "nosniff"}},
                               rendered);
        return;
    }
    if (request.uri == "/api/stats" &&
        (request.method == "GET" || request.method == "HEAD")) {
        std::string fast;
        const char *nativeStatsEnabled = std::getenv("TVWEB_NATIVE_STATS");
        const bool statsNative = nativeStatsEnabled && std::string(nativeStatsEnabled) == "1";
        const bool wantsEarly = queryParameter(request.query, "fast", fast) && fast == "1";
        if (wantsEarly && !statsNative) {
            (void)proxy_.forward(exchange);
            return;
        }

        Json system = service::collectSystem(systemRoot_, dataRoot_);
        // A fixed snapshot in place of the procfs read, for the comparison with Node.
        if (const char *fixture = std::getenv("TVWEB_STATS_SYSTEM_JSON")) {
            std::ifstream file(fixture);
            std::string contents((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            const Json given = Json::parse(contents);
            if (given.isObject()) system = given;
        }
        if (!system["ok"].isBoolean() || !system["ok"].boolean()) {
            sendMetricsJson(exchange, system.dump());
            return;
        }
        std::string response;
        std::string error;
        std::string with;
        const bool withSettings = queryParameter(request.query, "with", with) &&
                                  with == "settings";
        std::string reader = "stats";
        std::string requestedReader;
        if (queryParameter(request.query, "reader", requestedReader) &&
            requestedReader == "publish") {
            if (!isLoopbackPeer(request.remoteAddress)) {
                (void)exchange.respond(403,
                                       {{"Content-Type", "application/json"},
                                        {"Cache-Control", "no-store"},
                                        {"X-Content-Type-Options", "nosniff"}},
                                       "{\"ok\":false,\"error\":\"publish reader is TV-local\"}");
                return;
            }
            reader = "publish";
        }
        const Json systemWithWindows = withStatsWindows(system, reader);
        // Built here when switched on (TVWEB_NATIVE_STATS=1): the dashboard's whole
        // picture of the TV without Node, held to the answers recorded from the legacy server by the stats goldens.
        if (statsNative) {
            if (wantsEarly) {
                const Json early = statsCollector()->early(systemWithWindows);
                if (early.valid()) {
                    sendMetricsJson(exchange, early.dump());
                    return;
                }
            }
            Json stats = statsCollector()->collect(systemWithWindows);
            // PicCap's row, where the app is installed and answering.
            const Json piccapCapture = piccapStatus()->read();
            if (piccapCapture.isObject()) {
                Json copy = Json::object();
                for (const std::string &key : stats.keys()) copy.set(key, stats[key]);
                copy.set("piccapCapture", piccapCapture);
                stats = copy;
            }
            if (withSettings && stats["ok"].boolean()) {
                const Json settingsData = collectLgSettingsData({"sound", "hdmi", "devices"});
                const Json settingsResponse = service::lgSettingsResponse(
                    {"sound", "hdmi", "devices"}, settingsData["settings"], settingsData["descriptions"],
                    settingsData["dimensions"], lgSettingsInputNames());
                Json copy = Json::object();
                for (const std::string &key : stats.keys()) copy.set(key, stats[key]);
                copy.set("lgSettings", settingsResponse["rows"]);
                stats = copy;
            }
            sendMetricsJson(exchange, stats.dump());
            return;
        }
        if (!proxy_.requestNodeStats(systemWithWindows, withSettings, response, error)) {
            (void)exchange.respond(502,
                                   {{"Content-Type", "application/json; charset=utf-8"},
                                    {"Cache-Control", "no-store"},
                                    {"X-Content-Type-Options", "nosniff"}},
                                   "{\"ok\":false,\"error\":\"native service unavailable\"}");
            return;
        }
        sendMetricsJson(exchange, response);
        return;
    }
    if (request.uri == "/api/history" && request.method == "POST" && historyRecorder_) {
        if (!config_.allowControl) {
            sendSetupError(exchange, 403, "controls disabled in config");
            return;
        }
        if (!sameOriginRequest(request)) {
            sendSetupError(exchange, 403, "cross-origin request refused");
            return;
        }
        Json payload;
        if (!readSetupPayload(exchange, payload)) return;
        const std::string action = payload["action"].string();
        std::string error;
        bool ok = false;
        if (action == "enabled" && payload["enabled"].isBoolean())
            ok = historyRecorder_->setEnabled(payload["enabled"].boolean(), &error);
        else if (action == "clear")
            ok = historyRecorder_->clear(&error);
        else {
            sendSetupError(exchange, 400, "invalid history action");
            return;
        }
        Json response = historyRecorder_->status();
        response.set("ok", Json::flag(ok));
        if (!ok) response.set("error", Json::text(error.empty() ? "history operation failed" : error));
        (void)exchange.respond(ok ? 200 : 500,
                               {{"Content-Type", "application/json"},
                                {"Cache-Control", "no-store"},
                                {"X-Content-Type-Options", "nosniff"}},
                               response.dump());
        return;
    }

    if (request.uri == "/api/history" &&
        (request.method == "GET" || request.method == "HEAD")) {
        long days = 35;
        std::string rawDays;
        if (queryParameter(request.query, "days", rawDays) && !rawDays.empty()) {
            char *end = nullptr;
            errno = 0;
            const long parsed = std::strtol(rawDays.c_str(), &end, 10);
            if (end != rawDays.c_str() && errno != ERANGE && parsed >= 1 && parsed <= 400)
                days = parsed;
        }
        const int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::system_clock::now().time_since_epoch())
                                  .count();
        const int64_t fromMs = nowMs - static_cast<int64_t>(days) * 86400000LL;
        Json overlay;
        std::string error;
        if (historyRecorder_) {
            overlay = historyRecorder_->overlay(fromMs, nowMs);
            // Node still resolves installed-app display names for its HA and
            // dashboard compatibility consumers. The daemon remains the only
            // source of recorded hours and current playback state.
            Json titleOverlay;
            std::string titleError;
            if (proxy_.requestNodeHistoryOverlay(fromMs, nowMs, overlay["ids"],
                                                titleOverlay, titleError) &&
                titleOverlay["titles"].isObject()) {
                Json titles = overlay["titles"];
                for (const std::string &key : titleOverlay["titles"].keys()) {
                    if (titleOverlay["titles"][key].isString())
                        titles.set(key, titleOverlay["titles"][key]);
                }
                overlay.set("titles", titles);
            }
        } else if (!proxy_.requestNodeHistoryOverlay(fromMs, nowMs, Json(), overlay, error)) {
            Json failure = Json::object();
            failure.set("ok", Json::flag(false));
            failure.set("error", Json::text("native history is unavailable"));
            (void)exchange.respond(502,
                                   {{"Content-Type", "application/json"},
                                    {"Cache-Control", "no-store"},
                                    {"X-Content-Type-Options", "nosniff"}},
                                   failure.dump());
            return;
        }
        Json response;
        if (!service::queryHistory(dataRoot_ + "/history", fromMs, nowMs,
                                   overlay, response, error)) {
            Json failure = Json::object();
            failure.set("ok", Json::flag(false));
            failure.set("error", Json::text(error));
            (void)exchange.respond(500,
                                   {{"Content-Type", "application/json"},
                                    {"Cache-Control", "no-store"},
                                    {"X-Content-Type-Options", "nosniff"}},
                                   failure.dump());
            return;
        }
        sendMetricsJson(exchange, response.dump());
        return;
    }
    if (request.uri == "/api/screentime") {
        if (request.method == "POST" && !config_.allowControl) {
            sendSetupError(exchange, 403, say::msg("srv.controlsOff", "controls disabled in config"));
            return;
        }
        if (request.method == "POST" && !setupJsonContentType(request)) {
            sendSetupError(exchange, 415, "Content-Type must be application/json");
            return;
        }
        if (request.method == "POST" && rejectCrossSitePost(exchange))
            return;
        const std::string action = request.method == "POST" ? "update" : "status";
        Json settings = Json::object();
        if (action == "update" && config_.allowControl) {
            if (!exchange.setBodyLimit(32768)) {
                sendSetupError(exchange, 413, "request body too large", true);
                return;
            }
            std::string body;
            char buffer[2048];
            for (;;) {
                const int count = exchange.readBody(buffer, sizeof(buffer));
                if (count == 0) break;
                if (count < 0) {
                    sendSetupError(exchange,
                                   exchange.bodyLimitExceeded() ? 413 : 400,
                                   exchange.bodyLimitExceeded() ? "request body too large"
                                                                : "invalid request body",
                                   true);
                    return;
                }
                body.append(buffer, static_cast<size_t>(count));
            }
            if (exceedsUtf16CodeUnitLimit(body, 8192)) {
                sendSetupError(exchange, 413, "request body too large", true);
                return;
            }
            settings = Json::parse(body);
            if (!settings.isObject() && !settings.isArray()) {
                sendSetupError(exchange, 400, "malformed JSON");
                return;
            }
            Json validatedSettings;
            std::string validationError;
            if (!service::validateScreenTimePatch(settings, validatedSettings,
                                                  validationError)) {
                Json failure = Json::object();
                failure.set("ok", Json::flag(false));
                failure.set("error", Json::text(validationError));
                (void)exchange.respond(400,
                                       {{"Content-Type", "application/json"},
                                        {"Cache-Control", "no-store"},
                                        {"X-Content-Type-Options", "nosniff"}},
                                       failure.dump());
                return;
            }
            settings = validatedSettings;
        }
        if (action == "status") {
            Json response;
            std::string error;
            if (!collectNativeScreenTimeStatus(response, error)) {
                Json failure = Json::object();
                failure.set("ok", Json::flag(false));
                failure.set("error", Json::text(error));
                (void)exchange.respond(500,
                                       {{"Content-Type", "application/json"},
                                        {"Cache-Control", "no-store"},
                                        {"X-Content-Type-Options", "nosniff"}},
                                       failure.dump());
                return;
            }
            (void)exchange.respond(200,
                                   {{"Content-Type", "application/json"},
                                    {"Cache-Control", "no-store"},
                                    {"X-Content-Type-Options", "nosniff"}},
                                   response.dump());
            return;
        }
        Json savedSettings;
        std::string error;
        if (!ConfigStore(configPath_).patchObjectSection("screenTime", settings,
                                                          &savedSettings, &error)) {
            Json failure = Json::object();
            failure.set("ok", Json::flag(false));
            failure.set("error", Json::text("could not save setting: " + error));
            (void)exchange.respond(500,
                                   {{"Content-Type", "application/json"},
                                    {"Cache-Control", "no-store"},
                                    {"X-Content-Type-Options", "nosniff"}},
                                   failure.dump());
            return;
        }
        {
            std::lock_guard<std::mutex> lock(screenTimeSettingsMutex_);
            screenTimeSettings_ = savedSettings;
        }
        const std::string root = systemRoot_.empty() ? "/" : systemRoot_;
        const std::string homePath = root[root.size() - 1] == '/'
                                         ? root + "usr/palm/applications/com.webos.app.home"
                                         : root + "/usr/palm/applications/com.webos.app.home";
        Json configDocument = Json::object();
        configDocument.set("screenTime", savedSettings);
        const Json effectiveSettings = service::screenTimeSettings(
            configDocument, config_.allowPower, ::access(homePath.c_str(), F_OK) == 0);
        if (screenTimeEnforcer_) {
            std::string applyError;
            if (!screenTimeEnforcer_->apply(effectiveSettings, &applyError)) {
                Json failure = Json::object();
                failure.set("ok", Json::flag(false));
                failure.set("error", Json::text(applyError.empty()
                                                    ? "could not apply screen-time settings"
                                                    : applyError));
                (void)exchange.respond(500,
                                       {{"Content-Type", "application/json"},
                                        {"Cache-Control", "no-store"},
                                        {"X-Content-Type-Options", "nosniff"}},
                                       failure.dump());
                return;
            }
        }
        std::string reloadError;
        if (!proxy_.requestNodeConfigReload(reloadError) && logger_)
            logger_->warning("screentime: saved settings but could not reload Node config: " +
                             reloadError);
        Json response;
        if (!collectNativeScreenTimeStatus(response, error)) {
            Json failure = Json::object();
            failure.set("ok", Json::flag(false));
            failure.set("error", Json::text(error));
            (void)exchange.respond(500,
                                   {{"Content-Type", "application/json"},
                                    {"Cache-Control", "no-store"},
                                    {"X-Content-Type-Options", "nosniff"}},
                                   failure.dump());
            return;
        }
        (void)exchange.respond(200,
                               {{"Content-Type", "application/json"},
                                {"Cache-Control", "no-store"},
                                {"X-Content-Type-Options", "nosniff"},
                                {"Referrer-Policy", "no-referrer"}},
                               response.dump());
        return;
    }
    if (request.uri == "/api/preload/toggle" && request.method == "POST") {
        // Match the Node controls-off response, including its localization,
        // without reading or applying a mutation.
        if (!config_.allowControl) {
            sendSetupError(exchange, 403, say::msg("srv.controlsOff", "controls disabled in config"));
            return;
        }
        if (rejectCrossSitePost(exchange))
            return;
        setPreloadToggle(exchange);
        return;
    }
    if (request.uri == "/api/setup" && request.method == "GET") {
        (void)setupRead(exchange, config_, dataRoot_, luna_, proxy_, activeHandoffUrl());
        return;
    }
    if (request.uri == "/api/setup" && request.method == "POST") {
        if (!setupJsonContentType(request)) {
            sendSetupError(exchange, 415, "Content-Type must be application/json");
            return;
        }
        Json payload;
        if (!readSetupPayload(exchange, payload))
            return;
        const std::string action = payload["action"].string();
        if (action == "network") {
            (void)setNetworkAccess(exchange, config_, configPath_, proxy_, &payload);
        } else if (action == "alwaysReady") {
            Json actionPayload = payload;
            actionPayload.set("on", Json::flag(setupTruthy(payload["on"])));
            (void)setAlwaysReady(exchange, config_, luna_, &actionPayload);
        } else if (action == "done") {
            (void)finishSetup(exchange, config_, dataRoot_, &payload);
        } else if (action == "handoff") {
            (void)setupHandoffAction(exchange, "handoff", &payload);
        } else if (action == "handoffStop") {
            (void)setupHandoffAction(exchange, "handoffStop", &payload);
        } else {
            sendSetupError(exchange, 400, "unknown action");
        }
        return;
    }
    if (request.uri == "/api/processes" &&
        (request.method == "GET" || request.method == "HEAD")) {
        sendProcessMetrics(exchange, allProcessesRequested(request.query), procRoot_);
        return;
    }
    if (request.uri == "/api/logs" &&
        (request.method == "GET" || request.method == "HEAD")) {
        sendLogs(exchange);
        return;
    }
    if (request.uri == "/api/hdmi" &&
        (request.method == "GET" || request.method == "HEAD")) {
        sendHdmiInputs(exchange);
        return;
    }
    if (request.uri == "/api/game/fps" &&
        (request.method == "GET" || request.method == "HEAD")) {
        sendGameFrameRate(exchange);
        return;
    }
    if (request.uri == "/api/game" &&
        (request.method == "GET" || request.method == "HEAD")) {
        sendGameSettings(exchange);
        return;
    }
    if (request.uri == "/api/lgsettings" &&
        (request.method == "GET" || request.method == "HEAD")) {
        sendLgSettings(exchange);
        return;
    }
    if (request.uri == "/api/cpu" &&
        (request.method == "GET" || request.method == "HEAD")) {
        sendMetricsJson(exchange, collectCpuMetrics());
        return;
    }
    if (request.uri == "/api/settings" && request.method == "GET") {
        sendDashboardSettings(exchange);
        return;
    }
    if (request.uri == "/api/settings" && request.method == "POST") {
        setDashboardSettings(exchange);
        return;
    }
    if (request.uri == "/api/backup/apps/clear" && request.method == "POST") {
        if (!config_.allowControl) {
            sendSetupError(exchange, 403, say::msg("srv.controlsOff", "controls disabled in config"));
            return;
        }
        const std::string path = dataFilePath(dataRoot_, "restore-apps.json");
        std::string error;
        bool cleared = false;
        {
            std::lock_guard<std::mutex> lock(pendingBackupAppsMutex_);
            cleared = service::clearPendingBackupApps(path, &error);
        }
        Json result = Json::object();
        result.set("ok", Json::flag(cleared));
        if (cleared) {
            result.set("apps", Json::array());
        } else {
            result.set("error", Json::text("could not clear pending app list"));
            if (logger_)
                logger_->warning("pending backup apps: " + error);
        }
        (void)exchange.respond(cleared ? 200 : 500,
                               {{"Content-Type", "application/json; charset=utf-8"},
                                {"Cache-Control", "no-store"},
                                {"X-Content-Type-Options", "nosniff"}},
                               result.dump());
        return;
    }
    if (rejectCrossSitePost(exchange))
        return;
    if (request.method == "POST" && request.uri == "/api/apps/uninstall") {
        setNativeAppUninstall(exchange);
        return;
    }
    if (request.method == "POST" &&
        (request.uri == "/api/apps/hide" || request.uri == "/api/apps/unhide" ||
         request.uri == "/api/apps/unhide-all" ||
         request.uri == "/api/apps/tile-hiding")) {
        setNativeAppTileOperation(exchange);
        return;
    }
    if (request.method == "POST" &&
        (request.uri == "/api/apps/add-page" ||
        request.uri == "/api/apps/remove-page" ||
        request.uri == "/api/apps/edit-page" ||
        request.uri == "/api/apps/rename")) {
        handleSavedPage(exchange);
        return;
    }
    if (request.uri == "/api/remotekeys" && request.method == "POST") {
        setRemoteKey(exchange);
        return;
    }
    if (request.uri == "/api/adblock/custom" && request.method == "POST") {
        setAdBlockCustom(exchange);
        return;
    }
    if (request.uri == "/api/quickaccess" && request.method == "POST") {
        setQuickAccess(exchange);
        return;
    }
    if (request.uri == "/api/control" && request.method == "POST") {
        setControl(exchange);
        return;
    }
    if (request.uri == "/api/home/beat" && request.method == "POST") {
        sendHomeBeat(exchange);
        return;
    }
    if (request.uri == "/api/homescreen" && request.method == "POST") {
        setHomeScreen(exchange);
        return;
    }
    if (request.uri == "/api/setup/network") {
        (void)setNetworkAccess(exchange, config_, configPath_, proxy_);
        return;
    }
    if (request.uri == "/api/setup/done") {
        (void)finishSetup(exchange, config_, dataRoot_);
        return;
    }
    if (request.uri == "/api/setup/always-ready") {
        (void)setAlwaysReady(exchange, config_, luna_);
        return;
    }
    if (request.uri == "/api/setup/handoff") {
        (void)setupHandoffAction(exchange, "handoff");
        return;
    }
    if (request.uri == "/api/setup/handoff-stop") {
        (void)setupHandoffAction(exchange, "handoffStop");
        return;
    }
    if (staticAssets_.handle(exchange))
        return;
    // Read-only routes answered here. Node replied to every method with the same
    // body; no client sends another, so a wrong method now gets a plain 405.
    static const char *const kReadOnlyRoutes[] = {
        "/api/cpu", "/api/processes", "/api/hdmi", "/api/logs", "/api/stats",
        "/api/game", "/api/game/fps", "/api/lgsettings", "/api/oledcare",
        "/api/servicemenu",
    };
    for (const char *route : kReadOnlyRoutes) {
        if (request.uri == route) {
            sendSetupError(exchange, 405, "GET only");
            return;
        }
    }
    // Every route in the manifest is answered above, so a path that gets here is
    // one nothing serves. Node answers it with a 404 too; with no Node to ask the
    // daemon gives that 404 itself.
    if (nodeFreeStart()) {
        sendSetupError(exchange, 404, "not found");
        return;
    }
    (void)proxy_.forward(exchange);
}
} // namespace daemon
} // namespace nativetv
