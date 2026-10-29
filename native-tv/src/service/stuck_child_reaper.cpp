#include "service/stuck_child_reaper.hpp"

#include <dirent.h>
#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <sstream>
#include <thread>

namespace nativetv {
namespace service {
namespace {

std::string procPath(const std::string &root, const std::string &tail) {
    if (root.empty() || root == "/") return "/" + tail;
    size_t end = root.size();
    while (end > 1 && root[end - 1] == '/') --end;
    return root.substr(0, end) + "/" + tail;
}

bool numericPid(const char *text, pid_t &pid) {
    if (!text || !*text) return false;
    char *end = nullptr;
    errno = 0;
    const long parsed = std::strtol(text, &end, 10);
    if (errno || !end || *end || parsed <= 1 ||
        parsed > static_cast<long>(std::numeric_limits<pid_t>::max()))
        return false;
    pid = static_cast<pid_t>(parsed);
    return true;
}

std::string executablePath(const std::string &procRoot, pid_t pid) {
    const std::string path = procPath(
        procRoot, std::to_string(static_cast<long long>(pid)) + "/exe");
    char target[4096];
    const ssize_t count = readlink(path.c_str(), target, sizeof(target) - 1);
    if (count <= 0 || static_cast<size_t>(count) >= sizeof(target) - 1)
        return std::string();
    target[count] = '\0';
    return std::string(target);
}

bool parentPidFromStat(const std::string &path, pid_t &parentPid) {
    std::ifstream input(path.c_str());
    std::string line;
    if (!std::getline(input, line)) return false;
    const size_t commandEnd = line.find_last_of(')');
    if (commandEnd == std::string::npos) return false;
    std::istringstream fields(line.substr(commandEnd + 1));
    std::string state;
    long parsed = 0;
    if (!(fields >> state >> parsed) || parsed <= 0 ||
        parsed > static_cast<long>(std::numeric_limits<pid_t>::max()))
        return false;
    parentPid = static_cast<pid_t>(parsed);
    return true;
}

bool hasRemoteButtonsCommand(const std::string &procRoot, pid_t pid) {
    const std::string path = procPath(
        procRoot, std::to_string(static_cast<long long>(pid)) + "/cmdline");
    std::ifstream input(path.c_str(), std::ios::in | std::ios::binary);
    if (!input) return false;
    const std::string command((std::istreambuf_iterator<char>(input)),
                              std::istreambuf_iterator<char>());
    return command.find("remotebuttons") != std::string::npos;
}

std::vector<pid_t> findMatchingChildren(const std::string &procRoot,
                                        pid_t parentPid,
                                        const std::string &parentExe) {
    std::vector<pid_t> result;
    DIR *directory = opendir(procRoot.c_str());
    if (!directory) return result;
    struct dirent *entry = nullptr;
    while ((entry = readdir(directory)) != nullptr) {
        pid_t childPid = 0;
        if (!numericPid(entry->d_name, childPid) || childPid == parentPid)
            continue;
        const std::string base = std::to_string(static_cast<long long>(childPid));
        pid_t actualParent = 0;
        if (!parentPidFromStat(procPath(procRoot, base + "/stat"), actualParent) ||
            actualParent != parentPid ||
            executablePath(procRoot, childPid) != parentExe ||
            hasRemoteButtonsCommand(procRoot, childPid))
            continue;
        result.push_back(childPid);
    }
    closedir(directory);
    return result;
}

bool parseHex(const std::string &text, unsigned long long &value) {
    if (text.empty()) return false;
    char *end = nullptr;
    errno = 0;
    const unsigned long long parsed = std::strtoull(text.c_str(), &end, 16);
    if (errno || !end || *end) return false;
    value = parsed;
    return true;
}

std::string syscallLocation(const std::string &procRoot, pid_t pid) {
    const std::string base = std::to_string(static_cast<long long>(pid));
    std::ifstream syscall(procPath(procRoot, base + "/syscall").c_str());
    std::string number, field, pc;
    if (!(syscall >> number)) return "syscall ? at ? in ?";
    // syscall files contain the number, six arguments, stack pointer, then PC.
    for (int index = 0; index < 7; ++index)
        if (!(syscall >> field)) return "syscall " + number + " at ? in ?";
    if (!(syscall >> pc)) return "syscall " + number + " at ? in ?";

    unsigned long long programCounter = 0;
    if (!parseHex(pc, programCounter))
        return "syscall " + number + " at " + pc + " in ?";

    std::ifstream maps(procPath(procRoot, base + "/maps").c_str());
    std::string line;
    while (std::getline(maps, line)) {
        std::istringstream fields(line);
        std::string range, permissions, offset, device, inode;
        if (!(fields >> range >> permissions >> offset >> device >> inode)) continue;
        const size_t dash = range.find('-');
        if (dash == std::string::npos) continue;
        unsigned long long start = 0, end = 0, fileOffset = 0;
        if (!parseHex(range.substr(0, dash), start) ||
            !parseHex(range.substr(dash + 1), end) ||
            !parseHex(offset, fileOffset) ||
            programCounter < start || programCounter >= end)
            continue;
        std::string file;
        std::getline(fields, file);
        const size_t first = file.find_first_not_of(" \t");
        file = first == std::string::npos ? "anon" : file.substr(first);
        std::ostringstream result;
        result << "syscall " << number << " at " << pc << " in " << file
               << "+0x" << std::hex << (fileOffset + programCounter - start);
        return result.str();
    }
    return "syscall " + number + " at " + pc + " in ?";
}

} // namespace

bool reapStuckChildren(const std::string &procRoot, pid_t parentPid,
                      unsigned settleMs, std::vector<ReapedChild> &reaped,
                      std::string *error) {
    reaped.clear();
    if (error) error->clear();
    if (parentPid <= 1) {
        if (error) *error = "parent PID must be greater than 1";
        return false;
    }
    if (settleMs > 30000) {
        if (error) *error = "settle interval must not exceed 30000 ms";
        return false;
    }
    const std::string parentExe = executablePath(procRoot, parentPid);
    if (parentExe.empty()) return true;
    const std::vector<pid_t> first =
        findMatchingChildren(procRoot, parentPid, parentExe);
    if (first.empty()) return true;
    if (settleMs) std::this_thread::sleep_for(std::chrono::milliseconds(settleMs));
    const std::vector<pid_t> second =
        findMatchingChildren(procRoot, parentPid, parentExe);

    for (const pid_t childPid : first) {
        if (std::find(second.begin(), second.end(), childPid) == second.end())
            continue;
        const std::string location = syscallLocation(procRoot, childPid);
        if (kill(childPid, SIGKILL) == 0) {
            ReapedChild item;
            item.pid = childPid;
            item.location = location;
            reaped.push_back(item);
        } else if (errno != ESRCH) {
            if (error) {
                *error = "could not kill stuck child " +
                    std::to_string(static_cast<long long>(childPid));
            }
            return false;
        }
    }
    return true;
}

} // namespace service
} // namespace nativetv
