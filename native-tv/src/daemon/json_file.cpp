#include "daemon/json_file.hpp"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sstream>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace nativetv {
namespace {
enum class ReadStatus { Ok, Missing, Error };
std::atomic<unsigned long> quarantineSequence(0);

std::string directoryOf(const std::string &path) {
    size_t slash = path.find_last_of('/');
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
    std::string parent = directoryOf(path);
    if (parent == path || !ensureDirectory(parent, error))
        return false;
    if (mkdir(path.c_str(), 0700) == 0 || errno == EEXIST)
        return true;
    if (error)
        *error = systemError("create directory", path);
    return false;
}

bool syncDirectory(const std::string &path, std::string *error) {
    std::string directory = directoryOf(path);
    int fd = open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) {
        if (error)
            *error = systemError("open directory", directory);
        return false;
    }
    int result = fsync(fd);
    int saved = errno;
    close(fd);
    if (result != 0) {
        errno = saved;
        if (error)
            *error = systemError("sync directory", directory);
        return false;
    }
    return true;
}

bool atomicWrite(const std::string &path, const std::string &contents, std::string *error,
                 const std::function<void()> &beforeCommit = std::function<void()>()) {
    if (!ensureDirectory(directoryOf(path), error))
        return false;
    std::string pattern = path + ".tmp.XXXXXX";
    std::vector<char> name(pattern.begin(), pattern.end());
    name.push_back('\0');
    int fd = mkstemp(name.data());
    if (fd < 0) {
        if (error)
            *error = systemError("create temporary file for", path);
        return false;
    }
    std::string temporary(name.data());
    bool ok = fchmod(fd, 0600) == 0;
    if (!ok && error)
        *error = systemError("set permissions on", temporary);
    size_t offset = 0;
    while (ok && offset < contents.size()) {
        ssize_t written = write(fd, contents.data() + offset, contents.size() - offset);
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0) {
            ok = false;
            if (error)
                *error = written == 0 ? "write made no progress for " + temporary
                                      : systemError("write", temporary);
            break;
        }
        offset += static_cast<size_t>(written);
    }
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
    if (beforeCommit) {
        try {
            beforeCommit();
        } catch (...) {
            if (error)
                *error = "pre-commit hook failed for " + path;
            unlink(temporary.c_str());
            return false;
        }
    }
    if (rename(temporary.c_str(), path.c_str()) != 0) {
        if (error)
            *error = systemError("replace", path);
        unlink(temporary.c_str());
        return false;
    }
    return syncDirectory(path, error);
}

ReadStatus readFile(const std::string &path, size_t limit, std::string *contents,
                    std::string *error) {
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        if (errno == ENOENT)
            return ReadStatus::Missing;
        if (error)
            *error = systemError("open", path);
        return ReadStatus::Error;
    }
    struct stat info;
    if (fstat(fd, &info) != 0) {
        if (error)
            *error = systemError("inspect", path);
        close(fd);
        return ReadStatus::Error;
    }
    if (!S_ISREG(info.st_mode)) {
        if (error)
            *error = path + " is not a regular file";
        close(fd);
        return ReadStatus::Error;
    }
    if (info.st_size < 0 || static_cast<unsigned long long>(info.st_size) > limit) {
        if (error)
            *error = path + " exceeds the JSON file size limit";
        close(fd);
        return ReadStatus::Error;
    }
    contents->clear();
    contents->reserve(static_cast<size_t>(info.st_size));
    char buffer[16384];
    while (true) {
        ssize_t count = read(fd, buffer, sizeof(buffer));
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0) {
            if (error)
                *error = systemError("read", path);
            close(fd);
            return ReadStatus::Error;
        }
        if (count == 0)
            break;
        if (static_cast<size_t>(count) > limit ||
            contents->size() > limit - static_cast<size_t>(count)) {
            if (error)
                *error = path + " exceeds the JSON file size limit";
            close(fd);
            return ReadStatus::Error;
        }
        contents->append(buffer, static_cast<size_t>(count));
    }
    if (close(fd) != 0) {
        if (error)
            *error = systemError("close", path);
        return ReadStatus::Error;
    }
    return ReadStatus::Ok;
}

bool quarantine(const std::string &path, std::string *error) {
    std::ostringstream suffix;
    suffix << path << ".corrupt." << static_cast<long>(getpid()) << "."
           << quarantineSequence.fetch_add(1);
    std::string destination = suffix.str();
    if (rename(path.c_str(), destination.c_str()) != 0) {
        if (error)
            *error = systemError("quarantine", path);
        return false;
    }
    return syncDirectory(path, error);
}

} // namespace

JsonFile::JsonFile(std::string path, size_t maxBytes)
    : path_(std::move(path)), maxBytes_(std::min(maxBytes, static_cast<size_t>(8 * 1024 * 1024))) {}

JsonFileResult JsonFile::load() const {
    JsonFileResult result;
    std::string contents, error;
    Json value;
    ReadStatus current = readFile(path_, maxBytes_, &contents, &error);
    if (current == ReadStatus::Ok) {
        value = Json::parse(contents);
        if (value.valid()) {
            result.value = value;
            result.found = true;
            return result;
        }
        error = path_ + " contains invalid JSON";
        if (!quarantine(path_, &result.error))
            return result;
        result.error = error;
    } else if (current == ReadStatus::Error) {
        result.error = error;
        return result;
    }

    const std::string backup = path_ + ".last-good";
    std::string backupContents, backupError;
    ReadStatus previous = readFile(backup, maxBytes_, &backupContents, &backupError);
    if (previous == ReadStatus::Ok) {
        value = Json::parse(backupContents);
        if (value.valid()) {
            result.value = value;
            result.found = true;
            result.recovered = true;
            result.error.clear();
            std::string restoreError;
            if (!atomicWrite(path_, backupContents, &restoreError))
                result.error = restoreError;
            return result;
        }
        backupError = backup + " contains invalid JSON";
        if (!quarantine(backup, &result.error))
            return result;
    } else if (previous == ReadStatus::Error) {
        result.error = backupError;
        return result;
    }
    if (result.error.empty() && !backupError.empty())
        result.error = backupError;
    return result;
}

bool JsonFile::save(const Json &value, std::string *error,
                    const std::function<void()> &beforeCommit) const {
    if (!value.valid()) {
        if (error)
            *error = "cannot save an invalid JSON value";
        return false;
    }
    std::string contents = value.dump() + "\n";
    if (contents.size() > maxBytes_) {
        if (error)
            *error = "JSON value exceeds the file size limit";
        return false;
    }

    std::string previousContents, readError;
    ReadStatus previous = readFile(path_, maxBytes_, &previousContents, &readError);
    if (previous == ReadStatus::Error) {
        if (error)
            *error = readError;
        return false;
    }
    if (previous == ReadStatus::Ok) {
        if (Json::parse(previousContents).valid()) {
            if (!atomicWrite(path_ + ".last-good", previousContents, error))
                return false;
        } else if (!quarantine(path_, error)) {
            return false;
        }
    }
    return atomicWrite(path_, contents, error, beforeCommit);
}

const std::string &JsonFile::path() const {
    return path_;
}
} // namespace nativetv
