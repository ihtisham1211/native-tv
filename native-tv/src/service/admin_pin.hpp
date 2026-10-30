#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>

namespace nativetv {
namespace service {

// The admin PIN: four digits that open a short unlock session for the risky
// actions (installing and removing apps, SSH keys, restoring a backup, power).
// It is a second lock on top of the access token, not a replacement for it.
class AdminPin {
  public:
    using Clock = std::function<int64_t()>; // seconds, never going backwards

    enum class Result { Ok, Wrong, Locked, NotSet };

    explicit AdminPin(std::string file, Clock now = Clock());

    bool configured() const;
    // True while the PIN is the default, 0000, which anyone who knows it can use.
    bool usesDefault() const;
    // Seconds a lock-out has left, 0 when there is none.
    int lockedFor() const;
    // Checks a PIN and, when it is right, opens an unlock session. retryAfter is the
    // seconds a lock-out has left; five misses in a row start one and each further
    // miss doubles it, up to an hour, so guessing four digits is not practical.
    Result unlock(const std::string &pin, std::string &session, int &retryAfter);
    // True for a session that is open; a use keeps it open for another ten minutes.
    bool sessionValid(const std::string &session);
    void lock(const std::string &session);
    // Stores the PIN (four digits) in place of any other and ends every session.
    bool set(const std::string &pin);
    bool clear();

  private:
    bool load() const;

    std::string file_;
    Clock now_;
    mutable std::mutex mutex_;
    mutable bool loaded_ = false;
    mutable std::string salt_;
    mutable std::string hash_;
    mutable unsigned iterations_ = 0;
    mutable bool default_ = false;
    unsigned misses_ = 0;
    int64_t lockedUntil_ = 0;
    std::map<std::string, int64_t> sessions_; // token -> seconds it stays open until
};

bool validAdminPin(const std::string &pin);
// What onboarding offers when the owner does not pick one.
extern const char *const kDefaultAdminPin;

} // namespace service
} // namespace nativetv
