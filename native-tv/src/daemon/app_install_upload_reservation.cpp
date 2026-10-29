#include "daemon/app_install_upload_reservation.hpp"

#include "service/app_install_space.hpp"

#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <iomanip>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace nativetv {
namespace daemon {
namespace {

bool ensureDirectory(const std::string &path, std::string *error) {
    if (path.empty()) {
        if (error) *error = "install staging directory is empty";
        return false;
    }
    struct stat info{};
    if (stat(path.c_str(), &info) == 0) {
        if (S_ISDIR(info.st_mode)) return true;
        if (error) *error = "install staging path is not a directory";
        return false;
    }
    if (errno != ENOENT) {
        if (error) *error = "inspect install staging directory: " +
                            std::string(std::strerror(errno));
        return false;
    }
    const size_t slash = path.find_last_of('/');
    const std::string parent = slash == std::string::npos ? "." :
                               slash == 0 ? "/" : path.substr(0, slash);
    if (parent != path && !ensureDirectory(parent, error)) return false;
    if (mkdir(path.c_str(), 0700) == 0 || errno == EEXIST) {
        if (stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode)) return true;
    }
    if (error) *error = "create install staging directory: " +
                        std::string(std::strerror(errno));
    return false;
}

bool validUploadName(const std::string &file) {
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

bool createUploadName(std::string &file, std::string *error) {
    unsigned char random[8]{};
    const int source = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (source < 0) {
        if (error) *error = "open random source: " + std::string(std::strerror(errno));
        return false;
    }
    size_t offset = 0;
    while (offset < sizeof(random)) {
        const ssize_t count = read(source, random + offset, sizeof(random) - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) {
            const int savedErrno = count < 0 ? errno : EIO;
            close(source);
            if (error) *error = "read random source: " +
                                std::string(std::strerror(savedErrno));
            return false;
        }
        offset += static_cast<size_t>(count);
    }
    close(source);

    std::ostringstream name;
    name << "upload-" << std::hex << std::setfill('0');
    for (unsigned char value : random)
        name << std::setw(2) << static_cast<unsigned>(value);
    name << ".ipk";
    file = name.str();
    return true;
}

bool clearStaging(int directory, std::string *error) {
    const int duplicate = dup(directory);
    if (duplicate < 0) {
        if (error) *error = "read install staging directory: " +
                            std::string(std::strerror(errno));
        return false;
    }
    DIR *entries = fdopendir(duplicate);
    if (!entries) {
        const int savedErrno = errno;
        close(duplicate);
        if (error) *error = "read install staging directory: " +
                            std::string(std::strerror(savedErrno));
        return false;
    }

    bool ok = true;
    for (;;) {
        errno = 0;
        dirent *entry = readdir(entries);
        if (!entry) {
            if (errno != 0) {
                if (error) *error = "read install staging directory: " +
                                    std::string(std::strerror(errno));
                ok = false;
            }
            break;
        }
        const std::string name = entry->d_name;
        if (name == "." || name == "..") continue;
        if (unlinkat(directory, name.c_str(), 0) == 0 || errno == ENOENT)
            continue;
        if (error) *error = "clear install staging file: " +
                            std::string(std::strerror(errno));
        ok = false;
        break;
    }
    closedir(entries);
    return ok;
}

bool unlinkStagedFile(const std::string &directory, const std::string &file,
                      std::string *error) {
    if (!validUploadName(file)) {
        if (error) *error = "invalid staged upload file name";
        return false;
    }
    const int descriptor = open(directory.c_str(), O_RDONLY | O_DIRECTORY |
                                O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0) {
        if (errno == ENOENT) return true;
        if (error) *error = "open install staging directory: " +
                            std::string(std::strerror(errno));
        return false;
    }
    const bool ok = unlinkat(descriptor, file.c_str(), 0) == 0 || errno == ENOENT;
    const int savedErrno = errno;
    close(descriptor);
    if (!ok && error)
        *error = "remove staged upload: " + std::string(std::strerror(savedErrno));
    return ok;
}

} // namespace

AppInstallUploadReservation::AppInstallUploadReservation(
    std::chrono::milliseconds holdDuration)
    : holdDuration_(holdDuration) {}

AppInstallUploadReservation::ReserveResult AppInstallUploadReservation::reserve(
    const std::string &stagingDirectory, uint64_t bytes, uint64_t maximumBytes,
    bool installBusy, std::string &file, uint64_t &availableBytes,
    std::string *error) {
    file.clear();
    availableBytes = 0;
    if (error) error->clear();
    if (bytes == 0 || bytes > maximumBytes ||
        bytes > UINT64_MAX / 3)
        return ReserveResult::tooLarge;

    std::lock_guard<std::mutex> lock(mutex_);
    expireLocked(std::chrono::steady_clock::now());
    if (installBusy || !file_.empty()) return ReserveResult::busy;
    if (!ensureDirectory(stagingDirectory, error))
        return ReserveResult::failed;

    uint64_t currentAvailable = 0;
    if (!service::AppInstallSpace::availableBytes(stagingDirectory, currentAvailable, error))
        return ReserveResult::failed;
    availableBytes = currentAvailable;
    const uint64_t required = bytes * 3;
    if (currentAvailable < required) return ReserveResult::noSpace;

    const int directory = open(stagingDirectory.c_str(), O_RDONLY | O_DIRECTORY |
                               O_CLOEXEC | O_NOFOLLOW);
    if (directory < 0) {
        if (error) *error = "open install staging directory: " +
                            std::string(std::strerror(errno));
        return ReserveResult::failed;
    }
    const bool cleared = clearStaging(directory, error);
    close(directory);
    if (!cleared) return ReserveResult::failed;
    if (!createUploadName(file, error)) return ReserveResult::failed;

    file_ = file;
    touchedAt_ = std::chrono::steady_clock::now();
    return ReserveResult::reserved;
}

bool AppInstallUploadReservation::touch(const std::string &file) {
    std::lock_guard<std::mutex> lock(mutex_);
    expireLocked(std::chrono::steady_clock::now());
    if (file.empty() || file_ != file) return false;
    touchedAt_ = std::chrono::steady_clock::now();
    return true;
}

bool AppInstallUploadReservation::held() {
    std::lock_guard<std::mutex> lock(mutex_);
    expireLocked(std::chrono::steady_clock::now());
    return !file_.empty();
}

bool AppInstallUploadReservation::owns(const std::string &file) {
    std::lock_guard<std::mutex> lock(mutex_);
    expireLocked(std::chrono::steady_clock::now());
    return !file.empty() && file_ == file;
}

bool AppInstallUploadReservation::release(const std::string &stagingDirectory,
                                          const std::string &file, bool removeFile,
                                          std::string *error) {
    if (error) error->clear();
    if (!validUploadName(file)) {
        if (error) *error = "invalid staged upload file name";
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (file_ == file) file_.clear();
    }
    return !removeFile || unlinkStagedFile(stagingDirectory, file, error);
}

bool AppInstallUploadReservation::complete(const std::string &file) {
    std::lock_guard<std::mutex> lock(mutex_);
    expireLocked(std::chrono::steady_clock::now());
    if (file.empty() || file_ != file) return false;
    file_.clear();
    return true;
}

void AppInstallUploadReservation::expireLocked(
    std::chrono::steady_clock::time_point now) {
    if (!file_.empty() && now - touchedAt_ >= holdDuration_) file_.clear();
}

} // namespace daemon
} // namespace nativetv
