#include "service/ssh_keys.hpp"
#include "domain/say.hpp"
#include "core/sha256.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <iterator>
#include <mutex>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace nativetv {
namespace service {
namespace {
const char *const kAuthorizedKeys = "/home/root/.ssh/authorized_keys";
const char *const kShadowFile = "/etc/shadow";
const size_t kMaximumKeys = 50;
const size_t kMaximumLine = 16384;
const char *const kTypes[] = {
    "ssh-ed25519", "ssh-rsa", "ssh-dss",
    "ecdsa-sha2-nistp256", "ecdsa-sha2-nistp384", "ecdsa-sha2-nistp521",
    "sk-ssh-ed25519@openssh.com", "sk-ecdsa-sha2-nistp256@openssh.com"
};
std::mutex gSshKeysMutex;

std::string rootedPath(const std::string &root, const std::string &absolutePath) {
    if (root.empty() || root == "/") return absolutePath;
    size_t end = root.size();
    while (end > 1 && root[end - 1] == '/') --end;
    return root.substr(0, end) + absolutePath;
}

bool whitespace(char value) {
    return value == ' ' || value == '\t' || value == '\r' || value == '\n' ||
           value == '\f' || value == '\v';
}

std::string trim(const std::string &value) {
    size_t first = 0;
    while (first < value.size() && whitespace(value[first])) ++first;
    size_t last = value.size();
    while (last > first && whitespace(value[last - 1])) --last;
    return value.substr(first, last - first);
}

std::vector<std::string> words(const std::string &line) {
    std::vector<std::string> result;
    std::string word;
    bool quoted = false;
    for (char value : line) {
        if (value == '"') {
            quoted = !quoted;
            word.push_back(value);
        } else if (whitespace(value) && !quoted) {
            if (!word.empty()) {
                result.push_back(word);
                word.clear();
            }
        } else {
            word.push_back(value);
        }
    }
    if (quoted) return {};
    if (!word.empty()) result.push_back(word);
    return result;
}

int base64Value(unsigned char value) {
    if (value >= 'A' && value <= 'Z') return value - 'A';
    if (value >= 'a' && value <= 'z') return value - 'a' + 26;
    if (value >= '0' && value <= '9') return value - '0' + 52;
    if (value == '+') return 62;
    if (value == '/') return 63;
    return -1;
}

bool decodeBase64(const std::string &input, std::vector<uint8_t> &output) {
    output.clear();
    if (input.empty()) return false;
    size_t padding = 0;
    while (padding < input.size() && input[input.size() - 1 - padding] == '=') ++padding;
    if (padding > 2 || input.size() - padding == 0 || input.size() % 4 == 1) return false;
    for (size_t i = 0; i < input.size() - padding; ++i)
        if (base64Value(static_cast<unsigned char>(input[i])) < 0) return false;
    for (size_t i = 0; i < input.size() - padding; ++i)
        if (input[i] == '=') return false;

    uint32_t accumulator = 0;
    unsigned bits = 0;
    for (size_t i = 0; i < input.size() - padding; ++i) {
        accumulator = (accumulator << 6) |
            static_cast<uint32_t>(base64Value(static_cast<unsigned char>(input[i])));
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            output.push_back(static_cast<uint8_t>((accumulator >> bits) & 0xff));
        }
    }
    if (bits && (accumulator & ((1u << bits) - 1u)) != 0) return false;
    const size_t expectedPadding = bits == 0 ? 0 : (bits == 2 ? 1 : (bits == 4 ? 2 : 3));
    if (padding && padding != expectedPadding) return false;
    return true;
}

std::string encodeBase64(const uint8_t *data, size_t length, bool pad) {
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string output;
    output.reserve(((length + 2) / 3) * 4);
    for (size_t offset = 0; offset < length; offset += 3) {
        const size_t remaining = length - offset;
        const uint32_t value = (static_cast<uint32_t>(data[offset]) << 16) |
            (remaining > 1 ? static_cast<uint32_t>(data[offset + 1]) << 8 : 0) |
            (remaining > 2 ? static_cast<uint32_t>(data[offset + 2]) : 0);
        output.push_back(alphabet[(value >> 18) & 63]);
        output.push_back(alphabet[(value >> 12) & 63]);
        if (remaining > 1) output.push_back(alphabet[(value >> 6) & 63]);
        else if (pad) output.push_back('=');
        if (remaining > 2) output.push_back(alphabet[value & 63]);
        else if (pad) output.push_back('=');
    }
    return output;
}

std::array<uint8_t, 32> sha256(const std::vector<uint8_t> &input) {
    crypto::Sha256 hash;
    hash.update(input.data(), input.size());
    return hash.finish();
}

bool supportedType(const std::string &type) {
    return std::find(std::begin(kTypes), std::end(kTypes), type) != std::end(kTypes);
}

bool readFile(const std::string &path, std::string &contents) {
    std::ifstream input(path.c_str(), std::ios::in | std::ios::binary);
    if (!input) return false;
    std::ostringstream stream;
    stream << input.rdbuf();
    if (input.bad()) return false;
    contents = stream.str();
    return true;
}

bool readLines(const std::string &path, std::vector<std::string> &lines) {
    std::string contents;
    if (!readFile(path, contents)) return false;
    size_t start = 0;
    for (;;) {
        const size_t end = contents.find('\n', start);
        if (end == std::string::npos) {
            lines.push_back(contents.substr(start));
            break;
        }
        lines.push_back(contents.substr(start, end - start));
        start = end + 1;
    }
    while (!lines.empty() && trim(lines.back()).empty()) lines.pop_back();
    return true;
}

bool ensureDirectory(const std::string &path, mode_t mode, std::string &error) {
    if (path.empty() || path == "/") return true;
    struct stat info;
    if (stat(path.c_str(), &info) == 0) {
        if (!S_ISDIR(info.st_mode)) {
            error = "not a directory";
            return false;
        }
        return true;
    }
    if (errno != ENOENT) {
        error = std::strerror(errno);
        return false;
    }
    const size_t slash = path.find_last_of('/');
    if (slash != std::string::npos && slash > 0 &&
        !ensureDirectory(path.substr(0, slash), 0755, error))
        return false;
    if (mkdir(path.c_str(), mode) == 0) return true;
    if (errno == EEXIST) {
        if (stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode)) return true;
        error = "not a directory";
        return false;
    }
    error = std::strerror(errno);
    return false;
}

bool writeAll(int descriptor, const std::string &contents) {
    size_t offset = 0;
    while (offset < contents.size()) {
        const ssize_t count = write(descriptor, contents.data() + offset,
                                    contents.size() - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count == 0) errno = EIO;
        if (count <= 0) return false;
        offset += static_cast<size_t>(count);
    }
    return true;
}

bool writeLines(const std::string &file, const std::vector<std::string> &lines,
                std::string &error) {
    const size_t slash = file.find_last_of('/');
    const std::string directory = slash == std::string::npos ? "." : file.substr(0, slash);
    if (!ensureDirectory(directory, 0755, error)) return false;
    if (chmod(directory.c_str(), 0700) != 0) {
        error = std::strerror(errno);
        return false;
    }
    std::string contents;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i) contents.push_back('\n');
        contents += lines[i];
    }
    if (!lines.empty()) contents.push_back('\n');

    const std::string temporary = file + ".tmp";
    const int descriptor = open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW,
                                0600);
    if (descriptor < 0) {
        error = std::strerror(errno);
        return false;
    }
    int failure = 0;
    if (fchmod(descriptor, 0600) != 0) failure = errno;
    if (!failure && !writeAll(descriptor, contents)) failure = errno ? errno : EIO;
    if (!failure && fsync(descriptor) != 0) failure = errno;
    if (close(descriptor) != 0 && !failure) failure = errno;
    if (failure) {
        error = std::strerror(failure);
        unlink(temporary.c_str());
        return false;
    }
    if (rename(temporary.c_str(), file.c_str()) != 0) {
        error = std::strerror(errno);
        unlink(temporary.c_str());
        return false;
    }
    const int directoryFd = open(directory.c_str(), O_RDONLY);
    if (directoryFd >= 0) {
        (void)fsync(directoryFd);
        close(directoryFd);
    }
    return true;
}

Json error(const std::string &message) {
    Json response = Json::object();
    response.set("ok", Json::flag(false));
    response.set("error", Json::text(message));
    return response;
}

bool dropbearRunning(const std::string &procRoot) {
    DIR *directory = opendir(procRoot.c_str());
    if (!directory) return false;
    bool running = false;
    dirent *entry = nullptr;
    while (!running && (entry = readdir(directory)) != nullptr) {
        const std::string pid(entry->d_name);
        if (pid.empty() || !std::all_of(pid.begin(), pid.end(),
                [](unsigned char c) { return c >= '0' && c <= '9'; }))
            continue;
        std::string command;
        if (readFile(procRoot + "/" + pid + "/comm", command) && trim(command) == "dropbear")
            running = true;
    }
    closedir(directory);
    return running;
}

Json statusUnlocked(const std::string &systemRoot, const std::string &procRoot) {
    const std::string keyPath = rootedPath(systemRoot, kAuthorizedKeys);
    const std::string shadowPath = rootedPath(systemRoot, kShadowFile);
    std::vector<std::string> lines;
    const bool fileExists = readLines(keyPath, lines);
    Json keys = Json::array();
    for (const std::string &line : lines) {
        SshPublicKey key;
        if (!parseSshPublicKey(line, key)) continue;
        Json row = Json::object();
        row.set("type", Json::text(key.type));
        row.set("comment", Json::text(key.comment));
        row.set("options", Json::text(key.options));
        row.set("fingerprint", Json::text(key.fingerprint));
        keys.add(row);
    }
    Json passwordNow;
    std::string shadow;
    if (readFile(shadowPath, shadow)) {
        size_t start = 0;
        while (start <= shadow.size()) {
            const size_t end = shadow.find('\n', start);
            const std::string line = shadow.substr(start,
                end == std::string::npos ? std::string::npos : end - start);
            if (line.compare(0, 5, "root:") == 0) {
                const size_t colon = line.find(':', 5);
                if (colon != std::string::npos) {
                    const std::string field = line.substr(5, colon - 5);
                    passwordNow = Json::flag(!field.empty() && field != "*" &&
                                             field[0] != '!');
                }
                break;
            }
            if (end == std::string::npos) break;
            start = end + 1;
        }
    }
    Json response = Json::object();
    response.set("ok", Json::flag(true));
    response.set("running", Json::flag(dropbearRunning(procRoot)));
    response.set("fileExists", Json::flag(fileExists));
    response.set("keys", keys);
    response.set("passwordNow", passwordNow);
    response.set("passwordAfterReboot", Json::flag(!fileExists));
    return response;
}

Json succeededStatus(const std::string &systemRoot, const std::string &procRoot) {
    return statusUnlocked(systemRoot, procRoot);
}
} // namespace

std::string sshKeyFingerprint(const std::string &blob) {
    std::vector<uint8_t> decoded;
    if (!decodeBase64(blob, decoded)) return std::string();
    const std::array<uint8_t, 32> digest = sha256(decoded);
    return "SHA256:" + encodeBase64(digest.data(), digest.size(), false);
}

bool parseSshPublicKey(const std::string &line, SshPublicKey &key) {
    const std::string cleaned = trim(line);
    if (cleaned.empty() || cleaned[0] == '#') return false;
    const std::vector<std::string> tokens = words(cleaned);
    for (size_t index = 0; index + 1 < tokens.size() && index < 2; ++index) {
        if (!supportedType(tokens[index])) continue;
        const std::string &blob = tokens[index + 1];
        if (blob.empty() || !std::all_of(blob.begin(), blob.end(), [](unsigned char c) {
                return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                       (c >= '0' && c <= '9') || c == '+' || c == '/' || c == '=';
            }))
            return false;
        std::vector<uint8_t> decoded;
        if (!decodeBase64(blob, decoded) || decoded.size() < 4 + tokens[index].size())
            return false;
        const uint32_t length = (static_cast<uint32_t>(decoded[0]) << 24) |
            (static_cast<uint32_t>(decoded[1]) << 16) |
            (static_cast<uint32_t>(decoded[2]) << 8) | decoded[3];
        if (length != tokens[index].size() ||
            std::string(decoded.begin() + 4, decoded.begin() + 4 + length) != tokens[index])
            return false;
        key.type = tokens[index];
        key.blob = blob;
        key.options.clear();
        for (size_t i = 0; i < index; ++i) {
            if (i) key.options.push_back(' ');
            key.options += tokens[i];
        }
        key.comment.clear();
        for (size_t i = index + 2; i < tokens.size(); ++i) {
            if (!key.comment.empty()) key.comment.push_back(' ');
            key.comment += tokens[i];
        }
        key.fingerprint = "SHA256:" + encodeBase64(sha256(decoded).data(), 32, false);
        return true;
    }
    return false;
}

Json sshKeysStatus(const std::string &systemRoot, const std::string &procRoot) {
    std::lock_guard<std::mutex> lock(gSshKeysMutex);
    return statusUnlocked(systemRoot, procRoot);
}

Json addSshKey(const std::string &systemRoot, const std::string &procRoot,
               const std::string &input) {
    std::lock_guard<std::mutex> lock(gSshKeysMutex);
    const std::string line = trim(input);
    if (line.size() > kMaximumLine || line.find_first_of("\r\n") != std::string::npos)
        return error(say::msg("srv.ssh.oneKey", "Paste one public key, on one line."));
    if (line.find("PRIVATE KEY") != std::string::npos)
        return error(say::msg("srv.ssh.private", "That is a private key. Paste the public key, from the .pub file."));
    SshPublicKey key;
    if (!parseSshPublicKey(line, key))
        return error(say::msg("srv.ssh.notKey", "That is not an SSH public key."));
    const std::string path = rootedPath(systemRoot, kAuthorizedKeys);
    std::vector<std::string> lines;
    (void)readLines(path, lines);
    size_t keyCount = 0;
    for (const std::string &existing : lines) {
        SshPublicKey parsed;
        if (!parseSshPublicKey(existing, parsed)) continue;
        ++keyCount;
        if (parsed.blob == key.blob)
            return error(say::msg("srv.ssh.already", "That key is already there."));
    }
    if (keyCount >= kMaximumKeys)
        return error(say::msg("srv.ssh.tooMany", "There are already 50 keys."));
    lines.push_back(line);
    std::string failure;
    if (!writeLines(path, lines, failure))
        return error("Could not write " + std::string(kAuthorizedKeys) + ": " + failure);
    return succeededStatus(systemRoot, procRoot);
}

Json removeSshKey(const std::string &systemRoot, const std::string &procRoot,
                  const std::string &fingerprint) {
    std::lock_guard<std::mutex> lock(gSshKeysMutex);
    const std::string path = rootedPath(systemRoot, kAuthorizedKeys);
    std::vector<std::string> lines;
    if (!readLines(path, lines))
        return error(say::msg("srv.ssh.noSuchKey", "That key is not there."));
    std::vector<std::string> kept;
    bool removed = false;
    for (const std::string &line : lines) {
        SshPublicKey key;
        if (parseSshPublicKey(line, key) && key.fingerprint == fingerprint) {
            removed = true;
            continue;
        }
        kept.push_back(line);
    }
    if (!removed) return error(say::msg("srv.ssh.noSuchKey", "That key is not there."));
    std::string failure;
    if (!writeLines(path, kept, failure))
        return error("Could not write " + std::string(kAuthorizedKeys) + ": " + failure);
    return succeededStatus(systemRoot, procRoot);
}

Json deleteSshKeysFile(const std::string &systemRoot, const std::string &procRoot) {
    std::lock_guard<std::mutex> lock(gSshKeysMutex);
    const std::string path = rootedPath(systemRoot, kAuthorizedKeys);
    if (unlink(path.c_str()) != 0 && errno != ENOENT)
        return error("Could not delete " + std::string(kAuthorizedKeys) + ": " +
                     std::strerror(errno));
    return succeededStatus(systemRoot, procRoot);
}

} // namespace service
} // namespace nativetv
