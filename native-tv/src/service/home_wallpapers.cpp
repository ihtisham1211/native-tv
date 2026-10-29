#include "service/home_wallpapers.hpp"
#include "core/json.hpp"
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

namespace nativetv {
namespace service {
namespace {
const size_t MAX_WALLPAPER_BYTES = 5 * 1024 * 1024;

bool validSlot(const std::string &slot) {
    return slot == "morning" || slot == "afternoon" || slot == "evening" ||
           slot == "night";
}

uint16_t readBigEndian16(const std::string &bytes, size_t offset) {
    return static_cast<uint16_t>(
        (static_cast<uint16_t>(static_cast<unsigned char>(bytes[offset])) << 8) |
        static_cast<unsigned char>(bytes[offset + 1]));
}

bool writeAll(int descriptor, const std::string &bytes) {
    size_t offset = 0;
    while (offset < bytes.size()) {
        const ssize_t count = write(descriptor, bytes.data() + offset,
                                    bytes.size() - offset);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return false;
        offset += static_cast<size_t>(count);
    }
    return true;
}

void setError(std::string *error, const char *message) {
    if (error) *error = message;
}

void setErrnoError(std::string *error) {
    if (error) *error = std::strerror(errno);
}

std::string wallpaperPath(const std::string &slot, const std::string &dataRoot) {
    const std::string root = dataRoot.empty() ? "/var/lib/tvweb" : dataRoot;
    const char *separator = !root.empty() && root[root.size() - 1] == '/' ? "" : "/";
    return root + separator + "home-wallpaper-uploads/" + slot + ".jpg";
}

std::string wallpaperDirectory(const std::string &dataRoot) {
    const std::string root = dataRoot.empty() ? "/var/lib/tvweb" : dataRoot;
    const char *separator = !root.empty() && root[root.size() - 1] == '/' ? "" : "/";
    return root + separator + "home-wallpaper-uploads";
}

bool regularFileAt(int directory, const std::string &name, struct stat &info) {
    const int descriptor = openat(directory, name.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0) return false;
    const bool regular = fstat(descriptor, &info) == 0 && S_ISREG(info.st_mode);
    close(descriptor);
    return regular;
}

int64_t modifiedMilliseconds(const struct stat &info) {
#if defined(__APPLE__)
    return static_cast<int64_t>(info.st_mtimespec.tv_sec) * 1000 +
           info.st_mtimespec.tv_nsec / 1000000;
#else
    return static_cast<int64_t>(info.st_mtim.tv_sec) * 1000 +
           info.st_mtim.tv_nsec / 1000000;
#endif
}

std::string makeEtag(size_t size, const struct stat &info) {
    std::ostringstream etag;
    etag << '"' << std::hex << size << '-' << modifiedMilliseconds(info) << '"';
    return etag.str();
}
} // namespace

HomeWallpaperFile readHomeWallpaper(const std::string &slot,
                                    const std::string &dataRoot) {
    HomeWallpaperFile result;
    if (!validSlot(slot))
        return result;
    result.validSlot = true;

    const int descriptor = open(wallpaperPath(slot, dataRoot).c_str(),
                                 O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0)
        return result;
    struct stat info;
    bool ok = fstat(descriptor, &info) == 0 && S_ISREG(info.st_mode) &&
              info.st_size >= 0 &&
              static_cast<unsigned long long>(info.st_size) <= MAX_WALLPAPER_BYTES;
    if (!ok) {
        close(descriptor);
        return result;
    }

    result.bytes.reserve(static_cast<size_t>(info.st_size));
    char buffer[16384];
    for (;;) {
        const ssize_t count = read(descriptor, buffer, sizeof(buffer));
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0 || static_cast<size_t>(count) > MAX_WALLPAPER_BYTES ||
            result.bytes.size() > MAX_WALLPAPER_BYTES - static_cast<size_t>(count)) {
            ok = false;
            break;
        }
        if (count == 0)
            break;
        result.bytes.append(buffer, static_cast<size_t>(count));
    }
    close(descriptor);
    if (!ok) {
        result.bytes.clear();
        result.readError = true;
        return result;
    }

    result.found = true;
    result.etag = makeEtag(result.bytes.size(), info);
    return result;
}

Json homeWallpaperStatus(const std::string &dataRoot) {
    static const char *slots[] = {"morning", "afternoon", "evening", "night"};
    Json availableSlots = Json::object();
    std::string revision;
    bool available = false;
    bool uploading = false;
    const int directory = open(wallpaperDirectory(dataRoot).c_str(),
                               O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
    for (const char *slot : slots) {
        struct stat info = {};
        const std::string image = std::string(slot) + ".jpg";
        const bool exists = directory >= 0 && regularFileAt(directory, image, info);
        availableSlots.set(slot, Json::flag(exists));
        if (exists) {
            available = true;
            revision += std::to_string(modifiedMilliseconds(info)) + "-" +
                        std::to_string(static_cast<unsigned long long>(info.st_size));
        } else {
            revision += "0";
        }
        struct stat partial = {};
        if (directory >= 0 && regularFileAt(directory, image + ".part", partial))
            uploading = true;
        if (slot != slots[3]) revision.push_back('.');
    }
    if (directory >= 0) close(directory);
    Json result = Json::object();
    result.set("available", Json::flag(available));
    result.set("uploading", Json::flag(uploading));
    result.set("slots", availableSlots);
    result.set("revision", Json::text(revision));
    return result;
}

bool validHomeWallpaperJpeg(const std::string &bytes) {
    if (bytes.size() < 12 || bytes.size() > MAX_WALLPAPER_BYTES ||
        static_cast<unsigned char>(bytes[0]) != 0xff ||
        static_cast<unsigned char>(bytes[1]) != 0xd8 ||
        static_cast<unsigned char>(bytes[bytes.size() - 2]) != 0xff ||
        static_cast<unsigned char>(bytes[bytes.size() - 1]) != 0xd9)
        return false;

    size_t at = 2;
    unsigned components = 0;
    bool scanned = false;
    bool quantised = false;
    bool huffman = false;
    while (at + 2 <= bytes.size()) {
        if (static_cast<unsigned char>(bytes[at++]) != 0xff)
            return false;
        while (at < bytes.size() &&
               static_cast<unsigned char>(bytes[at]) == 0xff)
            ++at;
        if (at >= bytes.size())
            return false;
        const unsigned marker = static_cast<unsigned char>(bytes[at++]);
        if (marker == 0xd9)
            return at == bytes.size() && components && scanned && quantised && huffman;
        if (at + 2 > bytes.size())
            return false;
        const size_t length = readBigEndian16(bytes, at);
        if (length < 2 || length > bytes.size() - at)
            return false;

        if (marker == 0xc0 || marker == 0xc2) {
            if (length < 8)
                return false;
            const unsigned precision = static_cast<unsigned char>(bytes[at + 2]);
            const unsigned height = readBigEndian16(bytes, at + 3);
            const unsigned width = readBigEndian16(bytes, at + 5);
            const unsigned count = static_cast<unsigned char>(bytes[at + 7]);
            if (components || precision != 8 || (count != 1 && count != 3) ||
                length != 8 + 3 * count || !width || !height || width > 1920 ||
                height > 1080)
                return false;
            components = count;
        }
        if (marker == 0xdb) {
            if (length < 67) return false;
            quantised = true;
        }
        if (marker == 0xc4) {
            if (length < 20) return false;
            huffman = true;
        }
        if (marker == 0xda) {
            if (length < 3)
                return false;
            const unsigned scanComponents = static_cast<unsigned char>(bytes[at + 2]);
            if (!components || !quantised || !huffman || !scanComponents ||
                scanComponents > components || length != 6 + 2 * scanComponents)
                return false;
            at += length;
            const size_t scanStart = at;
            while (at < bytes.size()) {
                if (static_cast<unsigned char>(bytes[at]) != 0xff) {
                    ++at;
                    continue;
                }
                if (at + 1 >= bytes.size())
                    return false;
                const unsigned next = static_cast<unsigned char>(bytes[at + 1]);
                if (next == 0 || (next >= 0xd0 && next <= 0xd7)) {
                    at += 2;
                    continue;
                }
                break;
            }
            if (at == scanStart)
                return false;
            scanned = true;
            continue;
        }
        at += length;
    }
    return false;
}

bool removeHomeWallpapers(const std::string &dataRoot, std::string *error) {
    const std::string directoryPath = wallpaperDirectory(dataRoot);
    const int directory = open(directoryPath.c_str(),
                               O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
    if (directory < 0) {
        if (errno == ENOENT) {
            if (error) error->clear();
            return true;
        }
        setErrnoError(error);
        return false;
    }

    static const char *slots[] = {"morning", "afternoon", "evening", "night"};
    for (const char *slot : slots) {
        const std::string image = std::string(slot) + ".jpg";
        const std::string staging = image + ".part";
        const char *files[] = {image.c_str(), staging.c_str()};
        for (const char *file : files) {
            if (unlinkat(directory, file, 0) != 0 && errno != ENOENT) {
                setErrnoError(error);
                close(directory);
                return false;
            }
        }
    }
    (void)fsync(directory);
    close(directory);
    if (error) error->clear();
    return true;
}

HomeWallpaperSaveResult saveHomeWallpaper(const std::string &slot,
                                          const std::string &dataRoot,
                                          const std::string &bytes,
                                          std::string *error) {
    if (!validSlot(slot)) {
        setError(error, "unknown wallpaper slot");
        return HomeWallpaperSaveResult::invalidSlot;
    }
    if (!validHomeWallpaperJpeg(bytes)) {
        setError(error, "Choose a valid JPEG up to 1920 × 1080 pixels.");
        return HomeWallpaperSaveResult::invalidImage;
    }

    const std::string directoryPath = wallpaperDirectory(dataRoot);
    if (mkdir(directoryPath.c_str(), 0755) != 0 && errno != EEXIST) {
        setErrnoError(error);
        return HomeWallpaperSaveResult::ioError;
    }
    const int directory = open(directoryPath.c_str(),
                               O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
    if (directory < 0) {
        setErrnoError(error);
        return HomeWallpaperSaveResult::ioError;
    }

    struct statvfs space = {};
    if (statvfs(directoryPath.c_str(), &space) != 0) {
        close(directory);
        setErrnoError(error);
        return HomeWallpaperSaveResult::insufficientSpace;
    }
    const unsigned long long unit = space.f_frsize ? space.f_frsize : space.f_bsize;
    const unsigned long long blocks = space.f_bavail;
    const unsigned long long freeBytes = unit && blocks >
        std::numeric_limits<unsigned long long>::max() / unit
        ? std::numeric_limits<unsigned long long>::max() : blocks * unit;
    const unsigned long long required =
        static_cast<unsigned long long>(bytes.size()) + 5 * 1024 * 1024;
    if (freeBytes < required) {
        close(directory);
        setError(error, "Not enough confirmed free space for the wallpaper.");
        return HomeWallpaperSaveResult::insufficientSpace;
    }

    const std::string staging = slot + ".jpg.part";
    const std::string destination = slot + ".jpg";
    const int output = openat(directory, staging.c_str(),
                              O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW,
                              0600);
    if (output < 0) {
        close(directory);
        setErrnoError(error);
        return HomeWallpaperSaveResult::ioError;
    }
    struct stat stagingInfo = {};
    bool saved = fstat(output, &stagingInfo) == 0;
    int operationError = saved ? 0 : errno;
    if (saved && !S_ISREG(stagingInfo.st_mode)) {
        saved = false;
        operationError = EINVAL;
    }
    if (saved && fchmod(output, 0600) != 0) {
        saved = false;
        operationError = errno;
    }
    if (saved && !writeAll(output, bytes)) {
        saved = false;
        operationError = errno ? errno : EIO;
    }
    if (saved && fsync(output) != 0) {
        saved = false;
        operationError = errno;
    }
    close(output);
    if (saved && renameat(directory, staging.c_str(), directory,
                          destination.c_str()) != 0) {
        saved = false;
        operationError = errno;
    }
    if (saved)
        (void)fsync(directory);
    if (!saved) {
        (void)unlinkat(directory, staging.c_str(), 0);
        close(directory);
        errno = operationError ? operationError : EIO;
        setErrnoError(error);
        return HomeWallpaperSaveResult::ioError;
    }
    close(directory);
    if (error) error->clear();
    return HomeWallpaperSaveResult::saved;
}

} // namespace service
} // namespace nativetv
