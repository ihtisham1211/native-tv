#pragma once
#include "core/services.hpp"
#include <condition_variable>
#include <curl/curl.h>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace nativetv {
class HttpServices final : public Services {
    struct Request {
        std::string channel, path, body;
        unsigned generation;
        bool post, readiness;
    };
    std::string origin_;
    std::mutex mutex_;
    std::condition_variable condition_;
    bool stopping_ = false;
    bool curlReady_ = false;
    size_t completedBytes_ = 0, working_ = 0; // requests a worker has taken and not yet answered
    std::deque<Request> api_, images_;
    std::deque<Response> completed_;
    std::map<std::string, unsigned> generations_;
    std::string adminSession_;
    std::vector<std::thread> workers_;
    struct Progress {
        HttpServices *owner;
        const Request *request;
    };
    static int progress(void *context, curl_off_t, curl_off_t, curl_off_t, curl_off_t);
    void run(bool images);
    void stop() noexcept;

  public:
    explicit HttpServices(const std::string &origin);
    ~HttpServices() override;
    HttpServices(const HttpServices &) = delete;
    HttpServices &operator=(const HttpServices &) = delete;
    unsigned read(const std::string &channel, Operation operation,
                  const Json &arguments = Json()) override;
    unsigned image(const std::string &asset);
    unsigned write(const std::string &channel, Operation operation, const Json &body) override;
    void cancel(const std::string &channel) override;
    void adminSession(const std::string &session) override;
    std::vector<Response> drain();
    // Nothing queued, running or waiting to be drained: what a fixed-clock capture waits for.
    bool idle();

  private:
    unsigned get(const std::string &channel, const std::string &path, bool image,
                 bool readiness = false);
};
} // namespace nativetv
