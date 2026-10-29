#include "daemon/game_frame_rate.hpp"
#include "daemon/logger.hpp"
#include "daemon/luna.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>

namespace nativetv {
namespace daemon {
namespace {
const char *kBindUri = "com.webos.service.utp.extinputs/bind";
const char *kReadingUri = "com.webos.service.utp.extinputs/getVRRInfo";
const std::chrono::seconds kIdleTimeout(15);
// The stats' reader holds the bind for this long after its last read of an input
// (nothing reports a source change without MQTT), well above any scrape interval.
const std::chrono::seconds kReadIdleTimeout(300);
// The bind's first reply comes after a luna-send has started: a read waits this
// long for it rather than coming back empty after every switch to an input.
const std::chrono::milliseconds kBindWait(1500);
const std::chrono::milliseconds kOneShotTtl(2000);

Json emptyReading() {
    Json result = Json::object();
    result.set("frameRate", Json::numeric(0));
    result.set("vrrType", Json::text("off"));
    result.set("port", Json());
    return result;
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

bool unsupported(const Json &response) {
    if (!response["returnValue"].isBoolean() || response["returnValue"].boolean())
        return false;
    const std::string error = lower(response["errorText"].string());
    return error.find("unknown method") != std::string::npos ||
           error.find("unknown service") != std::string::npos;
}

Json readingFromReply(const Json &reply) {
    if (!reply.isObject() ||
        (reply["returnValue"].isBoolean() && !reply["returnValue"].boolean()))
        return Json();
    const Json info = reply["vrrInfo"];
    if (!info.isObject())
        return Json();

    double frameRate = 0;
    Json vrrType = info["vrrType"];
    const Json rawFrameRate = info["frameRate"];
    if (!rawFrameRate.isNumber() && info["sublist_1"].isString()) {
        const std::string raw = info["sublist_1"].string();
        char *end = nullptr;
        const double parsed = std::strtod(raw.c_str(), &end);
        if (end != raw.c_str() && std::isfinite(parsed))
            frameRate = parsed;
        const std::string type = info["sublist_2"].string();
        vrrType = type.empty() || type == "FIXED" ? Json::text("off")
                                                   : Json::text(type);
    } else if (rawFrameRate.isNumber() && std::isfinite(rawFrameRate.number())) {
        frameRate = rawFrameRate.number();
    }
    const bool vrrTypeTruthy = vrrType.valid() &&
        !(vrrType.isBoolean() && !vrrType.boolean()) &&
        !(vrrType.isNumber() && vrrType.number() == 0) &&
        !(vrrType.isString() && vrrType.string().empty());
    if (!vrrTypeTruthy)
        vrrType = Json::text("off");

    Json result = Json::object();
    result.set("frameRate", Json::numeric(frameRate));
    result.set("vrrType", vrrType);
    const Json port = reply["port"];
    const bool portTruthy = port.valid() &&
        !(port.isBoolean() && !port.boolean()) &&
        !(port.isNumber() && port.number() == 0) &&
        !(port.isString() && port.string().empty());
    result.set("port", portTruthy ? port : Json());
    return result;
}
} // namespace

GameFrameRate::GameFrameRate(EventLoop &loop, LunaClient &luna, Logger *logger)
    : loop_(loop), luna_(luna), logger_(logger), latest_(emptyReading()) {}

GameFrameRate::~GameFrameRate() {
    if (loop_.isLoopThread()) {
        stopOnLoop();
        return;
    }
    if (idleTimer_)
        (void)loop_.cancelTimer(idleTimer_);
    if (readIdleTimer_)
        (void)loop_.cancelTimer(readIdleTimer_);
    if (waitTimer_)
        (void)loop_.cancelTimer(waitTimer_);
    if (readingSubscription_)
        readingSubscription_->stop();
    if (bindSubscription_)
        bindSubscription_->stop();
}

Json GameFrameRate::frameRate() {
    Json latest;
    {
        std::lock_guard<std::mutex> lock(latestMutex_);
        latest = latest_;
    }
    (void)loop_.post([this]() { requestOnLoop(); });
    return latest;
}

void GameFrameRate::requestOnLoop() {
    if (!loop_.isLoopThread() || !supported_)
        return;
    if (idleTimer_)
        (void)loop_.cancelTimer(idleTimer_);
    idleTimer_ = loop_.scheduleAfter(kIdleTimeout, [this]() {
        idleTimer_ = 0;
        stopStream();
    });
    if (!idleTimer_) {
        stopStream();
        return;
    }
    if (!bindSubscription_)
        startBind();
    else if (!pipeline_.empty() && !readingSubscription_)
        startReading();
}

void GameFrameRate::startBind() {
    Json payload = Json::object();
    payload.set("subscribe", Json::flag(true));
    const unsigned long long generation = ++generation_;
    LunaSubscriptionHandlers handlers;
    handlers.message = [this, generation](const Json &response) {
        receiveBind(generation, response);
    };
    handlers.error = [this, generation](const std::string &error) {
        if (generation == generation_ && logger_)
            logger_->warning("game: input bind subscription: " + error);
    };
    handlers.close = [this, generation](int code, int signal) {
        receiveBindClose(generation, code, signal);
    };
    bindSubscription_ = luna_.subscribe(kBindUri, payload, "", std::move(handlers));
    if (!bindSubscription_ || !bindSubscription_->start()) {
        bindSubscription_.reset();
        if (logger_)
            logger_->warning("game: could not start input bind subscription");
    }
}

void GameFrameRate::startReading() {
    if (!loop_.isLoopThread() || pipeline_.empty() || readingSubscription_ || !supported_)
        return;
    Json payload = Json::object();
    payload.set("subscribe", Json::flag(true));
    payload.set("pipelineId", Json::text(pipeline_));
    payload.set("mode", Json::text("GameOptimizer"));
    payload.set("interval", Json::numeric(1000));
    const unsigned long long generation = generation_;
    LunaSubscriptionHandlers handlers;
    handlers.message = [this, generation](const Json &response) {
        receiveReading(generation, response);
    };
    handlers.error = [this, generation](const std::string &error) {
        if (generation == generation_ && logger_)
            logger_->warning("game: frame-rate subscription: " + error);
    };
    readingSubscription_ = luna_.subscribe(kReadingUri, payload, "", std::move(handlers));
    if (!readingSubscription_ || !readingSubscription_->start()) {
        readingSubscription_.reset();
        if (logger_)
            logger_->warning("game: could not start frame-rate subscription");
    }
}

void GameFrameRate::receiveBind(unsigned long long generation, const Json &response) {
    if (!loop_.isLoopThread() || generation != generation_)
        return;
    // Whatever the reply says, the reads waiting for the bind have their answer.
    struct FlushOnExit {
        GameFrameRate &self;
        ~FlushOnExit() { self.flushWaiting(); }
    } flushOnExit{*this};
    if (response["binded"].isBoolean() && !response["binded"].boolean()) {
        clearPipeline();
        return;
    }
    if (response["status"].string() == "disconnected") {
        clearPipeline();
        return;
    }
    if (unsupported(response)) {
        supported_ = false;
        stopStream();
        if (logger_)
            logger_->info("game: this TV does not support the Game Optimizer input pipeline");
        return;
    }
    const std::string pipeline = response["broadcastId"].string();
    if (pipeline.empty())
        return;
    if (pipeline != pipeline_) {
        if (readingSubscription_)
            readingSubscription_->stop();
        readingSubscription_.reset();
        pipeline_ = pipeline;
        setLatest(emptyReading());
    }
    if (idleTimer_ && !readingSubscription_)
        startReading();
}

void GameFrameRate::receiveReading(unsigned long long generation,
                                   const Json &response) {
    if (!loop_.isLoopThread() || generation != generation_)
        return;
    if (unsupported(response)) {
        supported_ = false;
        stopStream();
        if (logger_)
            logger_->info("game: this TV does not support Game Optimizer frame-rate readings");
        return;
    }
    const Json reading = readingFromReply(response);
    if (reading.valid())
        setLatest(reading);
}

void GameFrameRate::receiveBindClose(unsigned long long generation, int code, int signal) {
    if (!loop_.isLoopThread() || generation != generation_)
        return;
    clearPipeline();
    if (logger_ && (code != 0 || signal != 0))
        logger_->warning("game: input bind subscription closed (" +
                         std::to_string(code) + ", " + std::to_string(signal) + ")");
}

void GameFrameRate::clearPipeline() {
    ++generation_;
    if (readingSubscription_)
        readingSubscription_->stop();
    readingSubscription_.reset();
    if (bindSubscription_)
        bindSubscription_->stop();
    bindSubscription_.reset();
    pipeline_.clear();
    setLatest(emptyReading());
    flushWaiting();
}

void GameFrameRate::stopStream() {
    if (!loop_.isLoopThread())
        return;
    if (idleTimer_) {
        (void)loop_.cancelTimer(idleTimer_);
        idleTimer_ = 0;
    }
    if (inputShown_ && supported_) {
        // The stats' reader still holds the bind: only the stream goes.
        if (readingSubscription_)
            readingSubscription_->stop();
        readingSubscription_.reset();
        setLatest(emptyReading());
        return;
    }
    clearPipeline();
}

void GameFrameRate::stopOnLoop() {
    inputShown_ = false;
    if (readIdleTimer_) {
        (void)loop_.cancelTimer(readIdleTimer_);
        readIdleTimer_ = 0;
    }
    stopStream();
}

Json GameFrameRate::readForStats(bool inputShown) {
    if (loop_.isLoopThread())
        return Json();
    const std::shared_ptr<ReadRequest> request = std::make_shared<ReadRequest>();
    if (!loop_.post([this, request, inputShown]() { readOnLoop(request, inputShown); }))
        return Json();
    std::unique_lock<std::mutex> lock(request->mutex);
    // The bind wait and one Luna call, with room to spare.
    request->done.wait_for(lock, std::chrono::seconds(8), [&request]() { return request->finished; });
    return request->finished ? request->reading : Json();
}

void GameFrameRate::readOnLoop(const std::shared_ptr<ReadRequest> &request, bool inputShown) {
    setInputShown(inputShown);
    if (!inputShown_ || !supported_) {
        finishRead(request, Json());
        return;
    }
    if (pipeline_.empty() && !readingSubscription_) {
        waiting_.push_back(request);
        if (!waitTimer_) {
            waitTimer_ = loop_.scheduleAfter(kBindWait, [this]() {
                waitTimer_ = 0;
                flushWaiting();
            });
            if (!waitTimer_)
                flushWaiting();
        }
        return;
    }
    readPipeline(request);
}

void GameFrameRate::readPipeline(const std::shared_ptr<ReadRequest> &request) {
    // The stream already asks once a second.
    if (readingSubscription_) {
        Json latest;
        {
            std::lock_guard<std::mutex> lock(latestMutex_);
            latest = latest_;
        }
        finishRead(request, latest);
        return;
    }
    if (pipeline_.empty()) {
        finishRead(request, Json());
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (oneShotPipeline_ == pipeline_ && now >= oneShotAt_ && now - oneShotAt_ < kOneShotTtl) {
        finishRead(request, oneShotReading_);
        return;
    }
    Json payload = Json::object();
    payload.set("pipelineId", Json::text(pipeline_));
    payload.set("mode", Json::text("GameOptimizer"));
    const std::string pipeline = pipeline_;
    const bool accepted = luna_.call(kReadingUri, payload, [this, request, pipeline](const LunaReply &reply) {
        const Json reading = readingFromReply(reply.response);
        // Only a real answer is worth sharing; a lost one is asked again.
        if (reply.response.isObject()) {
            oneShotPipeline_ = pipeline;
            oneShotAt_ = std::chrono::steady_clock::now();
            oneShotReading_ = reading;
        }
        finishRead(request, reading);
    });
    if (!accepted)
        finishRead(request, Json());
}

void GameFrameRate::finishRead(const std::shared_ptr<ReadRequest> &request, const Json &reading) {
    std::lock_guard<std::mutex> lock(request->mutex);
    request->reading = reading;
    request->finished = true;
    request->done.notify_all();
}

void GameFrameRate::flushWaiting() {
    if (waitTimer_) {
        (void)loop_.cancelTimer(waitTimer_);
        waitTimer_ = 0;
    }
    std::vector<std::shared_ptr<ReadRequest>> list;
    list.swap(waiting_);
    for (const std::shared_ptr<ReadRequest> &request : list)
        readPipeline(request);
}

void GameFrameRate::setInputShown(bool shown) {
    inputShown_ = shown;
    if (readIdleTimer_) {
        (void)loop_.cancelTimer(readIdleTimer_);
        readIdleTimer_ = 0;
    }
    if (shown)
        readIdleTimer_ = loop_.scheduleAfter(kReadIdleTimeout, [this]() {
            readIdleTimer_ = 0;
            setInputShown(false);
        });
    holdBind();
}

void GameFrameRate::holdBind() {
    const bool wanted = supported_ && (idleTimer_ || inputShown_);
    if (wanted && !bindSubscription_)
        startBind();
    else if (!wanted && bindSubscription_)
        clearPipeline();
}

void GameFrameRate::stop() {
    if (loop_.isLoopThread()) {
        stopOnLoop();
        return;
    }
    (void)loop_.post([this]() { stopOnLoop(); });
}

void GameFrameRate::setLatest(const Json &reading) {
    std::lock_guard<std::mutex> lock(latestMutex_);
    latest_ = reading;
}

} // namespace daemon
} // namespace nativetv
