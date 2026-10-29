#include "daemon/media_seen.hpp"
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

namespace nativetv {
namespace daemon {
namespace {
std::string directoryOf(const std::string &path) {
    const size_t slash = path.find_last_of('/');
    if (slash == std::string::npos)
        return ".";
    if (slash == 0)
        return "/";
    return path.substr(0, slash);
}

std::string systemError(const std::string &action, const std::string &path) {
    return action + " " + path + ": " + std::strerror(errno);
}

bool ensureDirectory(const std::string &path, std::string *error) {
    if (path.empty() || path == "." || path == "/")
        return true;
    if (mkdir(path.c_str(), 0700) == 0)
        return true;
    if (errno == EEXIST) {
        struct stat info;
        if (stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode))
            return true;
        if (error)
            *error = path + " exists but is not a directory";
        return false;
    }
    if (errno != ENOENT) {
        if (error)
            *error = systemError("create directory", path);
        return false;
    }
    const std::string parent = directoryOf(path);
    if (parent == path || !ensureDirectory(parent, error))
        return false;
    if (mkdir(path.c_str(), 0700) == 0 || errno == EEXIST)
        return true;
    if (error)
        *error = systemError("create directory", path);
    return false;
}

bool alreadyMarked(const std::string &path, bool *marked, std::string *error) {
    *marked = false;
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        if (errno == ENOENT)
            return true;
        if (error)
            *error = systemError("open", path);
        return false;
    }

    struct stat info;
    if (fstat(fd, &info) != 0) {
        if (error)
            *error = systemError("inspect", path);
        close(fd);
        return false;
    }
    if (!S_ISREG(info.st_mode)) {
        if (error)
            *error = path + " is not a regular file";
        close(fd);
        return false;
    }

    char contents[3];
    size_t size = 0;
    bool ok = true;
    while (size < sizeof(contents)) {
        const ssize_t count = read(fd, contents + size, sizeof(contents) - size);
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0) {
            ok = false;
            if (error)
                *error = systemError("read", path);
            break;
        }
        if (count == 0)
            break;
        size += static_cast<size_t>(count);
    }
    const int closeResult = close(fd);
    if (!ok)
        return false;
    if (closeResult != 0) {
        if (error)
            *error = systemError("close", path);
        return false;
    }
    *marked = size == 2 && contents[0] == '1' && contents[1] == '\n';
    return true;
}

bool writeAll(int fd, const char *contents, size_t size, const std::string &path,
              std::string *error) {
    size_t offset = 0;
    while (offset < size) {
        const ssize_t written = write(fd, contents + offset, size - offset);
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0) {
            if (error)
                *error = written == 0 ? "write made no progress for " + path
                                      : systemError("write", path);
            return false;
        }
        offset += static_cast<size_t>(written);
    }
    return true;
}

bool syncDirectory(const std::string &path, std::string *error) {
    const std::string directory = directoryOf(path);
    const int fd = open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) {
        if (error)
            *error = systemError("open directory", directory);
        return false;
    }
    const int result = fsync(fd);
    const int saved = errno;
    close(fd);
    if (result != 0) {
        errno = saved;
        if (error)
            *error = systemError("sync directory", directory);
        return false;
    }
    return true;
}
} // namespace

bool markMediaSeen(const std::string &path, std::string *error) {
    bool marked = false;
    if (!alreadyMarked(path, &marked, error))
        return false;
    if (marked)
        return true;
    if (!ensureDirectory(directoryOf(path), error))
        return false;

    const std::string pattern = path + ".tmp.XXXXXX";
    std::vector<char> name(pattern.begin(), pattern.end());
    name.push_back('\0');
    const int fd = mkstemp(name.data());
    if (fd < 0) {
        if (error)
            *error = systemError("create temporary file for", path);
        return false;
    }
    const std::string temporary(name.data());
    bool ok = fchmod(fd, 0600) == 0;
    if (!ok && error)
        *error = systemError("set permissions on", temporary);
    if (ok)
        ok = writeAll(fd, "1\n", 2, temporary, error);
    if (ok && fsync(fd) != 0) {
        ok = false;
        if (error)
            *error = systemError("sync", temporary);
    }
    if (close(fd) != 0 && ok) {
        ok = false;
        if (error)
            *error = systemError("close", temporary);
    }
    if (!ok) {
        unlink(temporary.c_str());
        return false;
    }
    if (rename(temporary.c_str(), path.c_str()) != 0) {
        if (error)
            *error = systemError("replace", path);
        unlink(temporary.c_str());
        return false;
    }
    return syncDirectory(path, error);
}

} // namespace daemon
} // namespace nativetv
