#include "service/admin_pin.hpp"
#include "core/json.hpp"
#include "core/sha256.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <fstream>
#include <iterator>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace nativetv {
namespace service {
namespace {
// 20,000 rounds is about 0.1 s on the TV's CPU: slow enough to matter to anyone who
// reads the file off the TV, quick enough that a PIN check is not noticed.
const unsigned kIterations = 20000;
const int64_t kSessionSeconds = 600;
const size_t kMaxSessions = 16;
const unsigned kMissesBeforeLock = 5;
const int64_t kFirstLockSeconds = 30;
const int64_t kLongestLockSeconds = 3600;

typedef std::array<uint8_t, 32> Digest;

int64_t monotonicSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::string toHex(const uint8_t *bytes, size_t size) {
    static const char digits[] = "0123456789abcdef";
    std::string text;
    for (size_t index = 0; index < size; ++index) {
        text += digits[bytes[index] >> 4];
        text += digits[bytes[index] & 15];
    }
    return text;
}

bool fromHex(const std::string &text, std::vector<uint8_t> &bytes) {
    bytes.clear();
    if (text.size() % 2 != 0)
        return false;
    for (size_t index = 0; index < text.size(); index += 2) {
        int value = 0;
        for (size_t half = 0; half < 2; ++half) {
            const char c = text[index + half];
            const int digit = c >= '0' && c <= '9'   ? c - '0'
                              : c >= 'a' && c <= 'f' ? c - 'a' + 10
                                                     : -1;
            if (digit < 0)
                return false;
            value = value * 16 + digit;
        }
        bytes.push_back(static_cast<uint8_t>(value));
    }
    return true;
}

bool randomBytes(uint8_t *bytes, size_t size) {
    const int device = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (device < 0)
        return false;
    size_t done = 0;
    while (done < size) {
        const ssize_t count = read(device, bytes + done, size - done);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            break;
        done += static_cast<size_t>(count);
    }
    close(device);
    return done == size;
}

Digest hmacSha256(const std::string &key, const uint8_t *data, size_t size) {
    uint8_t block[64] = {0};
    if (key.size() > sizeof(block)) {
        crypto::Sha256 shortened;
        shortened.update(key.data(), key.size());
        const Digest digest = shortened.finish();
        std::copy(digest.begin(), digest.end(), block);
    } else {
        std::copy(key.begin(), key.end(), block);
    }
    uint8_t inner[64], outer[64];
    for (size_t index = 0; index < sizeof(block); ++index) {
        inner[index] = block[index] ^ 0x36;
        outer[index] = block[index] ^ 0x5c;
    }
    crypto::Sha256 first;
    first.update(inner, sizeof(inner));
    first.update(data, size);
    const Digest innerDigest = first.finish();
    crypto::Sha256 second;
    second.update(outer, sizeof(outer));
    second.update(innerDigest.data(), innerDigest.size());
    return second.finish();
}

// PBKDF2-HMAC-SHA256 for one 32-byte block, which is all a digest needs.
Digest stretch(const std::string &pin, const std::vector<uint8_t> &salt, unsigned iterations) {
    std::vector<uint8_t> first(salt);
    const uint8_t blockOne[4] = {0, 0, 0, 1};
    first.insert(first.end(), blockOne, blockOne + 4);
    Digest round = hmacSha256(pin, first.data(), first.size());
    Digest result = round;
    for (unsigned index = 1; index < iterations; ++index) {
        round = hmacSha256(pin, round.data(), round.size());
        for (size_t byte = 0; byte < result.size(); ++byte)
            result[byte] ^= round[byte];
    }
    return result;
}

bool sameText(const std::string &left, const std::string &right) {
    unsigned difference = static_cast<unsigned>(left.size() ^ right.size());
    for (size_t index = 0; index < left.size() && index < right.size(); ++index)
        difference |= static_cast<unsigned char>(left[index] ^ right[index]);
    return difference == 0;
}

bool writeOwnerOnly(const std::string &path, const std::string &contents) {
    const std::string temporary = path + ".tmp";
    const int descriptor = open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (descriptor < 0)
        return false;
    bool ok = true;
    size_t done = 0;
    while (ok && done < contents.size()) {
        const ssize_t count = write(descriptor, contents.data() + done, contents.size() - done);
        if (count < 0 && errno == EINTR)
            continue;
        ok = count > 0;
        if (ok)
            done += static_cast<size_t>(count);
    }
    ok = ok && fsync(descriptor) == 0;
    close(descriptor);
    if (!ok || rename(temporary.c_str(), path.c_str()) != 0) {
        unlink(temporary.c_str());
        return false;
    }
    return true;
}
} // namespace

bool validAdminPin(const std::string &pin) {
    if (pin.size() != 4)
        return false;
    for (size_t index = 0; index < pin.size(); ++index)
        if (pin[index] < '0' || pin[index] > '9')
            return false;
    return true;
}

AdminPin::AdminPin(std::string file, Clock now)
    : file_(std::move(file)), now_(now ? now : Clock(monotonicSeconds)) {}

bool AdminPin::load() const {
    if (loaded_)
        return !hash_.empty();
    loaded_ = true;
    std::ifstream input(file_.c_str(), std::ios::binary);
    if (!input)
        return false;
    const std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    const Json document = Json::parse(text);
    std::vector<uint8_t> check;
    if (!document.isObject() || !fromHex(document["salt"].string(), check) || check.empty() ||
        document["hash"].string().size() != 64 || document["iterations"].number() < 1)
        return false;
    salt_ = document["salt"].string();
    hash_ = document["hash"].string();
    iterations_ = static_cast<unsigned>(document["iterations"].number());
    return true;
}

bool AdminPin::configured() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return load();
}

int AdminPin::lockedFor() const {
    std::lock_guard<std::mutex> lock(mutex_);
    const int64_t left = lockedUntil_ - now_();
    return left > 0 ? static_cast<int>(left) : 0;
}

AdminPin::Result AdminPin::unlock(const std::string &pin, std::string &session, int &retryAfter) {
    std::lock_guard<std::mutex> lock(mutex_);
    retryAfter = 0;
    if (!load())
        return Result::NotSet;
    const int64_t now = now_();
    if (now < lockedUntil_) {
        retryAfter = static_cast<int>(lockedUntil_ - now);
        return Result::Locked;
    }
    std::vector<uint8_t> salt;
    bool right = false;
    if (validAdminPin(pin) && fromHex(salt_, salt)) {
        const Digest digest = stretch(pin, salt, iterations_);
        right = sameText(toHex(digest.data(), digest.size()), hash_);
    }
    if (!right) {
        ++misses_;
        if (misses_ >= kMissesBeforeLock) {
            const unsigned doublings = std::min(misses_ - kMissesBeforeLock, 7u);
            lockedUntil_ = now + std::min(kFirstLockSeconds << doublings, kLongestLockSeconds);
            retryAfter = static_cast<int>(lockedUntil_ - now);
        }
        return Result::Wrong;
    }
    uint8_t token[16];
    if (!randomBytes(token, sizeof(token)))
        return Result::Wrong; // no way to make an unguessable session, so none is made
    misses_ = 0;
    lockedUntil_ = 0;
    for (std::map<std::string, int64_t>::iterator it = sessions_.begin(); it != sessions_.end();)
        it = it->second <= now ? sessions_.erase(it) : std::next(it);
    while (sessions_.size() >= kMaxSessions)
        sessions_.erase(std::min_element(sessions_.begin(), sessions_.end(),
                                         [](const std::pair<const std::string, int64_t> &a,
                                            const std::pair<const std::string, int64_t> &b) {
                                             return a.second < b.second;
                                         }));
    session = toHex(token, sizeof(token));
    sessions_[session] = now + kSessionSeconds;
    return Result::Ok;
}

bool AdminPin::sessionValid(const std::string &session) {
    if (session.empty())
        return false;
    std::lock_guard<std::mutex> lock(mutex_);
    const std::map<std::string, int64_t>::iterator found = sessions_.find(session);
    if (found == sessions_.end())
        return false;
    const int64_t now = now_();
    if (found->second <= now) {
        sessions_.erase(found);
        return false;
    }
    found->second = now + kSessionSeconds;
    return true;
}

void AdminPin::lock(const std::string &session) {
    std::lock_guard<std::mutex> lock(mutex_);
    sessions_.erase(session);
}

bool AdminPin::set(const std::string &pin) {
    if (!validAdminPin(pin))
        return false;
    uint8_t salt[16];
    if (!randomBytes(salt, sizeof(salt)))
        return false;
    const Digest digest =
        stretch(pin, std::vector<uint8_t>(salt, salt + sizeof(salt)), kIterations);
    Json document = Json::object();
    document.set("salt", Json::text(toHex(salt, sizeof(salt))));
    document.set("hash", Json::text(toHex(digest.data(), digest.size())));
    document.set("iterations", Json::numeric(kIterations));
    std::lock_guard<std::mutex> lock(mutex_);
    if (!writeOwnerOnly(file_, document.dump()))
        return false;
    loaded_ = true;
    salt_ = document["salt"].string();
    hash_ = document["hash"].string();
    iterations_ = kIterations;
    sessions_.clear();
    misses_ = 0;
    lockedUntil_ = 0;
    return true;
}

bool AdminPin::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (unlink(file_.c_str()) != 0 && errno != ENOENT)
        return false;
    loaded_ = true;
    hash_.clear();
    salt_.clear();
    sessions_.clear();
    misses_ = 0;
    lockedUntil_ = 0;
    return true;
}

} // namespace service
} // namespace nativetv
