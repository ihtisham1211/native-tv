#pragma once

#include <cstdint>
#include <string>

namespace nativetv {
namespace service {

struct AppInstallDownloadCapacity {
    uint64_t freeBytes = 0;
    uint64_t requiredBytes = 0;
    uint64_t maxDownloadBytes = 0;
    bool freeBytesKnown = false;
    bool capacityOkay = true;
    bool spaceLimited = false;
};

class AppInstallSpace {
  public:
    static bool availableBytes(const std::string &path, uint64_t &bytes,
                               std::string *error = nullptr);
    static bool downloadCapacity(uint64_t requestedMaxBytes,
                                 uint64_t expectedPackageBytes,
                                 bool freeBytesKnown, uint64_t freeBytes,
                                 AppInstallDownloadCapacity &result,
                                 std::string *error = nullptr);
};

} // namespace service
} // namespace nativetv
