#pragma once

#include "core/json.hpp"
#include <cstddef>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace nativetv {
namespace service {

class HomeContent {
  public:
    using Fetcher = std::function<bool(const std::string &, Json &, std::string &)>;
    using Clock = std::function<int64_t()>;

    explicit HomeContent(std::string dataRoot, Fetcher fetcher = Fetcher(),
                         Clock clock = Clock());
    Json details(const std::string &tmdbKey, bool netflixEnabled,
                 const std::string &type, const std::string &id);
    Json search(const std::string &tmdbKey, const std::string &query,
                bool queryPresent = true);
    Json netflix(const std::string &tmdbKey, bool netflixEnabled,
                 const std::string &type, const std::string &id);

  private:
    struct Entry {
        int64_t at = 0;
        int64_t failedAt = 0;
        Json value;
    };
    struct Pending {
        bool done = false;
        Json value;
        std::condition_variable changed;
    };
    using Converter = std::function<Json(const Json &)>;

    Json request(const std::string &apiKey, const std::string &resource,
                 const std::string &query, int64_t lifetimeMs,
                 const Converter &convert);
    bool fetch(const std::string &url, Json &document, std::string &error) const;
    void loadCacheLocked();
    void saveCacheLocked();
    std::string urlFor(const std::string &apiKey, const std::string &resource,
                       const std::string &query) const;

    std::string cachePath_;
    Fetcher fetcher_;
    Clock clock_;
    std::mutex mutex_;
    std::condition_variable capacityChanged_;
    std::map<std::string, Entry> cache_;
    std::map<std::string, std::shared_ptr<Pending>> pending_;
    size_t active_ = 0;
    size_t queued_ = 0;
    bool cacheLoaded_ = false;
};

} // namespace service
} // namespace nativetv
