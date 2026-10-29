#pragma once

#include "core/json.hpp"
#include <deque>
#include <mutex>
#include <string>

namespace nativetv {
namespace daemon {

// Counts the native client's unexpected exits while it holds Home. `limit` of
// them within `windowMs` trip the breaker, and a tripped breaker stays tripped
// (later exits are only remembered) until reset(): nothing but a restart or the
// owner switching Home on again takes the place back. Times are milliseconds on
// any steady clock the caller keeps.
class HomeCrashBreaker {
  public:
    explicit HomeCrashBreaker(int limit = 3, long long windowMs = 120000);

    // True on the call that trips it, once.
    bool recordExit(long long nowMs, const std::string &why);
    bool tripped() const;
    void reset();
    // Exits still inside the window at `nowMs`.
    int recentExits(long long nowMs) const;
    int limit() const { return limit_; }
    long long windowMs() const { return windowMs_; }
    // {tripped, limit, windowSeconds, recentExits, lastExit?} for the status route.
    Json state(long long nowMs) const;

  private:
    struct Exit {
        long long at;
        std::string why;
    };
    void forgetOld(long long nowMs);

    const int limit_;
    const long long windowMs_;
    mutable std::mutex mutex_;
    std::deque<Exit> exits_;
    bool tripped_ = false;
};
} // namespace daemon
} // namespace nativetv
