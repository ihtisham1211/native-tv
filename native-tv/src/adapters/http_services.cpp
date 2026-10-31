#include "adapters/http_services.hpp"
#include "adapters/legacy_routes.hpp"
#include "adapters/readiness_retry.hpp"
#include <algorithm>
#include <curl/curl.h>
#include <stdexcept>

namespace nativetv {
namespace {
size_t collect(char *bytes, size_t size, size_t count, void *data) {
    std::string &result = *static_cast<std::string *>(data);
    if (size && count > (8 * 1024 * 1024 - result.size()) / size)
        return 0;
    result.append(bytes, size * count);
    return size * count;
}
} // namespace
HttpServices::HttpServices(const std::string &origin) : origin_(origin) {
    if (origin.compare(0, 7, "http://") != 0 ||
        origin.find_first_of("\r\n?#", 7) != std::string::npos)
        throw std::runtime_error("Invalid server origin");
    while (!origin_.empty() && origin_.back() == '/')
        origin_.pop_back();
    try {
        if (curl_global_init(CURL_GLOBAL_DEFAULT))
            throw std::runtime_error("HTTP initialization failed");
        curlReady_ = true;
        workers_.reserve(3);
        for (int i = 0; i < 3; ++i)
            workers_.emplace_back(&HttpServices::run, this, i != 0);
    } catch (...) {
        stop();
        throw;
    }
}
HttpServices::~HttpServices() {
    stop();
}
void HttpServices::stop() noexcept {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
        api_.clear();
        images_.clear();
    }
    condition_.notify_all();
    for (auto &worker : workers_)
        if (worker.joinable())
            worker.join();
    workers_.clear();
    if (curlReady_) {
        curl_global_cleanup();
        curlReady_ = false;
    }
}
int HttpServices::progress(void *context, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    auto &state = *static_cast<Progress *>(context);
    std::lock_guard<std::mutex> lock(state.owner->mutex_);
    bool stop = state.owner->stopping_ ||
                state.owner->generations_[state.request->channel] != state.request->generation;

    return stop ? 1 : 0;
}
unsigned HttpServices::read(const std::string &channel, Operation operation,
                            const Json &arguments) {
    std::string path;
    try {
        path = legacyRoute(operation, arguments);
    } catch (const std::invalid_argument &) {
        return 0; // an operation with no route is a request that did not start, as a refused path is
    }
    return get(channel, path, false, operation == Operation::Capabilities);
}
unsigned HttpServices::image(const std::string &path) {
    return get("image:" + path, legacyAssetRoute(path), true);
}
unsigned HttpServices::get(const std::string &channel, const std::string &path, bool image,
                           bool readiness) {
    if (path.empty() || path[0] != '/' || path.compare(0, 2, "//") == 0 ||
        path.find_first_of("\r\n") != std::string::npos)
        return 0;
    std::unique_lock<std::mutex> lock(mutex_);
    unsigned generation = ++generations_[channel];
    auto &queue = image ? images_ : api_;
    for (auto it = queue.begin(); it != queue.end();) {
        if (it->channel == channel)
            it = queue.erase(it);
        else
            ++it;
    }
    if (queue.size() >= 96) {
        if (image)
            generations_.erase(channel);
        lock.unlock();
        return 0;
    }
    queue.push_back({channel, path, "", generation, false, readiness});
    condition_.notify_all();
    lock.unlock();
    return generation;
}
unsigned HttpServices::write(const std::string &channel, Operation operation, const Json &body) {
    std::string path;
    try {
        path = legacyRoute(operation, body);
    } catch (const std::invalid_argument &) {
        return 0; // no route for the operation: it did not start, and the screen can say so
    }
    if (path.empty() || path[0] != '/' || path.compare(0, 2, "//") == 0 ||
        path.find_first_of("\r\n") != std::string::npos)
        return 0;
    std::unique_lock<std::mutex> lock(mutex_);
    if (api_.size() >= 96) {
        lock.unlock();
        return 0;
    }
    unsigned generation = ++generations_[channel];
    api_.push_back({channel, path, body.dump(), generation, true, false});
    condition_.notify_all();
    lock.unlock();
    return generation;
}
void HttpServices::cancel(const std::string &channel) {
    std::unique_lock<std::mutex> lock(mutex_);
    ++generations_[channel];
    lock.unlock();
}
// The server's session is 32 hex characters; anything else (a header line would break on it) is
// not kept.
void HttpServices::adminSession(const std::string &session) {
    const bool valid = session.size() <= 64 &&
                       session.find_first_not_of("0123456789abcdef") == std::string::npos;
    std::lock_guard<std::mutex> lock(mutex_);
    adminSession_ = valid ? session : std::string();
}
std::vector<Response> HttpServices::drain() {
    std::unique_lock<std::mutex> lock(mutex_);
    std::vector<Response> result;
    while (!completed_.empty()) {
        Response response = std::move(completed_.front());
        completed_.pop_front();
        completedBytes_ -= response.bytes.size();
        if (generations_[response.channel] == response.generation) {
            if (response.channel.compare(0, 6, "image:") == 0)
                generations_.erase(response.channel);
            result.push_back(std::move(response));
        }
    }
    lock.unlock();
    return result;
}
bool HttpServices::idle() {
    std::lock_guard<std::mutex> lock(mutex_);
    return api_.empty() && images_.empty() && completed_.empty() && !working_;
}
void HttpServices::run(bool image) {
    CURL *curl = curl_easy_init();
    for (;;) {
        std::unique_lock<std::mutex> lock(mutex_);
        auto &queue = image ? images_ : api_;
        while (!stopping_ && queue.empty())
            condition_.wait(lock);
        if (stopping_) {
            lock.unlock();
            break;
        }
        Request request = std::move(queue.front());
        queue.pop_front();
        const std::string session = image ? std::string() : adminSession_;
        bool current = generations_[request.channel] == request.generation;
        if (current)
            ++working_;
        lock.unlock();
        if (!current)
            continue;
        Response response;
        response.channel = request.channel;
        response.path = image ? request.channel.substr(6) : request.path;
        response.generation = request.generation;
        if (!curl)
            response.error = "Could not create a connection";
        else {
            using Clock = std::chrono::steady_clock;
            const auto readinessStarted = Clock::now();
            unsigned retries = 0;
            bool cancelled = false;
            for (;;) {
                response.bytes.clear();
                response.error.clear();
                response.status = 0;
                curl_easy_reset(curl);
                Progress progressState = {this, &request};
                curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
                curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress);
                curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &progressState);
                std::string url = origin_ + request.path;
                curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
                curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
                // A write may wait on the TV: stopping or starting LG's LG Promotions & Tips Popups
                // service takes 12.3 s on the B3, so a 12 s limit rolled the switch back to a state
                // the service no longer had.
                long timeoutMs = request.post ? 40000L : 12000L;
                long connectTimeoutMs = 2000L;
                if (request.readiness) {
                    auto elapsed = std::chrono::duration_cast<ReadinessRetry::Milliseconds>(
                        Clock::now() - readinessStarted);
                    timeoutMs = ReadinessRetry::attemptTimeoutMs(elapsed);
                    connectTimeoutMs = std::min<long>(500L, timeoutMs);
                    if (!timeoutMs) {
                        response.error = "Server readiness retry budget expired";
                        break;
                    }
                } else if (image)
                    timeoutMs = 8000L;
                curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, connectTimeoutMs);
                curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeoutMs);
                curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, collect);
                curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.bytes);
                curl_easy_setopt(curl, CURLOPT_PROXY, "");
                curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
                curl_slist *headers = nullptr;
                if (request.post)
                    headers = curl_slist_append(headers, "Content-Type: application/json");
                if (!session.empty())
                    headers = curl_slist_append(headers, ("X-Admin-Session: " + session).c_str());
                if (headers)
                    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
                if (request.post) {
                    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request.body.c_str());
                    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,
                                     static_cast<long>(request.body.size()));
                }
                CURLcode code = curl_easy_perform(curl);
                if (code != CURLE_OK)
                    response.error = curl_easy_strerror(code);
                curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status);
                if (headers)
                    curl_slist_free_all(headers);

                bool transportFailure =
                    code == CURLE_COULDNT_CONNECT || code == CURLE_COULDNT_RESOLVE_HOST ||
                    code == CURLE_GOT_NOTHING || code == CURLE_OPERATION_TIMEDOUT;
                auto elapsed = std::chrono::duration_cast<ReadinessRetry::Milliseconds>(
                    Clock::now() - readinessStarted);
                if (!request.readiness ||
                    !ReadinessRetry::shouldRetry(
                        retries, ReadinessRetry::retryable(transportFailure, response.status),
                        elapsed))
                    break;

                auto wait =
                    std::min(ReadinessRetry::delay(retries), ReadinessRetry::budget() - elapsed);
                std::unique_lock<std::mutex> retryLock(mutex_);
                condition_.wait_for(retryLock, wait, [this, &request] {
                    return stopping_ || generations_[request.channel] != request.generation;
                });
                cancelled = stopping_ || generations_[request.channel] != request.generation;
                retryLock.unlock();
                if (cancelled)
                    break;
                ++retries;
            }
            if (cancelled) {
                lock.lock();
                --working_;
                lock.unlock();
                continue;
            }
        }
        lock.lock();
        if (completedBytes_ + response.bytes.size() > 24 * 1024 * 1024) {
            response.bytes.clear();
            response.error = "Reply queue is full";
        }
        if (completed_.size() >= 256) {
            completedBytes_ -= completed_.front().bytes.size();
            completed_.pop_front();
        }
        completedBytes_ += response.bytes.size();
        completed_.push_back(std::move(response));
        --working_;
        lock.unlock();
    }
    if (curl)
        curl_easy_cleanup(curl);
}
} // namespace nativetv
