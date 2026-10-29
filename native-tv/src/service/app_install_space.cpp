#include "service/app_install_space.hpp"

#include <cerrno>
#include <cstring>
#include <sys/statvfs.h>

namespace nativetv {
namespace service {

bool AppInstallSpace::availableBytes(const std::string &path, uint64_t &bytes,
                                     std::string *error) {
    bytes = 0;
    if (error) error->clear();
    if (path.empty()) {
        if (error) *error = "install staging path is empty";
        return false;
    }

    struct statvfs info{};
    if (statvfs(path.c_str(), &info) != 0) {
        if (error) *error = "read install staging capacity: " + std::string(std::strerror(errno));
        return false;
    }
    const uint64_t blockSize = info.f_frsize ? static_cast<uint64_t>(info.f_frsize)
                                              : static_cast<uint64_t>(info.f_bsize);
    if (!blockSize || info.f_bavail > info.f_blocks) {
        if (error) *error = "install staging capacity is invalid";
        return false;
    }

    // JSON numbers are doubles. Keep the byte count within their exact integer
    // range so the Node bridge does not round the value before comparing sizes.
    constexpr uint64_t kMaxExactJsonInteger = 9007199254740991ULL;
    const uint64_t availableBlocks = static_cast<uint64_t>(info.f_bavail);
    if (availableBlocks > kMaxExactJsonInteger / blockSize) {
        if (error) *error = "install staging capacity exceeds the supported range";
        return false;
    }
    bytes = availableBlocks * blockSize;
    return true;
}

bool AppInstallSpace::downloadCapacity(uint64_t requestedMaxBytes,
                                       uint64_t expectedPackageBytes,
                                       bool freeBytesKnown, uint64_t freeBytes,
                                       AppInstallDownloadCapacity &result,
                                       std::string *error) {
    result = AppInstallDownloadCapacity{};
    if (error) error->clear();
    if (!requestedMaxBytes || expectedPackageBytes > UINT64_MAX / 3) {
        if (error) *error = "invalid app download capacity request";
        return false;
    }

    result.freeBytes = freeBytes;
    result.freeBytesKnown = freeBytesKnown;
    result.requiredBytes = expectedPackageBytes ? expectedPackageBytes * 3 : 3;
    result.capacityOkay = !freeBytesKnown || freeBytes >= result.requiredBytes;
    result.maxDownloadBytes = requestedMaxBytes;
    if (freeBytesKnown) {
        const uint64_t spaceLimit = freeBytes / 3;
        if (spaceLimit < result.maxDownloadBytes) {
            result.maxDownloadBytes = spaceLimit;
            result.spaceLimited = true;
        }
        if (result.maxDownloadBytes == 0) result.capacityOkay = false;
    }
    return true;
}

} // namespace service
} // namespace nativetv
