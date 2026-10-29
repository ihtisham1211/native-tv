#include "daemon/child_process.hpp"
#include "daemon/event_loop.hpp"
#include "daemon/luna.hpp"
#include "daemon/state_subscriptions.hpp"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
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

std::string temporaryPath() {
    char pattern[] = "/tmp/nativetv-state-XXXXXX";
    const int descriptor = mkstemp(pattern);
    check(descriptor >= 0, "could not create fake Luna log");
    close(descriptor);
    return pattern;
}

std::string temporaryDirectory() {
    char pattern[] = "/tmp/nativetv-media-seen-XXXXXX";
    char *created = mkdtemp(pattern);
    check(created != nullptr, "could not create media marker test directory");
    return created;
}

class ScopedEnvironment {
  public:
    ScopedEnvironment(const std::string &name, const std::string &value)
        : name_(name), hadPrevious_(std::getenv(name.c_str()) != nullptr),
          previous_(hadPrevious_ ? std::getenv(name.c_str()) : "") {
        check(setenv(name_.c_str(), value.c_str(), 1) == 0,
              "could not set test environment variable " + name_);
    }

    ~ScopedEnvironment() {
        if (hadPrevious_)
            setenv(name_.c_str(), previous_.c_str(), 1);
        else
            unsetenv(name_.c_str());
    }

  private:
    std::string name_;
    bool hadPrevious_;
    std::string previous_;
};

std::string readText(const std::string &path) {
    std::ifstream input(path.c_str(), std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
}

std::vector<std::string> readLines(const std::string &path) {
    std::ifstream input(path.c_str());
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(input, line))
        lines.push_back(line);
    return lines;
}

size_t countUri(const std::vector<std::string> &lines, const std::string &uri) {
    size_t count = 0;
    for (const std::string &line : lines)
        if (line.find(uri) != std::string::npos)
            ++count;
    return count;
}

void finishLoop(EventLoop &loop, StateSubscriptions &state,
                std::thread &thread) {
    const bool posted = loop.post([&loop, &state]() {
        state.stop();
        loop.scheduleAfter(std::chrono::milliseconds(150), [&loop]() { loop.stop(); });
    });
    if (!posted)
        loop.stop();
    thread.join();
}

void testGroupsAndReferenceCounts() {
    const std::string logPath = temporaryPath();
    EventLoop loop(8, 32, 128);
    ChildProcessRunner children(fastStarts());
    LunaClientOptions lunaOptions;
    lunaOptions.executable = NATIVETV_FAKE_LUNA_SEND;
    lunaOptions.environment.push_back("NATIVETV_FAKE_LUNA_MODE=state-groups");
    lunaOptions.environment.push_back("NATIVETV_FAKE_LUNA_LOG=" + logPath);
    LunaClient luna(loop, children, nullptr, lunaOptions);

    std::mutex mutex;
    std::condition_variable condition;
    std::vector<state::Event> events;
    std::vector<std::string> invalidations;
    std::vector<std::string> statusChanges;
    StateSubscriptions live(loop, luna, state::MappingOptions(), nullptr,
                            [&invalidations](const std::string &group) {
                                invalidations.push_back(group);
                            },
                            [&statusChanges, &condition](const std::string &group, bool isLive) {
                                statusChanges.push_back(group + (isLive ? ":live" : ":stopped"));
                                condition.notify_all();
                            });
    live.cache().onChange([&](const state::Event &event) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            events.push_back(event);
        }
        condition.notify_all();
    });
    check(!live.startGroup("application"),
          "lifecycle controls should only run on the event-loop thread");

    std::mutex setupMutex;
    bool setupOk = false;
    check(loop.post([&]() {
              const bool appOwner = live.startGroup("application");
              const bool defaults = live.start();
              const bool unknown = live.startGroup("missing-group");
              {
                  std::lock_guard<std::mutex> lock(setupMutex);
                  setupOk = appOwner && defaults && !unknown;
              }
              condition.notify_all();
          }),
          "could not queue state subscriptions startup");
    std::thread loopThread([&]() { loop.run(); });

    bool gotDefaultState = false;
    {
        std::unique_lock<std::mutex> lock(mutex);
        gotDefaultState = condition.wait_for(lock, std::chrono::seconds(4), [&]() {
            return events.size() >= 11 && live.isLive("media");
        });
    }
    if (!gotDefaultState) {
        finishLoop(loop, live, loopThread);
        std::remove(logPath.c_str());
        throw std::runtime_error("default state groups did not publish their initial values");
    }

    check(!live.isLive("surface") && !live.isLive("focus"),
          "surface and focus should remain lazy until requested");
    check(live.isLive("application") && live.isLive("power") && live.isLive("audio") &&
              live.isLive("media"),
          "a successful first reply should make active state groups live");
    check(loop.post([&]() {
              const bool surface = live.startGroup("surface");
              const bool focus = live.startGroup("focus");
              if (!surface || !focus)
                  loop.stop();
          }),
          "could not queue surface and focus state groups");

    bool gotWindowState = false;
    {
        std::unique_lock<std::mutex> lock(mutex);
        gotWindowState = condition.wait_for(lock, std::chrono::seconds(4), [&]() {
            return events.size() >= 13;
        });
    }
    if (!gotWindowState) {
        finishLoop(loop, live, loopThread);
        std::remove(logPath.c_str());
        throw std::runtime_error("requested surface and focus groups did not publish");
    }

    Json snapshot = live.snapshot();
    check(snapshot["application"]["app_id"].string() == "com.webos.app.netflix" &&
              snapshot["surface"]["control_app_id"].string() == "com.webos.app.netflix" &&
              snapshot["focus"]["control_app_id"].string() == "com.webos.app.netflix",
          "native group reducers should feed the shared snapshot");
    {
        std::lock_guard<std::mutex> lock(mutex);
        check(!events.empty(), "state change listeners should receive native updates");
        for (const state::Event &event : events)
            check(event.snapshot && event.tvReported,
                  "first subscription replies should carry snapshot metadata");
    }
    check(setupOk, "default and group subscription lifecycle calls should be accepted");

    bool applicationStayedLive = false;
    bool applicationStoppedAtLastOwner = false;
    check(loop.post([&]() {
              live.stop();
              applicationStayedLive = live.isLive("application");
              live.stopGroup("application");
              applicationStoppedAtLastOwner = !live.isLive("application");
          }),
          "could not queue reference-count release");

    finishLoop(loop, live, loopThread);
    const std::vector<std::string> calls = readLines(logPath);
    std::remove(logPath.c_str());
    check(countUri(calls, "com.webos.applicationManager/getForegroundAppInfo") == 1,
          "a default owner and a group owner should share one application subscription");
    check(countUri(calls, "com.webos.service.settings/getSystemSettings") == 1 &&
              countUri(calls, "com.webos.service.tvpower/power/getPowerState") == 1 &&
              countUri(calls, "com.webos.service.audio/master/getVolume") == 1 &&
              countUri(calls, "com.webos.service.acb/getForegroundAppInfo") == 1,
          "default state groups should each start one Luna subscription");
    check(countUri(calls, "com.webos.surfacemanager/getForegroundAppInfo") == 1 &&
              countUri(calls, "com.webos.surfacemanager/getFocusedWindowInfo") == 1,
          "surface and focus should each start only when requested");
    check(applicationStayedLive && applicationStoppedAtLastOwner,
          "a shared group should stop after its last owner releases it");
    check(invalidations.size() >= 4,
          "successful picture, power, application and audio replies should invalidate reads");
    check(std::find(statusChanges.begin(), statusChanges.end(), "application:live") !=
                  statusChanges.end() &&
              std::find(statusChanges.begin(), statusChanges.end(), "application:stopped") !=
                  statusChanges.end(),
          "native subscription liveness changes should be reported to the state bridge");
}

void testLegacyMediaFallback() {
    const std::string logPath = temporaryPath();
    EventLoop loop(4, 16, 64);
    ChildProcessRunner children(fastStarts());
    LunaClientOptions lunaOptions;
    lunaOptions.executable = NATIVETV_FAKE_LUNA_SEND;
    lunaOptions.environment.push_back("NATIVETV_FAKE_LUNA_MODE=media-acb-absent");
    lunaOptions.environment.push_back("NATIVETV_FAKE_LUNA_LOG=" + logPath);
    LunaClient luna(loop, children, nullptr, lunaOptions);
    StateSubscriptions live(loop, luna);

    check(loop.post([&]() { live.startGroup("media"); }),
          "could not queue media subscription startup");
    std::thread loopThread([&]() { loop.run(); });
    bool receivedLegacy = false;
    for (int attempt = 0; attempt < 60; ++attempt) {
        const Json snapshot = live.snapshot();
        const Json media = Json::parse(snapshot["media"]["snapshot"].string());
        if (media["state"].string() == "paused" && live.isLive("media")) {
            receivedLegacy = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    finishLoop(loop, live, loopThread);
    const std::vector<std::string> calls = readLines(logPath);
    std::remove(logPath.c_str());
    check(receivedLegacy, "the older media service should populate native play state");
    check(countUri(calls, "com.webos.service.acb/getForegroundAppInfo") == 1 &&
              countUri(calls, "com.webos.media/getForegroundAppInfo") == 1,
          "missing ACB service should switch to the legacy media service once");
}

void runMediaPersistenceFixture(const std::string &mode,
                                const std::string &expectedState) {
    const std::string logPath = temporaryPath();
    EventLoop loop(4, 16, 64);
    ChildProcessRunner children(fastStarts());
    LunaClientOptions lunaOptions;
    lunaOptions.executable = NATIVETV_FAKE_LUNA_SEND;
    lunaOptions.environment.push_back("NATIVETV_FAKE_LUNA_MODE=" + mode);
    lunaOptions.environment.push_back("NATIVETV_FAKE_LUNA_LOG=" + logPath);
    LunaClient luna(loop, children, nullptr, lunaOptions);
    StateSubscriptions live(loop, luna);
    check(loop.post([&]() { live.startGroup("media"); }),
          "could not queue media persistence fixture");
    std::thread loopThread([&]() { loop.run(); });
    bool received = false;
    for (int attempt = 0; attempt < 60; ++attempt) {
        const Json snapshot = live.snapshot();
        const Json encoded = snapshot["media"]["snapshot"];
        if (live.isLive("media") && encoded.isString()) {
            const Json media = Json::parse(encoded.string());
            if (media["state"].string() == expectedState) {
                received = true;
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    finishLoop(loop, live, loopThread);
    std::remove(logPath.c_str());
    check(received, "media persistence fixture did not receive " + expectedState);
}

// test-state-push.js: a TV without SurfaceManager's window reports stops asking;
// a busy service is only a failed read, and asked again.
void testRefusedWindowSubscriptions() {
    const std::string logPath = temporaryPath();
    EventLoop loop(4, 16, 64);
    ChildProcessRunner children(fastStarts());
    LunaClientOptions lunaOptions;
    lunaOptions.executable = NATIVETV_FAKE_LUNA_SEND;
    lunaOptions.environment.push_back("NATIVETV_FAKE_LUNA_MODE=window-refused");
    lunaOptions.environment.push_back("NATIVETV_FAKE_LUNA_LOG=" + logPath);
    LunaClient luna(loop, children, nullptr, lunaOptions);
    StateSubscriptions live(loop, luna);
    check(loop.post([&]() {
              live.startGroup("surface");
              live.startGroup("focus");
          }),
          "could not queue the window subscriptions");
    std::thread loopThread([&]() { loop.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(2300));
    finishLoop(loop, live, loopThread);
    const std::vector<std::string> calls = readLines(logPath);
    std::remove(logPath.c_str());
    check(countUri(calls, "com.webos.surfacemanager/getForegroundAppInfo") >= 2,
          "a busy service must be asked again");
    check(countUri(calls, "com.webos.surfacemanager/getFocusedWindowInfo") == 1,
          "a method the TV does not have must not be asked again");
    check(!live.isLive("focus") && !live.isLive("surface"),
          "a refused window group is not live");
}

void testMediaSeenPersistence() {
    const std::string root = temporaryDirectory();
    const std::string markerPath = root + "/media_seen";
    {
        ScopedEnvironment dataDirectory("TVWEB_DATA_DIR", root);
        runMediaPersistenceFixture("media-idle", "idle");
        check(access(markerPath.c_str(), F_OK) != 0,
              "an idle media snapshot must not create media_seen");

        runMediaPersistenceFixture("state-groups", "playing");
        check(readText(markerPath) == "1\n",
              "native playback must persist the legacy plain-text media_seen marker");
        struct stat info;
        check(stat(markerPath.c_str(), &info) == 0 && (info.st_mode & 0777) == 0600,
              "native media_seen marker must be private");

        runMediaPersistenceFixture("media-idle", "idle");
        check(readText(markerPath) == "1\n",
              "an idle snapshot must preserve the previously seen marker");
    }
    std::remove(markerPath.c_str());
    rmdir(root.c_str());
}

void testLegacyAudioFallback() {
    const std::string logPath = temporaryPath();
    EventLoop loop(4, 16, 64);
    ChildProcessRunner children(fastStarts());
    LunaClientOptions lunaOptions;
    lunaOptions.executable = NATIVETV_FAKE_LUNA_SEND;
    lunaOptions.environment.push_back("NATIVETV_FAKE_LUNA_MODE=audio-unknown-method");
    lunaOptions.environment.push_back("NATIVETV_FAKE_LUNA_LOG=" + logPath);
    LunaClient luna(loop, children, nullptr, lunaOptions);
    StateSubscriptions live(loop, luna);

    check(loop.post([&]() {
              if (!live.startGroup("audio"))
                  loop.stop();
          }),
          "could not queue audio subscription startup");
    std::thread loopThread([&]() { loop.run(); });
    bool receivedLegacy = false;
    for (int attempt = 0; attempt < 40; ++attempt) {
        const Json snapshot = live.snapshot();
        if (snapshot["audio"]["volume"].number() == 17 &&
            snapshot["audio"]["muted"].boolean() &&
            snapshot["audio"]["output"].string() == "TV Speaker") {
            receivedLegacy = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    finishLoop(loop, live, loopThread);
    const std::vector<std::string> calls = readLines(logPath);
    std::remove(logPath.c_str());
    check(receivedLegacy, "older flat volume response should populate native audio state");
    check(countUri(calls, "com.webos.service.audio/master/getVolume") == 1 &&
              countUri(calls, "com.webos.audio/getVolume") == 1,
          "unknown newer audio method should switch once to the legacy service");
}

void testReconnectStartsANewSnapshot() {
    const std::string logPath = temporaryPath();
    EventLoop loop(4, 16, 64);
    ChildProcessRunner children(fastStarts());
    LunaClientOptions lunaOptions;
    lunaOptions.executable = NATIVETV_FAKE_LUNA_SEND;
    lunaOptions.environment.push_back("NATIVETV_FAKE_LUNA_MODE=state-reconnect");
    lunaOptions.environment.push_back("NATIVETV_FAKE_LUNA_LOG=" + logPath);
    LunaClient luna(loop, children, nullptr, lunaOptions);
    StateSubscriptions live(loop, luna);
    std::mutex mutex;
    std::condition_variable condition;
    std::vector<state::Event> events;
    live.cache().onChange([&](const state::Event &event) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            events.push_back(event);
        }
        condition.notify_all();
    });

    check(loop.post([&]() { live.startGroup("application"); }),
          "could not queue reconnect state group");
    std::thread loopThread([&]() { loop.run(); });
    bool reconnected = false;
    {
        std::unique_lock<std::mutex> lock(mutex);
        reconnected = condition.wait_for(lock, std::chrono::seconds(5), [&]() {
            return events.size() >= 6;
        });
    }
    if (!reconnected) {
        finishLoop(loop, live, loopThread);
        std::remove(logPath.c_str());
        throw std::runtime_error("state group did not reconnect and deliver a new snapshot");
    }

    std::vector<state::Event> received;
    {
        std::lock_guard<std::mutex> lock(mutex);
        received = events;
    }
    finishLoop(loop, live, loopThread);
    const std::vector<std::string> calls = readLines(logPath);
    std::remove(logPath.c_str());
    check(countUri(calls, "com.webos.applicationManager/getForegroundAppInfo") >= 2,
          "closed state subscriptions should reconnect");
    check(received.size() >= 6, "both app snapshots should emit their changed fields");
    for (size_t index = 0; index < 6; ++index)
        check(received[index].snapshot,
              "the first reply after a reconnect must be marked as a snapshot");
    check(received[0].value.string() == "com.webos.app.netflix" &&
              received[3].value.string() == "com.webos.app.youtube",
          "the reconnected group should replace its prior app snapshot");
}
} // namespace

int main() {
    const std::string dataRoot = temporaryDirectory();
    int result = EXIT_SUCCESS;
    try {
        {
            ScopedEnvironment dataDirectory("TVWEB_DATA_DIR", dataRoot);
            testGroupsAndReferenceCounts();
            testLegacyAudioFallback();
            testLegacyMediaFallback();
            testRefusedWindowSubscriptions();
            testMediaSeenPersistence();
            testReconnectStartsANewSnapshot();
        }
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        result = EXIT_FAILURE;
    }
    std::remove((dataRoot + "/media_seen").c_str());
    rmdir(dataRoot.c_str());
    if (result != EXIT_SUCCESS)
        return result;
    std::cout << "native state subscriptions tests passed\n";
    return EXIT_SUCCESS;
}
