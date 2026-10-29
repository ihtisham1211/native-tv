#include "daemon/hdmi_seen.hpp"
#include <cerrno>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <set>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

namespace nativetv {
namespace daemon {
namespace {
const size_t MAX_MARKER_BYTES = 64 * 1024;

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
    if (error)
        *error = systemError("create directory", path);
    return false;
}

std::string trim(const std::string &value) {
    size_t first = 0;
    while (first < value.size() &&
           std::isspace(static_cast<unsigned char>(value[first])))
        ++first;
    size_t last = value.size();
    while (last > first && std::isspace(static_cast<unsigned char>(value[last - 1])))
        --last;
    return value.substr(first, last - first);
}

bool readExisting(const std::string &path, std::string *contents,
                  std::set<std::string> *fields, std::string *error) {
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
    if (!S_ISREG(info.st_mode) || info.st_size < 0 ||
        static_cast<unsigned long long>(info.st_size) > MAX_MARKER_BYTES) {
        if (error)
            *error = path + " is not a regular marker file within the size limit";
        close(fd);
        return false;
    }

    contents->clear();
    char buffer[4096];
    bool ok = true;
    for (;;) {
        const ssize_t count = read(fd, buffer, sizeof(buffer));
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
        if (static_cast<size_t>(count) > MAX_MARKER_BYTES ||
            contents->size() > MAX_MARKER_BYTES - static_cast<size_t>(count)) {
            ok = false;
            if (error)
                *error = path + " exceeds the marker size limit";
            break;
        }
        contents->append(buffer, static_cast<size_t>(count));
    }
    const int closeResult = close(fd);
    if (!ok)
        return false;
    if (closeResult != 0) {
        if (error)
            *error = systemError("close", path);
        return false;
    }

    size_t start = 0;
    while (start <= contents->size()) {
        const size_t end = contents->find('\n', start);
        const size_t length = (end == std::string::npos ? contents->size() : end) - start;
        const std::string field = trim(contents->substr(start, length));
        if (!field.empty())
            fields->insert(field);
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    return true;
}

bool writeAll(int fd, const std::string &contents, const std::string &path,
              std::string *error) {
    size_t offset = 0;
    while (offset < contents.size()) {
        const ssize_t written = write(fd, contents.data() + offset, contents.size() - offset);
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

bool atomicWrite(const std::string &path, const std::string &contents,
                 std::string *error) {
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
        ok = writeAll(fd, contents, temporary, error);
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
} // namespace

Json readHdmiSeen(const std::string &path, std::string *error) {
    std::string contents;
    std::set<std::string> fields;
    if (!readExisting(path, &contents, &fields, error))
        return Json::object();
    Json result = Json::object();
    for (const std::string &field : fields)
        result.set(field, Json::flag(true));
    return result;
}

bool recordHdmiSeen(const std::string &path, const Json &diagnostics,
                    std::string *error) {
    if (!diagnostics.isObject()) {
        if (error)
            *error = "HDMI diagnostics are not an object";
        return false;
    }
    std::string previous;
    std::set<std::string> fields;
    if (!readExisting(path, &previous, &fields, error))
        return false;
    bool added = false;
    for (const std::string &key : diagnostics.keys()) {
        if (key != "port" && diagnostics[key].valid())
            added = fields.insert(key).second || added;
    }
    if (!added)
        return true;

    std::string contents;
    for (const std::string &field : fields)
        contents += field + "\n";
    if (contents == previous)
        return true;
    return atomicWrite(path, contents, error);
}

} // namespace daemon
} // namespace nativetv
