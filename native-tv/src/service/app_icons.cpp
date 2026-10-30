#include "service/app_icons.hpp"
#include "core/json.hpp"
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <fcntl.h>
#include <limits.h>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace nativetv {
namespace service {
namespace {
const size_t MAX_APPINFO_BYTES = 64 * 1024;
const size_t MAX_ICON_BYTES = 16 * 1024 * 1024;
const size_t MAX_PACK_HEADER_BYTES = 16 * 1024;
const char PACK_MAGIC[] = "NTVICONS 1\n";

bool validAppId(const std::string &appId) {
    if (appId.empty() || appId.size() > 128)
        return false;
    for (size_t index = 0; index < appId.size(); ++index) {
        const unsigned char character = static_cast<unsigned char>(appId[index]);
        const bool alphaNumeric = (character >= 'a' && character <= 'z') ||
                                  (character >= 'A' && character <= 'Z') ||
                                  (character >= '0' && character <= '9');
        if (alphaNumeric || character == '_' || character == '-' ||
            (index > 0 && character == '.'))
            continue;
        return false;
    }
    return true;
}

std::string rooted(const std::string &systemRoot, const std::string &path) {
    const std::string root = systemRoot.empty() ? "/" : systemRoot;
    if (root == "/")
        return path;
    size_t end = root.size();
    while (end > 1 && root[end - 1] == '/')
        --end;
    return root.substr(0, end) + path;
}

bool readBounded(const std::string &path, size_t maximumBytes, std::string &contents) {
    const int descriptor = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0)
        return false;
    struct stat info;
    bool ok = fstat(descriptor, &info) == 0 && S_ISREG(info.st_mode) &&
              info.st_size >= 0 &&
              static_cast<unsigned long long>(info.st_size) <= maximumBytes;
    contents.clear();
    if (ok) {
        contents.reserve(static_cast<size_t>(info.st_size));
        char buffer[16384];
        for (;;) {
            const ssize_t count = read(descriptor, buffer, sizeof(buffer));
            if (count < 0 && errno == EINTR)
                continue;
            if (count < 0) {
                ok = false;
                break;
            }
            if (count == 0)
                break;
            if (static_cast<size_t>(count) > maximumBytes ||
                contents.size() > maximumBytes - static_cast<size_t>(count)) {
                ok = false;
                break;
            }
            contents.append(buffer, static_cast<size_t>(count));
        }
    }
    close(descriptor);
    if (!ok)
        contents.clear();
    return ok;
}

bool canonicalInside(const std::string &root, const std::string &candidate,
                     std::string &resolved) {
    char output[PATH_MAX];
    if (!realpath(candidate.c_str(), output))
        return false;
    resolved.assign(output);
    return resolved.size() > root.size() &&
           resolved.compare(0, root.size(), root) == 0 && resolved[root.size()] == '/';
}

bool appDirectory(const std::string &appId, const std::string &systemRoot,
                  std::string &directory) {
    const char *const bases[] = {
        "/media/developer/apps/usr/palm/applications",
        "/media/cryptofs/apps/usr/palm/applications",
        "/media/system/apps/usr/palm/applications",
        "/usr/palm/applications",
        "/mnt/otncabi/usr/palm/applications",
        "/mnt/otycabi/usr/palm/applications"
    };
    for (size_t index = 0; index < sizeof(bases) / sizeof(bases[0]); ++index) {
        const std::string candidate = rooted(systemRoot, bases[index]) + "/" + appId;
        if (access(candidate.c_str(), F_OK) == 0) {
            directory = candidate;
            return true;
        }
    }
    return false;
}

std::string iconEtag(size_t size, const struct stat &info) {
#if defined(__APPLE__)
    const int64_t modifiedMs = static_cast<int64_t>(info.st_mtimespec.tv_sec) * 1000 +
                               info.st_mtimespec.tv_nsec / 1000000;
#else
    const int64_t modifiedMs = static_cast<int64_t>(info.st_mtim.tv_sec) * 1000 +
                               info.st_mtim.tv_nsec / 1000000;
#endif
    std::ostringstream etag;
    etag << '"' << std::hex << size << '-' << modifiedMs << '"';
    return etag.str();
}
// A title as the pack keys it: lower-case letters and digits, "+" as "plus".
std::string titleKey(const std::string &title) {
    std::string key;
    for (size_t index = 0; index < title.size(); ++index) {
        const unsigned char character = static_cast<unsigned char>(title[index]);
        if (character >= 'A' && character <= 'Z')
            key += static_cast<char>(character - 'A' + 'a');
        else if ((character >= 'a' && character <= 'z') || (character >= '0' && character <= '9'))
            key += static_cast<char>(character);
        else if (character == '+')
            key += "plus";
    }
    return key;
}

// The pack is "NTVICONS 1\n", one "key offset length\n" line per icon with the
// offset counted from the first data byte, an empty line, then the PNGs.
bool packedIcon(const std::string &packPath, const std::string &key, AppIconFile &result) {
    if (key.empty() || packPath.empty())
        return false;
    const int descriptor = open(packPath.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0)
        return false;
    struct stat info;
    bool found = false;
    char head[MAX_PACK_HEADER_BYTES];
    const ssize_t headBytes = fstat(descriptor, &info) == 0 && S_ISREG(info.st_mode)
                                  ? pread(descriptor, head, sizeof(head), 0)
                                  : -1;
    const std::string header = headBytes > 0 ? std::string(head, static_cast<size_t>(headBytes))
                                             : std::string();
    const size_t magicSize = sizeof(PACK_MAGIC) - 1;
    const size_t listEnd = header.find("\n\n", magicSize - 1);
    if (header.compare(0, magicSize, PACK_MAGIC) == 0 && listEnd != std::string::npos) {
        const unsigned long long dataStart = listEnd + 2;
        const std::string wanted = key + " ";
        size_t lineStart = magicSize;
        while (lineStart <= listEnd && !found) {
            const size_t lineEnd = header.find('\n', lineStart);
            if (header.compare(lineStart, wanted.size(), wanted) == 0) {
                char *rest = nullptr;
                const char *fields = header.c_str() + lineStart + wanted.size();
                const unsigned long long offset = strtoull(fields, &rest, 10);
                const unsigned long long length = strtoull(rest, nullptr, 10);
                const unsigned long long fileSize = static_cast<unsigned long long>(info.st_size);
                if (length > 0 && length <= MAX_ICON_BYTES && dataStart <= fileSize &&
                    offset <= fileSize - dataStart && length <= fileSize - dataStart - offset) {
                    result.bytes.assign(static_cast<size_t>(length), '\0');
                    size_t done = 0;
                    while (done < length) {
                        const ssize_t count = pread(descriptor, &result.bytes[done], length - done,
                                                    static_cast<off_t>(dataStart + offset + done));
                        if (count < 0 && errno == EINTR)
                            continue;
                        if (count <= 0)
                            break;
                        done += static_cast<size_t>(count);
                    }
                    found = done == length;
                }
                break;
            }
            lineStart = lineEnd + 1;
        }
    }
    close(descriptor);
    if (!found) {
        result.bytes.clear();
        return false;
    }
    result.etag = iconEtag(result.bytes.size(), info);
    result.found = true;
    return true;
}
} // namespace

AppIconFile readAppIcon(const std::string &appId, const std::string &systemRoot,
                        const std::string &assetsRoot) {
    AppIconFile result;
    if (!validAppId(appId))
        return result;
    const std::string packPath =
        assetsRoot.empty() ? std::string() : assetsRoot + "/app-icons/app-icons.pack";
    if (packedIcon(packPath, appId, result))
        return result;

    std::string locatedDirectory;
    if (!appDirectory(appId, systemRoot, locatedDirectory))
        return result;
    char realDirectory[PATH_MAX];
    if (!realpath(locatedDirectory.c_str(), realDirectory))
        return result;
    const std::string appRoot(realDirectory);
    struct stat directoryInfo;
    if (stat(appRoot.c_str(), &directoryInfo) != 0 || !S_ISDIR(directoryInfo.st_mode))
        return result;

    std::string appInfoText;
    if (!readBounded(appRoot + "/appinfo.json", MAX_APPINFO_BYTES, appInfoText))
        return result;
    const Json appInfo = Json::parse(appInfoText);
    if (!appInfo.isObject())
        return result;
    if (packedIcon(packPath, titleKey(appInfo["title"].string()), result))
        return result;
    std::string iconRelative = appInfo["largeIcon"].string();
    if (iconRelative.empty())
        iconRelative = appInfo["icon"].string();
    if (iconRelative.empty())
        iconRelative = "icon.png";
    if (iconRelative.find('\0') != std::string::npos)
        return result;
    if (!iconRelative.empty() && iconRelative[0] == '$')
        iconRelative.erase(0, 1);

    std::string base = appRoot;
    const std::string assetsBase = appInfo["sysAssetsBasePath"].string();
    if (!assetsBase.empty())
        base += "/" + assetsBase;
    const std::vector<std::string> candidates = {
        base + "/" + iconRelative,
        base + "/hd1080/" + iconRelative,
        base + "/hd720/" + iconRelative,
        appRoot + "/" + iconRelative,
        appRoot + "/hd1080/" + iconRelative,
        appRoot + "/icon.png",
        appRoot + "/assets/icon.png"
    };

    std::string iconPath;
    for (const std::string &candidate : candidates) {
        if (canonicalInside(appRoot, candidate, iconPath))
            break;
        iconPath.clear();
    }
    if (iconPath.empty())
        return result;

    const int descriptor = open(iconPath.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0)
        return result;
    struct stat info;
    bool ok = fstat(descriptor, &info) == 0 && S_ISREG(info.st_mode) &&
              info.st_size >= 0 &&
              static_cast<unsigned long long>(info.st_size) <= MAX_ICON_BYTES;
    if (ok) {
        result.bytes.reserve(static_cast<size_t>(info.st_size));
        char buffer[16384];
        for (;;) {
            const ssize_t count = read(descriptor, buffer, sizeof(buffer));
            if (count < 0 && errno == EINTR)
                continue;
            if (count < 0) {
                ok = false;
                break;
            }
            if (count == 0)
                break;
            if (static_cast<size_t>(count) > MAX_ICON_BYTES ||
                result.bytes.size() > MAX_ICON_BYTES - static_cast<size_t>(count)) {
                ok = false;
                break;
            }
            result.bytes.append(buffer, static_cast<size_t>(count));
        }
    }
    close(descriptor);
    if (!ok) {
        result.bytes.clear();
        return result;
    }
    result.etag = iconEtag(result.bytes.size(), info);
    result.found = true;
    return result;
}

} // namespace service
} // namespace nativetv
