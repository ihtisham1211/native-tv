#include "daemon/home_takeover_stager.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <sstream>
#include <utility>
#include <vector>

namespace nativetv {
namespace daemon {
namespace {
const size_t kMaximumStagedFileBytes = 16 * 1024 * 1024;
const char *const kAppInfo =
    "{\n"
    "  \"id\": \"com.webos.app.home\",\n"
    "  \"version\": \"1.0.0\",\n"
    "  \"vendor\": \"tvweb\",\n"
    "  \"type\": \"web\",\n"
    "  \"main\": \"index.html\",\n"
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

bool writeAtomic(const std::string &path, const std::string &contents,
                 std::string &error) {
    const std::string temporary = path + ".new";
    (void)unlink(temporary.c_str());
    int flags = O_WRONLY | O_CREAT | O_EXCL;
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    const int descriptor = open(temporary.c_str(), flags, 0644);
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
    if (fchmod(descriptor, 0644) != 0 || fsync(descriptor) != 0) {
        error = "could not finish " + temporary + ": " + std::strerror(errno);
        close(descriptor);
        unlink(temporary.c_str());
        return false;
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
                    bool &changed, std::string &error) {
    struct stat info;
    if (lstat(destination.c_str(), &info) == 0) {
        if (!S_ISREG(info.st_mode)) {
            error = "staged path is not a regular file: " + destination;
            return false;
        }
        std::string existing;
        if (!readRegularFile(destination, existing, error)) return false;
        if (existing == contents) return true;
    } else if (errno != ENOENT) {
        error = "could not inspect " + destination + ": " + std::strerror(errno);
        return false;
    }
    if (!writeAtomic(destination, contents, error)) return false;
    changed = true;
    return true;
}

bool copyIfChanged(const std::string &source, const std::string &destination,
                   bool &changed, std::string &error) {
    std::string contents;
    if (!readRegularFile(source, contents, error)) return false;
    return writeIfChanged(destination, contents, changed, error);
}

bool configureHomePort(std::string &contents, int port, std::string &error) {
    if (port < 1 || port > 65535) {
        error = "configured Home server port is outside 1 to 65535";
        return false;
    }
    const std::string prefix = "var PORT = ";
    const std::string marker = "// tvweb:port";
    const size_t declaration = contents.find(prefix);
    if (declaration == std::string::npos ||
        contents.find(prefix, declaration + prefix.size()) != std::string::npos) {
        error = "Home launcher must contain exactly one tvweb:port declaration";
        return false;
    }
    const size_t digits = declaration + prefix.size();
    const size_t semicolon = contents.find(';', digits);
    const size_t markerAt = semicolon == std::string::npos
        ? std::string::npos : contents.find(marker, semicolon + 1);
    if (semicolon == std::string::npos || markerAt == std::string::npos ||
        markerAt == semicolon + 1 ||
        contents.find(marker, markerAt + marker.size()) != std::string::npos) {
        error = "Home launcher has an invalid tvweb:port declaration";
        return false;
    }
    for (size_t i = digits; i < semicolon; ++i) {
        if (contents[i] < '0' || contents[i] > '9') {
            error = "Home launcher has an invalid tvweb:port declaration";
            return false;
        }
    }
    for (size_t i = semicolon + 1; i < markerAt; ++i) {
        if (contents[i] != ' ' && contents[i] != '\t') {
            error = "Home launcher has an invalid tvweb:port declaration";
            return false;
        }
    }
    if (semicolon == digits) {
        error = "Home launcher has an invalid tvweb:port declaration";
        return false;
    }
    contents.replace(digits, semicolon - digits, std::to_string(port));
    return true;
}

Json failed(const std::string &error) {
    Json result = Json::object();
    result.set("ok", Json::flag(false));
    result.set("error", Json::text(error));
    return result;
}
} // namespace

HomeTakeoverStager::HomeTakeoverStager(std::string sourceRoot,
                                       std::string stagingRoot, int port)
    : sourceRoot_(std::move(sourceRoot)), stagingRoot_(std::move(stagingRoot)),
      port_(port) {}

Json HomeTakeoverStager::stage() const {
    std::string error;
    const std::string sourceAssets = join(sourceRoot_, "assets");
    const std::string stagedAssets = join(stagingRoot_, "assets");
    if (!ensureDirectory(stagingRoot_, error) || !ensureDirectory(stagedAssets, error))
        return failed(error);

    bool changed = false;
    std::string homeIndex;
    if (!readRegularFile(join(sourceRoot_, "index.html"), homeIndex, error) ||
        !configureHomePort(homeIndex, port_, error) ||
        !writeIfChanged(join(stagingRoot_, "index.html"), homeIndex,
                        changed, error))
        return failed(error);

    DIR *directory = opendir(sourceAssets.c_str());
    if (!directory) return failed("could not read " + sourceAssets + ": " + std::strerror(errno));
    std::vector<std::string> names;
    errno = 0;
    while (dirent *entry = readdir(directory)) {
        const std::string name(entry->d_name);
        if (name == "." || name == "..") continue;
        if (name.find('/') != std::string::npos) {
            closedir(directory);
            return failed("invalid Home app asset filename");
        }
        names.push_back(name);
    }
    const int readError = errno;
    closedir(directory);
    if (readError != 0) return failed("could not read " + sourceAssets);
    std::sort(names.begin(), names.end());
    for (const std::string &name : names) {
        if (!copyIfChanged(join(sourceAssets, name), join(stagedAssets, name),
                           changed, error)) return failed(error);
    }
    if (!writeIfChanged(join(stagingRoot_, "appinfo.json"), kAppInfo,
                        changed, error))
        return failed(error);

    Json result = Json::object();
    result.set("ok", Json::flag(true));
    result.set("changed", Json::flag(changed));
    return result;
}

} // namespace daemon
} // namespace nativetv
