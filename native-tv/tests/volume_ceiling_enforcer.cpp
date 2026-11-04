#include "daemon/child_process.hpp"
#include "daemon/event_loop.hpp"
#include "daemon/luna.hpp"
#include "daemon/volume_ceiling_enforcer.hpp"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <functional>
#include <future>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <time.h>
#include <unistd.h>
#include <vector>

#ifndef NATIVETV_FAKE_LUNA_SEND
#error NATIVETV_FAKE_LUNA_SEND must name the native fake luna-send fixture
#endif

using namespace nativetv;
using namespace nativetv::daemon;

namespace {
void check(bool value, const std::string &message) {
    if (!value)
        throw std::runtime_error(message);
}

const auto kShort = std::chrono::milliseconds(300);

// A local time on a fixed day, as milliseconds since the epoch.
int64_t localTime(int hour, int minute) {
    struct tm local = {};
    local.tm_year = 2026 - 1900;
    local.tm_mon = 10;
    local.tm_mday = 2;
    local.tm_hour = hour;
    local.tm_min = minute;
    local.tm_isdst = -1;
    return static_cast<int64_t>(mktime(&local)) * 1000;
}

// One enforcer on a loop, against the stand-in luna-send, with its calls logged.
struct Rig {
    std::string root;
    std::string details;
    std::atomic<int64_t> wall{localTime(12, 0)};
    std::atomic<double> steady{1000};
    std::atomic<double> uptime{100000};
    EventLoop loop{2, 32, 64};
    ChildProcessRunner children;
    LunaClientOptions lunaOptions;
    std::unique_ptr<LunaClient> luna;
    std::unique_ptr<CommandPolicyExecutor> commandPolicy;
    state::Cache state;
    std::unique_ptr<VolumeCeilingEnforcer> enforcer;
    std::thread loopThread;

    Rig(const std::string &mode, const std::string &settings, bool allowControl = true,
        bool power = true, double startingUptime = 100000) {
        uptime = startingUptime;
        char temporary[] = "/tmp/nativetv-volume-ceiling-XXXXXX";
        if (!mkdtemp(temporary))
            throw std::runtime_error("could not create the fixture directory");
        root = temporary;
        details = root + "/luna-details.jsonl";
        setenv("NATIVETV_FAKE_LUNA_MODE", mode.c_str(), 1);
        setenv("NATIVETV_FAKE_LUNA_DETAILS", details.c_str(), 1);
        lunaOptions.executable = NATIVETV_FAKE_LUNA_SEND;
        luna.reset(new LunaClient(loop, children, nullptr, lunaOptions));
        commandPolicy.reset(new CommandPolicyExecutor(*luna));
        powerEvents(power);

        VolumeCeilingTimings timings;
        timings.check = std::chrono::milliseconds(50);
        timings.settle = std::chrono::milliseconds(5);
        timings.refused = std::chrono::milliseconds(300000);
        timings.wallNowMs = [this]() { return wall.load(); };
        timings.steadyNowMs = [this]() { return steady.load(); };
        timings.uptimeSeconds = [this]() { return uptime.load(); };
        check(loop.post([=]() {
                  enforcer.reset(new VolumeCeilingEnforcer(
                      loop, state, *luna, root + "/proc", Json::parse(settings), allowControl,
                      nullptr, timings, commandPolicy.get()));
                  (void)enforcer->start();
              }), "the enforcer should start on the event loop");
        loopThread = std::thread([this]() { loop.run(); });
    }

    ~Rig() {
        (void)loop.post([this]() {
            if (enforcer)
                enforcer->stop();
            enforcer.reset();
            loop.stop();
        });
        if (loopThread.joinable())
            loopThread.join();
        unlink(details.c_str());
        rmdir(root.c_str());
        unsetenv("NATIVETV_FAKE_LUNA_MODE");
        unsetenv("NATIVETV_FAKE_LUNA_DETAILS");
    }

    // The state changes on the loop, as the daemon's subscriptions deliver them, and after
    // anything already posted (a new setting) has been taken.
    void onLoop(std::function<void()> work) {
        const std::shared_ptr<std::promise<void>> done = std::make_shared<std::promise<void>>();
        std::future<void> finished = done->get_future();
        check(loop.post([work, done]() {
                  work();
                  done->set_value();
              }), "the loop should take the state change");
        check(finished.wait_for(std::chrono::seconds(3)) == std::future_status::ready,
              "the loop should run the state change");
    }
    void powerEvents(bool on) {
        state.update(state::Event{"power", "systemOn", Json::flag(on), wall.load(), false, true});
        state.update(state::Event{"power", "screenOn", Json::flag(on), wall.load(), false, true});
    }
    void setPower(bool on) {
        onLoop([this, on]() { powerEvents(on); });
    }
    void setVolume(int level) {
        onLoop([this, level]() {
            state.update(state::Event{"audio", "volume", Json::numeric(level), wall.load(),
                                      false, true});
        });
    }
    // The levels the enforcer has set, in order.
    std::vector<int> levels() const {
        std::vector<int> result;
        std::ifstream input(details.c_str());
        std::string line;
        while (std::getline(input, line)) {
            const Json call = Json::parse(line);
            if (call["uri"].string() == "com.webos.audio/setVolume")
                result.push_back(static_cast<int>(call["payload"]["volume"].number()));
        }
        return result;
    }
    bool waitForLevels(size_t count, std::chrono::milliseconds timeout = std::chrono::seconds(3)) {
        const auto end = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < end) {
            if (levels().size() >= count)
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return levels().size() >= count;
    }
};

std::string listed(const std::vector<int> &levels) {
    std::ostringstream text;
    for (int level : levels)
        text << level << ' ';
    return text.str();
}

void holdsTheCeiling() {
    Rig rig("volume-ceiling", "{\"enabled\":true,\"maxVolume\":30}");
    rig.setVolume(50);
    check(rig.waitForLevels(1), "a volume over the ceiling should be set back");
    check(rig.levels() == std::vector<int>({30}), "it is set to the ceiling, not below it");
    rig.setVolume(30);
    rig.setVolume(20);
    std::this_thread::sleep_for(kShort);
    check(rig.levels().size() == 1,
          "a volume at or under the ceiling is left alone: " + listed(rig.levels()));
    rig.setVolume(45);
    check(rig.waitForLevels(2) && rig.levels() == std::vector<int>({30, 30}),
          "a later rise over the ceiling is set back again");
}

void leavesAnUnsetCeilingAlone() {
    Rig rig("volume-ceiling", "{\"enabled\":false,\"maxVolume\":30}");
    rig.setVolume(90);
    std::this_thread::sleep_for(kShort);
    check(rig.levels().empty(), "nothing is set while the ceiling is off");
    check(rig.enforcer->apply(Json::parse("{\"enabled\":true,\"maxVolume\":40}")),
          "new settings should be taken on the loop");
    check(rig.waitForLevels(1) && rig.levels() == std::vector<int>({40}),
          "switching the ceiling on holds a volume already over it");
    check(rig.enforcer->apply(Json::parse("{\"enabled\":false,\"maxVolume\":40}")),
          "settings should be taken on the loop");
    rig.setVolume(80);
    std::this_thread::sleep_for(kShort);
    check(rig.levels().size() == 1, "switching it off stops it");
}

void holdsTheNightLimit() {
    Rig rig("volume-ceiling",
            "{\"enabled\":true,\"maxVolume\":60,\"nightMaxVolume\":20,"
            "\"nightStart\":\"22:00\",\"nightEnd\":\"07:00\"}");
    rig.setVolume(40);
    std::this_thread::sleep_for(kShort);
    check(rig.levels().empty(), "by day the night limit is not held");
    // The window opens under a volume already set: the periodic check finds it.
    rig.wall = localTime(23, 0);
    check(rig.waitForLevels(1) && rig.levels() == std::vector<int>({20}),
          "the night limit is held once the night starts, even with no volume change");
    rig.setVolume(20);
    rig.wall = localTime(7, 30);
    rig.setVolume(55);
    std::this_thread::sleep_for(kShort);
    check(rig.levels().size() == 1, "the maximum holds again when the night ends");
}

void setsTheStartupVolume() {
    // Switched on from standby.
    {
        Rig rig("volume-ceiling",
                "{\"enabled\":true,\"maxVolume\":50,\"startupVolume\":15}", true, false);
        std::this_thread::sleep_for(kShort);
        check(rig.levels().empty(), "a TV in standby gets no start-up volume");
        rig.setPower(true);
        check(rig.waitForLevels(1) && rig.levels() == std::vector<int>({15}),
              "the start-up volume is set when the TV comes on");
        rig.setPower(true);
        std::this_thread::sleep_for(kShort);
        check(rig.levels().size() == 1, "it is set once for one switch-on");
    }
    // A server restarted on a TV that has been on for a long time.
    {
        Rig rig("volume-ceiling", "{\"enabled\":true,\"startupVolume\":25}");
        std::this_thread::sleep_for(kShort);
        check(rig.levels().empty(), "a restarted server leaves the volume of a TV long on alone");
    }
    // A server started with the TV, which sees no change of power.
    {
        Rig rig("volume-ceiling", "{\"enabled\":true,\"startupVolume\":25}", true, true, 30);
        check(rig.waitForLevels(1) && rig.levels() == std::vector<int>({25}),
              "a server started with the TV sets the start-up volume");
    }
    // Held to the ceiling.
    {
        Rig rig("volume-ceiling",
                "{\"enabled\":true,\"maxVolume\":20,\"startupVolume\":65}", true, true, 30);
        check(rig.waitForLevels(1) && rig.levels() == std::vector<int>({20}),
              "the start-up volume does not pass the ceiling");
    }
    // Off: nothing, whatever the start-up volume.
    {
        Rig rig("volume-ceiling", "{\"enabled\":false,\"startupVolume\":25}", true, true, 30);
        std::this_thread::sleep_for(kShort);
        check(rig.levels().empty(), "a ceiling that is off sets no start-up volume");
    }
}

void stopsAskingAnOutputThatRefuses() {
    Rig rig("volume-ceiling-refused", "{\"enabled\":true,\"maxVolume\":30}");
    rig.setVolume(50);
    check(rig.waitForLevels(1), "a refused volume is still tried once");
    rig.setVolume(55);
    std::this_thread::sleep_for(kShort);
    check(rig.levels().size() == 1,
          "an output that takes no set level is not asked again at once: " + listed(rig.levels()));
    rig.steady = rig.steady.load() + 300001;
    rig.setVolume(60);
    check(rig.waitForLevels(2), "it is asked again after a while");
}

void doesNothingWithControlsOff() {
    Rig rig("volume-ceiling", "{\"enabled\":true,\"maxVolume\":30}", false);
    rig.setVolume(80);
    std::this_thread::sleep_for(kShort);
    check(rig.levels().empty(), "with controls off in the config the volume is not set");
}

void leavesOutTheNightWithoutAClock() {
    Rig rig("volume-ceiling",
            "{\"enabled\":true,\"maxVolume\":60,\"nightMaxVolume\":20,"
            "\"nightStart\":\"00:00\",\"nightEnd\":\"23:59\"}");
    rig.wall = 1000;
    rig.setVolume(40);
    std::this_thread::sleep_for(kShort);
    check(rig.levels().empty(), "an unset clock leaves the night window out");
    rig.setVolume(70);
    check(rig.waitForLevels(1) && rig.levels() == std::vector<int>({60}),
          "the maximum still holds with the clock unset");
}
} // namespace

int main() {
    struct Case {
        const char *name;
        void (*run)();
    };
    const Case cases[] = {
        {"holds the ceiling", holdsTheCeiling},
        {"leaves an unset ceiling alone", leavesAnUnsetCeilingAlone},
        {"holds the night limit", holdsTheNightLimit},
        {"sets the start-up volume", setsTheStartupVolume},
        {"stops asking an output that refuses", stopsAskingAnOutputThatRefuses},
        {"does nothing with controls off", doesNothingWithControlsOff},
        {"leaves out the night without a clock", leavesOutTheNightWithoutAClock}};
    int failed = 0;
    for (const Case &testCase : cases) {
        try {
            testCase.run();
        } catch (const std::exception &error) {
            std::fprintf(stderr, "volume ceiling enforcer check failed (%s): %s\n",
                         testCase.name, error.what());
            ++failed;
        }
    }
    if (!failed)
        std::printf("volume ceiling enforcer checks passed\n");
    return failed ? 1 : 0;
}
