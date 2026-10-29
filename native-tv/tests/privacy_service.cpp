#include "service/privacy_service.hpp"
#include <cerrno>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <string>
#include <ftw.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace nativetv::service;

namespace {
void check(bool condition, const std::string &message) {
    if (!condition) throw std::runtime_error(message);
}

std::string makeRoot() {
    char pattern[] = "/tmp/nativetv-privacy-service-XXXXXX";
    char *path = ::mkdtemp(pattern);
    if (!path) throw std::runtime_error("could not create privacy test directory");
    return path;
}

void makeDirectory(const std::string &path) {
    size_t offset = 1;
    while (offset <= path.size()) {
        const size_t end = path.find('/', offset);
        const std::string current = path.substr(
            0, end == std::string::npos ? path.size() : end);
        if (!current.empty() && ::mkdir(current.c_str(), 0700) != 0 && errno != EEXIST)
            throw std::runtime_error("could not create " + current);
        if (end == std::string::npos) break;
        offset = end + 1;
    }
}

void write(const std::string &path, const std::string &contents) {
    const size_t slash = path.find_last_of('/');
    makeDirectory(path.substr(0, slash));
    std::ofstream file(path.c_str(), std::ios::binary);
    if (!file) throw std::runtime_error("could not write " + path);
    file.write(contents.data(), static_cast<std::streamsize>(contents.size()));
}

int removePath(const char *path, const struct stat *, int, struct FTW *) {
    return ::remove(path);
}

void removeTree(const std::string &root) {
    ::nftw(root.c_str(), removePath, 16, FTW_DEPTH | FTW_PHYS);
}

void testDynamicServiceAndProcessMatching() {
    const std::string root = makeRoot();
    const std::string systemRoot = root + "/system";
    const std::string procRoot = root + "/proc";
    const std::string serviceFile = systemRoot +
        "/usr/share/dbus-1/system-services/admanager.service";
    const std::string descriptor =
        "[D-BUS Service]\n"
        "Name=com.webos.service.admanager;com.webos.service.admanager2;\n"
        "Exec=/usr/sbin/admanager --dbus\n"
        "Type = dynamic\n";
    try {
        write(serviceFile, descriptor);
        check(privacyServiceWouldStart(systemRoot, procRoot,
                                       "com.webos.service.admanager"),
              "an absent dynamic service is reported as start-on-query");
        check(privacyServiceWouldStart(systemRoot, procRoot,
                                       "com.webos.service.admanager2"),
              "all names declared in the bus service file are recognized");
        check(!privacyServiceWouldStart(systemRoot, procRoot,
                                        "com.webos.service.other"),
              "unmapped services are not treated as dynamic");

        write(procRoot + "/101/cmdline", "worker\0/usr/sbin/admanager\0");
        std::vector<std::string> commands = privacyProcessFirstArguments(procRoot);
        check(commands.size() == 1 && commands[0] == "worker",
              "process snapshots use only argv zero, as the legacy ps projection does");
        check(privacyServiceWouldStart(systemRoot, procRoot,
                                       "com.webos.service.admanager"),
              "a process mentioning the executable after argv zero is not mistaken for it");

        write(procRoot + "/101/cmdline", "/usr/sbin/admanager\0--dbus\0");
        commands = privacyProcessFirstArguments(procRoot);
        check(commands.size() == 1 && commands[0] == "/usr/sbin/admanager",
              "process snapshots preserve the executable argv zero");
        check(!privacyServiceWouldStart(systemRoot, procRoot,
                                        "com.webos.service.admanager"),
              "the actual on-demand process is recognized by argv zero");

        write(serviceFile,
              "[D-BUS Service]\nName=com.webos.service.admanager;\n"
              "Exec=/usr/sbin/admanager\nType=static\n");
        check(!privacyServiceWouldStart(systemRoot, procRoot,
                                        "com.webos.service.admanager"),
              "static services are safe to query under the legacy policy");
    } catch (...) {
        removeTree(root);
        throw;
    }
    removeTree(root);
}
} // namespace

int main() {
    testDynamicServiceAndProcessMatching();
    return 0;
}
