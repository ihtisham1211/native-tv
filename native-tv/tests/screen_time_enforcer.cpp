#include "daemon/child_process.hpp"
#include "daemon/event_loop.hpp"
#include "daemon/luna.hpp"
#include "daemon/screen_time_enforcer.hpp"
#include "service/history_recorder.hpp"
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

#ifndef NATIVETV_FAKE_LUNA_SEND
#error NATIVETV_FAKE_LUNA_SEND must name the native fake luna-send fixture
#endif

using namespace nativetv;
using namespace nativetv::daemon;
using namespace nativetv::service;

namespace {
void check(bool value, const std::string &message) {
    if (!value)
        throw std::runtime_error(message);
}

std::string readFile(const std::string &path) {
    std::ifstream input(path.c_str());
    std::ostringstream contents;
    contents << input.rdbuf();
    return contents.str();
}

std::vector<unsigned char> readBytes(const std::string &path) {
    std::ifstream input(path.c_str(), std::ios::binary);
    return std::vector<unsigned char>(std::istreambuf_iterator<char>(input),
                                      std::istreambuf_iterator<char>());
}

bool inputEventIs(const std::vector<unsigned char> &bytes, size_t offset,
                  uint16_t expectedType, uint16_t expectedCode, int32_t expectedValue) {
    if (bytes.size() < offset + 16)
        return false;
    uint16_t type = 0;
    uint16_t code = 0;
    int32_t value = 0;
    std::memcpy(&type, bytes.data() + offset + 8, sizeof(type));
    std::memcpy(&code, bytes.data() + offset + 10, sizeof(code));
    std::memcpy(&value, bytes.data() + offset + 12, sizeof(value));
    return type == expectedType && code == expectedCode && value == expectedValue;
}

bool waitFor(const std::string &path, const std::string &needle,
             std::chrono::milliseconds timeout) {
    const auto end = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < end) {
        if (readFile(path).find(needle) != std::string::npos)
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return readFile(path).find(needle) != std::string::npos;
}

unsigned occurrences(const std::string &text, const std::string &needle) {
    unsigned count = 0;
    size_t offset = 0;
    while ((offset = text.find(needle, offset)) != std::string::npos) {
        ++count;
        offset += needle.size();
    }
    return count;
}

bool waitForOccurrences(const std::string &path, const std::string &needle,
                       unsigned expected, std::chrono::milliseconds timeout) {
    const auto end = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < end) {
        if (occurrences(readFile(path), needle) >= expected)
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return occurrences(readFile(path), needle) >= expected;
}

void cleanup(const std::string &root) {
    unlink((root + "/screentime.json").c_str());
    unlink((root + "/screentime.json.last-good").c_str());
    unlink((root + "/luna-details.jsonl").c_str());
    rmdir((root + "/history").c_str());
    unlink((root + "/dev/input/event3").c_str());
    unlink((root + "/dev/input/event1").c_str());
    unlink((root + "/proc/bus/input/devices").c_str());
    rmdir((root + "/dev/input").c_str());
    rmdir((root + "/dev").c_str());
    rmdir((root + "/proc/bus/input").c_str());
    rmdir((root + "/proc/bus").c_str());
    rmdir((root + "/proc").c_str());
    rmdir((root + "/usr/palm/applications/com.webos.app.home").c_str());
    rmdir((root + "/usr/palm/applications").c_str());
    rmdir((root + "/usr/palm").c_str());
    rmdir((root + "/usr").c_str());
    rmdir(root.c_str());
}

bool runAppAndScreenSaverCase() {
    char temporary[] = "/tmp/nativetv-screen-time-actions-XXXXXX";
    const char *created = mkdtemp(temporary);
    if (!created)
        return false;
    const std::string root(created);
    const std::string details = root + "/luna-details.jsonl";
    for (const std::string &directory : {
             root + "/usr", root + "/usr/palm", root + "/usr/palm/applications",
             root + "/usr/palm/applications/com.webos.app.home", root + "/proc",
             root + "/proc/bus", root + "/proc/bus/input", root + "/dev",
             root + "/dev/input"}) {
        if (mkdir(directory.c_str(), 0700) != 0) {
            cleanup(root);
            return false;
        }
    }
    {
        std::ofstream devices((root + "/proc/bus/input/devices").c_str());
        devices << "N: Name=\"LGE RCU\"\nH: Handlers=kbd event3\n";
        std::ofstream event((root + "/dev/input/event3").c_str(),
                            std::ios::binary | std::ios::trunc);
    }
    setenv("NATIVETV_FAKE_LUNA_MODE", "screen-time", 1);
    setenv("NATIVETV_FAKE_LUNA_DETAILS", details.c_str(), 1);

    int64_t wall = 1793016000000LL;
    double monotonic = 0;
    EventLoop loop(2, 32, 64);
    ChildProcessRunner children;
    LunaClientOptions lunaOptions;
    lunaOptions.executable = NATIVETV_FAKE_LUNA_SEND;
    LunaClient luna(loop, children, nullptr, lunaOptions);
    CommandPolicyExecutor commandPolicy(luna);
    state::Cache state;
    Json power = Json::object();
    power.set("state", Json::text("Active"));
    power.set("systemOn", Json::flag(true));
    power.set("screenOn", Json::flag(true));
    Json application = Json::object();
    application.set("app", Json::text("netflix"));
    application.set("app_id", Json::text("com.webos.app.netflix"));
    Json snapshot = Json::object();
    snapshot.set("power", power);
    snapshot.set("application", application);
    for (const char *key : {"state", "systemOn", "screenOn"})
        state.update(state::Event{"power", key, power[key], wall, true, true});
    for (const char *key : {"app", "app_id"})
        state.update(state::Event{"application", key, application[key], wall, true, true});

    HistoryRecorderOptions historyOptions;
    historyOptions.directory = root + "/history";
    historyOptions.enabled = true;
    historyOptions.wallNowMs = [&wall]() { return wall; };
    historyOptions.monotonicNowMs = [&monotonic]() { return monotonic; };
    HistoryRecorder history(historyOptions);
    history.start(snapshot);
    state.onChange([&state, &history](const state::Event &event) {
        history.observe(event, state.snapshot());
    });
    wall += 61000;
    monotonic += 61000;
    const Json watched = history.totals(wall - 86400000, wall);

    ScreenTimeTimings timings;
    timings.check = std::chrono::milliseconds(1000);
    timings.grace = std::chrono::milliseconds(30);
    timings.blank = std::chrono::milliseconds(10);
    timings.again = std::chrono::milliseconds(500);
    timings.settle = std::chrono::milliseconds(5);
    timings.screenSaver = std::chrono::milliseconds(10);
    timings.verify = std::chrono::milliseconds(2000);
    timings.keyPress = std::chrono::milliseconds(5);
    timings.powerSettle = std::chrono::milliseconds(20);
    timings.wallNowMs = [&wall]() { return wall; };
    ScreenTimeTimings verificationTimings = timings;
    verificationTimings.verify = std::chrono::milliseconds(100);

    const Json appSettings = Json::parse(
        "{\"enabled\":true,\"dailyMinutes\":0,\"bedtimeStart\":\"\","
        "\"bedtimeEnd\":\"\",\"action\":\"off\",\"appAction\":\"home\","
        "\"apps\":{\"netflix\":1}}");
    const Json screenSettings = Json::parse(
        "{\"enabled\":true,\"dailyMinutes\":1,\"bedtimeStart\":\"\","
        "\"bedtimeEnd\":\"\",\"action\":\"screenOff\",\"appAction\":\"home\","
        "\"apps\":{}}");
    std::unique_ptr<ScreenTimeEnforcer> enforcer;
    bool ok = false;
    std::thread loopThread;
    try {
        check(watched["netflix"].number() >= 60,
              "the action fixture should contain more than one minute of Netflix watch time");
        check(loop.post([&]() {
                  enforcer.reset(new ScreenTimeEnforcer(loop, state, history, luna, root,
                                                        root + "/proc", root, appSettings,
                                                        true, true, nullptr, timings));
                  (void)enforcer->start();
              }), "per-app limit policy should start on the event loop");
        loopThread = std::thread([&]() { loop.run(); });
        check(waitFor(details, "applicationManager", std::chrono::seconds(3)),
              "an exceeded per-app limit should launch Home after the grace period");
        check(occurrences(readFile(details), "applicationManager") == 1,
              "a single unchanged app limit should launch Home once during the grace action");

        const Json appOffSettings = Json::parse(
            "{\"enabled\":true,\"dailyMinutes\":0,\"bedtimeStart\":\"\","
            "\"bedtimeEnd\":\"\",\"action\":\"off\",\"appAction\":\"off\","
            "\"apps\":{\"netflix\":1}}");
        const std::string powerOffUri = "powerOff";
        const unsigned powerOffBeforeAppAction =
            occurrences(readFile(details), powerOffUri);
        check(loop.post([&]() {
                  enforcer->stop();
                  enforcer.reset(new ScreenTimeEnforcer(loop, state, history, luna, root,
                                                        root + "/proc", root,
                                                        appOffSettings, true, true,
                                                        nullptr, timings,
                                                        &commandPolicy));
                  (void)enforcer->start();
              }), "per-app power-off policy should restart on the event loop");
        check(waitForOccurrences(details, powerOffUri, powerOffBeforeAppAction + 1,
                                 std::chrono::seconds(3)),
              "an app limit configured to power off should issue the power-off command");
        check(loop.post([&]() {
                  enforcer->stop();
              }), "per-app power-off policy should stop after its action is observed");

        const Json appScreenOffSettings = Json::parse(
            "{\"enabled\":true,\"dailyMinutes\":0,\"bedtimeStart\":\"\","
            "\"bedtimeEnd\":\"\",\"action\":\"off\",\"appAction\":\"screenOff\","
            "\"apps\":{\"netflix\":1}}");
        const unsigned screenOffBeforeAppAction =
            occurrences(readFile(details), "turnOffScreen");
        check(loop.post([&]() {
                  enforcer.reset(new ScreenTimeEnforcer(loop, state, history, luna, root,
                                                        root + "/proc", root,
                                                        appScreenOffSettings, true, true,
                                                        nullptr, verificationTimings,
                                                        &commandPolicy));
                  (void)enforcer->start();
              }), "per-app screen-off policy should restart on the event loop");
        check(waitForOccurrences(details, "turnOffScreen", screenOffBeforeAppAction + 1,
                                 std::chrono::seconds(3)),
              "an app limit configured to blank the screen should issue screen-off");
        check(loop.post([&]() {
                  state::Event screenOff{"power", "screenOn", Json::flag(false), wall,
                                         false, true};
                  state.update(screenOff);
                  enforcer->stop();
              }), "per-app screen-off policy should observe the panel-off state");

        const unsigned screenOffCallsBeforeSaver =
            occurrences(readFile(details), "turnOffScreen");
        check(loop.post([&]() {
                  enforcer->stop();
                  enforcer.reset();
                  state::Event screenOn{"power", "screenOn", Json::flag(true), wall,
                                        false, true};
                  state.update(screenOn);
                  Json saverPower = state.snapshot()["power"];
                  saverPower.set("state", Json::text("Screen Saver"));
                  state::Event saver{"power", "state", saverPower["state"], wall, false, true};
                  state.update(saver);
                  enforcer.reset(new ScreenTimeEnforcer(loop, state, history, luna, root,
                                                        root + "/proc", root, screenSettings,
                                                        true, true, nullptr, verificationTimings));
                  (void)enforcer->start();
              }), "screen-saver policy should restart on the event loop");
        check(waitForOccurrences(details, "turnOffScreen", screenOffCallsBeforeSaver + 1,
                                 std::chrono::seconds(3)),
              "the screen saver should be dismissed before the screen is turned off");
        const std::vector<unsigned char> keyEvents = readBytes(root + "/dev/input/event3");
        check(keyEvents.size() == 64 &&
                  inputEventIs(keyEvents, 0, 1, 158, 1) &&
                  inputEventIs(keyEvents, 16, 0, 0, 0) &&
                  inputEventIs(keyEvents, 32, 1, 158, 0) &&
                  inputEventIs(keyEvents, 48, 0, 0, 0),
              "RCU Back injection should write down, sync, up and sync input events");
        check(waitForOccurrences(details, "turnOffScreen", screenOffCallsBeforeSaver + 2,
                                 std::chrono::seconds(3)),
              "screen-off verification should retry when LG leaves the panel on");
        check(loop.post([&]() {
                  state::Event screenOff{"power", "screenOn", Json::flag(false), wall,
                                         false, true};
                  state.update(screenOff);
              }), "screen-off verification should receive the panel state");
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        check(occurrences(readFile(details), "turnOffScreen") ==
                  screenOffCallsBeforeSaver + 2,
              "screen-off verification should stop retrying after the panel stays off");

        const unsigned screenOffCallsBeforeFallback =
            occurrences(readFile(details), "turnOffScreen");
        check(loop.post([&]() {
                  enforcer->stop();
                  enforcer.reset();
                  state::Event screenOn{"power", "screenOn", Json::flag(true), wall,
                                        false, true};
                  state.update(screenOn);
                  {
                      std::ofstream devices((root + "/proc/bus/input/devices").c_str(),
                                            std::ios::trunc);
                      devices << "N: Name=\"LGE RCU\"\nH: Handlers=kbd event4\n";
                      std::ofstream fallback((root + "/dev/input/event1").c_str(),
                                             std::ios::binary | std::ios::trunc);
                  }
                  enforcer.reset(new ScreenTimeEnforcer(loop, state, history, luna, root,
                                                        root + "/proc", root, screenSettings,
                                                        true, true, nullptr, timings));
                  (void)enforcer->start();
              }), "screen-saver fallback policy should restart on the event loop");
        check(waitForOccurrences(details, "turnOffScreen", screenOffCallsBeforeFallback + 1,
                                 std::chrono::seconds(3)),
              "a stale discovered RCU node should fall back to event1 and still turn off the screen");
        const std::vector<unsigned char> fallbackEvents = readBytes(root + "/dev/input/event1");
        check(fallbackEvents.size() == 64 &&
                  inputEventIs(fallbackEvents, 0, 1, 158, 1) &&
                  inputEventIs(fallbackEvents, 32, 1, 158, 0),
              "event1 fallback should receive the complete RCU Back key press");

        check(loop.post([&]() {
                  enforcer->stop();
                  enforcer.reset();
                  (void)rmdir((root + "/usr/palm/applications/com.webos.app.home").c_str());
                  state::Event active{"power", "state", Json::text("Active"), wall,
                                      false, true};
                  state.update(active);
                  enforcer.reset(new ScreenTimeEnforcer(loop, state, history, luna, root,
                                                        root + "/proc", root, appSettings,
                                                        true, true, nullptr, timings));
                  (void)enforcer->start();
              }), "webOS without the Home app should apply its power fallback");
        const unsigned powerOffBeforeNoHome =
            occurrences(readFile(details), "powerOff");
        check(waitForOccurrences(details, "powerOff", powerOffBeforeNoHome + 1,
                                 std::chrono::seconds(3)),
              "an app limit set to Home should switch off older firmware without the Home app");
        check(occurrences(readFile(details), "applicationManager") == 1,
              "the no-Home firmware fallback should not try to launch the missing Home app");

        const unsigned screenOffCallsBeforeNoPowerFallback =
            occurrences(readFile(details), "turnOffScreen");
        const unsigned powerOffCallsBeforeNoPowerFallback =
            occurrences(readFile(details), "powerOff");
        check(loop.post([&]() {
                  enforcer->stop();
                  enforcer.reset();
                  enforcer.reset(new ScreenTimeEnforcer(loop, state, history, luna, root,
                                                        root + "/proc", root, appSettings,
                                                        true, false, nullptr, timings));
                  (void)enforcer->start();
              }), "webOS without power permission should apply its screen-off fallback");
        check(waitForOccurrences(details, "turnOffScreen",
                                 screenOffCallsBeforeNoPowerFallback + 1,
                                 std::chrono::seconds(3)),
              "without Home or power permission, an app limit should turn off only the screen");
        check(occurrences(readFile(details), "powerOff") ==
                  powerOffCallsBeforeNoPowerFallback,
              "the no-power firmware fallback must not issue another power-off command");

        const Json appWarnSettings = Json::parse(
            "{\"enabled\":true,\"dailyMinutes\":0,\"bedtimeStart\":\"\","
            "\"bedtimeEnd\":\"\",\"action\":\"off\",\"appAction\":\"warn\","
            "\"apps\":{\"netflix\":1}}");
        const unsigned notificationsBeforeAppWarn =
            occurrences(readFile(details), "com.webos.notification");
        const unsigned powerOffCallsBeforeAppWarn =
            occurrences(readFile(details), "powerOff");
        const unsigned screenOffCallsBeforeAppWarn =
            occurrences(readFile(details), "turnOffScreen");
        check(loop.post([&]() {
                  enforcer->stop();
                  enforcer.reset();
                  enforcer.reset(new ScreenTimeEnforcer(loop, state, history, luna, root,
                                                        root + "/proc", root, appWarnSettings,
                                                        true, true, nullptr, timings));
                  (void)enforcer->start();
              }), "per-app warn-only policy should start on the event loop");
        check(waitForOccurrences(details, "com.webos.notification",
                                 notificationsBeforeAppWarn + 1,
                                 std::chrono::seconds(3)),
              "an exhausted per-app warn-only limit should show its toast");
        std::string callsAfterAppWarn = readFile(details);
        check(callsAfterAppWarn.find("Today's time for this is used up.") !=
                  std::string::npos &&
                  occurrences(callsAfterAppWarn, "powerOff") ==
                      powerOffCallsBeforeAppWarn &&
                  occurrences(callsAfterAppWarn, "turnOffScreen") ==
                      screenOffCallsBeforeAppWarn,
              "per-app warn-only should notify without changing power or screen state");

        const Json tvWarnSettings = Json::parse(
            "{\"enabled\":true,\"dailyMinutes\":1,\"bedtimeStart\":\"\","
            "\"bedtimeEnd\":\"\",\"action\":\"warn\",\"appAction\":\"home\","
            "\"apps\":{}}");
        const unsigned notificationsBeforeTvWarn =
            occurrences(callsAfterAppWarn, "com.webos.notification");
        check(loop.post([&]() {
                  enforcer->stop();
                  enforcer.reset();
                  enforcer.reset(new ScreenTimeEnforcer(loop, state, history, luna, root,
                                                        root + "/proc", root, tvWarnSettings,
                                                        true, true, nullptr, timings));
                  (void)enforcer->start();
              }), "whole-TV warn-only policy should start on the event loop");
        check(waitForOccurrences(details, "com.webos.notification",
                                 notificationsBeforeTvWarn + 1,
                                 std::chrono::seconds(3)),
              "an exhausted daily warn-only limit should show its toast");
        const std::string callsAfterTvWarn = readFile(details);
        check(callsAfterTvWarn.find("Today's screen time is used up.") !=
                  std::string::npos &&
                  occurrences(callsAfterTvWarn, "powerOff") ==
                      powerOffCallsBeforeAppWarn &&
                  occurrences(callsAfterTvWarn, "turnOffScreen") ==
                      screenOffCallsBeforeAppWarn,
              "whole-TV warn-only should notify without changing power or screen state");

        const Json nearLimitSettings = Json::parse(
            "{\"enabled\":true,\"dailyMinutes\":2,\"bedtimeStart\":\"\","
            "\"bedtimeEnd\":\"\",\"action\":\"off\",\"appAction\":\"home\","
            "\"apps\":{\"netflix\":2}}");
        const unsigned notificationsBeforeNearLimit =
            occurrences(callsAfterTvWarn, "com.webos.notification");
        check(loop.post([&]() {
                  enforcer->stop();
                  enforcer.reset();
                  enforcer.reset(new ScreenTimeEnforcer(loop, state, history, luna, root,
                                                        root + "/proc", root,
                                                        nearLimitSettings, true, true,
                                                        nullptr, timings));
                  (void)enforcer->start();
              }), "near-limit warning policy should start on the event loop");
        check(waitForOccurrences(details, "com.webos.notification",
                                 notificationsBeforeNearLimit + 2,
                                 std::chrono::seconds(3)),
              "daily and per-app limits should warn within five minutes");
        std::this_thread::sleep_for(std::chrono::milliseconds(1100));
        const std::string callsAfterNearLimit = readFile(details);
        check(callsAfterNearLimit.find("1 minute of screen time left today.") !=
                  std::string::npos &&
                  callsAfterNearLimit.find("1 minute left for this today.") !=
                  std::string::npos &&
                  occurrences(callsAfterNearLimit, "com.webos.notification") ==
                      notificationsBeforeNearLimit + 2 &&
                  occurrences(callsAfterNearLimit, "powerOff") ==
                      powerOffCallsBeforeAppWarn &&
                  occurrences(callsAfterNearLimit, "turnOffScreen") ==
                      screenOffCallsBeforeAppWarn,
              "near-limit warnings should be distinct, shown once, and have no action");

        ScreenTimeTimings settleTimings = timings;
        settleTimings.settle = std::chrono::milliseconds(200);
        const unsigned notificationsBeforeSettle =
            occurrences(callsAfterNearLimit, "com.webos.notification");
        check(loop.post([&]() {
                  enforcer->stop();
                  enforcer.reset();
                  state::Event screenOff{"power", "screenOn", Json::flag(false), wall,
                                         false, true};
                  state.update(screenOff);
                  enforcer.reset(new ScreenTimeEnforcer(loop, state, history, luna, root,
                                                        root + "/proc", root, appWarnSettings,
                                                        true, true, nullptr, settleTimings));
                  (void)enforcer->start();
              }), "screen-on settling policy should start while the panel is off");
        check(loop.post([&]() {
                  state::Event screenOn{"power", "screenOn", Json::flag(true), wall,
                                        false, true};
                  state.update(screenOn);
              }), "screen-on settling should receive the panel-on event");
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        check(occurrences(readFile(details), "com.webos.notification") ==
                  notificationsBeforeSettle,
              "an app-limit toast should wait while the panel is waking");
        check(waitForOccurrences(details, "com.webos.notification",
                                 notificationsBeforeSettle + 1,
                                 std::chrono::seconds(3)),
              "the app-limit toast should appear after the panel-settle delay");

        const Json cancelSettings = Json::parse(
            "{\"enabled\":true,\"dailyMinutes\":1,\"bedtimeStart\":\"\","
            "\"bedtimeEnd\":\"\",\"action\":\"off\",\"appAction\":\"home\","
            "\"apps\":{}}");
        ScreenTimeTimings cancelTimings = timings;
        cancelTimings.grace = std::chrono::milliseconds(250);
        const unsigned notificationsBeforeCancel =
            occurrences(readFile(details), "com.webos.notification");
        const unsigned powerOffBeforeCancel =
            occurrences(readFile(details), "powerOff");
        check(loop.post([&]() {
                  enforcer->stop();
                  enforcer.reset();
                  enforcer.reset(new ScreenTimeEnforcer(loop, state, history, luna, root,
                                                        root + "/proc", root, cancelSettings,
                                                        true, true, nullptr, cancelTimings));
                  (void)enforcer->start();
              }), "cancelled daily action policy should start on the event loop");
        check(waitForOccurrences(details, "com.webos.notification",
                                 notificationsBeforeCancel + 1,
                                 std::chrono::seconds(3)),
              "a due daily policy should start its grace period before cancellation");
        check(loop.post([&]() {
                  state::Event screenOff{"power", "screenOn", Json::flag(false), wall,
                                         false, true};
                  state.update(screenOff);
              }), "screen-off cancellation should update native state on the event loop");
        std::this_thread::sleep_for(std::chrono::milliseconds(350));
        check(occurrences(readFile(details), "powerOff") == powerOffBeforeCancel,
              "turning the screen off during the grace period should cancel power-off");
        check(loop.post([&]() {
                  enforcer->stop();
                  enforcer.reset();
                  state::Event screenOn{"power", "screenOn", Json::flag(true), wall,
                                        false, true};
                  state.update(screenOn);
              }), "screen-on state should be restored after the cancellation check");

        struct tm local;
        const time_t bedtimeNow = static_cast<time_t>(wall / 1000);
        check(localtime_r(&bedtimeNow, &local) != nullptr,
              "the bedtime fixture needs the host's local clock");
        // Wide enough for any int, as GCC checks the format against the types.
        char bedtimeStart[32] = {};
        char bedtimeEnd[32] = {};
        std::snprintf(bedtimeStart, sizeof(bedtimeStart), "%02d:00",
                      (local.tm_hour + 23) % 24);
        std::snprintf(bedtimeEnd, sizeof(bedtimeEnd), "%02d:00",
                      (local.tm_hour + 2) % 24);
        Json bedtimeSettings = Json::object();
        bedtimeSettings.set("enabled", Json::flag(true));
        bedtimeSettings.set("dailyMinutes", Json::numeric(0));
        bedtimeSettings.set("bedtimeStart", Json::text(bedtimeStart));
        bedtimeSettings.set("bedtimeEnd", Json::text(bedtimeEnd));
        bedtimeSettings.set("action", Json::text("off"));
        bedtimeSettings.set("appAction", Json::text("home"));
        bedtimeSettings.set("apps", Json::object());
        const unsigned powerOffBeforeBedtime =
            occurrences(readFile(details), "powerOff");
        check(loop.post([&]() {
                  if (enforcer) {
                      enforcer->stop();
                      enforcer.reset();
                  }
                  enforcer.reset(new ScreenTimeEnforcer(loop, state, history, luna, root,
                                                        root + "/proc", root,
                                                        bedtimeSettings, true, true, nullptr,
                                                        timings));
                  (void)enforcer->start();
              }), "bedtime enforcement should start on the event loop");
        check(waitForOccurrences(details, "powerOff", powerOffBeforeBedtime + 1,
                                 std::chrono::seconds(3)),
              "an active bedtime limit should power off the TV after its grace period");
        const Json persisted = Json::parse(readFile(root + "/screentime.json"));
        check(persisted["switchedOff"]["bedtime"].boolean(),
              "a bedtime power-off should persist the bedtime marker before the command");

        const std::string callsBeforeHistoryDisabled = readFile(details);
        check(loop.post([&]() {
                  enforcer->stop();
                  enforcer.reset();
                  std::string error;
                  (void)history.setEnabled(false, &error);
                  enforcer.reset(new ScreenTimeEnforcer(loop, state, history, luna, root,
                                                        root + "/proc", root, cancelSettings,
                                                        true, true, nullptr, timings));
                  (void)enforcer->start();
              }), "screen-time enforcement should restart with history disabled");
        std::this_thread::sleep_for(std::chrono::milliseconds(1100));
        check(readFile(details) == callsBeforeHistoryDisabled,
              "screen-time limits should not notify or act while watch-time history is disabled");
        ok = true;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "screen-time action check failed: %s\n", error.what());
        std::fprintf(stderr, "Luna calls: %s\n", readFile(details).c_str());
    }

    (void)loop.post([&]() {
        if (enforcer)
            enforcer->stop();
        enforcer.reset();
        loop.stop();
    });
    if (loopThread.joinable())
        loopThread.join();
    history.stop();
    cleanup(root);
    unsetenv("NATIVETV_FAKE_LUNA_MODE");
    unsetenv("NATIVETV_FAKE_LUNA_DETAILS");
    return ok;
}
} // namespace

int main() {
    char temporary[] = "/tmp/nativetv-screen-time-runtime-XXXXXX";
    const char *created = mkdtemp(temporary);
    if (!created) {
        std::fprintf(stderr, "could not create screen-time runtime fixture\n");
        return 1;
    }
    const std::string root(created);
    const std::string details = root + "/luna-details.jsonl";
    for (const std::string &directory : {
             root + "/usr", root + "/usr/palm", root + "/usr/palm/applications",
             root + "/usr/palm/applications/com.webos.app.home", root + "/proc",
             root + "/proc/bus", root + "/proc/bus/input", root + "/dev",
             root + "/dev/input"})
        if (mkdir(directory.c_str(), 0700) != 0) {
            std::fprintf(stderr, "could not create screen-time runtime fixture directory\n");
            cleanup(root);
            return 1;
        }
    {
        std::ofstream devices((root + "/proc/bus/input/devices").c_str());
        devices << "N: Name=\"LGE RCU\"\nH: Handlers=kbd event3\n";
        std::ofstream event((root + "/dev/input/event3").c_str(),
                            std::ios::binary | std::ios::trunc);
    }
    setenv("NATIVETV_FAKE_LUNA_MODE", "screen-time", 1);
    setenv("NATIVETV_FAKE_LUNA_DETAILS", details.c_str(), 1);

    int64_t wall = 1793016000000LL;
    double monotonic = 0;
    EventLoop loop(2, 32, 64);
    ChildProcessRunner children;
    LunaClientOptions lunaOptions;
    lunaOptions.executable = NATIVETV_FAKE_LUNA_SEND;
    LunaClient luna(loop, children, nullptr, lunaOptions);

    state::Cache state;
    Json power = Json::object();
    power.set("state", Json::text("Active"));
    power.set("systemOn", Json::flag(true));
    power.set("screenOn", Json::flag(true));
    Json application = Json::object();
    application.set("app", Json::text("netflix"));
    application.set("app_id", Json::text("com.webos.app.netflix"));
    Json snapshot = Json::object();
    snapshot.set("power", power);
    snapshot.set("application", application);
    state.update(state::Event{"power", "state", power["state"], wall, true, true});
    state.update(state::Event{"power", "systemOn", power["systemOn"], wall, true, true});
    state.update(state::Event{"power", "screenOn", power["screenOn"], wall, true, true});
    state.update(state::Event{"application", "app", application["app"], wall, true, true});
    state.update(state::Event{"application", "app_id", application["app_id"], wall, true, true});

    HistoryRecorderOptions historyOptions;
    historyOptions.directory = root + "/history";
    historyOptions.enabled = true;
    historyOptions.wallNowMs = [&wall]() { return wall; };
    historyOptions.monotonicNowMs = [&monotonic]() { return monotonic; };
    HistoryRecorder history(historyOptions);
    history.start(snapshot);
    state.onChange([&state, &history](const state::Event &event) {
        history.observe(event, state.snapshot());
    });
    wall += 61000;
    monotonic += 61000;

    Json settings = Json::parse(
        "{\"enabled\":true,\"dailyMinutes\":1,\"bedtimeStart\":\"\","
        "\"bedtimeEnd\":\"\",\"action\":\"off\",\"appAction\":\"home\","
        "\"apps\":{}}");
    ScreenTimeTimings timings;
    timings.check = std::chrono::milliseconds(1000);
    timings.grace = std::chrono::milliseconds(30);
    timings.blank = std::chrono::milliseconds(10);
    timings.again = std::chrono::milliseconds(500);
    timings.settle = std::chrono::milliseconds(5);
    timings.screenSaver = std::chrono::milliseconds(10);
    timings.verify = std::chrono::milliseconds(2000);
    timings.keyPress = std::chrono::milliseconds(5);
    timings.powerSettle = std::chrono::milliseconds(20);
    timings.wallNowMs = [&wall]() { return wall; };

    std::unique_ptr<ScreenTimeEnforcer> enforcer;
    check(loop.post([&]() {
              enforcer.reset(new ScreenTimeEnforcer(loop, state, history, luna, root,
                                                    "/proc", root, settings, true, true,
                                                    nullptr, timings));
              (void)enforcer->start();
          }), "screen-time enforcement should start on the event loop");
    std::thread loopThread([&]() { loop.run(); });

    bool ok = false;
    try {
        check(waitFor(details, "com.webos.notification",
                      std::chrono::seconds(3)),
              "a limit due now should show its toast before the grace action");
        check(waitFor(details, "powerOff",
                      std::chrono::seconds(3)),
              "the daily action should run when the grace timer expires");
        check(occurrences(readFile(details), "com.webos.notification") == 1,
              "an already reached daily limit should not emit an extra near-limit warning");
        Json saved = Json::parse(readFile(root + "/screentime.json"));
        check(saved["switchedOff"]["daily"].boolean(),
              "a daily power-off is persisted before the device command");

        check(loop.post([&]() {
                  enforcer->stop();
                  enforcer.reset();
                  enforcer.reset(new ScreenTimeEnforcer(loop, state, history, luna, root,
                                                        "/proc", root, settings, true, true,
                                                        nullptr, timings));
                  (void)enforcer->start();
              }), "screen-time state should reload after a runtime restart");
        check(waitFor(details, "turnOffScreen",
                      std::chrono::seconds(3)),
              "a switched-on TV should blank its screen again after a persisted limit");
        const unsigned screenOffCalls = occurrences(
            readFile(details), "turnOffScreen");
        check(screenOffCalls >= 1, "the restart flow should issue screenOff");
        check(waitForOccurrences(details, "powerOff", 2,
                                 std::chrono::seconds(3)),
              "a persisted limit should power off again after the switched-on grace period");

        Json raised = Json::parse(
            "{\"enabled\":true,\"dailyMinutes\":2,\"bedtimeStart\":\"\","
            "\"bedtimeEnd\":\"\",\"action\":\"off\",\"appAction\":\"home\","
            "\"apps\":{}}");
        check(loop.post([&]() {
                  std::string applyError;
                  (void)enforcer->apply(raised, &applyError);
              }), "changed settings should be submitted on the event loop");
        const auto removeDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (std::chrono::steady_clock::now() < removeDeadline &&
               access((root + "/screentime.json").c_str(), F_OK) == 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        check(access((root + "/screentime.json").c_str(), F_OK) != 0,
              "changing limits clears the persisted switched-off marker");

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        const std::string callsBeforeDeniedPolicy = readFile(details);
        check(loop.post([&]() {
                  enforcer->stop();
                  enforcer.reset();
                  enforcer.reset(new ScreenTimeEnforcer(loop, state, history, luna, root,
                                                        "/proc", root, settings, false, true,
                                                        nullptr, timings));
                  (void)enforcer->start();
              }), "controls-off policy should restart with the configured gate");
        const auto deniedDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (std::chrono::steady_clock::now() < deniedDeadline &&
               access((root + "/screentime.json").c_str(), F_OK) != 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        check(access((root + "/screentime.json").c_str(), F_OK) == 0,
              "a reached daily limit remains recorded while the control gate is off");
        check(readFile(details) == callsBeforeDeniedPolicy,
              "automatic screen-time actions must respect allowControl");

        ok = true;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        std::fprintf(stderr, "Luna calls: %s\n", readFile(details).c_str());
    }

    (void)loop.post([&]() {
        if (enforcer)
            enforcer->stop();
        enforcer.reset();
        loop.stop();
    });
    loopThread.join();
    cleanup(root);
    unsetenv("NATIVETV_FAKE_LUNA_MODE");
    unsetenv("NATIVETV_FAKE_LUNA_DETAILS");
    if (!ok)
        return 1;
    if (!runAppAndScreenSaverCase())
        return 1;
    std::cout << "native screen-time runtime checks passed\n";
    return 0;
}
