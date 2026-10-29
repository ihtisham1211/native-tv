#pragma once

#include "core/json.hpp"
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace nativetv {
namespace service {

struct HomeImage {
    bool found = false;
    std::vector<unsigned char> bytes;
    std::string etag;
};

class HomeImageProxy {
  public:
    using Fetcher = std::function<bool(const std::string &, std::string &,
                                       std::string &)>;
    using Clock = std::function<int64_t()>;

    explicit HomeImageProxy(std::string dataRoot, Fetcher fetcher = Fetcher(),
                            Clock clock = Clock());
    ~HomeImageProxy();
    HomeImageProxy(const HomeImageProxy &) = delete;
    HomeImageProxy &operator=(const HomeImageProxy &) = delete;

    HomeImage image(const std::string &url, const std::string &priority = "");
    void retainAndWarm(const Json &rows);
    static bool allowedUrl(const std::string &url);
    static std::string cacheFilename(const std::string &url);

  private:
    struct Job {
        std::string url;
        std::string path;
        unsigned priority = 1;
        bool done = false;
        bool succeeded = false;
        std::condition_variable changed;
    };

    void workerLoop();
    bool fetch(const std::string &url, std::string &bytes, std::string &error);
    bool writeCache(const std::string &path, const std::string &bytes);
    bool readCache(const std::string &path, HomeImage &image) const;
    std::shared_ptr<Job> enqueueLocked(const std::string &url,
                                      unsigned priority);
    void warmUrl(const std::string &url);
    void cleanUnused(const std::vector<std::string> &usedFiles);

    std::string imageDir_;
    Fetcher fetcher_;
    Clock clock_;
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<std::thread> workers_;
    std::vector<std::shared_ptr<Job>> queued_;
    std::map<std::string, std::shared_ptr<Job>> jobs_;
    std::atomic<bool> stopping_{false};
};

} // namespace service
} // namespace nativetv
