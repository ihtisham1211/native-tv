#include "daemon/child_process.hpp"
#include "daemon/event_loop.hpp"
#include "daemon/luna.hpp"
#include "daemon/remote_battery.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>

using namespace nativetv;
using namespace nativetv::daemon;

namespace {
using Clock = std::chrono::steady_clock;

void check(bool condition, const std::string &message) {
    if (!condition)
        throw std::runtime_error(message);
}

std::string temporaryDirectory() {
    char pattern[] = "/tmp/nativetv-remote-battery-XXXXXX";
    char *created = mkdtemp(pattern);
    check(created != nullptr, "could not create remote battery test directory");
    return created;
}

size_t connectCount(const std::string &path) {
    std::ifstream input(path.c_str());
    std::string line;
    size_t count = 0;
    while (std::getline(input, line)) {
        const Json record = Json::parse(line);
        if (record["uri"].string() ==
            "com.webos.service.bluetooth2/gatt/connect")
            ++count;
    }
    return count;
}

std::string readFile(const std::string &path) {
    std::ifstream input(path.c_str());
    return std::string(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
}

void waitUntilCount(const std::string &path, size_t wanted,
                    std::chrono::milliseconds timeout,
                    std::vector<Clock::time_point> &observed) {
    const Clock::time_point deadline = Clock::now() + timeout;
    size_t previous = observed.size();
    while (Clock::now() < deadline) {
        const size_t count = connectCount(path);
        while (previous < count) {
            observed.push_back(Clock::now());
            ++previous;
        }
        if (count >= wanted)
            return;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

void runScenario(bool failDiscovery, EventLoop::Milliseconds retry,
                 EventLoop::Milliseconds success, EventLoop::Milliseconds earlyCheck,
                 EventLoop::Milliseconds minimumInterval) {
    const std::string root = temporaryDirectory();
    const std::string dataRoot = root + "/data";
    const std::string systemRoot = root + "/system";
    const std::string details = root + "/luna.jsonl";
    check(mkdir(dataRoot.c_str(), 0700) == 0, "could not create battery data root");
    check(mkdir(systemRoot.c_str(), 0700) == 0, "could not create fake system root");
    check(mkdir((systemRoot + "/etc").c_str(), 0700) == 0,
          "could not create fake system root");
    {
        std::ofstream issue((systemRoot + "/etc/issue").c_str());
        issue << "webOS TV 9.2.0\n";
    }

    ChildRunnerOptions childOptions;
    childOptions.startupGap = std::chrono::milliseconds(0);
    childOptions.regularGap = std::chrono::milliseconds(0);
    childOptions.startupWindow = std::chrono::milliseconds(0);
    ChildProcessRunner children(childOptions);
    EventLoop loop(8, 32, 128);
    LunaClientOptions lunaOptions;
    lunaOptions.executable = NATIVETV_FAKE_LUNA_SEND;
    lunaOptions.environment.push_back("NATIVETV_FAKE_LUNA_MODE=remote-battery");
    lunaOptions.environment.push_back("NATIVETV_FAKE_LUNA_DETAILS=" + details);
    lunaOptions.environment.push_back(std::string("NATIVETV_FAKE_LUNA_DISCOVER_FAIL=") +
                                     (failDiscovery ? "1" : "0"));
    LunaClient luna(loop, children, nullptr, lunaOptions);
    RemoteBatteryTimings timings;
    timings.check = std::chrono::milliseconds(15);
    timings.settle = std::chrono::milliseconds(2);
    timings.discover = std::chrono::milliseconds(2);
    timings.retry = retry;
    timings.success = success;
    RemoteBattery battery(loop, luna, dataRoot, systemRoot, nullptr, timings);

    check(loop.post([&]() {
              check(battery.start(), "battery poller did not start on event loop");
              battery.setPowerOn(true);
          }), "could not schedule battery poller startup");
    std::thread loopThread([&]() { loop.run(); });

    std::vector<Clock::time_point> observed;
    waitUntilCount(details, 1, std::chrono::seconds(3), observed);
    const bool firstObserved = observed.size() == 1;
    bool noEarlyRetry = false;
    if (firstObserved) {
        std::this_thread::sleep_for(earlyCheck);
        noEarlyRetry = connectCount(details) == 1;
        waitUntilCount(details, 2, std::chrono::seconds(3), observed);
    }
    const bool secondObserved = observed.size() >= 2;
    const auto elapsed = secondObserved
        ? std::chrono::duration_cast<std::chrono::milliseconds>(observed[1] - observed[0])
        : std::chrono::milliseconds(0);
    const std::string lunaCalls = readFile(details);

    check(loop.post([&]() {
              battery.stop();
              loop.scheduleAfter(std::chrono::milliseconds(30), [&loop]() { loop.stop(); });
          }), "could not stop battery poller");
    loopThread.join();
    std::remove((dataRoot + "/remote-battery.json").c_str());
    std::remove(details.c_str());
    std::remove((systemRoot + "/etc/issue").c_str());
    rmdir((systemRoot + "/etc").c_str());
    rmdir(systemRoot.c_str());
    rmdir(dataRoot.c_str());
    rmdir(root.c_str());

    check(firstObserved, "first GATT connection was not observed; Luna calls: " + lunaCalls);
    check(noEarlyRetry,
          failDiscovery ? "failed battery read retried before the scaled retry interval"
                        : "successful battery read retried before the scaled hourly interval");
    check(secondObserved, "battery poller did not retry after its due interval");
    check(elapsed >= minimumInterval,
          failDiscovery ? "retry interval was shorter than its configured delay"
                        : "success interval was shorter than its configured delay");
}
} // namespace

int main() {
    try {
        const RemoteBatteryTimings production;
        check(production.check == std::chrono::minutes(10) &&
                  production.settle == std::chrono::seconds(3) &&
                  production.discover == std::chrono::seconds(2) &&
                  production.retry == std::chrono::minutes(5) &&
                  production.success == std::chrono::hours(1),
              "native remote battery production intervals match the Node poller");
        // The production values are one hour after success and five minutes
        // after failure. Scaled intervals keep the integration test fast while
        // exercising the same timer paths and the periodic-check interaction.
        runScenario(false, std::chrono::milliseconds(180),
                    std::chrono::milliseconds(320), std::chrono::milliseconds(220),
                    std::chrono::milliseconds(285));
        runScenario(true, std::chrono::milliseconds(180),
                    std::chrono::milliseconds(320), std::chrono::milliseconds(100),
                    std::chrono::milliseconds(155));
        std::cout << "native remote battery production and retry timing checks passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "native remote battery checks failed: " << error.what() << '\n';
        return 1;
    }
}
