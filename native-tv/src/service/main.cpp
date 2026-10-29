#include "service/process_metrics.hpp"
#include "service/stuck_child_reaper.hpp"
#include "service/system_metrics.hpp"
#include <cmath>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace {
unsigned configuredPort(const std::string &dataRoot) {
    std::string path = dataRoot;
    if (!path.empty() && path[path.size() - 1] != '/') path += '/';
    path += "config.json";
    std::ifstream file(path.c_str(), std::ios::in | std::ios::binary);
    if (!file) return 8080;
    std::ostringstream contents;
    contents << file.rdbuf();
    const nativetv::Json config = nativetv::Json::parse(contents.str());
    const nativetv::Json value = config["port"];
    if (value.isNumber()) {
        const double number = value.number();
        if (number >= 1 && number <= 65535 &&
            number == std::floor(number))
            return static_cast<unsigned>(number);
        return 8080;
    }
    if (!value.isString() || value.string().empty()) return 8080;
    unsigned port = 0;
    for (const char character : value.string()) {
        if (character < '0' || character > '9') return 8080;
        const unsigned digit = static_cast<unsigned>(character - '0');
        if (port > (65535U - digit) / 10U) return 8080;
        port = port * 10U + digit;
    }
    return port == 0 ? 8080 : port;
}
} // namespace

int main(int argc, char **argv) {
    using namespace nativetv;
    try {
        std::string operation, root = "/proc", systemRoot = "/";
        std::string dataRoot = "/var/lib/tvweb";
        pid_t parentPid = 0;
        unsigned settleMs = 3000;
        bool all = false, procRootSet = false, systemRootSet = false,
             dataRootSet = false, parentPidSet = false, settleMsSet = false;
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "processes" || arg == "cpu" || arg == "system" ||
                arg == "port" || arg == "reap-stuck-children") {
                if (!operation.empty())
                    throw std::runtime_error("One operation is required");
                operation = arg;
            } else if (arg == "--all")
                all = true;
            else if (arg == "--proc-root" && i + 1 < argc) {
                root = argv[++i];
                procRootSet = true;
            } else if (arg == "--root" && i + 1 < argc) {
                systemRoot = argv[++i];
                systemRootSet = true;
            } else if (arg == "--data-root") {
                if (i + 1 >= argc)
                    throw std::runtime_error("--data-root requires a value");
                dataRoot = argv[++i];
                dataRootSet = true;
            } else if (arg == "--pid") {
                if (i + 1 >= argc)
                    throw std::runtime_error("--pid requires a value");
                const std::string value = argv[++i];
                char *end = nullptr;
                errno = 0;
                const long parsed = std::strtol(value.c_str(), &end, 10);
                if (errno || !end || *end || parsed <= 1 ||
                    parsed > static_cast<long>(std::numeric_limits<pid_t>::max()))
                    throw std::runtime_error("--pid must be a process ID greater than 1");
                parentPid = static_cast<pid_t>(parsed);
                parentPidSet = true;
            } else if (arg == "--settle-ms") {
                if (i + 1 >= argc)
                    throw std::runtime_error("--settle-ms requires a value");
                const std::string value = argv[++i];
                char *end = nullptr;
                errno = 0;
                const unsigned long parsed = std::strtoul(value.c_str(), &end, 10);
                if (errno || !end || *end || parsed > 30000)
                    throw std::runtime_error("--settle-ms must be between 0 and 30000");
                settleMs = static_cast<unsigned>(parsed);
                settleMsSet = true;
            } else
                throw std::runtime_error("Unknown argument: " + arg);
        }
        const bool validOptions = operation == "processes"
            ? !systemRootSet && !dataRootSet && !parentPidSet && !settleMsSet
            : operation == "cpu"
                ? !all && !systemRootSet && !dataRootSet && !parentPidSet && !settleMsSet
                : operation == "system"
                    ? !all && !procRootSet && !parentPidSet && !settleMsSet
                    : operation == "port"
                        ? !all && !procRootSet && !systemRootSet &&
                              !parentPidSet && !settleMsSet
                        : operation == "reap-stuck-children"
                            ? !all && !systemRootSet && !dataRootSet && parentPidSet
                            : false;
        if (operation.empty() || !validOptions)
            throw std::runtime_error("nativetv-service processes [--all] | cpu [--proc-root "
                                     "path] | system [--root path] [--data-root path] | "
                                     "port [--data-root path] | reap-stuck-children --pid PID "
                                     "[--proc-root path] [--settle-ms ms]");
        if (operation == "port") {
            std::printf("%u\n", configuredPort(dataRoot));
            return 0;
        }
        if (operation == "reap-stuck-children") {
            std::vector<service::ReapedChild> reaped;
            std::string error;
            if (!service::reapStuckChildren(root, parentPid, settleMs, reaped, &error))
                throw std::runtime_error(error.empty()
                    ? "could not inspect or kill stuck child processes" : error);
            for (const service::ReapedChild &child : reaped)
                std::printf("child %lld stuck before exec (%s) - killed it\n",
                            static_cast<long long>(child.pid), child.location.c_str());
            return 0;
        }
        Json response = Json::object();
        response.set("protocol", Json::numeric(1));
        response.set("result", operation == "processes" ? service::collectMemory(root, all)
                               : operation == "cpu"     ? service::collectCpu(root)
                                                        : service::collectSystem(systemRoot, dataRoot));
        std::puts(response.dump().c_str());
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "Native Tv service: %s\n", error.what());
        return 1;
    }
}
