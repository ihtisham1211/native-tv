#include "daemon/luna.hpp"
#include <algorithm>
#include <cstdlib>
#include <exception>
#include <atomic>
#include <sstream>
#include <stdexcept>

namespace nativetv {
namespace daemon {
namespace {
std::string withoutScheme(const std::string &uri) {
    return uri.compare(0, 7, "luna://") == 0 ? uri.substr(7) : uri;
}

bool serviceNotFound(const Json &response) {
    Json returned = response["returnValue"];
    if (!returned.isBoolean() || returned.boolean())
        return false;
    return response["errorText"].string().find("Service does not exist") != std::string::npos;
}

bool serviceSucceeded(const Json &response) {
    Json returned = response["returnValue"];
    return !returned.isBoolean() || returned.boolean();
}

std::string childFailure(const ChildResult &result) {
    if (!result.error.empty())
        return result.error;
    if (result.timedOut)
        return "luna-send timed out";
    if (result.outputLimitExceeded)
        return "luna-send reply exceeded the output limit";
    if (result.signal)
        return "luna-send terminated by signal " + std::to_string(result.signal);
    if (result.exitCode != 0)
        return "luna-send exited with status " + std::to_string(result.exitCode);
    return "";
}
} // namespace

LunaClient::LunaClient(EventLoop &loop, ChildProcessRunner &children, Logger *logger,
                       LunaClientOptions options)
    : loop_(loop), children_(children), logger_(logger), options_(std::move(options)) {
    if (options_.executable.empty()) {
        const char *configured = std::getenv("TVWEB_LUNA_SEND");
        options_.executable = configured && *configured ? configured : "/usr/bin/luna-send";
    }
    aliases_.push_back({"com.webos.service.settings/", "com.webos.settingsservice/", 0});
    aliases_.push_back({"com.webos.service.systemservice/", "com.palm.systemservice/", 0});
}

std::shared_ptr<LunaSubscription> LunaClient::subscribe(
    const std::string &uri, const Json &payload, const std::string &appId,
    LunaSubscriptionHandlers handlers) {
    if (uri.empty())
        return std::shared_ptr<LunaSubscription>();
    return std::shared_ptr<LunaSubscription>(new LunaSubscription(
        *this, withoutScheme(uri), payload.valid() ? payload : Json::object(), appId,
        std::move(handlers)));
}

bool LunaClient::call(const std::string &uri, const Json &payload, Callback callback,
                      const std::string &appId, bool urgent,
                      std::chrono::milliseconds timeout, bool allowLongWait) {
    if (uri.empty())
        return false;
    std::shared_ptr<Job> job = std::make_shared<Job>();
    job->uri = withoutScheme(uri);
    job->payload = payload.valid() ? payload : Json::object();
    job->appId = appId;
    job->callback = std::move(callback);
    job->timeout = timeout;
    job->allowLongWait = allowLongWait;
    job->urgent = urgent;
    if (loop_.isLoopThread()) {
        enqueue(job);
        startWaiting();
        return true;
    }
    return loop_.post([this, job]() {
        enqueue(job);
        startWaiting();
    });
}

void LunaClient::enqueue(const std::shared_ptr<Job> &job, bool retryFirst) {
    if (job->urgent) {
        if (retryFirst) {
            waiting_.push_front(job);
            return;
        }
        std::deque<std::shared_ptr<Job>>::iterator position = waiting_.begin();
        while (position != waiting_.end() && (*position)->urgent)
            ++position;
        waiting_.insert(position, job);
        return;
    }
    if (retryFirst) {
        std::deque<std::shared_ptr<Job>>::iterator position = waiting_.begin();
        while (position != waiting_.end() && (*position)->urgent)
            ++position;
        waiting_.insert(position, job);
        return;
    }
    waiting_.push_back(job);
}

void LunaClient::startWaiting() {
    while (!waiting_.empty()) {
        const bool urgent = waiting_.front()->urgent;
        if (urgent ? (urgentRunning_ > 0 || running_ >= 3) : running_ >= 2)
            return;
        std::shared_ptr<Job> job = waiting_.front();
        waiting_.pop_front();
        ++running_;
        if (urgent)
            ++urgentRunning_;
        run(job);
    }
}

void LunaClient::run(const std::shared_ptr<Job> &job) {
    const std::string uri = resolveUri(job->uri);
    note("call " + uri);
    std::vector<std::string> args;
    args.push_back(options_.executable);
    if (!job->appId.empty()) {
        args.push_back("-a");
        args.push_back(job->appId);
    }
    args.push_back("-n");
    args.push_back("1");
    if (!job->allowLongWait) {
        args.push_back("-w");
        args.push_back("2000");
    }
    args.push_back("-f");
    args.push_back("luna://" + uri);
    args.push_back(job->payload.dump());

    ChildOptions childOptions;
    childOptions.arguments = std::move(args);
    childOptions.environment = options_.environment;
    childOptions.timeout = job->timeout.count() > 0 ? job->timeout : options_.timeout;
    childOptions.maximumOutputBytes = options_.maximumReplyBytes;
    childOptions.urgent = job->urgent;
    std::shared_ptr<ChildResult> result = std::make_shared<ChildResult>();
    if (!loop_.submitBlocking([this, childOptions, result]() {
            *result = children_.run(childOptions);
        }, [this, job, result, uri](std::exception_ptr failure) {
            if (logger_ && logger_->enabled(LogLevel::Debug)) {
                std::ostringstream line;
                line << "luna: done " << uri << " in "
                     << std::chrono::duration_cast<std::chrono::milliseconds>(
                            result->endedAt - result->startedAt).count()
                     << "ms";
                if (!result->error.empty())
                    line << " (" << result->error << ")";
                else if (result->timedOut)
                    line << " (timed out)";
                else if (result->signal)
                    line << " (signal " << result->signal << ")";
                logger_->debug(line.str());
            }
            complete(job, result, failure);
        })) {
        if (running_ > 0)
            --running_;
        if (job->urgent && urgentRunning_ > 0)
            --urgentRunning_;
        LunaReply reply;
        reply.error = "Luna worker queue is full";
        if (job->callback)
            job->callback(reply);
    }
}

void LunaClient::complete(const std::shared_ptr<Job> &job,
                          const std::shared_ptr<ChildResult> &result,
                          std::exception_ptr failure) {
    if (running_ > 0)
        --running_;
    if (job->urgent && urgentRunning_ > 0)
        --urgentRunning_;

    if (failure) {
        try {
            std::rethrow_exception(failure);
        } catch (const std::exception &error) {
            result->error = error.what();
        } catch (...) {
            result->error = "unknown Luna worker error";
        }
    }

    if (result->signal && !result->timedOut) {
        const bool retry = !job->retried;
        if (logger_)
            logger_->warning("luna: " + job->uri + " died (signal " +
                             std::to_string(result->signal) + ") before answering" +
                             (retry ? ", trying once more" : ", giving up"));
        if (retry) {
            job->retried = true;
            enqueue(job, true);
            startWaiting();
            return;
        }
    }

    Json parsed;
    if (result->succeeded() && !result->standardOutput.empty())
        parsed = Json::parse(result->standardOutput);

    ServiceAlias *alias = findAlias(job->uri);
    if (alias && alias->state == 0 && serviceNotFound(parsed) && !job->serviceRetried) {
        alias->state = -1;
        job->serviceRetried = true;
        enqueue(job, true);
        startWaiting();
        return;
    }
    if (alias && alias->state == 0 && parsed.valid() && serviceSucceeded(parsed))
        alias->state = 1;

    LunaReply reply;
    reply.response = parsed;
    reply.raw = result->standardOutput;
    reply.error = childFailure(*result);
    reply.startedAt = result->startedAt;
    reply.endedAt = result->endedAt;
    reply.timedOut = result->timedOut;
    if (parsed.valid() && result->error.empty() && result->exitCode == 0)
        reply.error.clear();

    startWaiting();
    if (job->callback)
        job->callback(reply);
}

std::string LunaClient::resolveUri(const std::string &uri) const {
    const ServiceAlias *alias = findAlias(uri);
    if (alias && alias->state < 0)
        return alias->older + uri.substr(alias->newer.size());
    return uri;
}

LunaClient::ServiceAlias *LunaClient::findAlias(const std::string &uri) {
    for (ServiceAlias &alias : aliases_)
        if (uri.compare(0, alias.newer.size(), alias.newer) == 0)
            return &alias;
    return nullptr;
}

const LunaClient::ServiceAlias *LunaClient::findAlias(const std::string &uri) const {
    for (const ServiceAlias &alias : aliases_)
        if (uri.compare(0, alias.newer.size(), alias.newer) == 0)
            return &alias;
    return nullptr;
}

void LunaClient::note(const std::string &message) {
    if (logger_ && logger_->enabled(LogLevel::Debug) && !message.empty())
        logger_->debug("luna: " + message);
}

LunaSubscription::LunaSubscription(LunaClient &client, std::string uri, Json payload,
                                   std::string appId, LunaSubscriptionHandlers handlers)
    : client_(client), uri_(std::move(uri)), payload_(std::move(payload)),
      appId_(std::move(appId)), handlers_(std::move(handlers)) {}

LunaSubscription::~LunaSubscription() {
    std::shared_ptr<std::atomic<bool>> cancellation = cancellationToken();
    if (cancellation)
        cancellation->store(true, std::memory_order_relaxed);
}

bool LunaSubscription::start() {
    std::shared_ptr<LunaSubscription> self = shared_from_this();
    if (client_.loop_.isLoopThread()) {
        startOnLoop();
        return true;
    }
    return client_.loop_.post([self]() { self->startOnLoop(); });
}

void LunaSubscription::stop() {
    std::shared_ptr<LunaSubscription> self = shared_from_this();
    if (client_.loop_.isLoopThread()) {
        stopOnLoop();
        return;
    }
    if (!client_.loop_.post([self]() { self->stopOnLoop(); })) {
        std::shared_ptr<std::atomic<bool>> cancellation = cancellationToken();
        if (cancellation)
            cancellation->store(true, std::memory_order_relaxed);
    }
}

void LunaSubscription::startOnLoop() {
    if (!stopped_)
        return;
    stopped_ = false;
    retryDelay_ = std::chrono::milliseconds(1000);
    retryImmediately_ = false;
    if (active_) {
        restartAfterStop_ = true;
        return;
    }
    connectOnLoop();
}

void LunaSubscription::stopOnLoop() {
    stopped_ = true;
    restartAfterStop_ = false;
    retryImmediately_ = false;
    if (retryTimer_) {
        client_.loop_.cancelTimer(retryTimer_);
        retryTimer_ = 0;
    }
    std::shared_ptr<std::atomic<bool>> cancellation = cancellationToken();
    if (cancellation)
        cancellation->store(true, std::memory_order_relaxed);
    buffer_.clear();
    ++generation_; // discard already-posted output from the old child
}

void LunaSubscription::connectOnLoop() {
    if (stopped_ || active_)
        return;
    active_ = true;
    const unsigned long long generation = ++generation_;
    std::shared_ptr<std::atomic<bool>> cancellation =
        std::make_shared<std::atomic<bool>>(false);
    {
        std::lock_guard<std::mutex> lock(cancellationMutex_);
        cancellation_ = cancellation;
    }
    buffer_.clear();

    const std::string uri = client_.resolveUri(uri_);
    client_.note("subscribe " + uri);
    std::vector<std::string> arguments;
    arguments.push_back(client_.options_.executable);
    if (!appId_.empty()) {
        arguments.push_back("-a");
        arguments.push_back(appId_);
    }
    arguments.push_back("-i");
    arguments.push_back("luna://" + uri);
    arguments.push_back(payload_.dump());

    std::weak_ptr<LunaSubscription> weak = shared_from_this();
    std::shared_ptr<ChildResult> result = std::make_shared<ChildResult>();
    ChildOptions options;
    options.arguments = std::move(arguments);
    options.environment = client_.options_.environment;
    options.timeout = std::chrono::milliseconds(0); // subscriptions end when stopped or disconnected
    options.maximumOutputBytes = 0;
    options.captureOutput = false;
    options.cancellation = cancellation;
    options.onStandardOutput = [weak, generation, cancellation](const std::string &chunk) {
        std::shared_ptr<LunaSubscription> self = weak.lock();
        if (!self)
            return;
        if (!self->client_.loop_.post([weak, generation, chunk]() {
                std::shared_ptr<LunaSubscription> current = weak.lock();
                if (current)
                    current->onOutput(generation, chunk);
        }) && cancellation) {
            cancellation->store(true, std::memory_order_relaxed);
        }
    };
    options.onStandardError = [weak, generation](const std::string &chunk) {
        std::shared_ptr<LunaSubscription> self = weak.lock();
        if (!self)
            return;
        self->client_.loop_.post([weak, generation, chunk]() {
            std::shared_ptr<LunaSubscription> current = weak.lock();
            if (current && current->generation_ == generation && !current->stopped_)
                current->reportError(chunk);
        });
    };

    if (!client_.loop_.submitBlocking([&runner = client_.children_, options, result]() {
            *result = runner.run(options);
        }, [weak, generation, result](std::exception_ptr failure) {
            std::shared_ptr<LunaSubscription> self = weak.lock();
            if (self)
                self->onExit(generation, *result, failure);
        })) {
        active_ = false;
        reportError("Luna worker queue is full");
        retryTimer_ = client_.loop_.scheduleAfter(retryDelay_, [weak]() {
            std::shared_ptr<LunaSubscription> self = weak.lock();
            if (self) {
                self->retryTimer_ = 0;
                self->connectOnLoop();
            }
        });
        retryDelay_ = std::min(retryDelay_ * 2, std::chrono::milliseconds(30000));
    }
}

void LunaSubscription::onOutput(unsigned long long generation, const std::string &chunk) {
    if (generation != generation_ || stopped_)
        return;
    buffer_ += chunk;
    size_t start = std::string::npos;
    int depth = 0;
    bool quoted = false;
    bool escaped = false;
    for (size_t i = 0; i < buffer_.size(); ++i) {
        const char ch = buffer_[i];
        if (start == std::string::npos) {
            if (ch != '{')
                continue;
            start = i;
        }
        if (quoted) {
            if (escaped)
                escaped = false;
            else if (ch == '\\')
                escaped = true;
            else if (ch == '"')
                quoted = false;
        } else if (ch == '"') {
            quoted = true;
        } else if (ch == '{' || ch == '[') {
            ++depth;
        } else if (ch == '}' || ch == ']') {
            if (--depth == 0) {
                std::string raw = buffer_.substr(start, i - start + 1);
                Json parsed = Json::parse(raw);
                if (!parsed.valid()) {
                    reportError("invalid JSON in Luna subscription: " + raw);
                } else {
                    LunaClient::ServiceAlias *alias = client_.findAlias(uri_);
                    if (alias && alias->state == 0 && serviceNotFound(parsed)) {
                        alias->state = -1;
                        retryImmediately_ = true;
                        std::shared_ptr<std::atomic<bool>> cancellation = cancellationToken();
                        if (cancellation)
                            cancellation->store(true, std::memory_order_relaxed);
                    } else {
                        if (alias && alias->state == 0 && serviceSucceeded(parsed))
                            alias->state = 1;
                        if (handlers_.message)
                            handlers_.message(parsed);
                    }
                }
                start = std::string::npos;
                depth = 0;
                quoted = false;
                escaped = false;
            }
        }
    }
    if (start == std::string::npos)
        buffer_.clear();
    else
        buffer_ = buffer_.substr(start);
    if (buffer_.size() > 1024 * 1024) {
        buffer_.clear();
        reportError("Luna subscription response exceeded 1 MiB");
        std::shared_ptr<std::atomic<bool>> cancellation = cancellationToken();
        if (cancellation)
            cancellation->store(true, std::memory_order_relaxed);
    }
}

void LunaSubscription::onExit(unsigned long long generation, const ChildResult &result,
                              std::exception_ptr failure) {
    (void)generation;
    active_ = false;
    {
        std::lock_guard<std::mutex> lock(cancellationMutex_);
        cancellation_.reset();
    }
    if (stopped_)
        return;
    if (failure) {
        try {
            std::rethrow_exception(failure);
        } catch (const std::exception &error) {
            reportError(error.what());
        } catch (...) {
            reportError("unknown Luna subscription worker error");
        }
    } else if (!result.error.empty()) {
        reportError(result.error);
    }
    if (handlers_.close)
        handlers_.close(result.exitCode, result.signal);
    if (stopped_)
        return;
    if (restartAfterStop_) {
        restartAfterStop_ = false;
        retryDelay_ = std::chrono::milliseconds(1000);
        connectOnLoop();
        return;
    }
    const std::chrono::milliseconds delay = retryImmediately_
                                               ? std::chrono::milliseconds(0)
                                               : retryDelay_;
    retryImmediately_ = false;
    std::weak_ptr<LunaSubscription> weak = shared_from_this();
    retryTimer_ = client_.loop_.scheduleAfter(delay, [weak]() {
        std::shared_ptr<LunaSubscription> self = weak.lock();
        if (self) {
            self->retryTimer_ = 0;
            self->connectOnLoop();
        }
    });
    if (delay.count() > 0)
        retryDelay_ = std::min(retryDelay_ * 2, std::chrono::milliseconds(30000));
}

void LunaSubscription::reportError(const std::string &error) {
    if (handlers_.error)
        handlers_.error(error);
}

std::shared_ptr<std::atomic<bool>> LunaSubscription::cancellationToken() const {
    std::lock_guard<std::mutex> lock(cancellationMutex_);
    return cancellation_;
}
} // namespace daemon
} // namespace nativetv
