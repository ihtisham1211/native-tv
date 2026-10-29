#include "service/history.hpp"
#include "service/history_recorder.hpp"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

// Fixture folders are made and removed with the shell; a failure shows in the checks that read them.
static void runShell(const std::string &command) {
    if (std::system(command.c_str()) != 0) {
    }
}


using namespace nativetv;
using namespace nativetv::service;

namespace {
unsigned checks = 0;
void check(bool value, const char *message) {
    ++checks;
    if (!value)
        throw std::runtime_error(message);
}
void writeFile(const std::string &path, const std::string &value) {
    std::ofstream output(path.c_str(), std::ios::binary | std::ios::trunc);
    if (!output)
        throw std::runtime_error("could not write history fixture");
    output << value;
}
} // namespace

int main() {
    char temporary[] = "/tmp/nativetv-history-XXXXXX";
    const char *created = mkdtemp(temporary);
    if (!created) {
        std::fprintf(stderr, "could not create history fixture directory\n");
        return 1;
    }
    const std::string directory(created);
    try {
        writeFile(directory + "/2026-10.json",
                  "{\"v\":1,\"hours\":{"
                  "\"2026-10-31T22\":{\"netflix\":300},"
                  "\"2026-10-31T23\":{\"netflix\":600,\"screen_off\":30},"
                  "\"2026-10-30T23\":{\"outside\":900}},"
                  "\"titles\":{\"netflix\":\"Netflix old\"}}");
        writeFile(directory + "/2026-11.json",
                  "{\"v\":1,\"hours\":{\"2026-11-01T00\":{\"hdmi2\":120}},"
                  "\"titles\":{\"hdmi2\":\"PlayStation\"}}");

        Json overlay = Json::object();
        overlay.set("enabled", Json::flag(true));
        overlay.set("readPersisted", Json::flag(true));
        overlay.set("clockSet", Json::flag(true));
        overlay.set("current", Json::text("hdmi2"));
        Json overlayHours = Json::object();
        Json liveHour = Json::object();
        liveHour.set("netflix", Json::numeric(720));
        liveHour.set("hdmi2", Json::numeric(90));
        overlayHours.set("2026-10-31T23", liveHour);
        overlay.set("hours", overlayHours);
        Json freshTitles = Json::object();
        freshTitles.set("netflix", Json::text("Netflix"));
        overlay.set("titles", freshTitles);

        Json result;
        std::string error;
        const int64_t from = 1793484000000LL; // 2026-10-31T22:00Z
        const int64_t to = 1793491200000LL;   // 2026-11-01T00:00Z
        check(queryHistory(directory, from, to, overlay, result, error),
              "A persisted query and live overlay should succeed");
        check(result["ok"].boolean() && result["enabled"].boolean() &&
                  result["current"].string() == "hdmi2" && result["clockSet"].boolean(),
              "The query preserves current history state");
        if (!(result["hours"].keys().size() == 3 &&
              result["hours"]["2026-10-31T22"]["netflix"].number() == 300 &&
              result["hours"]["2026-10-31T23"]["netflix"].number() == 720 &&
              result["hours"]["2026-10-31T23"]["screen_off"].number() == 30 &&
              result["hours"]["2026-10-31T23"]["hdmi2"].number() == 90 &&
              result["hours"]["2026-11-01T00"]["hdmi2"].number() == 120 &&
              !result["hours"]["2026-10-30T23"].valid()))
            throw std::runtime_error("history range or overlay mismatch: " + result.dump());
        check(result["titles"]["netflix"].string() == "Netflix" &&
                  result["titles"]["hdmi2"].string() == "PlayStation" &&
                  !result["titles"]["screen_off"].valid(),
              "Live titles override saved titles for keys in the query");
        check(!queryHistory(directory, from, to, Json::object(), result, error) && !error.empty(),
              "Malformed live overlays are rejected");

        overlay.set("readPersisted", Json::flag(false));
        overlay.set("hours", Json::object());
        check(queryHistory(directory, from, to, overlay, result, error) &&
                  result["hours"].keys().empty(),
              "A disabled recorder preserves the legacy unloaded-history behavior");

        const std::string recorderDirectory = directory + "/recorder";
        int64_t recorderWall = 1793491140000LL; // 2026-10-31T23:59:00Z
        double recorderMono = 1000.0;
        HistoryRecorderOptions recorderOptions;
        recorderOptions.directory = recorderDirectory;
        recorderOptions.enabled = true;
        recorderOptions.wallNowMs = [&recorderWall]() { return recorderWall; };
        recorderOptions.monotonicNowMs = [&recorderMono]() { return recorderMono; };
        HistoryRecorder recorder(recorderOptions);
        Json snapshot = Json::object();
        Json power = Json::object();
        power.set("state", Json::text("Active"));
        power.set("systemOn", Json::flag(true));
        power.set("screenOn", Json::flag(true));
        snapshot.set("power", power);
        Json application = Json::object();
        application.set("app", Json::text("netflix"));
        application.set("app_id", Json::text("com.webos.app.netflix"));
        application.set("display_title", Json::text("Netflix"));
        snapshot.set("application", application);
        recorder.start(snapshot);

        recorderMono += 120000;
        recorderWall += 120000;
        application.set("app", Json::text("youtube"));
        application.set("app_id", Json::text("com.webos.app.youtube"));
        application.set("display_title", Json::text("YouTube"));
        snapshot.set("application", application);
        state::Event appChanged{"application", "app", Json::text("youtube"),
                                recorderWall, false, true};
        recorder.observe(appChanged, snapshot);

        recorderMono += 60000;
        recorderWall += 60000;
        // LG can report the raw state as Active while the explicit screenOn
        // subscription already says the panel is blank, matching Node's keyFor.
        power.set("state", Json::text("Active"));
        power.set("screenOn", Json::flag(false));
        snapshot.set("power", power);
        state::Event screenOff{"power", "screenOn", Json::flag(false),
                               recorderWall, false, true};
        recorder.observe(screenOff, snapshot);
        recorderMono += 30000;
        recorderWall += 30000;
        Json recorded = recorder.overlay(1793487600000LL, 1793498400000LL);
        check(recorded["enabled"].boolean() && recorded["running"].boolean() &&
                  recorded["readPersisted"].boolean() &&
                  recorded["current"].string() == "screen_off",
              "The native recorder reports enabled state and current screen-off key");
        Json nativeQuery;
        check(queryHistory(recorderDirectory, 1793487600000LL, 1793498400000LL,
                           recorded, nativeQuery, error) &&
                  nativeQuery["hours"]["2026-10-31T23"]["netflix"].number() == 60 &&
                  nativeQuery["hours"]["2026-11-01T00"]["netflix"].number() == 60 &&
                  nativeQuery["hours"]["2026-11-01T00"]["youtube"].number() == 60 &&
                  nativeQuery["hours"]["2026-11-01T00"]["screen_off"].number() == 30,
              "The native query merges persisted hours and the live in-memory overlay");
        check(nativeQuery["titles"]["youtube"].string() == "YouTube",
              "Native history retains display titles for its application keys");
        const Json runtimeTotals = recorder.totals(1793487600000LL, 1793498400000LL);
        check(runtimeTotals["netflix"].number() == 120 &&
                  runtimeTotals["youtube"].number() == 60 &&
                  runtimeTotals["screen_off"].number() == 30,
              "Screen-time totals checkpoint live history without reading or writing monthly files");
        check(recorder.flush(&error), "Native monthly history should flush atomically");
        const Json persistedOctober = Json::parse([&recorderDirectory]() {
                  std::ifstream input((recorderDirectory + "/2026-10.json").c_str());
                  std::ostringstream contents;
                  contents << input.rdbuf();
                  return contents.str();
              }());
        const Json persistedNovember = Json::parse([&recorderDirectory]() {
                  std::ifstream input((recorderDirectory + "/2026-11.json").c_str());
                  std::ostringstream contents;
                  contents << input.rdbuf();
                  return contents.str();
              }());
        check(persistedOctober["ids"]["netflix"].string() == "com.webos.app.netflix" &&
                  persistedNovember["ids"]["youtube"].string() == "com.webos.app.youtube",
              "Native monthly files preserve the legacy IDs field");
        check(recorder.clear(&error) && !recorder.status()["kept"].boolean(),
              "Clear removes persisted and pending native history");
        check(recorder.setEnabled(false, &error) && !recorder.status()["running"].boolean(),
              "The native recorder can be disabled without leaving its subscriptions active");
        writeFile(recorderDirectory + "/2025-09.json",
                  "{\"v\":1,\"hours\":{\"2025-09-01T10\":{\"old\":5}},\"titles\":{}}");
        writeFile(recorderDirectory + "/2025-10.json",
                  "{\"v\":1,\"hours\":{\"2025-10-01T10\":{\"kept\":5}},\"titles\":{}}");
        recorderWall = 1793484000000LL; // 2026-10-31, so October 2025 is retained.
        HistoryRecorder prunedRecorder(recorderOptions);
        check(access((recorderDirectory + "/2025-09.json").c_str(), F_OK) != 0 &&
                  access((recorderDirectory + "/2025-10.json").c_str(), F_OK) == 0,
              "Native startup prunes months older than the 13-month retention window");
        check(prunedRecorder.clear(&error), "Native Clear removes retained month files");

        // The watch summary for the telemetry topic: today and this week by the
        // TV's own day, the most watched title today, idle time left out.
        const std::string summaryDirectory = directory + "/summary";
        mkdir(summaryDirectory.c_str(), 0700);
        writeFile(summaryDirectory + "/2026-10.json",
                  "{\"v\":1,\"hours\":{"
                  "\"2026-10-28T10\":{\"youtube\":3600,\"screen_off\":600},"
                  "\"2026-10-27T09\":{\"netflix\":7200},"
                  "\"2026-10-21T09\":{\"old\":9999}},"
                  "\"titles\":{\"youtube\":\"YouTube\"}}");
        int64_t summaryWall = 1793199600000LL;
        HistoryRecorderOptions summaryOptions;
        summaryOptions.directory = summaryDirectory;
        summaryOptions.enabled = true;
        summaryOptions.wallNowMs = [&summaryWall]() { return summaryWall; };
        summaryOptions.monotonicNowMs = []() { return 0.0; };
        HistoryRecorderOptions offOptions = summaryOptions;
        offOptions.enabled = false;
        HistoryRecorder offRecorder(offOptions);
        check(!offRecorder.summary(summaryWall, 0).valid(),
              "No watch summary while watch history is off");
        HistoryRecorder summaryRecorder(summaryOptions);
        for (int offsetMinutes : {0, 300}) {
            const Json summary = summaryRecorder.summary(summaryWall, offsetMinutes);
            check(summary["today_h"].number() == 1 && summary["week_h"].number() == 3 &&
                      summary["top_today"].string() == "YouTube",
                  "The watch summary counts today and the week from the TV's own midnight");
        }
        check(!summaryRecorder.summary(1000, 0).valid(),
              "No watch summary while the clock is unset");

        const std::string clockDirectory = directory + "/clock";
        int64_t delayedWall = 1000;
        double delayedMono = 5000.0;
        HistoryRecorderOptions delayedOptions;
        delayedOptions.directory = clockDirectory;
        delayedOptions.enabled = true;
        delayedOptions.wallNowMs = [&delayedWall]() { return delayedWall; };
        delayedOptions.monotonicNowMs = [&delayedMono]() { return delayedMono; };
        HistoryRecorder delayedRecorder(delayedOptions);
        power.set("state", Json::text("Active"));
        power.set("systemOn", Json::flag(true));
        power.set("screenOn", Json::flag(true));
        application.set("app", Json::text("netflix"));
        application.set("app_id", Json::text("com.webos.app.netflix"));
        application.set("display_title", Json::text("Netflix"));
        snapshot.set("power", power);
        snapshot.set("application", application);
        delayedRecorder.start(snapshot);
        delayedMono += 60000;
        application.set("app", Json::text("youtube"));
        application.set("app_id", Json::text("com.webos.app.youtube"));
        application.set("display_title", Json::text("YouTube"));
        snapshot.set("application", application);
        delayedRecorder.observe(appChanged, snapshot);
        Json beforeClock = delayedRecorder.overlay(0, 2000000000000LL);
        check(!beforeClock["clockSet"].boolean() && beforeClock["hours"].keys().empty(),
              "Native history holds monotonic segments until the wall clock is trusted");
        delayedWall = 1793484000000LL; // 2026-10-31T22:00:00Z
        delayedMono += 30000;
        delayedWall += 30000;
        power.set("state", Json::text("ScreenOff"));
        power.set("screenOn", Json::flag(false));
        snapshot.set("power", power);
        delayedRecorder.observe(screenOff, snapshot);
        Json afterClockOverlay = delayedRecorder.overlay(1793480400000LL, 1793491200000LL);
        Json afterClock;
        check(queryHistory(clockDirectory, 1793480400000LL, 1793491200000LL,
                           afterClockOverlay, afterClock, error) &&
                  afterClock["clockSet"].boolean() &&
                  afterClock["hours"]["2026-10-31T21"]["netflix"].number() == 60 &&
                  afterClock["hours"]["2026-10-31T22"]["youtube"].number() == 30,
              "Native history places held segments once the system clock becomes valid");
        delayedRecorder.stop(&error);

        // test-history.js 2 and 2b: time with the panel off, or with LG's screen saver
        // showing, is kept apart; a moment of screen off before the TV goes off is not kept.
        for (const bool saver : {false, true}) {
            const std::string splitDirectory = directory + (saver ? "/saver" : "/split");
            mkdir(splitDirectory.c_str(), 0700);
            int64_t wall = 1792003800000LL; // 2026-10-14T18:50:00Z
            double mono = 1000.0;
            HistoryRecorderOptions splitOptions;
            splitOptions.directory = splitDirectory;
            splitOptions.enabled = true;
            splitOptions.wallNowMs = [&wall]() { return wall; };
            splitOptions.monotonicNowMs = [&mono]() { return mono; };
            HistoryRecorder splitRecorder(splitOptions);
            Json splitSnapshot = Json::object(), splitPower = Json::object(), splitApp = Json::object();
            splitPower.set("state", Json::text("Active"));
            splitPower.set("systemOn", Json::flag(true));
            splitPower.set("screenOn", Json::flag(true));
            splitApp.set("app", Json::text(saver ? "netflix" : "spotify"));
            splitApp.set("app_id", Json::text(saver ? "netflix" : "spotify"));
            splitApp.set("display_title", Json::text(saver ? "netflix" : "spotify"));
            splitSnapshot.set("power", splitPower);
            splitSnapshot.set("application", splitApp);
            splitRecorder.start(splitSnapshot);
            const auto powerEvent = [&](const char *key, const Json &value) {
                splitPower.set(key, value);
                splitSnapshot.set("power", splitPower);
                splitRecorder.observe(state::Event{"power", key, value, wall, false, true}, splitSnapshot);
            };
            const auto advance = [&](int64_t milliseconds) {
                wall += milliseconds;
                mono += milliseconds;
            };
            const auto hourOf = [&](const char *key) {
                std::string splitError;
                check(splitRecorder.flush(&splitError), "Native history flushes");
                std::ifstream input((splitDirectory + "/2026-10.json").c_str());
                std::ostringstream contents;
                contents << input.rdbuf();
                return Json::parse(contents.str())["hours"]["2026-10-14T18"][key].number(0);
            };
            if (!saver) {
                advance(60000);
                powerEvent("screenOn", Json::flag(false));
                advance(120000);
                powerEvent("systemOn", Json::flag(false));
                check(hourOf("spotify") == 60 && hourOf("screen_off") == 120,
                      "Time with the panel off is kept apart as screen_off");
                powerEvent("systemOn", Json::flag(true));
                powerEvent("screenOn", Json::flag(true));
                advance(1000);
                powerEvent("screenOn", Json::flag(false));
                advance(20);
                powerEvent("systemOn", Json::flag(false));
                check(hourOf("spotify") == 61 && hourOf("screen_off") == 120,
                      "A moment of screen off before the TV goes off is not kept");
            } else {
                advance(60000);
                powerEvent("state", Json::text("Screen Saver"));
                advance(300000);
                powerEvent("state", Json::text("Active"));
                advance(30000);
                powerEvent("systemOn", Json::flag(false));
                check(hourOf("netflix") == 90 && hourOf("screen_saver") == 300,
                      "Time with LG's screen saver showing is kept apart, and not as watched");
            }
            std::string stopError;
            splitRecorder.stop(&stopError);
            check(splitRecorder.clear(&stopError), "Split history is cleared");
            rmdir(splitDirectory.c_str());
        }

        // test-history.js 1, 4, 5 and 6: counted per app and split at the hour, nothing
        // counted until it is turned on, a restart reads back what was kept and "today"
        // follows the TV's time zone, and Clear leaves counting going.
        {
            const std::string watchDirectory = directory + "/watch";
            mkdir(watchDirectory.c_str(), 0700);
            int64_t wall = 1792003800000LL; // 2026-10-14T18:50:00Z
            double mono = 1000.0;
            HistoryRecorderOptions watchOptions;
            watchOptions.directory = watchDirectory;
            watchOptions.enabled = false;
            watchOptions.wallNowMs = [&wall]() { return wall; };
            watchOptions.monotonicNowMs = [&mono]() { return mono; };
            Json watchSnapshot = Json::object(), watchPower = Json::object(), watchApp = Json::object();
            watchPower.set("state", Json::text("Active"));
            watchPower.set("systemOn", Json::flag(true));
            watchPower.set("screenOn", Json::flag(true));
            const auto showApp = [&](HistoryRecorder &recorder, const char *app, const char *title) {
                watchApp.set("app", Json::text(app));
                watchApp.set("app_id", Json::text(app));
                watchApp.set("display_title", Json::text(title));
                watchSnapshot.set("power", watchPower);
                watchSnapshot.set("application", watchApp);
                recorder.observe(state::Event{"application", "app", Json::text(app), wall, false, true},
                                 watchSnapshot);
            };
            const auto power = [&](HistoryRecorder &recorder, bool on) {
                watchPower.set("systemOn", Json::flag(on));
                watchSnapshot.set("power", watchPower);
                recorder.observe(state::Event{"power", "systemOn", Json::flag(on), wall, false, true},
                                 watchSnapshot);
            };
            const auto advance = [&](int64_t minutes) {
                wall += minutes * 60000;
                mono += minutes * 60000;
            };
            const auto monthFile = [&]() {
                std::ifstream input((watchDirectory + "/2026-10.json").c_str());
                std::ostringstream contents;
                contents << input.rdbuf();
                return Json::parse(contents.str());
            };

            // 4. Nothing is counted while off; turning it on counts, turning it off writes what was.
            HistoryRecorder recorder(watchOptions);
            watchApp.set("app", Json::text("netflix"));
            watchSnapshot.set("power", watchPower);
            watchSnapshot.set("application", watchApp);
            recorder.start(watchSnapshot);
            showApp(recorder, "netflix", "Netflix");
            advance(1);
            power(recorder, false);
            check(access((watchDirectory + "/2026-10.json").c_str(), F_OK) != 0 &&
                      !recorder.summary(wall, 0).valid(),
                  "Nothing is written or summarised while history is off");
            check(recorder.setEnabled(true, &error), "History turns on");
            power(recorder, true);
            showApp(recorder, "netflix", "Netflix");
            advance(1);
            check(recorder.setEnabled(false, &error) &&
                      monthFile()["hours"]["2026-10-14T18"]["netflix"].number() == 60,
                  "Turning it off writes what was counted");

            // 1. Counted per app and split at the hour (18:50 + 20 min crosses 19:00).
            check(recorder.clear(&error), "History is cleared before the next run");
            wall = 1792003800000LL;
            mono = 3000.0;
            HistoryRecorder counted(watchOptions);
            counted.setEnabled(true, &error);
            power(counted, true);
            showApp(counted, "netflix", "Netflix");
            advance(20);
            showApp(counted, "hdmi2", "PlayStation");
            advance(5);
            power(counted, false);
            check(counted.flush(&error), "History flushes");
            const Json hours = monthFile();
            check(hours["hours"]["2026-10-14T18"]["netflix"].number() == 600 &&
                      hours["hours"]["2026-10-14T19"]["netflix"].number() == 600 &&
                      hours["hours"]["2026-10-14T19"]["hdmi2"].number() == 300,
                  "Time is counted per app and split at the hour");
            check(hours["titles"]["hdmi2"].string() == "PlayStation" &&
                      hours["titles"]["netflix"].string() == "Netflix",
                  "and each app keeps its own title, not the next app's");

            // 5. A restart reads back what was kept.
            counted.stop(&error);
            HistoryRecorder restarted(watchOptions);
            restarted.setEnabled(true, &error);
            const Json totals = restarted.totals(wall - 3600000, wall + 3600000);
            check(totals["netflix"].number() == 1200, "A restart reads back what was kept");

            // The offset: local midnight is 22:00 UTC (CEST), so 18:50 to 19:20 UTC is today.
            HistoryRecorder zoned(watchOptions);
            zoned.setEnabled(true, &error);
            check(zoned.clear(&error), "History is cleared for the time zone case");
            wall = 1792003800000LL;
            mono = 5000.0;
            power(zoned, true);
            showApp(zoned, "netflix", "Netflix");
            advance(90);
            Json summary = zoned.summary(wall, 120);
            check(summary["today_h"].number() == 1.5 && summary["week_h"].number() == 1.5 &&
                      summary["top_today"].string() == "Netflix",
                  "Today counts from the TV's own midnight");
            advance(120); // 22:20 UTC is 00:20 the next day there
            summary = zoned.summary(wall, 120);
            check(summary["today_h"].number() == 0.33 && summary["week_h"].number() == 3.5,
                  "Past local midnight only the new day is today, the week keeps both");

            // 6. Clear, and counting goes on from it.
            check(zoned.clear(&error), "Clear removes what was kept");
            advance(1);
            check(zoned.flush(&error) && monthFile()["hours"]["2026-10-14T22"]["netflix"].number() == 60,
                  "Counting goes on from the clear");
            zoned.stop(&error);
            runShell(("rm -rf '" + watchDirectory + "'"));
        }

        std::remove((directory + "/2026-10.json").c_str());
        std::remove((directory + "/2026-11.json").c_str());
        rmdir(recorderDirectory.c_str());
        std::remove((clockDirectory + "/2026-10.json").c_str());
        rmdir(clockDirectory.c_str());
        rmdir(directory.c_str());
        std::printf("%u native history checks passed\n", checks);
        return 0;
    } catch (const std::exception &error) {
        std::remove((directory + "/2026-10.json").c_str());
        std::remove((directory + "/2026-11.json").c_str());
        rmdir((directory + "/recorder").c_str());
        std::remove((directory + "/clock/2026-10.json").c_str());
        rmdir((directory + "/clock").c_str());
        rmdir(directory.c_str());
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
