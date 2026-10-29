#include "daemon/luna.hpp"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>

using namespace nativetv;
using namespace nativetv::daemon;

namespace {
void check(bool condition, const std::string &message) {
    if (!condition)
        throw std::runtime_error(message);
}

ChildRunnerOptions fastStarts() {
    ChildRunnerOptions options;
    options.startupGap = std::chrono::milliseconds(0);
    options.regularGap = std::chrono::milliseconds(0);
    options.startupWindow = std::chrono::milliseconds(0);
    return options;
}

void testLegacyServiceFallbackCase(const std::string &mode, const std::string &uri,
                                   const std::string &expectedUri) {
    EventLoop loop(4, 16, 32);
    ChildProcessRunner children(fastStarts());
    LoggerOptions loggerOptions;
    loggerOptions.environmentLevel = "debug";
    loggerOptions.sink = [](LogLevel, const std::string &, bool) {};
    Logger logger(loggerOptions);
    LunaClientOptions options;
    const char *previousLunaSend = std::getenv("TVWEB_LUNA_SEND");
    const bool hadPreviousLunaSend = previousLunaSend != nullptr;
    const std::string oldLunaSend = previousLunaSend ? previousLunaSend : "";
    if (mode == "legacy-settings")
        options.executable = NATIVETV_FAKE_LUNA_SEND;
    else
        check(setenv("TVWEB_LUNA_SEND", NATIVETV_FAKE_LUNA_SEND, 1) == 0,
              "could not set TVWEB_LUNA_SEND seam");
    options.environment.push_back("NATIVETV_FAKE_LUNA_MODE=" + mode);
    LunaClient luna(loop, children, &logger, options);
    if (mode != "legacy-settings") {
        int restoreResult = hadPreviousLunaSend
                                ? setenv("TVWEB_LUNA_SEND", oldLunaSend.c_str(), 1)
                                : unsetenv("TVWEB_LUNA_SEND");
        check(restoreResult == 0, "could not restore TVWEB_LUNA_SEND seam");
    }

    std::mutex mutex;
    std::condition_variable condition;
    int calls = 0;
    bool success = true;
    bool accepted = luna.call(uri, Json::object(), [&](const LunaReply &first) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!first.error.empty() || !first.response["legacy"].boolean() ||
                first.response["uri"].string() != expectedUri)
                success = false;
        }
        const bool acceptedSecond = luna.call(uri, Json::object(), [&](const LunaReply &second) {
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (!second.error.empty() || !second.response["legacy"].boolean() ||
                    second.response["uri"].string() != expectedUri)
                    success = false;
                ++calls;
            }
            condition.notify_one();
        });
        if (!acceptedSecond) {
            std::lock_guard<std::mutex> lock(mutex);
            success = false;
            ++calls;
            condition.notify_one();
        }
    });
    check(accepted, "first Luna call should be accepted");

    std::thread loopThread([&]() { loop.run(); });
    {
        std::unique_lock<std::mutex> lock(mutex);
        check(condition.wait_for(lock, std::chrono::seconds(3), [&]() { return calls == 1; }),
              "legacy service retry did not complete");
    }
    loop.stop();
    loopThread.join();
    check(success, "new service name should fall back and remain cached as legacy");
}

void testLegacyServiceFallback() {
    testLegacyServiceFallbackCase("legacy-settings",
        "com.webos.service.settings/getSystemSettings",
        "com.webos.settingsservice/getSystemSettings");
    testLegacyServiceFallbackCase("legacy-systemservice",
        "com.webos.service.systemservice/clock/setTime",
        "com.palm.systemservice/clock/setTime");
}

void testTwoCallLimit() {
    EventLoop loop(4, 16, 32);
    ChildProcessRunner children(fastStarts());
    LunaClientOptions options;
    options.executable = NATIVETV_FAKE_LUNA_SEND;
    options.environment.push_back("NATIVETV_FAKE_LUNA_MODE=slow");
    LunaClient luna(loop, children, nullptr, options);

    std::mutex mutex;
    std::condition_variable condition;
    std::vector<LunaReply> replies;
    bool accepted = true;
    for (int i = 0; i < 4; ++i) {
        Json payload = Json::object();
        payload.set("index", Json::numeric(i));
        accepted = luna.call("com.webos.audio/getVolume", payload,
                             [&](const LunaReply &reply) {
            bool allDone = false;
            {
                std::lock_guard<std::mutex> lock(mutex);
                replies.push_back(reply);
                allDone = replies.size() == 4;
            }
            condition.notify_one();
            if (allDone)
                loop.stop();
        }) && accepted;
    }
    check(accepted, "all four calls should be accepted by the event loop");
    std::thread loopThread([&]() { loop.run(); });
    {
        std::unique_lock<std::mutex> lock(mutex);
        check(condition.wait_for(lock, std::chrono::seconds(4), [&]() { return replies.size() == 4; }),
              "queued Luna calls did not all complete");
    }
    loop.stop();
    loopThread.join();

    std::vector<std::pair<std::chrono::steady_clock::time_point, int>> events;
    for (const LunaReply &reply : replies) {
        check(reply.error.empty() && reply.response["returnValue"].boolean(),
              "fake Luna call should succeed");
        events.push_back({reply.startedAt, 1});
        events.push_back({reply.endedAt, -1});
    }
    std::sort(events.begin(), events.end(), [](const auto &a, const auto &b) {
        if (a.first != b.first)
            return a.first < b.first;
        return a.second < b.second; // an end at a timestamp precedes a start
    });
    int active = 0;
    int maximum = 0;
    for (const auto &event : events) {
        active += event.second;
        maximum = std::max(maximum, active);
    }
    check(maximum == 2, "Luna client should run at most two one-shot calls in parallel");
}

void testSubscriptionFramesAndStop() {
    EventLoop loop(3, 16, 32);
    ChildProcessRunner children(fastStarts());
    LunaClientOptions options;
    options.executable = NATIVETV_FAKE_LUNA_SEND;
    options.environment.push_back("NATIVETV_FAKE_LUNA_MODE=subscription");
    LunaClient luna(loop, children, nullptr, options);

    std::mutex mutex;
    std::condition_variable condition;
    std::vector<int> counts;
    std::shared_ptr<LunaSubscription> subscription;
    LunaSubscriptionHandlers handlers;
    handlers.message = [&](const Json &message) {
        const int count = static_cast<int>(message["count"].number());
        {
            std::lock_guard<std::mutex> lock(mutex);
            counts.push_back(count);
        }
        if (count == 2)
            subscription->stop();
        condition.notify_one();
    };
    subscription = luna.subscribe("com.webos.audio/getVolume", Json::object(), "", handlers);
    check(subscription && subscription->start(), "subscription should be accepted");
    std::thread loopThread([&]() { loop.run(); });
    {
        std::unique_lock<std::mutex> lock(mutex);
        check(condition.wait_for(lock, std::chrono::seconds(3), [&]() {
                  return counts.size() >= 2;
              }), "subscription did not frame both JSON messages");
    }
    loop.stop();
    loopThread.join();
    check(counts.size() == 2 && counts[0] == 1 && counts[1] == 2,
          "subscription messages should arrive in order and stop cleanly");
}

// test-state-push.js: a reply over several lines and chunks is read whole, and a
// brace inside a string does not end it.
void testSubscriptionMultilineFragments() {
    EventLoop loop(3, 16, 32);
    ChildProcessRunner children(fastStarts());
    LunaClientOptions options;
    options.executable = NATIVETV_FAKE_LUNA_SEND;
    options.environment.push_back("NATIVETV_FAKE_LUNA_MODE=subscription-pretty");
    LunaClient luna(loop, children, nullptr, options);

    std::mutex mutex;
    std::condition_variable condition;
    std::vector<Json> messages;
    std::shared_ptr<LunaSubscription> subscription;
    LunaSubscriptionHandlers handlers;
    handlers.message = [&](const Json &message) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            messages.push_back(message);
        }
        condition.notify_one();
    };
    subscription = luna.subscribe("com.webos.surfacemanager/getForegroundAppInfo",
                                  Json::object(), "", handlers);
    check(subscription && subscription->start(), "pretty subscription should start");
    std::thread loopThread([&]() { loop.run(); });
    {
        std::unique_lock<std::mutex> lock(mutex);
        check(condition.wait_for(lock, std::chrono::seconds(3), [&]() {
                  return messages.size() >= 2;
              }), "the multi-line reply and the compact one after it were not both delivered");
    }
    subscription->stop();
    loop.stop();
    loopThread.join();
    check(messages[0]["foregroundAppInfo"][static_cast<size_t>(0)]["appId"].string() == "test}\"app" &&
              messages[0]["foregroundAppInfo"][static_cast<size_t>(0)]["displayId"].number() == 0,
          "a fragmented multi-line reply must be read whole");
    check(messages[1]["count"].number() == 2, "the reply after it must follow");
}

void testSubscriptionBackoff() {
    EventLoop loop(3, 16, 32);
    ChildProcessRunner children(fastStarts());
    LunaClientOptions options;
    options.executable = NATIVETV_FAKE_LUNA_SEND;
    options.environment.push_back("NATIVETV_FAKE_LUNA_MODE=subscription-drop");
    LunaClient luna(loop, children, nullptr, options);

    std::mutex mutex;
    std::condition_variable condition;
    std::vector<std::chrono::steady_clock::time_point> closes;
    std::shared_ptr<LunaSubscription> subscription;
    LunaSubscriptionHandlers handlers;
    handlers.close = [&](int, int) {
        bool finished;
        {
            std::lock_guard<std::mutex> lock(mutex);
            closes.push_back(std::chrono::steady_clock::now());
            finished = closes.size() == 2;
        }
        if (finished) {
            subscription->stop();
            loop.stop();
        }
        condition.notify_one();
    };
    subscription = luna.subscribe("com.webos.audio/getVolume", Json::object(), "", handlers);
    check(subscription && subscription->start(), "drop-test subscription should start");
    std::thread loopThread([&]() { loop.run(); });
    {
        std::unique_lock<std::mutex> lock(mutex);
        check(condition.wait_for(lock, std::chrono::seconds(4), [&]() {
                  return closes.size() >= 2;
              }), "subscription did not reconnect after it closed");
    }
    loop.stop();
    loopThread.join();
    check(closes.size() == 2 && closes[1] - closes[0] >= std::chrono::milliseconds(900),
          "first reconnect should use the one-second backoff");
}

void testSubscriptionServiceFallback() {
    EventLoop loop(3, 16, 32);
    ChildProcessRunner children(fastStarts());
    LunaClientOptions options;
    options.executable = NATIVETV_FAKE_LUNA_SEND;
    options.environment.push_back("NATIVETV_FAKE_LUNA_MODE=legacy-settings");
    LunaClient luna(loop, children, nullptr, options);

    std::mutex mutex;
    std::condition_variable condition;
    bool receivedLegacy = false;
    std::shared_ptr<LunaSubscription> subscription;
    LunaSubscriptionHandlers handlers;
    handlers.message = [&](const Json &message) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            receivedLegacy = message["uri"].string() ==
                             "com.webos.settingsservice/getSystemSettings";
        }
        subscription->stop();
        condition.notify_one();
    };
    subscription = luna.subscribe("com.webos.service.settings/getSystemSettings",
                                  Json::object(), "", handlers);
    check(subscription && subscription->start(), "legacy subscription should start");
    std::thread loopThread([&]() { loop.run(); });
    {
        std::unique_lock<std::mutex> lock(mutex);
        check(condition.wait_for(lock, std::chrono::seconds(3), [&]() {
                  return receivedLegacy;
              }), "subscription did not retry using the legacy settings service");
    }
    loop.stop();
    loopThread.join();
    check(receivedLegacy, "subscription fallback should deliver the old service response");
}

// A luna-send stand-in that dies from a signal, hangs, or answers, by
// LUNA_MODE, and counts its runs in LUNA_COUNT.
struct ScriptedLuna {
    std::string directory;
    std::string executable;
    std::string countFile;

    explicit ScriptedLuna(const std::string &mode) {
        char pattern[] = "/tmp/nativetv-luna-XXXXXX";
        const char *made = mkdtemp(pattern);
        check(made != nullptr, "could not make a scratch directory");
        directory = made;
        executable = directory + "/luna-send";
        countFile = directory + "/count";
        std::ofstream script(executable.c_str());
        script << "#!/bin/sh\n"
               << "echo run >> \"$LUNA_COUNT\"\n"
               << "if [ \"$LUNA_MODE\" = abort-once ] && [ ! -f \"$LUNA_COUNT.mark\" ]; then\n"
               << "  : > \"$LUNA_COUNT.mark\"; kill -KILL $$\n"
               << "fi\n"
               << "[ \"$LUNA_MODE\" = abort-always ] && kill -KILL $$\n"
               << "[ \"$LUNA_MODE\" = hang ] && exec /bin/sleep 5\n"
               << "echo '{\"returnValue\":true,\"modelName\":\"50UP81006LR\"}'\n";
        script.close();
        check(chmod(executable.c_str(), 0755) == 0, "could not make the stand-in executable");
        modeVariable = "LUNA_MODE=" + mode;
    }
    ~ScriptedLuna() {
        unlink(countFile.c_str());
        unlink((countFile + ".mark").c_str());
        unlink(executable.c_str());
        rmdir(directory.c_str());
    }
    std::vector<std::string> environment() const {
        return {modeVariable, "LUNA_COUNT=" + countFile};
    }
    int runs() const {
        std::ifstream file(countFile.c_str());
        std::string line;
        int count = 0;
        while (std::getline(file, line))
            ++count;
        return count;
    }
    std::string modeVariable;
};

LunaReply callAndWait(LunaClient &luna, EventLoop &loop, const std::string &uri) {
    std::mutex mutex;
    std::condition_variable condition;
    bool done = false;
    LunaReply result;
    check(luna.call(uri, Json::object(), [&](const LunaReply &reply) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            result = reply;
            done = true;
        }
        condition.notify_one();
    }), "scripted Luna call was not accepted");
    std::thread loopThread([&]() { loop.run(); });
    {
        std::unique_lock<std::mutex> lock(mutex);
        const bool finished = condition.wait_for(lock, std::chrono::seconds(8),
                                                 [&]() { return done; });
        loop.stop();
        lock.unlock();
        loopThread.join();
        check(finished, "scripted Luna call did not complete");
    }
    return result;
}

bool logged(const Logger &logger, const std::string &fragment) {
    for (const LogRecord &record : logger.snapshot(200))
        if (record.message.find(fragment) != std::string::npos)
            return true;
    return false;
}

// A logger that keeps its records in the ring and prints nothing.
struct QuietLogger {
    static LoggerOptions options(const char *level) {
        LoggerOptions loggerOptions;
        loggerOptions.environmentLevel = level;
        loggerOptions.sink = [](LogLevel, const std::string &, bool) {};
        return loggerOptions;
    }
    explicit QuietLogger(const char *level) : logger(options(level)) {}
    Logger logger;
};

void testAbortedCallRunsOnce() {
    const std::string uri = "com.webos.service.tv.systemproperty/getSystemProperties";
    {
        // A child killed before answering is run once more and the second answer used.
        ScriptedLuna scripted("abort-once");
        EventLoop loop(4, 16, 32);
        ChildProcessRunner children(fastStarts());
        QuietLogger quiet("info");
        Logger &logger = quiet.logger;
        LunaClientOptions options;
        options.executable = scripted.executable;
        options.environment = scripted.environment();
        LunaClient luna(loop, children, &logger, options);
        const LunaReply reply = callAndWait(luna, loop, uri);
        check(scripted.runs() == 2, "an aborted call should run once more");
        check(reply.error.empty() && reply.response["modelName"].string() == "50UP81006LR",
              "the second run's answer should be returned");
        check(logged(logger, "died (signal 9) before answering, trying once more"),
              "the abort and the retry should be logged");
    }
    {
        // Only once: a second death is given up on.
        ScriptedLuna scripted("abort-always");
        EventLoop loop(4, 16, 32);
        ChildProcessRunner children(fastStarts());
        QuietLogger quiet("info");
        Logger &logger = quiet.logger;
        LunaClientOptions options;
        options.executable = scripted.executable;
        options.environment = scripted.environment();
        LunaClient luna(loop, children, &logger, options);
        const LunaReply reply = callAndWait(luna, loop, uri);
        check(scripted.runs() == 2, "a second abort should not be retried");
        check(!reply.error.empty() && !reply.response.valid(),
              "a call that died twice should report an error and no answer");
        check(logged(logger, "giving up"), "giving up should be logged");
    }
}

void testTimedOutCallIsNotRetried() {
    ScriptedLuna scripted("hang");
    EventLoop loop(4, 16, 32);
    ChildProcessRunner children(fastStarts());
    QuietLogger quiet("debug");
    Logger &logger = quiet.logger;
    LunaClientOptions options;
    options.executable = scripted.executable;
    options.environment = scripted.environment();
    options.timeout = std::chrono::milliseconds(300);
    LunaClient luna(loop, children, &logger, options);
    const std::string uri = "com.webos.service.tvpower/power/getPowerState";
    const LunaReply reply = callAndWait(luna, loop, uri);
    check(reply.timedOut, "the call should report that it timed out");
    check(scripted.runs() == 1, "a call stopped by the timeout should not be retried");
    check(!logged(logger, "died"), "a timeout is not a death to log");
    // At the debug level each call logs its start and end, and a timeout says so.
    check(logged(logger, "luna: call " + uri), "the start of a call should be logged");
    check(logged(logger, "luna: done " + uri) && logged(logger, "(timed out)"),
          "the end of a timed out call should be logged as such");
}

void testDebugLoggingOnlyAtDebugLevel() {
    const std::string uri = "com.webos.service.tvpower/power/getPowerState";
    for (const char *level : {"info", "debug"}) {
        ScriptedLuna scripted("answer");
        EventLoop loop(4, 16, 32);
        ChildProcessRunner children(fastStarts());
        QuietLogger quiet(level);
        Logger &logger = quiet.logger;
        LunaClientOptions options;
        options.executable = scripted.executable;
        options.environment = scripted.environment();
        LunaClient luna(loop, children, &logger, options);
        const LunaReply reply = callAndWait(luna, loop, uri);
        check(reply.error.empty(), "the scripted call should be answered");
        const bool debug = std::string(level) == "debug";
        check(logged(logger, "luna: call " + uri) == debug &&
                  logged(logger, "luna: done " + uri) == debug,
              std::string("calls are logged only at the debug level (") + level + ")");
    }
}

// Urgent calls go ahead of the queue with one extra slot, one at a time.
void testUrgentSlot() {
    EventLoop loop(4, 16, 32);
    ChildProcessRunner children(fastStarts());
    LunaClientOptions options;
    options.executable = NATIVETV_FAKE_LUNA_SEND;
    options.environment.push_back("NATIVETV_FAKE_LUNA_MODE=slow");
    LunaClient luna(loop, children, nullptr, options);

    // Index 0-3 are normal, 10-12 urgent; each fake call takes about 160 ms.
    const int order[] = {0, 1, 2, 3, 10, 11, 12};
    std::mutex mutex;
    std::condition_variable condition;
    std::vector<std::pair<int, LunaReply>> replies;
    for (int index : order) {
        Json payload = Json::object();
        payload.set("index", Json::numeric(index));
        check(luna.call("com.webos.audio/getVolume", payload,
                        [&, index](const LunaReply &reply) {
                            {
                                std::lock_guard<std::mutex> lock(mutex);
                                replies.push_back({index, reply});
                            }
                            condition.notify_one();
                        },
                        "", index >= 10),
              "urgent slot calls should be accepted");
    }
    std::thread loopThread([&]() { loop.run(); });
    {
        std::unique_lock<std::mutex> lock(mutex);
        const bool finished = condition.wait_for(lock, std::chrono::seconds(8),
                                                 [&]() { return replies.size() == 7; });
        loop.stop();
        lock.unlock();
        loopThread.join();
        check(finished, "queued normal and urgent calls did not all complete");
    }

    const auto started = [&](int index) {
        for (const auto &entry : replies)
            if (entry.first == index) return entry.second.startedAt;
        throw std::runtime_error("no reply for call " + std::to_string(index));
    };
    const auto ended = [&](int index) {
        for (const auto &entry : replies)
            if (entry.first == index) return entry.second.endedAt;
        throw std::runtime_error("no reply for call " + std::to_string(index));
    };
    std::vector<std::pair<std::chrono::steady_clock::time_point, int>> events;
    for (const auto &entry : replies) {
        check(entry.second.error.empty(), "fake Luna call should succeed");
        events.push_back({entry.second.startedAt, 1});
        events.push_back({entry.second.endedAt, -1});
    }
    std::sort(events.begin(), events.end(), [](const auto &a, const auto &b) {
        if (a.first != b.first)
            return a.first < b.first;
        return a.second < b.second; // an end at a timestamp precedes a start
    });
    int active = 0, maximum = 0;
    for (const auto &event : events) {
        active += event.second;
        maximum = std::max(maximum, active);
    }
    check(maximum == 3, "an urgent call should take one slot beyond the two, and no more");
    // The key presses run one at a time, in the order made.
    check(started(11) >= ended(10) && started(12) >= ended(11),
          "urgent calls should run one at a time");
    // And ahead of the normal calls that were waiting: the first key press starts at
    // once in the extra slot, while normal calls 2 and 3 wait for a slot of their own.
    // (Which later call starts first depends on which of equal 160 ms calls ends first.)
    check(started(10) <= started(2) && started(10) <= started(3),
          "an urgent call should not wait behind the normal calls waiting");
}

} // namespace

int main() {
    try {
        testLegacyServiceFallback();
        testTwoCallLimit();
        testSubscriptionFramesAndStop();
        testSubscriptionMultilineFragments();
        testSubscriptionBackoff();
        testSubscriptionServiceFallback();
        testUrgentSlot();
        testAbortedCallRunsOnce();
        testTimedOutCallIsNotRetried();
        testDebugLoggingOnlyAtDebugLevel();
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "Luna test failed: %s\n", error.what());
        return 1;
    }
}
