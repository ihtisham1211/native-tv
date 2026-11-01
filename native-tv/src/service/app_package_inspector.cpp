#include "service/app_package_inspector.hpp"
#include "domain/say.hpp"
#include "core/sha256.hpp"

#include <zlib.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <functional>
#include <fcntl.h>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace nativetv {
namespace service {
namespace {

const uint64_t kUploadBytes = 512ULL * 1024 * 1024;
const uint64_t kControlBytes = 1024ULL * 1024;
const uint64_t kDataBytes = 2ULL * 1024 * 1024 * 1024;
const size_t kAppInfoBytes = 64 * 1024;
const size_t kMaximumEntries = 20000;
const size_t kIoBytes = 16 * 1024;

// Made where they are used, as say::msg() remembers what it makes for the language of a request.
std::string truncated() { return say::msg("srv.install.ipk.truncated", "the ipk is truncated"); }
std::string badTar() {
    return say::msg("srv.install.ipk.badTar", "the ipk holds a damaged package archive");
}
std::string tooMany() { return say::msg("srv.install.ipk.tooMany", "the ipk holds too many files"); }
std::string unsupported() {
    return say::msg("srv.install.ipk.unsupported",
                    "the ipk uses a file format this installer does not read");
}
std::string tooBig() { return say::msg("srv.install.ipk.tooBig", "the ipk unpacks to more than it may"); }

struct Member {
    uint64_t start = 0;
    uint64_t size = 0;
};

struct Members {
    Member control;
    Member data;
};

struct TarEntry {
    std::string name;
    char type = '0';
    uint64_t size = 0;
    std::string link;
};

struct AppInfo {
    std::string id;
    std::string title;
    std::string version;
    std::string icon;
    bool hasIcon = false;
};

struct PackageInfo {
    std::string name;
    std::string version;
    std::vector<AppInfo> apps;
    std::vector<std::string> services;
    std::vector<std::string> machines;
};

bool readAt(int descriptor, uint64_t offset, void *destination, size_t size) {
    if (offset > static_cast<uint64_t>(std::numeric_limits<off_t>::max()))
        return false;
    char *bytes = static_cast<char *>(destination);
    size_t received = 0;
    while (received < size) {
        const uint64_t position = offset + received;
        if (position > static_cast<uint64_t>(std::numeric_limits<off_t>::max()))
            return false;
        const ssize_t count = pread(descriptor, bytes + received, size - received,
                                    static_cast<off_t>(position));
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return false;
        received += static_cast<size_t>(count);
    }
    return true;
}

bool hashFile(int descriptor, uint64_t fileSize, std::string &digest) {
    crypto::Sha256 hash;
    std::array<unsigned char, kIoBytes> buffer = {};
    uint64_t offset = 0;
    while (offset < fileSize) {
        const size_t count = static_cast<size_t>(std::min<uint64_t>(
            buffer.size(), fileSize - offset));
        if (!readAt(descriptor, offset, buffer.data(), count))
            return false;
        hash.update(buffer.data(), count);
        offset += count;
    }
    digest = crypto::hexDigest(hash.finish());
    return true;
}

bool unchanged(const struct stat &before, const struct stat &after) {
    if (before.st_dev != after.st_dev || before.st_ino != after.st_ino ||
        before.st_size != after.st_size)
        return false;
#if defined(__APPLE__)
    return before.st_mtimespec.tv_sec == after.st_mtimespec.tv_sec &&
           before.st_mtimespec.tv_nsec == after.st_mtimespec.tv_nsec &&
           before.st_ctimespec.tv_sec == after.st_ctimespec.tv_sec &&
           before.st_ctimespec.tv_nsec == after.st_ctimespec.tv_nsec;
#else
    return before.st_mtim.tv_sec == after.st_mtim.tv_sec &&
           before.st_mtim.tv_nsec == after.st_mtim.tv_nsec &&
           before.st_ctim.tv_sec == after.st_ctim.tv_sec &&
           before.st_ctim.tv_nsec == after.st_ctim.tv_nsec;
#endif
}

std::string trim(const std::string &value) {
    const size_t first = value.find_first_not_of(" \t\r\n\0", 0);
    if (first == std::string::npos)
        return std::string();
    const size_t last = value.find_last_not_of(" \t\r\n\0");
    return value.substr(first, last - first + 1);
}

std::string fixedString(const unsigned char *bytes, size_t size) {
    size_t length = 0;
    while (length < size && bytes[length] != 0)
        ++length;
    return std::string(reinterpret_cast<const char *>(bytes), length);
}

std::string arField(const unsigned char *bytes, size_t size) {
    size_t begin = 0;
    size_t end = size;
    while (begin < end && bytes[begin] == ' ')
        ++begin;
    while (end > begin && bytes[end - 1] == ' ')
        --end;
    return std::string(reinterpret_cast<const char *>(bytes + begin), end - begin);
}

bool parseDecimal(const unsigned char *bytes, size_t size, uint64_t &value) {
    const std::string text = trim(std::string(reinterpret_cast<const char *>(bytes), size));
    if (text.empty() || text.size() > 12)
        return false;
    value = 0;
    for (unsigned char character : text) {
        if (!std::isdigit(character))
            return false;
        const unsigned digit = character - '0';
        if (value > (std::numeric_limits<uint64_t>::max() - digit) / 10)
            return false;
        value = value * 10 + digit;
    }
    return true;
}

bool parseOctal(const unsigned char *bytes, size_t size, uint64_t &value) {
    std::string text(reinterpret_cast<const char *>(bytes), size);
    while (!text.empty() && (text.back() == '\0' || text.back() == ' '))
        text.pop_back();
    size_t begin = 0;
    while (begin < text.size() && text[begin] == ' ')
        ++begin;
    if (begin == text.size()) {
        value = 0;
        return true;
    }
    value = 0;
    for (size_t index = begin; index < text.size(); ++index) {
        const unsigned char character = static_cast<unsigned char>(text[index]);
        if (character < '0' || character > '7')
            return false;
        const unsigned digit = character - '0';
        if (value > (std::numeric_limits<uint64_t>::max() - digit) / 8)
            return false;
        value = value * 8 + digit;
    }
    return true;
}

bool readMembers(int descriptor, uint64_t fileSize, Members &members,
                 std::string &error) {
    std::array<unsigned char, 8> magic = {};
    if (fileSize < magic.size() || !readAt(descriptor, 0, magic.data(), magic.size()) ||
        std::memcmp(magic.data(), "!<arch>\n", magic.size()) != 0) {
        error = say::msg("srv.install.ipk.notAr", "the file is not an ipk archive");
        return false;
    }

    std::map<std::string, Member> found;
    uint64_t position = magic.size();
    unsigned count = 0;
    while (position < fileSize) {
        if (fileSize - position < 60) {
            error = truncated();
            return false;
        }
        std::array<unsigned char, 60> header = {};
        if (!readAt(descriptor, position, header.data(), header.size())) {
            error = truncated();
            return false;
        }
        std::string name = arField(header.data(), 16);
        if (!name.empty() && name.back() == '/')
            name.pop_back();
        uint64_t size = 0;
        if (header[58] != '`' || header[59] != '\n' ||
            !parseDecimal(header.data() + 48, 10, size)) {
            error = say::msg("srv.install.ipk.notAr", "the file is not an ipk archive");
            return false;
        }
        if ((name != "debian-binary" && name != "control.tar.gz" &&
             name != "data.tar.gz") || found.find(name) != found.end() || ++count > 3) {
            error = "the ipk holds an unexpected part: " + name.substr(0, 40);
            return false;
        }
        const uint64_t body = position + header.size();
        if (size > fileSize - body) {
            error = truncated();
            return false;
        }
        if (name != "debian-binary" && size < 18) {
            error = badTar();
            return false;
        }
        found[name] = Member{body, size};
        const uint64_t padded = size + (size % 2);
        if (padded > fileSize - body)
            position = fileSize;
        else
            position = body + padded;
    }
    if (found.size() != 3 || found.find("debian-binary") == found.end() ||
        found.find("control.tar.gz") == found.end() ||
        found.find("data.tar.gz") == found.end()) {
        error = say::msg("srv.install.ipk.parts", "the ipk is missing a part");
        return false;
    }
    members.control = found["control.tar.gz"];
    members.data = found["data.tar.gz"];
    return true;
}

bool packageName(const std::string &name) {
    if (name.empty() || name.size() > 128 ||
        !std::isalnum(static_cast<unsigned char>(name[0])))
        return false;
    for (unsigned char character : name) {
        if (!std::isalnum(character) && character != '.' && character != '_' &&
            character != '+' && character != '-')
            return false;
    }
    return true;
}

bool packageVersion(const std::string &version) {
    if (version.empty() || version.size() > 64 ||
        !std::isalnum(static_cast<unsigned char>(version[0])))
        return false;
    for (unsigned char character : version) {
        if (!std::isalnum(character) && character != '.' && character != '+' &&
            character != '~' && character != ':' && character != '_' &&
            character != '-')
            return false;
    }
    return true;
}

std::string truncateUtf16(const std::string &value, size_t maximum) {
    size_t offset = 0;
    size_t units = 0;
    while (offset < value.size()) {
        const unsigned char first = static_cast<unsigned char>(value[offset]);
        size_t bytes = 1;
        size_t codeUnits = 1;
        if ((first & 0xe0) == 0xc0 && offset + 1 < value.size()) bytes = 2;
        else if ((first & 0xf0) == 0xe0 && offset + 2 < value.size()) bytes = 3;
        else if ((first & 0xf8) == 0xf0 && offset + 3 < value.size()) {
            bytes = 4;
            codeUnits = 2;
        }
        if (units + codeUnits > maximum)
            break;
        units += codeUnits;
        offset += bytes;
    }
    return value.substr(0, offset);
}

class TarWalker {
  public:
    using EntryHandler = std::function<bool(const TarEntry &, size_t &, std::string &)>;
    using FileHandler = std::function<bool(const TarEntry &,
                                           const std::vector<unsigned char> &,
                                           std::string &)>;

    TarWalker(EntryHandler entryHandler, FileHandler fileHandler)
        : entryHandler_(std::move(entryHandler)), fileHandler_(std::move(fileHandler)) {}

    bool push(const unsigned char *bytes, size_t size, std::string &error) {
        size_t position = 0;
        while (position < size) {
            if (mode_ == Mode::End) {
                for (size_t index = position; index < size; ++index) {
                    if (bytes[index] != 0) {
                        error = badTar();
                        return false;
                    }
                }
                return true;
            }
            if (mode_ == Mode::Header) {
                const size_t count = std::min(header_.size() - headerLength_, size - position);
                std::memcpy(header_.data() + headerLength_, bytes + position, count);
                headerLength_ += count;
                position += count;
                if (headerLength_ == header_.size()) {
                    headerLength_ = 0;
                    if (!parseHeader(error))
                        return false;
                }
            } else if (mode_ == Mode::Body) {
                const size_t count = static_cast<size_t>(std::min<uint64_t>(left_, size - position));
                if (keep_.size() < keepBytes_) {
                    const size_t take = std::min(count, keepBytes_ - keep_.size());
                    keep_.insert(keep_.end(), bytes + position, bytes + position + take);
                }
                left_ -= count;
                position += count;
                if (left_ == 0 && !finishBody(error))
                    return false;
            } else {
                const size_t count = static_cast<size_t>(std::min<uint64_t>(padding_, size - position));
                padding_ -= count;
                position += count;
                if (padding_ == 0)
                    mode_ = Mode::Header;
            }
        }
        return true;
    }

    bool finish(std::string &error) const {
        if (mode_ == Mode::Body || mode_ == Mode::Padding ||
            (mode_ == Mode::Header && headerLength_ > 0)) {
            error = truncated();
            return false;
        }
        return true;
    }

  private:
    enum class Mode { Header, Body, Padding, End };

    bool parseHeader(std::string &error) {
        bool zero = true;
        for (unsigned char value : header_) {
            if (value != 0) {
                zero = false;
                break;
            }
        }
        if (zero) {
            mode_ = Mode::End;
            return true;
        }
        uint64_t expectedChecksum = 0;
        if (!parseOctal(header_.data() + 148, 8, expectedChecksum)) {
            error = badTar();
            return false;
        }
        uint64_t checksum = 0;
        for (size_t index = 0; index < header_.size(); ++index)
            checksum += (index >= 148 && index < 156) ? 32 : header_[index];
        if (checksum != expectedChecksum) {
            error = badTar();
            return false;
        }
        if (++entries_ > kMaximumEntries) {
            error = tooMany();
            return false;
        }
        if ((header_[124] & 0x80) != 0) {
            error = unsupported();
            return false;
        }
        entry_.type = header_[156] ? static_cast<char>(header_[156]) : '0';
        if (entry_.type == 'L' || entry_.type == 'K' || entry_.type == 'x' ||
            entry_.type == 'g' || entry_.type == 'X') {
            error = unsupported();
            return false;
        }
        if (!parseOctal(header_.data() + 124, 12, entry_.size)) {
            error = badTar();
            return false;
        }
        entry_.name = fixedString(header_.data(), 100);
        if (std::memcmp(header_.data() + 257, "ustar", 5) == 0) {
            const std::string prefix = fixedString(header_.data() + 345, 155);
            if (!prefix.empty())
                entry_.name = prefix + "/" + entry_.name;
        }
        entry_.link = fixedString(header_.data() + 157, 100);
        keep_.clear();
        keepBytes_ = 0;
        if (!entryHandler_(entry_, keepBytes_, error))
            return false;
        if (keepBytes_ > entry_.size) {
            error = badTar();
            return false;
        }
        left_ = entry_.size;
        padding_ = (512 - entry_.size % 512) % 512;
        if (left_ == 0) {
            return finishBody(error);
        }
        mode_ = Mode::Body;
        return true;
    }

    bool finishBody(std::string &error) {
        if (!keep_.empty() && !fileHandler_(entry_, keep_, error))
            return false;
        keep_.clear();
        mode_ = padding_ ? Mode::Padding : Mode::Header;
        return true;
    }

    EntryHandler entryHandler_;
    FileHandler fileHandler_;
    std::array<unsigned char, 512> header_ = {};
    size_t headerLength_ = 0;
    size_t entries_ = 0;
    uint64_t left_ = 0;
    uint64_t padding_ = 0;
    size_t keepBytes_ = 0;
    std::vector<unsigned char> keep_;
    TarEntry entry_;
    Mode mode_ = Mode::Header;
};

bool gunzipMember(int descriptor, const Member &member, uint64_t maximumOutput,
                  TarWalker &walker, std::string &error) {
    z_stream stream = {};
    if (inflateInit2(&stream, MAX_WBITS + 16) != Z_OK) {
        error = "could not initialize gzip reader";
        return false;
    }
    std::array<unsigned char, kIoBytes> input = {};
    std::array<unsigned char, kIoBytes> output = {};
    uint64_t compressedRead = 0;
    uint64_t totalOutput = 0;
    bool success = false;
    for (;;) {
        if (stream.avail_in == 0 && compressedRead < member.size) {
            const size_t count = static_cast<size_t>(
                std::min<uint64_t>(input.size(), member.size - compressedRead));
            if (!readAt(descriptor, member.start + compressedRead, input.data(), count)) {
                error = truncated();
                break;
            }
            compressedRead += count;
            stream.next_in = input.data();
            stream.avail_in = static_cast<uInt>(count);
        }
        stream.next_out = output.data();
        stream.avail_out = static_cast<uInt>(output.size());
        const int result = inflate(&stream, Z_NO_FLUSH);
        const size_t produced = output.size() - stream.avail_out;
        if (produced > maximumOutput - totalOutput) {
            error = tooBig();
            break;
        }
        totalOutput += produced;
        if (produced && !walker.push(output.data(), produced, error))
            break;
        if (result == Z_STREAM_END) {
            if (stream.avail_in != 0 || compressedRead != member.size) {
                error = badTar();
                break;
            }
            success = walker.finish(error);
            break;
        }
        if (result != Z_OK) {
            error = truncated();
            break;
        }
        if (stream.avail_in == 0 && compressedRead == member.size && produced == 0) {
            error = truncated();
            break;
        }
    }
    inflateEnd(&stream);
    return success;
}

bool validName(const std::string &name) {
    if (name.empty() || name.size() > 128 ||
        !std::isalnum(static_cast<unsigned char>(name[0])))
        return false;
    for (unsigned char character : name) {
        if (!std::isalnum(character) && character != '.' && character != '_' &&
            character != '+' && character != '-')
            return false;
    }
    return true;
}

bool cleanPath(const std::string &name, std::vector<std::string> &segments) {
    segments.clear();
    if (!name.empty() && name[0] == '/')
        return false;
    size_t start = 0;
    while (start <= name.size()) {
        const size_t end = name.find('/', start);
        const std::string segment = name.substr(start, end == std::string::npos
            ? std::string::npos : end - start);
        if (segment == "..")
            return false;
        if (!segment.empty() && segment != ".")
            segments.push_back(segment);
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    return true;
}

bool linkInsideOwnId(const std::vector<std::string> &segments, char type,
                     const std::string &target) {
    if (segments.size() < 5 || segments[0] != "usr" || segments[1] != "palm" ||
        (segments[2] != "applications" && segments[2] != "services") ||
        !validName(segments[3]) || (!target.empty() && target[0] == '/'))
        return false;
    std::vector<std::string> resolved;
    if (type == '2')
        resolved.assign(segments.begin(), segments.end() - 1);
    size_t start = 0;
    while (start <= target.size()) {
        const size_t end = target.find('/', start);
        const std::string part = target.substr(start, end == std::string::npos
            ? std::string::npos : end - start);
        if (part == "..") {
            if (resolved.empty())
                return false;
            resolved.pop_back();
        } else if (!part.empty() && part != ".") {
            resolved.push_back(part);
        }
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    if (resolved.size() < 4)
        return false;
    for (size_t index = 0; index < 4; ++index) {
        if (resolved[index] != segments[index])
            return false;
    }
    return true;
}

std::string machineName(const std::vector<unsigned char> &bytes) {
    if (bytes.size() < 20 || bytes[0] != 0x7f || bytes[1] != 'E' ||
        bytes[2] != 'L' || bytes[3] != 'F')
        return std::string();
    const unsigned machine = bytes[5] == 2
        ? (static_cast<unsigned>(bytes[18]) << 8) | bytes[19]
        : (static_cast<unsigned>(bytes[19]) << 8) | bytes[18];
    switch (machine) {
    case 3: return "x86";
    case 8: return "mips";
    case 40: return "arm";
    case 62: return "x86_64";
    case 183: return "aarch64";
    case 243: return "riscv";
    default: return "e_machine " + std::to_string(machine);
    }
}

void addUnique(std::vector<std::string> &values, const std::string &value) {
    if (std::find(values.begin(), values.end(), value) == values.end())
        values.push_back(value);
}

std::string parseControlField(const std::string &line, std::string &key) {
    const size_t colon = line.find(':');
    if (colon == std::string::npos || colon == 0 ||
        !std::isalpha(static_cast<unsigned char>(line[0])))
        return std::string();
    for (size_t index = 1; index < colon; ++index) {
        const unsigned char value = static_cast<unsigned char>(line[index]);
        if (!std::isalnum(value) && value != '-')
            return std::string();
    }
    key = line.substr(0, colon);
    size_t first = colon + 1;
    while (first < line.size() && std::isspace(static_cast<unsigned char>(line[first])))
        ++first;
    size_t last = line.size();
    while (last > first && std::isspace(static_cast<unsigned char>(line[last - 1])))
        --last;
    return line.substr(first, last - first);
}

bool parseControl(const std::vector<unsigned char> &bytes, std::string &package,
                  std::string &version) {
    const std::string text(bytes.begin(), bytes.end());
    size_t start = 0;
    while (start <= text.size()) {
        const size_t end = text.find('\n', start);
        const std::string line = text.substr(start, end == std::string::npos
            ? std::string::npos : end - start);
        std::string key;
        const std::string value = parseControlField(line, key);
        if (!key.empty()) {
            if (key == "Package" && package.empty()) package = value;
            else if (key == "Version" && version.empty()) version = value;
        }
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    return packageName(package) && packageVersion(version);
}

std::string joinedPath(const std::vector<std::string> &segments, size_t count) {
    std::string path = "/";
    for (size_t index = 0; index < count; ++index) {
        if (index) path += "/";
        path += segments[index];
    }
    return path;
}

bool inspectControl(int descriptor, const Member &member, PackageInfo &package,
                    std::string &error) {
    bool foundControl = false;
    std::vector<unsigned char> control;
    TarWalker walker(
        [&foundControl](const TarEntry &entry, size_t &keep, std::string &failure) {
            std::vector<std::string> segments;
            if (!cleanPath(entry.name, segments)) {
                failure = "the ipk holds an unsafe path: " + entry.name.substr(0, 80);
                return false;
            }
            if (segments.size() == 1 && segments[0] == "control" && entry.type == '0' &&
                !foundControl) {
                if (entry.size > kControlBytes) {
                    failure = tooBig();
                    return false;
                }
                foundControl = true;
                keep = static_cast<size_t>(entry.size);
            }
            return true;
        },
        [&control](const TarEntry &, const std::vector<unsigned char> &body,
                   std::string &) {
            control = body;
            return true;
        });
    if (!gunzipMember(descriptor, member, kControlBytes, walker, error))
        return false;
    if (control.empty() || !parseControl(control, package.name, package.version)) {
        error = say::msg("srv.install.ipk.noControl", "the ipk has no usable control file");
        return false;
    }
    return true;
}

bool inspectData(int descriptor, const Member &member, PackageInfo &package,
                 std::string &error) {
    std::map<std::string, size_t> appIndices;
    std::set<std::string> links;
    std::string capturedTag;
    std::string capturedId;
    TarWalker walker(
        [&package, &appIndices, &links, &capturedTag, &capturedId](
            const TarEntry &entry, size_t &keep, std::string &failure) {
            capturedTag.clear();
            capturedId.clear();
            std::vector<std::string> segments;
            if (!cleanPath(entry.name, segments)) {
                failure = "the ipk holds an unsafe path: " + entry.name.substr(0, 80);
                return false;
            }
            for (size_t count = 1; count <= segments.size(); ++count) {
                if (links.find(joinedPath(segments, count)) != links.end()) {
                    failure = "the ipk holds an unsafe path: " + entry.name.substr(0, 80);
                    return false;
                }
            }
            if (entry.type == '1' || entry.type == '2') {
                if (!linkInsideOwnId(segments, entry.type, entry.link)) {
                    failure = "the ipk holds an unsafe path: " + entry.name.substr(0, 80);
                    return false;
                }
                links.insert(joinedPath(segments, segments.size()));
            } else if (entry.type != '0' && entry.type != '5') {
                failure = unsupported();
                return false;
            }
            if (segments.size() < 4 || segments[0] != "usr" || segments[1] != "palm" ||
                (segments[2] != "applications" && segments[2] != "services"))
                return true;
            const std::string &id = segments[3];
            if (!validName(id)) {
                failure = "the ipk holds an unsafe path: " + entry.name.substr(0, 80);
                return false;
            }
            const bool regular = entry.type == '0';
            if (segments[2] == "services") {
                addUnique(package.services, id);
                if (regular && segments.size() >= 5 && entry.size >= 20) {
                    capturedTag = "elf";
                    keep = 20;
                }
                return true;
            }
            if (appIndices.find(id) == appIndices.end()) {
                AppInfo app;
                app.id = id;
                app.title = id;
                appIndices[id] = package.apps.size();
                package.apps.push_back(app);
            }
            if (regular && segments.size() == 5 && segments[4] == "appinfo.json") {
                if (entry.size > kAppInfoBytes) {
                    failure = "the ipk holds an unreadable appinfo.json for " + id;
                    return false;
                }
                capturedTag = "appinfo";
                capturedId = id;
                keep = static_cast<size_t>(entry.size);
            } else if (regular && segments.size() >= 6 && segments[4] == "bin" &&
                       entry.size >= 20) {
                capturedTag = "elf";
                keep = 20;
            } else {
                capturedTag.clear();
            }
            return true;
        },
        [&package, &appIndices, &capturedTag, &capturedId](
            const TarEntry &, const std::vector<unsigned char> &body,
            std::string &failure) {
            if (capturedTag == "elf") {
                const std::string machine = machineName(body);
                if (!machine.empty()) addUnique(package.machines, machine);
            } else if (capturedTag == "appinfo") {
                const std::string text(body.begin(), body.end());
                const Json appInfo = Json::parse(text);
                if (!appInfo.isObject() || !appInfo["id"].isString() ||
                    appInfo["id"].string() != capturedId) {
                    failure = "the ipk holds an unreadable appinfo.json for " + capturedId;
                    return false;
                }
                const auto found = appIndices.find(capturedId);
                if (found == appIndices.end()) {
                    failure = "the ipk holds an unreadable appinfo.json for " + capturedId;
                    return false;
                }
                AppInfo &app = package.apps[found->second];
                if (appInfo["title"].isString()) {
                    const std::string title = truncateUtf16(appInfo["title"].string(), 200);
                    if (!title.empty()) app.title = title;
                }
                if (appInfo["version"].isString())
                    app.version = truncateUtf16(appInfo["version"].string(), 64);
                if (appInfo["icon"].isString()) {
                    const std::string icon = truncateUtf16(appInfo["icon"].string(), 200);
                    if (!icon.empty()) {
                        app.icon = icon;
                        app.hasIcon = true;
                    }
                }
            }
            return true;
        });
    if (!gunzipMember(descriptor, member, kDataBytes, walker, error))
        return false;
    if (package.apps.empty() && package.services.empty()) {
        error = say::msg("srv.install.ipk.empty", "the ipk contains no app or service");
        return false;
    }
    return true;
}

bool safeFileName(const std::string &name) {
    if (name.empty() || name == "." || name == ".." || name.size() > 255 ||
        name.find('/') != std::string::npos || name.find('\\') != std::string::npos ||
        name.size() < 5 || name.compare(name.size() - 4, 4, ".ipk") != 0)
        return false;
    for (unsigned char character : name) {
        if (character < 0x20 || character == 0x7f)
            return false;
    }
    return true;
}

Json metadataJson(const PackageInfo &package, const std::string &sha256) {
    Json result = Json::object();
    result.set("package", Json::text(package.name));
    result.set("version", Json::text(package.version));
    result.set("sha256", Json::text(sha256));
    Json apps = Json::array();
    for (const AppInfo &app : package.apps) {
        Json row = Json::object();
        row.set("id", Json::text(app.id));
        row.set("title", Json::text(app.title));
        row.set("version", Json::text(app.version));
        row.set("icon", app.hasIcon ? Json::text(app.icon) : Json());
        apps.add(row);
    }
    result.set("apps", apps);
    Json services = Json::array();
    for (const std::string &name : package.services)
        services.add(Json::text(name));
    result.set("services", services);
    Json machines = Json::array();
    for (const std::string &name : package.machines)
        machines.add(Json::text(name));
    result.set("machines", machines);
    return result;
}

} // namespace

bool AppPackageInspector::inspect(const std::string &stagingDirectory,
                                  const std::string &fileName, Json &result,
                                  std::string &error) {
    result = Json();
    error.clear();
    if (stagingDirectory.empty() || !safeFileName(fileName)) {
        error = "the file is not in the staging directory";
        return false;
    }
    const int directory = open(stagingDirectory.c_str(), O_RDONLY | O_CLOEXEC |
                               O_DIRECTORY | O_NOFOLLOW);
    if (directory < 0) {
        error = "the ipk could not be read: " + std::string(std::strerror(errno));
        return false;
    }
    const int descriptor = openat(directory, fileName.c_str(),
                                  O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    const int openError = errno;
    close(directory);
    if (descriptor < 0) {
        error = "the ipk could not be read: " + std::string(std::strerror(openError));
        return false;
    }
    struct stat info = {};
    if (fstat(descriptor, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < 0) {
        error = "the ipk could not be read: not a regular staged file";
        close(descriptor);
        return false;
    }
    if (static_cast<uint64_t>(info.st_size) > kUploadBytes) {
        error = "the package is larger than the upload limit";
        close(descriptor);
        return false;
    }
    Members members;
    if (!readMembers(descriptor, static_cast<uint64_t>(info.st_size), members, error)) {
        close(descriptor);
        return false;
    }
    PackageInfo package;
    if (!inspectControl(descriptor, members.control, package, error) ||
        !inspectData(descriptor, members.data, package, error)) {
        close(descriptor);
        return false;
    }
    std::string digest;
    if (!hashFile(descriptor, static_cast<uint64_t>(info.st_size), digest)) {
        error = "the ipk could not be read while hashing";
        close(descriptor);
        return false;
    }
    struct stat after = {};
    if (fstat(descriptor, &after) != 0 || !unchanged(info, after)) {
        error = "the ipk changed while it was being inspected";
        close(descriptor);
        return false;
    }
    close(descriptor);
    result = metadataJson(package, digest);
    return true;
}

} // namespace service
} // namespace nativetv
