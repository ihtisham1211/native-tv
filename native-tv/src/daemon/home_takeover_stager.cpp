#include "daemon/home_takeover_stager.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>
#include <utility>
#include <vector>

namespace nativetv {
namespace daemon {
namespace {
const size_t kMaximumStagedFileBytes = 16 * 1024 * 1024;
const size_t kMaximumStagedFiles = 1000;
// sam reads "main" and starts it as root, without the jailer, with no APPID in
// its environment. LG's SDL names the client's window from APPID, and without
// it the window is never shown (webOS 11.2.0), so the start script sets it.
const char *const kAppInfo =
    "{\n"
    "  \"id\": \"com.webos.app.home\",\n"
    "  \"version\": \"1.0.0\",\n"
    "  \"vendor\": \"tvweb\",\n"
    "  \"type\": \"native\",\n"
    "  \"main\": \"nativetv-home\",\n"
    "  \"title\": \"Home\",\n"
    "  \"icon\": \"assets/icon80.png\",\n"
    "  \"largeIcon\": \"assets/icon130.png\",\n"
    "  \"iconColor\": \"#000000\",\n"
    "  \"bgColor\": \"#000000\",\n"
    "  \"resolution\": \"1920x1080\",\n"
    "  \"visible\": false,\n"
    "  \"disableBackHistoryAPI\": true,\n"
    "  \"appCategory\": [\n"
    "    \"home\"\n"
    "  ]\n"
    "}\n";
const char *const kStartScript =
    "#!/bin/sh\n"
    "# Native Tv Home, started by sam as LG's Home (staged by the Native Tv server).\n"
    "export APPID=com.webos.app.home\n"
    "exec \"$(dirname \"$0\")/nativetv-tv\" \"$@\"\n";
// The dashboard app's own files, which Home does not take: its installer, its
// package manifest and its manifest, which is replaced by kAppInfo.
const char *const kSourceOnly[] = {"install-app.sh", "packageinfo.json", "appinfo.json"};

std::string join(const std::string &directory, const std::string &name) {
    return directory + (directory.empty() || directory.back() == '/' ? "" : "/") + name;
}

bool ensureDirectory(const std::string &path, std::string &error) {
    if (path.empty() || path == "/") return true;
    struct stat info;
    if (lstat(path.c_str(), &info) == 0) {
        if (S_ISDIR(info.st_mode)) return true;
        error = path + " is not a directory";
        return false;
    }
    if (errno != ENOENT) {
        error = "could not inspect " + path + ": " + std::strerror(errno);
        return false;
    }
    const size_t slash = path.find_last_of('/');
    if (slash != std::string::npos && slash > 0 &&
        !ensureDirectory(path.substr(0, slash), error)) return false;
    if (mkdir(path.c_str(), 0755) == 0 || errno == EEXIST) {
        if (lstat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode)) return true;
    }
    error = "could not create " + path + ": " + std::strerror(errno);
    return false;
}

bool readRegularFile(const std::string &path, std::string &contents,
                     std::string &error) {
    struct stat info;
    if (lstat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode)) {
        error = "not a regular file: " + path;
        return false;
    }
    if (info.st_size < 0 || static_cast<uint64_t>(info.st_size) > kMaximumStagedFileBytes) {
        error = "file exceeds the Home app staging limit: " + path;
        return false;
    }
    std::ifstream input(path.c_str(), std::ios::in | std::ios::binary);
    if (!input) {
        error = "could not read " + path;
        return false;
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    if (!input.good() && !input.eof()) {
        error = "could not read " + path;
        return false;
    }
    contents = buffer.str();
    if (contents.size() > kMaximumStagedFileBytes) {
        error = "file exceeds the Home app staging limit: " + path;
        return false;
    }
    return true;
}

// Written beside the file and renamed over it: a client started from the old
// copy keeps the inode it mapped. `mtime` (seconds, 0 for none) is the source's,
// so the next stage can tell an unchanged copy without reading it.
bool writeAtomic(const std::string &path, const std::string &contents, mode_t mode,
                 time_t mtime, std::string &error) {
    const std::string temporary = path + ".new";
    (void)unlink(temporary.c_str());
    int flags = O_WRONLY | O_CREAT | O_EXCL;
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    const int descriptor = open(temporary.c_str(), flags, 0600);
    if (descriptor < 0) {
        error = "could not create " + temporary + ": " + std::strerror(errno);
        return false;
    }
    size_t offset = 0;
    while (offset < contents.size()) {
        const ssize_t written = write(descriptor, contents.data() + offset,
                                      contents.size() - offset);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) {
            error = "could not write " + temporary + ": " + std::strerror(errno);
            close(descriptor);
            unlink(temporary.c_str());
            return false;
        }
        offset += static_cast<size_t>(written);
    }
    if (fchmod(descriptor, mode) != 0 || fsync(descriptor) != 0) {
        error = "could not finish " + temporary + ": " + std::strerror(errno);
        close(descriptor);
        unlink(temporary.c_str());
        return false;
    }
    if (mtime > 0) {
        struct timeval times[2];
        times[0].tv_sec = times[1].tv_sec = mtime;
        times[0].tv_usec = times[1].tv_usec = 0;
        (void)futimes(descriptor, times);
    }
    if (close(descriptor) != 0) {
        error = "could not close " + temporary + ": " + std::strerror(errno);
        unlink(temporary.c_str());
        return false;
    }
    if (rename(temporary.c_str(), path.c_str()) != 0) {
        error = "could not install " + path + ": " + std::strerror(errno);
        unlink(temporary.c_str());
        return false;
    }
    return true;
}

bool writeIfChanged(const std::string &destination, const std::string &contents,
                    mode_t mode, time_t mtime, bool &changed, std::string &error) {
    struct stat info;
    if (lstat(destination.c_str(), &info) == 0) {
        if (!S_ISREG(info.st_mode)) {
            error = "staged path is not a regular file: " + destination;
            return false;
        }
        std::string existing;
        if (!readRegularFile(destination, existing, error)) return false;
        if (existing == contents) {
            // Same bytes: the mode and time are put right without a new file.
            if ((info.st_mode & 07777) != mode) (void)chmod(destination.c_str(), mode);
            if (mtime > 0 && info.st_mtime != mtime) {
                struct timeval times[2];
                times[0].tv_sec = times[1].tv_sec = mtime;
                times[0].tv_usec = times[1].tv_usec = 0;
                (void)utimes(destination.c_str(), times);
            }
            return true;
        }
    } else if (errno != ENOENT) {
        error = "could not inspect " + destination + ": " + std::strerror(errno);
        return false;
    }
    if (!writeAtomic(destination, contents, mode, mtime, error)) return false;
    changed = true;
    return true;
}

// The client is about 7.5 MB in 109 files: a copy with the source's size, time
// and mode is taken as current, so a start does not read it all twice.
bool copyIfChanged(const std::string &source, const std::string &destination,
                   mode_t mode, bool &changed, std::string &error) {
    struct stat sourceInfo;
    struct stat stagedInfo;
    if (lstat(source.c_str(), &sourceInfo) != 0 || !S_ISREG(sourceInfo.st_mode)) {
        error = "not a regular file: " + source;
        return false;
    }
    if (lstat(destination.c_str(), &stagedInfo) == 0 && S_ISREG(stagedInfo.st_mode) &&
        stagedInfo.st_size == sourceInfo.st_size &&
        stagedInfo.st_mtime == sourceInfo.st_mtime &&
        (stagedInfo.st_mode & 07777) == mode)
        return true;
    std::string contents;
    if (!readRegularFile(source, contents, error)) return false;
    return writeIfChanged(destination, contents, mode, sourceInfo.st_mtime, changed, error);
}

bool safeName(const std::string &name) {
    if (name.empty() || name == "." || name == "..") return false;
    for (const char c : name)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '.' || c == '_' || c == '-' || c == '+'))
            return false;
    return true;
}

bool listDirectory(const std::string &path, std::vector<std::string> &names,
                   std::string &error) {
    DIR *directory = opendir(path.c_str());
    if (!directory) {
        error = "could not read " + path + ": " + std::strerror(errno);
        return false;
    }
    errno = 0;
    while (dirent *entry = readdir(directory)) {
        const std::string name(entry->d_name);
        if (name != "." && name != "..") names.push_back(name);
        errno = 0;
    }
    const int readError = errno;
    closedir(directory);
    if (readError != 0) {
        error = "could not read " + path;
        return false;
    }
    std::sort(names.begin(), names.end());
    return true;
}

// The client's files, as paths relative to `root`: regular files only, in
// directories of their own, with names runtime_payload.py would give them.
bool clientFiles(const std::string &root, const std::string &relative,
                 std::vector<std::string> &files, std::string &error) {
    std::vector<std::string> names;
    if (!listDirectory(join(root, relative), names, error)) return false;
    for (const std::string &name : names) {
        if (relative.empty() &&
            std::find(std::begin(kSourceOnly), std::end(kSourceOnly), name) != std::end(kSourceOnly))
            continue;
        if (!safeName(name) || name == kHomeStartScript) {
            error = "invalid Native Tv app filename: " + join(relative, name);
            return false;
        }
        const std::string path = relative.empty() ? name : relative + "/" + name;
        struct stat info;
        if (lstat(join(root, path).c_str(), &info) != 0) {
            error = "could not inspect " + join(root, path) + ": " + std::strerror(errno);
            return false;
        }
        if (S_ISDIR(info.st_mode)) {
            if (!clientFiles(root, path, files, error)) return false;
        } else if (S_ISREG(info.st_mode)) {
            files.push_back(path);
        } else {
            error = "not a regular file: " + join(root, path);
            return false;
        }
        if (files.size() > kMaximumStagedFiles) {
            error = "the Native Tv app has more files than Home stages";
            return false;
        }
    }
    return true;
}

// What an earlier stage left that this one does not have: the web stand-in of
// earlier releases (index.html, its snapshot), or a file a release dropped.
void removeOthers(const std::string &root, const std::string &relative,
                  const std::set<std::string> &wanted, bool &changed) {
    std::vector<std::string> names;
    std::string ignored;
    if (!listDirectory(join(root, relative), names, ignored)) return;
    for (const std::string &name : names) {
        const std::string path = relative.empty() ? name : relative + "/" + name;
        const std::string full = join(root, path);
        struct stat info;
        if (lstat(full.c_str(), &info) != 0) continue;
        if (S_ISDIR(info.st_mode)) {
            removeOthers(root, path, wanted, changed);
            if (rmdir(full.c_str()) == 0) changed = true;
        } else if (!wanted.count(path) && unlink(full.c_str()) == 0) {
            changed = true;
        }
    }
}

Json failed(const std::string &error) {
    Json result = Json::object();
    result.set("ok", Json::flag(false));
    result.set("error", Json::text(error));
    return result;
}
} // namespace

HomeTakeoverStager::HomeTakeoverStager(std::string sourceRoot, std::string stagingRoot)
    : sourceRoot_(std::move(sourceRoot)), stagingRoot_(std::move(stagingRoot)) {}

Json HomeTakeoverStager::stage() const {
    std::string error;
    struct stat info;
    if (stat(join(sourceRoot_, kHomeClientExecutable).c_str(), &info) != 0 ||
        !S_ISREG(info.st_mode))
        return failed("the Native Tv app's files are not on this TV (" +
                      join(sourceRoot_, kHomeClientExecutable) + ")");
    std::vector<std::string> files;
    if (!clientFiles(sourceRoot_, "", files, error) || !ensureDirectory(stagingRoot_, error))
        return failed(error);

    bool changed = false;
    std::set<std::string> wanted{"appinfo.json", kHomeStartScript};
    for (const std::string &file : files) {
        const size_t slash = file.find_last_of('/');
        if (slash != std::string::npos &&
            !ensureDirectory(join(stagingRoot_, file.substr(0, slash)), error))
            return failed(error);
        const mode_t mode = file == kHomeClientExecutable ? 0755 : 0644;
        if (!copyIfChanged(join(sourceRoot_, file), join(stagingRoot_, file), mode, changed, error))
            return failed(error);
        wanted.insert(file);
    }
    if (!writeIfChanged(join(stagingRoot_, kHomeStartScript), kStartScript, 0755, 0, changed,
                        error))
        return failed(error);
    removeOthers(stagingRoot_, "", wanted, changed);
    // Last, so sam never reads a manifest whose files are not all there.
    if (!writeIfChanged(join(stagingRoot_, "appinfo.json"), kAppInfo, 0644, 0, changed, error))
        return failed(error);

    Json result = Json::object();
    result.set("ok", Json::flag(true));
    result.set("changed", Json::flag(changed));
    return result;
}

} // namespace daemon
} // namespace nativetv
