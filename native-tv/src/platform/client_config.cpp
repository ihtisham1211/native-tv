#include "platform/client_config.hpp"

#include "core/json.hpp"

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace nativetv {
namespace {
const unsigned kDefaultPort = 8080;
const size_t kMaxConfigBytes = 4096;

std::string join(const std::string &root, const std::string &child) {
    return (root.empty() || root.back() == '/' ? root : root + "/") + child;
}

std::string errorText(const std::string &operation, const std::string &path) {
    return operation + " " + path + ": " + std::strerror(errno);
}

bool writeAll(int descriptor, const std::string &contents) {
    size_t offset = 0;
    while (offset < contents.size()) {
        ssize_t written = write(descriptor, contents.data() + offset,
                                contents.size() - offset);
        if (written < 0 && errno == EINTR) continue;
        if (written == 0) {
            errno = EIO;
            return false;
        }
        if (written < 0) return false;
        offset += static_cast<size_t>(written);
    }
    return true;
}

std::string parentDirectory(const std::string &path, std::string &name) {
    const size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) {
        name = path;
        return ".";
    }
    name = path.substr(slash + 1);
    if (slash == 0) return "/";
    return path.substr(0, slash);
}

unsigned configuredPort(const std::string &contents) {
    const Json config = Json::parse(contents);
    const Json value = config["port"];
    if (!config.isObject() || !value.isNumber())
        throw std::runtime_error("client server config must contain a numeric port");
    const double port = value.number();
    if (!std::isfinite(port) || std::floor(port) != port || port < 1 || port > 65535)
        throw std::runtime_error("client server config port must be between 1 and 65535");
    return static_cast<unsigned>(port);
}
} // namespace

std::string clientServerConfigPath() {
    const char *configured = std::getenv("NATIVETV_CLIENT_CONFIG");
    return configured && *configured ? configured
                                     : "/var/lib/tvweb/native/client-config.json";
}

std::string clientServerConfigPathForDataRoot(const std::string &dataRoot) {
    return join(join(dataRoot, "native"), "client-config.json");
}

std::string clientServerOrigin(const std::string &configPath) {
    int descriptor = open(configPath.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0) {
        if (errno == ENOENT)
            return "http://127.0.0.1:" + std::to_string(kDefaultPort);
        throw std::runtime_error(errorText("could not open client server config", configPath));
    }
    struct stat info = {};
    if (fstat(descriptor, &info) != 0 || !S_ISREG(info.st_mode) ||
        info.st_size < 0 || static_cast<size_t>(info.st_size) > kMaxConfigBytes) {
        close(descriptor);
        throw std::runtime_error("client server config is not a bounded regular file");
    }
    std::string contents;
    contents.reserve(static_cast<size_t>(info.st_size));
    char buffer[1024];
    while (true) {
        ssize_t count = read(descriptor, buffer, sizeof(buffer));
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) {
            const std::string message = errorText("could not read client server config",
                                                  configPath);
            close(descriptor);
            throw std::runtime_error(message);
        }
        if (count == 0) break;
        if (contents.size() + static_cast<size_t>(count) > kMaxConfigBytes) {
            close(descriptor);
            throw std::runtime_error("client server config exceeds the size limit");
        }
        contents.append(buffer, static_cast<size_t>(count));
    }
    close(descriptor);
    return "http://127.0.0.1:" + std::to_string(configuredPort(contents));
}

bool writeClientServerPort(const std::string &configPath, unsigned port,
                           std::string *error) {
    if (port < 1 || port > 65535) {
        if (error) *error = "client server port must be between 1 and 65535";
        return false;
    }
    std::string name;
    const std::string parent = parentDirectory(configPath, name);
    if (name.empty() || name == "." || name == "..") {
        if (error) *error = "client server config path has no filename";
        return false;
    }
    int directory = open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (directory < 0 && errno == ENOENT) {
        if (mkdir(parent.c_str(), 0755) != 0 && errno != EEXIST) {
            if (error) *error = errorText("could not create client config directory", parent);
            return false;
        }
        directory = open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    }
    if (directory < 0) {
        if (error) *error = errorText("could not open client config directory", parent);
        return false;
    }
    Json document = Json::object();
    document.set("port", Json::numeric(static_cast<double>(port)));
    const std::string contents = document.dump() + "\n";
    std::string temporary;
    int descriptor = -1;
    for (unsigned attempt = 0; attempt < 32; ++attempt) {
        temporary = "." + name + ".tmp." + std::to_string(getpid()) +
                    "." + std::to_string(attempt);
        descriptor = openat(directory, temporary.c_str(),
                            O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                            0644);
        if (descriptor >= 0 || errno != EEXIST) break;
    }
    if (descriptor < 0) {
        if (error) *error = errorText("could not create client server config", configPath);
        close(directory);
        return false;
    }

    bool ok = fchmod(descriptor, 0644) == 0 && writeAll(descriptor, contents) &&
              fsync(descriptor) == 0;
    int savedError = ok ? 0 : errno;
    if (close(descriptor) != 0 && ok) {
        ok = false;
        savedError = errno;
    }
    if (ok && renameat(directory, temporary.c_str(), directory, name.c_str()) != 0) {
        ok = false;
        savedError = errno;
    }
    if (ok && fsync(directory) != 0) {
        ok = false;
        savedError = errno;
    }
    if (!ok) {
        unlinkat(directory, temporary.c_str(), 0);
        if (error) {
            errno = savedError;
            *error = errorText("could not publish client server config", configPath);
        }
    }
    close(directory);
    return ok;
}

} // namespace nativetv
