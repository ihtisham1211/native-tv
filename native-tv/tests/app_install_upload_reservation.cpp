#include "daemon/app_install_upload_reservation.hpp"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <unistd.h>

namespace {

bool check(bool condition, const char *message) {
    if (condition) return true;
    std::cerr << message << '\n';
    return false;
}

bool isUploadName(const std::string &file) {
    if (file.size() != 27 || file.compare(0, 7, "upload-") != 0 ||
        file.compare(23, 4, ".ipk") != 0)
        return false;
    for (size_t index = 7; index < 23; ++index) {
        const char value = file[index];
        if (!((value >= '0' && value <= '9') || (value >= 'a' && value <= 'f')))
            return false;
    }
    return true;
}

bool writeFile(const std::string &path) {
    std::ofstream output(path.c_str(), std::ios::binary);
    output << "fixture";
    return output.good();
}

} // namespace

int main() {
    char pattern[] = "/tmp/nativetv-install-reservation-XXXXXX";
    char *created = mkdtemp(pattern);
    if (!created) {
        std::cerr << "could not create install reservation fixture\n";
        return 1;
    }
    const std::string directory(created);
    const std::string stalePath = directory + "/upload-stale.ipk";
    const bool staleWritten = writeFile(stalePath);

    bool ok = check(staleWritten, "could not create stale staged file");
    nativetv::daemon::AppInstallUploadReservation reservation;
    std::string file;
    std::string error;
    uint64_t availableBytes = 0;
    const auto reserved = reservation.reserve(directory, 64, 1024, false,
                                               file, availableBytes, &error);
    ok = check(reserved == nativetv::daemon::AppInstallUploadReservation::ReserveResult::reserved,
               "a valid upload should reserve staging") && ok;
    ok = check(isUploadName(file), "reservation should return a random upload basename") && ok;
    ok = check(availableBytes > 0, "reservation should report available capacity") && ok;
    ok = check(access(stalePath.c_str(), F_OK) != 0,
               "reserving staging should clear abandoned files") && ok;
    ok = check(reservation.held() && reservation.owns(file),
               "the reserved upload should hold the lease") && ok;

    std::string secondFile;
    uint64_t secondAvailable = 0;
    const auto second = reservation.reserve(directory, 64, 1024, false,
                                             secondFile, secondAvailable);
    ok = check(second == nativetv::daemon::AppInstallUploadReservation::ReserveResult::busy,
               "a second upload must not reserve the same staging directory") && ok;
    const auto installBusy = reservation.reserve(directory, 64, 1024, true,
                                                  secondFile, secondAvailable);
    ok = check(installBusy == nativetv::daemon::AppInstallUploadReservation::ReserveResult::busy,
               "an active installer job must prevent upload reservation") && ok;

    const std::string stagedPath = directory + "/" + file;
    ok = check(writeFile(stagedPath), "could not create staged upload fixture") && ok;
    ok = check(reservation.touch(file), "touch should renew the matching lease") && ok;
    ok = check(!reservation.touch("upload-0000000000000000.ipk"),
               "touch should reject a different upload") && ok;
    ok = check(reservation.release(directory, file, true, &error),
               "release should remove the owned staged file") && ok;
    ok = check(!reservation.held() && access(stagedPath.c_str(), F_OK) != 0,
               "release should free the lease and delete the staged file") && ok;

    const auto tooLarge = reservation.reserve(directory, 1025, 1024, false,
                                               secondFile, secondAvailable);
    ok = check(tooLarge == nativetv::daemon::AppInstallUploadReservation::ReserveResult::tooLarge,
               "reservation should reject uploads over the configured limit") && ok;

    const auto completed = reservation.reserve(directory, 64, 1024, false,
                                                file, availableBytes);
    ok = check(completed == nativetv::daemon::AppInstallUploadReservation::ReserveResult::reserved,
               "a released hold should allow a later upload") && ok;
    const std::string completedPath = directory + "/" + file;
    ok = check(writeFile(completedPath), "could not create completed upload fixture") && ok;
    ok = check(reservation.complete(file), "completion should transfer staged-file ownership") && ok;
    ok = check(!reservation.held() && access(completedPath.c_str(), F_OK) == 0,
               "completion should free the hold and retain the file for the installer") && ok;
    unlink(completedPath.c_str());

    nativetv::daemon::AppInstallUploadReservation expiring(
        std::chrono::milliseconds(20));
    const auto expiringResult = expiring.reserve(directory, 64, 1024, false,
                                                  file, availableBytes);
    ok = check(expiringResult == nativetv::daemon::AppInstallUploadReservation::ReserveResult::reserved,
               "a new reservation should establish an expiration lease") && ok;
    std::this_thread::sleep_for(std::chrono::milliseconds(35));
    ok = check(!expiring.held() && !expiring.owns(file),
               "an untouched reservation should expire") && ok;
    const auto afterExpiry = expiring.reserve(directory, 64, 1024, false,
                                               secondFile, secondAvailable);
    ok = check(afterExpiry == nativetv::daemon::AppInstallUploadReservation::ReserveResult::reserved,
               "an expired reservation should allow another upload") && ok;
    expiring.release(directory, secondFile, true);

    unlink(stalePath.c_str());
    rmdir(directory.c_str());
    if (!ok) return 1;
    std::cout << "native app install upload reservation checks passed\n";
    return 0;
}
