#include "service/app_install_space.hpp"

#include <cstdlib>
#include <iostream>
#include <sys/statvfs.h>
#include <unistd.h>

int main() {
    char pattern[] = "/tmp/nativetv-install-space-XXXXXX";
    char *directory = mkdtemp(pattern);
    if (!directory) {
        std::cerr << "could not create install-space fixture\n";
        return 1;
    }

    struct statvfs expected{};
    uint64_t actualBytes = 0;
    std::string error;
    // Other tests write to the same disk while this one runs, so the free space moves between
    // the two reads: the answer has to agree with statvfs to within a few MB, not to the byte.
    bool ok = statvfs(directory, &expected) == 0 &&
              nativetv::service::AppInstallSpace::availableBytes(
                  directory, actualBytes, &error) &&
              error.empty();
    if (ok) {
        const uint64_t expectedBytes =
            static_cast<uint64_t>(expected.f_bavail) *
            static_cast<uint64_t>(expected.f_frsize ? expected.f_frsize : expected.f_bsize);
        const uint64_t drift = actualBytes > expectedBytes ? actualBytes - expectedBytes
                                                           : expectedBytes - actualBytes;
        ok = drift < 64ull * 1024 * 1024;
    }
    if (!ok) {
        std::cerr << "install-space result did not match statvfs: " << error << '\n';
        rmdir(directory);
        return 1;
    }

    uint64_t missingBytes = 1;
    if (nativetv::service::AppInstallSpace::availableBytes(
            std::string(directory) + "/missing", missingBytes, &error) ||
        missingBytes != 0 || error.empty()) {
        std::cerr << "missing staging path should return an error\n";
        rmdir(directory);
        return 1;
    }

    if (!nativetv::service::AppInstallSpace::availableBytes(directory, actualBytes) ||
        actualBytes == 0) {
        std::cerr << "optional error output should be supported\n";
        rmdir(directory);
        return 1;
    }

    using nativetv::service::AppInstallDownloadCapacity;
    using nativetv::service::AppInstallSpace;
    AppInstallDownloadCapacity capacity;
    const uint64_t requestedMax = 512ULL * 1024 * 1024;
    if (!AppInstallSpace::downloadCapacity(requestedMax, 1000, true, 3000,
                                            capacity, &error) ||
        !capacity.capacityOkay || capacity.requiredBytes != 3000 ||
        capacity.maxDownloadBytes != 1000 || !capacity.spaceLimited ||
        !error.empty()) {
        std::cerr << "exact three-times package capacity should be allowed\n";
        rmdir(directory);
        return 1;
    }

    if (!AppInstallSpace::downloadCapacity(requestedMax, 1000, true, 2999,
                                            capacity, &error) ||
        capacity.capacityOkay || capacity.requiredBytes != 3000 ||
        capacity.maxDownloadBytes != 999) {
        std::cerr << "a one-byte capacity shortfall should be refused\n";
        rmdir(directory);
        return 1;
    }

    if (!AppInstallSpace::downloadCapacity(4096, 100, true, 100000,
                                            capacity, &error) ||
        !capacity.capacityOkay || capacity.maxDownloadBytes != 4096 ||
        capacity.spaceLimited) {
        std::cerr << "the configured package cap should win when space is ample\n";
        rmdir(directory);
        return 1;
    }

    if (!AppInstallSpace::downloadCapacity(requestedMax, 0, false, 0,
                                            capacity, &error) ||
        !capacity.capacityOkay || capacity.freeBytesKnown ||
        capacity.maxDownloadBytes != requestedMax) {
        std::cerr << "unknown free space should retain the absolute package cap\n";
        rmdir(directory);
        return 1;
    }

    if (!AppInstallSpace::downloadCapacity(requestedMax, 0, true, 2,
                                            capacity, &error) ||
        capacity.capacityOkay || capacity.requiredBytes != 3 ||
        capacity.maxDownloadBytes != 0) {
        std::cerr << "less than three free bytes should refuse a package download\n";
        rmdir(directory);
        return 1;
    }

    if (AppInstallSpace::downloadCapacity(requestedMax, UINT64_MAX, true,
                                           actualBytes, capacity, &error) ||
        error.empty()) {
        std::cerr << "overflowing package sizes should be rejected\n";
        rmdir(directory);
        return 1;
    }
    rmdir(directory);
    std::cout << "native app install space checks passed\n";
    return 0;
}
