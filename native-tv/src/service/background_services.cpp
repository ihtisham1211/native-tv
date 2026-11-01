#include "service/background_services.hpp"
#include "domain/say.hpp"
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

namespace nativetv {
namespace service {
namespace {
struct CatalogItem {
    const char *id;
    const char *bin;
    const char *exe;
    const char *titleKey; // each text sits beside its say::msg() key, for scripts/check-strings.py
    const char *title;
    const char *unit;
    const char *upstart;
    const char *badgeKey; // null for a size such as "2.1 MB RAM", which reads the same in every language
    const char *badge;
    const char *descriptionKey;
    const char *description;
    bool hidden;
};

const CatalogItem kCatalog[] = {
    {"mycar", "com.webos.service.mycar", nullptr,
     "srv.service.mycar", "Car-to-Home Telematics", "com.webos.service.mycar.service", nullptr,
     nullptr, "2.1 MB RAM",
     "srv.service.mycar.desc",
     "Hyundai/Kia Bluelink vehicle status listener in Home Dashboard. Unnecessary unless linking a connected vehicle.", false},
    {"camera", "com.webos.service.camera2", nullptr,
     "srv.service.camera", "USB Camera Listener", "com.webos.service.camera.service", nullptr,
     nullptr, "2.5 MB RAM",
     "srv.service.camera.desc",
     "Background USB webcam detection daemon. Unnecessary unless a USB webcam is physically attached.", false},
    {"uploadd", "uploadd", nullptr,
     "srv.service.uploadd", "Telemetry & Diagnostics Uploader", "uploadd.service", "uploadd",
     "srv.service.badge.telemetry", "Telemetry",
     "srv.service.uploadd.desc",
     "Uploads crash logs, diagnostic traces, and usage telemetry to LG servers.", false},
    {"rdxd", "rdxd", nullptr,
     "srv.service.rdxd", "Remote Diagnostics Daemon", "rdxd.service", "rdxd",
     "srv.service.badge.flashWrites", "Flash writes",
     "srv.service.rdxd.desc",
     "Continuously logs diagnostic traces, core dumps, and event data to internal flash memory.", false},
    {"crashreportd", "crashreportd", nullptr,
     "srv.service.crashreportd", "Jira Crash Reporter", nullptr, "crashreportd",
     "srv.service.badge.telemetry", "Telemetry",
     "srv.service.crashreportd.desc",
     "Background daemon on older webOS versions that generates automated crash reports.", false},
    {"contentminer", "contentminer", nullptr,
     "srv.service.contentminer", "ACR Content Miner", "contentminer.service", nullptr,
     nullptr, "4.2 MB RAM",
     "srv.service.contentminer.desc",
     "Scans and indexes on-screen audio/video content for advertising and viewing habit telemetry.", false},
    {"nudge", "nudge", nullptr,
     "srv.service.nudge", "LG Promotions & Tips Popups", "nudge.service", nullptr,
     nullptr, "3.1 MB RAM",
     "srv.service.nudge.desc",
     "Pushes marketing notifications, promotional popups, and feature tips to the TV interface.", false},
    {"remotelogger", "remotelogger", nullptr,
     "srv.service.remotelogger", "Remote Logging Daemon", "remotelogger.service", "remotelogger",
     nullptr, "1.7 MB RAM",
     "srv.service.remotelogger.desc",
     "Ships system log messages to remote servers.", false},
    {"pushclient", "com.webos.service.pushclient", nullptr,
     "srv.service.pushclient", "LG Push Messages", "com.webos.service.pushclient.service", nullptr,
     nullptr, "4.9 MB RAM",
     "srv.service.pushclient.desc",
     "Receives messages LG sends to the TV, such as promotions and sports alerts.", false},
    {"ocp", "ocpservice", nullptr,
     "srv.service.ocp", "LG Cloud Link", "ocp.service", nullptr,
     nullptr, "6.4 MB RAM",
     "srv.service.ocp.desc",
     "Keeps the TV linked to LG's ThinQ cloud. Unnecessary unless the TV is controlled from the LG ThinQ app.", false},
    {"livepick", "livepick-plus", "/usr/sbin/livepick-plus",
     "srv.service.livepick", "Live Plus", nullptr, nullptr,
     nullptr, "4.5 MB RAM",
     "srv.service.livepick.desc",
     "Works with screen recognition (ACR) to show interactive offers about what is on screen.", false},
    {"admanager", "admanager", "/usr/sbin/admanager",
     "srv.service.admanager", "LG Advertising Service", nullptr, nullptr,
     nullptr, "4.3 MB RAM",
     "srv.service.admanager.desc",
     "Fetches the ads shown on the TV and keeps its advertising ID.", false},
    {"sportsalarm", "sportsalarm", "/usr/sbin/sportsalarm",
     "srv.service.sportsalarm", "Sports Alerts", nullptr, nullptr,
     nullptr, "3.8 MB RAM",
     "srv.service.sportsalarm.desc",
     "Pops up scores for the teams followed on the TV.", false},
    {"iotclient", "iot-client", "/usr/palm/services/com.webos.service.iotclient/iot-client",
     "srv.service.iotclient", "ThinQ Smart Home", nullptr, nullptr,
     nullptr, "9.3 MB RAM",
     "srv.service.iotclient.desc",
     "Controls LG ThinQ appliances from the TV's Home Hub. Unnecessary without ThinQ appliances.", false},
    {"buddyconnector", "com.webos.service.buddyconnector", "/usr/palm/services/com.webos.service.buddyconnector/com.webos.service.buddyconnector",
     "srv.service.buddyconnector", "ThinQ Device Pairing", nullptr, nullptr,
     nullptr, "7.0 MB RAM",
     "srv.service.buddyconnector.desc",
     "Pairs the TV with other LG ThinQ devices.", false},
    {"thinqai", "lg.thinqai.adapter", "/usr/sbin/lg.thinqai.adapter",
     "srv.service.thinqai", "ThinQ AI", nullptr, nullptr,
     nullptr, "3.0 MB RAM",
     "srv.service.thinqai.desc",
     "Connects the TV to LG's ThinQ AI cloud service.", false},
    {"familycare", "com.webos.service.familycare", "/usr/palm/services/com.webos.service.familycare/index.js",
     "srv.service.familycare", "Family Care", nullptr, nullptr,
     nullptr, "4.6 MB RAM",
     "srv.service.familycare.desc",
     "Tracks screen time for LG's Family Care limits. Unnecessary unless Family Care is set up.", false},
    {"voiceconductor", "voiceconductor", nullptr,
     "srv.service.voiceconductor", "LG Voice Assistant", "voiceconductor.service", nullptr,
     nullptr, "",
     "srv.service.voiceconductor.desc",
     "LG's voice assistant: the AI button, voice search and the microphone.", true}
};

std::string rooted(const std::string &root, const std::string &absolute) {
    if (root.empty() || root == "/") return absolute;
    size_t end = root.size();
    while (end > 1 && root[end - 1] == '/') --end;
    return root.substr(0, end) + absolute;
}

bool exists(const std::string &path) {
    return access(path.c_str(), F_OK) == 0;
}

std::string dataPath(const std::string &dataRoot, const std::string &name) {
    return (dataRoot.empty() ? "/var/lib/tvweb" : dataRoot) + "/" + name;
}

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
        if (error) *error = "could not inspect " + path;
        return false;
    }
    const std::string parent = directoryOf(path);
    if (parent != path && !ensureDirectory(parent, error)) return false;
    if (mkdir(path.c_str(), 0755) == 0 || errno == EEXIST) return true;
    if (error) *error = "could not create " + path;
    return false;
}

bool writeAtomic(const std::string &path, const std::string &contents, mode_t mode,
                 std::string *error) {
    if (!ensureDirectory(directoryOf(path), error)) return false;
    std::string pattern = path + ".tmp.XXXXXX";
    std::vector<char> temporaryName(pattern.begin(), pattern.end());
    temporaryName.push_back('\0');
    const int fd = mkstemp(temporaryName.data());
    if (fd < 0) {
        if (error) *error = "could not create temporary file for " + path;
        return false;
    }
    const std::string temporary(temporaryName.data());
    bool ok = fchmod(fd, mode) == 0;
    size_t offset = 0;
    while (ok && offset < contents.size()) {
        const ssize_t count = write(fd, contents.data() + offset, contents.size() - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) {
            ok = false;
            break;
        }
        offset += static_cast<size_t>(count);
    }
    if (ok && fsync(fd) != 0) ok = false;
    if (close(fd) != 0) ok = false;
    if (!ok || rename(temporary.c_str(), path.c_str()) != 0) {
        const int saved = errno;
        unlink(temporary.c_str());
        if (error) *error = "could not atomically write " + path + ": " +
                            std::strerror(saved);
        return false;
    }
    const int directory = open(directoryOf(path).c_str(), O_RDONLY | O_DIRECTORY);
    if (directory >= 0) {
        (void)fsync(directory);
        close(directory);
    }
    return true;
}

bool readText(const std::string &path, std::string &contents) {
    std::ifstream input(path.c_str(), std::ios::in | std::ios::binary);
    if (!input) return false;
    std::ostringstream buffer;
    buffer << input.rdbuf();
    if (input.bad()) return false;
    contents = buffer.str();
    return true;
}

bool systemctlAvailable(const std::string &root) {
    return exists(rooted(root, "/bin/systemctl")) ||
           exists(rooted(root, "/usr/bin/systemctl"));
}

std::string systemctlExecutable(const std::string &root) {
    const std::string bin = rooted(root, "/bin/systemctl");
    if (exists(bin)) return bin;
    const std::string usrBin = rooted(root, "/usr/bin/systemctl");
    return exists(usrBin) ? usrBin : std::string();
}

bool initctlAvailable(const std::string &root) {
    return exists(rooted(root, "/sbin/initctl")) ||
           exists(rooted(root, "/usr/sbin/initctl"));
}

std::string initctlExecutable(const std::string &root) {
    const std::string sbin = rooted(root, "/sbin/initctl");
    if (exists(sbin)) return sbin;
    const std::string usrSbin = rooted(root, "/usr/sbin/initctl");
    return exists(usrSbin) ? usrSbin : std::string();
}

bool unitExists(const std::string &root, const char *unit) {
    if (!unit) return false;
    const std::string name(unit);
    const std::string paths[] = {
        "/etc/systemd/system/" + name,
        "/lib/systemd/system/" + name,
        "/usr/lib/systemd/system/" + name
    };
    for (const std::string &path : paths)
        if (exists(rooted(root, path))) return true;
    return false;
}

bool serviceExists(const CatalogItem &item, const std::string &root,
                   bool hasSystemctl, bool hasInitctl) {
    if (item.exe) return exists(rooted(root, item.exe));
    if (hasSystemctl && unitExists(root, item.unit)) return true;
    return hasInitctl && item.upstart &&
        exists(rooted(root, std::string("/etc/init/") + item.upstart + ".conf"));
}

std::vector<std::string> itemPrograms(const CatalogItem &item,
                                      const std::string &root) {
    static const char *shared[] = {
        "/bin/sh", "/bin/bash", "/bin/busybox", "/bin/mkdir",
        "/bin/systemctl", "/usr/bin/luna-send", "/usr/bin/luna-send-pub",
        "/usr/bin/iotjs", "/usr/bin/node"
    };
    std::vector<std::string> result;
    const auto add = [&](const std::string &path) {
        if (path.empty() || !exists(rooted(root, path))) return;
        for (const char *reserved : shared)
            if (path == reserved) return;
        if (std::find(result.begin(), result.end(), path) == result.end())
            result.push_back(path);
    };
    if (item.unit) {
        const char *directories[] = {
            "/etc/systemd/system/", "/lib/systemd/system/",
            "/usr/lib/systemd/system/"
        };
        for (const char *directory : directories) {
            std::string contents;
            const std::string unitPath = rooted(root, std::string(directory) + item.unit);
            if (!readText(unitPath, contents)) continue;
            std::istringstream lines(contents);
            std::string line;
            while (std::getline(lines, line)) {
                const std::string prefix = "ExecStart=";
                if (line.compare(0, prefix.size(), prefix) != 0) continue;
                size_t start = prefix.size();
                while (start < line.size() &&
                       std::string("-@+!:").find(line[start]) != std::string::npos)
                    ++start;
                const size_t end = line.find_first_of(" \t", start);
                if (start < line.size()) add(line.substr(start, end - start));
            }
            break;
        }
    }
    if (item.bin) add(std::string("/usr/sbin/") + item.bin);
    if (item.exe) add(item.exe);
    return result;
}

const CatalogItem *findItem(const std::string &id) {
    for (const CatalogItem &item : kCatalog)
        if (id == item.id) return &item;
    return nullptr;
}

bool numeric(const std::string &name) {
    return !name.empty() && std::all_of(name.begin(), name.end(),
        [](unsigned char c) { return std::isdigit(c) != 0; });
}

std::string basename(const std::string &path) {
    const size_t slash = path.find_last_of('/');
    return path.substr(slash == std::string::npos ? 0 : slash + 1);
}

std::string readNullSeparated(const std::string &path, std::vector<std::string> &values) {
    std::ifstream input(path.c_str(), std::ios::in | std::ios::binary);
    if (!input) return std::string();
    std::string contents((std::istreambuf_iterator<char>(input)),
                         std::istreambuf_iterator<char>());
    size_t start = 0;
    while (start < contents.size()) {
        const size_t end = contents.find('\0', start);
        values.push_back(contents.substr(start,
            end == std::string::npos ? std::string::npos : end - start));
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return contents;
}

std::map<std::string, bool> runningPrograms(const std::string &procRoot) {
    std::map<std::string, bool> running;
    DIR *directory = opendir(procRoot.c_str());
    if (!directory) return running;
    dirent *entry = nullptr;
    while ((entry = readdir(directory)) != nullptr) {
        const std::string pid(entry->d_name);
        if (!numeric(pid)) continue;
        char executable[4096];
        const std::string path = procRoot + "/" + pid + "/exe";
        const ssize_t length = readlink(path.c_str(), executable, sizeof(executable) - 1);
        if (length <= 0) continue;
        executable[length] = '\0';
        std::string name = basename(executable);
        if (name == "iotjs") {
            std::vector<std::string> arguments;
            readNullSeparated(procRoot + "/" + pid + "/cmdline", arguments);
            if (arguments.size() > 1) {
                const size_t slash = arguments[1].find_last_of('/');
                if (slash != std::string::npos)
                    name = basename(arguments[1].substr(0, slash));
            }
        }
        running[name] = true;
    }
    closedir(directory);
    return running;
}

} // namespace

bool backgroundServicesSupported(const std::string &systemRoot) {
    return systemctlAvailable(systemRoot) || initctlAvailable(systemRoot);
}

std::string backgroundSystemctlExecutable(const std::string &systemRoot) {
    return systemctlExecutable(systemRoot);
}

std::string backgroundInitctlExecutable(const std::string &systemRoot) {
    return initctlExecutable(systemRoot);
}

std::vector<BackgroundServiceProbe> backgroundServiceProbes(
    const std::string &systemRoot) {
    const bool hasSystemctl = systemctlAvailable(systemRoot);
    const bool hasInitctl = initctlAvailable(systemRoot);
    std::vector<BackgroundServiceProbe> result;
    if (!hasSystemctl && !hasInitctl) return result;
    for (const CatalogItem &item : kCatalog) {
        if (item.hidden || !serviceExists(item, systemRoot, hasSystemctl, hasInitctl))
            continue;
        BackgroundServiceProbe probe;
        probe.id = item.id;
        if (hasSystemctl && item.unit) probe.unit = item.unit;
        else if (hasInitctl && item.upstart) probe.upstart = item.upstart;
        result.push_back(probe);
    }
    return result;
}

bool backgroundServiceInfo(const std::string &id, const std::string &systemRoot,
                           BackgroundServiceInfo &info) {
    const CatalogItem *item = findItem(id);
    if (!item || item->hidden) return false;
    const bool hasSystemctl = systemctlAvailable(systemRoot);
    const bool hasInitctl = initctlAvailable(systemRoot);
    if (!serviceExists(*item, systemRoot, hasSystemctl, hasInitctl)) return false;
    info.id = item->id;
    if (item->bin) info.bin = item->bin;
    if (item->exe) info.exe = item->exe;
    if (item->unit) info.unit = item->unit;
    if (item->upstart) info.upstart = item->upstart;
    if (id == "thinqai") info.jail = "native_mvpd";
    return true;
}

bool backgroundHiddenServiceInfo(const std::string &id,
                                 const std::string &systemRoot,
                                 BackgroundServiceInfo &info) {
    if (!backgroundHiddenServiceDefinition(id, info)) return false;
    const CatalogItem *item = findItem(id);
    if (!item) return false;
    const bool hasSystemctl = systemctlAvailable(systemRoot);
    const bool hasInitctl = initctlAvailable(systemRoot);
    if (!serviceExists(*item, systemRoot, hasSystemctl, hasInitctl)) return false;
    return true;
}

bool backgroundHiddenServiceDefinition(const std::string &id,
                                       BackgroundServiceInfo &info) {
    const CatalogItem *item = findItem(id);
    if (!item || !item->hidden) return false;
    info.id = item->id;
    if (item->bin) info.bin = item->bin;
    if (item->exe) info.exe = item->exe;
    if (item->unit) info.unit = item->unit;
    if (item->upstart) info.upstart = item->upstart;
    return true;
}

bool backgroundHiddenServiceAvailable(const std::string &id,
                                      const std::string &systemRoot) {
    BackgroundServiceInfo info;
    return backgroundHiddenServiceInfo(id, systemRoot, info);
}

std::vector<std::string> backgroundServicePrograms(
    const std::string &id, const std::string &systemRoot) {
    const CatalogItem *item = findItem(id);
    return item ? itemPrograms(*item, systemRoot) : std::vector<std::string>();
}

std::vector<int> backgroundServiceProcessIds(const std::string &id,
                                             const std::string &procRoot) {
    std::vector<int> result;
    const CatalogItem *item = findItem(id);
    if (!item || !item->bin) return result;
    DIR *directory = opendir(procRoot.c_str());
    if (!directory) return result;
    dirent *entry = nullptr;
    while ((entry = readdir(directory)) != nullptr) {
        const std::string pid(entry->d_name);
        if (!numeric(pid)) continue;
        char executable[4096];
        const std::string executablePath = procRoot + "/" + pid + "/exe";
        const ssize_t length = readlink(executablePath.c_str(), executable,
                                        sizeof(executable) - 1);
        if (length <= 0) continue;
        executable[length] = '\0';
        std::string name = basename(executable);
        if (name == "iotjs") {
            std::vector<std::string> arguments;
            (void)readNullSeparated(procRoot + "/" + pid + "/cmdline", arguments);
            if (arguments.size() > 1) {
                const size_t slash = arguments[1].find_last_of('/');
                if (slash != std::string::npos)
                    name = basename(arguments[1].substr(0, slash));
            }
        }
        if (name == item->bin) result.push_back(std::atoi(pid.c_str()));
    }
    closedir(directory);
    return result;
}

std::string backgroundServiceStubPath(const std::string &dataRoot, bool javascript) {
    return dataPath(dataRoot, javascript ? "service-off.js" : "service-off");
}

bool backgroundServiceStubbed(const std::string &procRoot, const std::string &path) {
    std::string mounts;
    return readText(procRoot + "/mounts", mounts) &&
           mounts.find(" " + path + " ") != std::string::npos;
}

bool readDisabledBackgroundServices(const std::string &dataRoot,
                                    std::vector<std::string> &disabled,
                                    std::string *error) {
    disabled.clear();
    const std::string path = dataPath(dataRoot, "disabled_services.json");
    if (access(path.c_str(), F_OK) != 0) {
        if (errno == ENOENT) return true;
        if (error) *error = "could not inspect " + path;
        return false;
    }
    std::string contents;
    if (!readText(path, contents)) {
        if (error) *error = "could not read " + path;
        return false;
    }
    Json parsed;
    try {
        parsed = Json::parse(contents);
    } catch (...) {
        if (error) *error = "disabled services file is malformed";
        return false;
    }
    if (!parsed.isArray()) {
        if (error) *error = "disabled services file must contain an array";
        return false;
    }
    for (const Json &item : parsed.items())
        if (item.isString() &&
            std::find(disabled.begin(), disabled.end(), item.string()) == disabled.end())
            disabled.push_back(item.string());
    return true;
}

bool setBackgroundServiceDisabled(const std::string &dataRoot, const std::string &id,
                                  bool disabled,
                                  std::vector<std::string> &disabledServices,
                                  std::string *error) {
    if (!readDisabledBackgroundServices(dataRoot, disabledServices, error)) return false;
    disabledServices.erase(std::remove_if(disabledServices.begin(), disabledServices.end(),
        [](const std::string &entry) {
            return entry == "tvdataexchanger" || entry == "alwaysready";
        }), disabledServices.end());
    const std::vector<std::string>::iterator found =
        std::find(disabledServices.begin(), disabledServices.end(), id);
    if (disabled && found == disabledServices.end()) disabledServices.push_back(id);
    if (!disabled && found != disabledServices.end()) disabledServices.erase(found);
    Json value = Json::array();
    for (const std::string &entry : disabledServices) value.add(Json::text(entry));
    return writeAtomic(dataPath(dataRoot, "disabled_services.json"), value.dump() + "\n",
                       0644, error);
}

bool syncBackgroundServiceBootScript(const std::string &systemRoot,
                                    const std::string &dataRoot,
                                    const std::vector<std::string> &disabledServices,
                                    std::string *error) {
    const std::string path = rooted(systemRoot,
                                    "/var/lib/webosbrew/init.d/20-tvweb-services");
    if (disabledServices.empty()) {
        if (unlink(path.c_str()) != 0 && errno != ENOENT) {
            if (error) *error = "could not remove " + path;
            return false;
        }
        return true;
    }

    const std::string shellStub = backgroundServiceStubPath(dataRoot, false);
    const std::string jsStub = backgroundServiceStubPath(dataRoot, true);
    std::vector<std::string> lines = {
        "#!/bin/sh", "# tvweb managed disabled services",
        "BOOTLOG=/var/lib/webosbrew/tvweb-boot.log",
        "bootlog() { echo \"$(date): [up $(cut -d\" \" -f1 /proc/uptime)s] $*\" >>\"$BOOTLOG\"; }",
        "mkdir /tmp/tvweb-services-ran 2>/dev/null || { bootlog \"services: already run this boot\"; exit 0; }",
        "bootlog \"services: start\"", "if which systemctl >/dev/null 2>&1; then",
        "  printf '#!/bin/sh\\nexit 0\\n' >" + shellStub + "; chmod 755 " + shellStub,
        "  printf 'process.exit(0);\\n' >" + jsStub,
        "  off() { case \"$1\" in *.js) s=" + jsStub + " ;; *) s=" + shellStub + " ;; esac",
        "    grep -q \" $1 \" /proc/mounts || mount --bind \"$s\" \"$1\"; }",
        "  jail() { j=/var/palm/jail/$2; [ -e \"$3\" ] || return",
        "    grep -q \" $j/usr \" /proc/mounts || /usr/bin/jailer -t \"$1\" -i \"$2\" -p /usr /bin/true >/dev/null 2>&1",
        "    grep -q \" $j/usr \" /proc/mounts && off \"$j$3\"; }"
    };
    std::vector<std::string> units;
    std::vector<std::string> jobs;
    const auto switchedOff = [&disabledServices](const CatalogItem &item) {
        return std::find(disabledServices.begin(), disabledServices.end(), item.id) !=
               disabledServices.end();
    };
    // A jail first and beside the rest: jailer took about 7 s to set one up at
    // 22 s, and lg.thinqai.adapter is launched at about 32 s.
    for (const CatalogItem &item : kCatalog)
        if (switchedOff(item) && item.id == std::string("thinqai"))
            lines.push_back("  jail \"native_mvpd\" \"lg.thinqai.adapter\" \"/usr/sbin/lg.thinqai.adapter\" &");
    for (const CatalogItem &item : kCatalog) {
        if (!switchedOff(item)) continue;
        for (const std::string &program : itemPrograms(item, systemRoot))
            lines.push_back("  off \"" + program + "\"");
        if (item.unit) units.push_back(item.unit);
        if (item.upstart) jobs.push_back(item.upstart);
    }
    lines.push_back("  wait");
    lines.push_back("  bootlog \"services: stubs bound\"");
    if (!units.empty()) {
        std::string stop = "  systemctl stop";
        for (const std::string &unit : units) stop += " \"" + unit + "\"";
        lines.push_back(stop + " 2>/dev/null");
    }
    if (!jobs.empty()) {
        lines.push_back("elif which initctl >/dev/null 2>&1; then");
        for (const std::string &job : jobs)
            lines.push_back("  initctl stop \"" + job + "\" 2>/dev/null");
    }
    lines.push_back("fi");
    lines.push_back("bootlog \"services: stopped\"");
    std::string script;
    for (const std::string &line : lines) script += line + "\n";
    std::string existing;
    if (readText(path, existing) && existing == script) return true;
    return writeAtomic(path, script, 0755, error);
}

Json backgroundServicesStatus(const std::string &systemRoot,
                              const std::string &procRoot,
                              const std::string &dataRoot,
                              const std::map<std::string, bool> &managerRunning) {
    const bool hasSystemctl = systemctlAvailable(systemRoot);
    const bool hasInitctl = initctlAvailable(systemRoot);
    if (!hasSystemctl && !hasInitctl) {
        Json unsupported = Json::object();
        unsupported.set("ok", Json::flag(true));
        unsupported.set("supported", Json::flag(false));
        unsupported.set("services", Json::array());
        return unsupported;
    }

    const std::map<std::string, bool> processes = runningPrograms(procRoot);
    std::vector<std::string> disabled;
    (void)readDisabledBackgroundServices(dataRoot, disabled);
    Json rows = Json::array();
    for (const CatalogItem &item : kCatalog) {
        if (item.hidden || !serviceExists(item, systemRoot, hasSystemctl, hasInitctl))
            continue;
        Json row = Json::object();
        row.set("id", Json::text(item.id));
        row.set("title", Json::text(say::msg(item.titleKey, item.title)));
        row.set("badge", Json::text(item.badgeKey ? say::msg(item.badgeKey, item.badge)
                                                  : std::string(item.badge)));
        row.set("desc", Json::text(say::msg(item.descriptionKey, item.description)));
        if (item.unit) row.set("unit", Json::text(item.unit));
        else if (item.upstart) row.set("unit", Json::text(item.upstart));
        else row.set("unit", Json());  // null, as the server lists a service with neither
        row.set("disabled", Json::flag(std::find(disabled.begin(), disabled.end(), item.id) !=
                                        disabled.end()));
        const bool processSeen = item.bin && processes.find(item.bin) != processes.end();
        const std::map<std::string, bool>::const_iterator manager =
            managerRunning.find(item.id);
        row.set("running", Json::flag(processSeen ||
            (manager != managerRunning.end() && manager->second)));
        rows.add(row);
    }
    Json response = Json::object();
    response.set("ok", Json::flag(true));
    response.set("supported", Json::flag(true));
    response.set("services", rows);
    return response;
}

} // namespace service
} // namespace nativetv
