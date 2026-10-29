#include "daemon/home_crash_breaker.hpp"

namespace nativetv {
namespace daemon {

HomeCrashBreaker::HomeCrashBreaker(int limit, long long windowMs)
    : limit_(limit < 1 ? 1 : limit), windowMs_(windowMs < 1 ? 1 : windowMs) {}

void HomeCrashBreaker::forgetOld(long long nowMs) {
    while (!exits_.empty() && nowMs - exits_.front().at >= windowMs_) exits_.pop_front();
}

bool HomeCrashBreaker::recordExit(long long nowMs, const std::string &why) {
    std::lock_guard<std::mutex> lock(mutex_);
    forgetOld(nowMs);
    exits_.push_back({nowMs, why});
    if (tripped_ || static_cast<int>(exits_.size()) < limit_) return false;
    tripped_ = true;
    return true;
}

bool HomeCrashBreaker::tripped() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return tripped_;
}

void HomeCrashBreaker::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    exits_.clear();
    tripped_ = false;
}

int HomeCrashBreaker::recentExits(long long nowMs) const {
    std::lock_guard<std::mutex> lock(mutex_);
    int count = 0;
    for (const Exit &exit : exits_)
        if (nowMs - exit.at < windowMs_) ++count;
    return count;
}

Json HomeCrashBreaker::state(long long nowMs) const {
    Json result = Json::object();
    result.set("tripped", Json::flag(tripped()));
    result.set("limit", Json::numeric(limit_));
    result.set("windowSeconds", Json::numeric(static_cast<double>(windowMs_ / 1000)));
    result.set("recentExits", Json::numeric(recentExits(nowMs)));
    std::lock_guard<std::mutex> lock(mutex_);
    if (!exits_.empty()) result.set("lastExit", Json::text(exits_.back().why));
    return result;
}
} // namespace daemon
} // namespace nativetv
