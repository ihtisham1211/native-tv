#pragma once

#include "core/json.hpp"
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace nativetv {
namespace service {

class HomeDiscover {
  public:
    using Fetcher = std::function<bool(const std::string &, Json &, std::string &)>;
    using Clock = std::function<int64_t()>;

    explicit HomeDiscover(std::string dataRoot, Fetcher fetcher = Fetcher(),
                          Clock clock = Clock());
    ~HomeDiscover();
    HomeDiscover(const HomeDiscover &) = delete;
    HomeDiscover &operator=(const HomeDiscover &) = delete;

    Json feed(const std::string &tmdbKey, const std::string &youtubeKey);

  private:
    void workerLoop();
    void refresh(const std::string &tmdbKey, const std::string &youtubeKey);
    bool fetch(const std::string &url, Json &document, std::string &error);
    void loadCacheLocked();
    void saveCacheLocked();
    bool dueLocked(int64_t nowMs) const;
    std::string cachePath_;
    Fetcher fetcher_;
    Clock clock_;
    std::mutex mutex_;
    std::condition_variable changed_;
    std::thread worker_;
    std::atomic<bool> stopping_{false};
    bool cacheLoaded_ = false;
    bool refreshRequested_ = false;
    bool refreshing_ = false;
    bool hasFailed_ = false;
    int64_t failedAtMs_ = 0;
    std::string requestedTmdbKey_;
    std::string requestedYoutubeKey_;
    Json cache_;
};

} // namespace service
} // namespace nativetv
