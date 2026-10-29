#include "service/remote_input.hpp"
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>
#include <fcntl.h>
#include <unistd.h>

namespace nativetv {
namespace service {
namespace {
const unsigned char kEvKey = 1;
const unsigned char kEvSyn = 0;
const unsigned kMediaKeys[] = {207, 119, 128, 208, 168};

std::string rooted(const std::string &root, const std::string &path) {
    return root.empty() || root == "/" ? path : root + path;
}

bool isSupportedKey(unsigned code) {
    return std::find(std::begin(kMediaKeys), std::end(kMediaKeys), code) !=
           std::end(kMediaKeys);
}

std::string rcuEventPath(const std::string &procRoot,
                         const std::string &systemRoot) {
    std::ifstream input(rooted(procRoot, "/bus/input/devices").c_str());
    bool rcu = false;
    std::string line;
    while (std::getline(input, line)) {
        if (line.find("Name=\"LGE RCU\"") != std::string::npos ||
            line.find("Name=\"Smart Remote RCU Input\"") != std::string::npos) {
            rcu = true;
            continue;
        }
        if (!rcu || line.find("Handlers=") == std::string::npos)
            continue;
        const size_t marker = line.find("event");
        if (marker == std::string::npos) {
            rcu = false;
            continue;
        }
        size_t end = marker + 5;
        while (end < line.size() && std::isdigit(static_cast<unsigned char>(line[end])))
            ++end;
        if (end > marker + 5)
            return rooted(systemRoot, "/dev/input/") + line.substr(marker, end - marker);
        rcu = false;
    }
    return rooted(systemRoot, "/dev/input/event1");
}

void encodeEvent(unsigned char (&event)[16], unsigned type, unsigned code, int value) {
    std::memset(event, 0, sizeof(event));
    event[8] = static_cast<unsigned char>(type & 0xff);
    event[9] = static_cast<unsigned char>((type >> 8) & 0xff);
    event[10] = static_cast<unsigned char>(code & 0xff);
    event[11] = static_cast<unsigned char>((code >> 8) & 0xff);
    const uint32_t encoded = static_cast<uint32_t>(value);
    event[12] = static_cast<unsigned char>(encoded & 0xff);
    event[13] = static_cast<unsigned char>((encoded >> 8) & 0xff);
    event[14] = static_cast<unsigned char>((encoded >> 16) & 0xff);
    event[15] = static_cast<unsigned char>((encoded >> 24) & 0xff);
}

bool writeAll(int descriptor, const unsigned char *bytes, size_t size) {
    size_t offset = 0;
    while (offset < size) {
        const ssize_t written = write(descriptor, bytes + offset, size - offset);
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0)
            return false;
        offset += static_cast<size_t>(written);
    }
    return true;
}

bool writeEvent(int descriptor, unsigned type, unsigned code, int value) {
    unsigned char event[16];
    encodeEvent(event, type, code, value);
    return writeAll(descriptor, event, sizeof(event));
}

bool pressRelease(int descriptor, unsigned code, unsigned durationMs) {
    const bool pressed = writeEvent(descriptor, kEvKey, code, 1) &&
                         writeEvent(descriptor, kEvSyn, 0, 0);
    if (!pressed) {
        (void)writeEvent(descriptor, kEvKey, code, 0);
        (void)writeEvent(descriptor, kEvSyn, 0, 0);
        return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(durationMs));
    const bool released = writeEvent(descriptor, kEvKey, code, 0) &&
                          writeEvent(descriptor, kEvSyn, 0, 0);
    if (!released) {
        (void)writeEvent(descriptor, kEvKey, code, 0);
        (void)writeEvent(descriptor, kEvSyn, 0, 0);
    }
    return released;
}
} // namespace

static bool injectRemoteKey(const std::string &procRoot,
                            const std::string &systemRoot, unsigned code,
                            unsigned durationMs) {
    const std::string preferred = rcuEventPath(procRoot, systemRoot);
    const std::string fallback = rooted(systemRoot, "/dev/input/event1");
    int descriptor = open(preferred.c_str(), O_WRONLY | O_CLOEXEC | O_NONBLOCK);
    if (descriptor < 0 && preferred != fallback)
        descriptor = open(fallback.c_str(), O_WRONLY | O_CLOEXEC | O_NONBLOCK);
    if (descriptor < 0)
        return false;
    const bool succeeded = pressRelease(descriptor, code, durationMs);
    close(descriptor);
    return succeeded;
}

bool injectRemoteMediaKey(const std::string &procRoot,
                          const std::string &systemRoot, unsigned code) {
    return isSupportedKey(code) &&
           injectRemoteKey(procRoot, systemRoot, code, 50);
}

bool injectRemoteBackKey(const std::string &procRoot,
                         const std::string &systemRoot) {
    return injectRemoteKey(procRoot, systemRoot, 158, 50);
}

bool injectRemoteHomeKey(const std::string &procRoot,
                         const std::string &systemRoot) {
    return injectRemoteKey(procRoot, systemRoot, 125, 100);
}

} // namespace service
} // namespace nativetv
