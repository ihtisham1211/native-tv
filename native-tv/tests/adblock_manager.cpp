// test/test-privacy.js, the SDP grace period: nextlgsdp.com, the TV's clock host, is left
// reachable until the clock is valid, so the TV can set its time from it. The TV is a
// luna-send stand-in that answers getSystemTime from a file the test rewrites; the hosts
// table is only copied, never mounted. (The two-minute wait for a time from another source
// is a 120 s timer, and is not run here.)
#include "daemon/adblock_manager.hpp"
#include "daemon/child_process.hpp"
#include "daemon/event_loop.hpp"
#include "daemon/luna.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

using namespace nativetv;
using namespace nativetv::daemon;

namespace {
void check(bool value, const std::string &message) {
    if (!value) throw std::runtime_error(message);
}

std::string readFile(const std::string &path) {
    std::ifstream input(path.c_str(), std::ios::in | std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
}

void writeFile(const std::string &path, const std::string &text, bool executable = false) {
    {
        std::ofstream output(path.c_str(), std::ios::out | std::ios::trunc | std::ios::binary);
        output << text;
    }
    if (executable) chmod(path.c_str(), 0755);
}

void shell(const std::string &command) {
    const int status = system(command.c_str());
    (void)status;
}

ChildRunnerOptions fastStarts() {
    ChildRunnerOptions options;
    options.startupGap = std::chrono::milliseconds(0);
    options.regularGap = std::chrono::milliseconds(0);
    options.startupWindow = std::chrono::milliseconds(0);
    return options;
}

service::AdBlockMountOperations copyingMounts() {
    service::AdBlockMountOperations mounts;
    mounts.bind = [](const std::string &source, const std::string &target) {
        writeFile(target, readFile(source));
        return true;
    };
    mounts.unmount = [](const std::string &) { return true; };
    return mounts;
}

struct Rig {
    std::string directory, data, system, timeFile;
    EventLoop loop;
    ChildProcessRunner children;
    std::unique_ptr<LunaClient> luna;
    std::unique_ptr<AdBlockManager> manager;
    std::thread loopThread;

    Rig() : loop(4, 16, 32), children(fastStarts()) {
        char pattern[] = "/tmp/nativetv-adblock-XXXXXX";
        check(mkdtemp(pattern) != nullptr, "temporary directory");
        directory = pattern;
        data = directory + "/data";
        system = directory + "/system";
        timeFile = directory + "/time.json";
        shell("mkdir -p '" + data + "' '" + system + "/etc' '" + system + "/var/luna/preferences'");
        writeFile(data + "/adblock_enabled", "full");
        writeFile(system + "/etc/hosts", "127.0.0.1\tlocalhost\n");
        // getSystemTime from the file; every other call is simply answered.
        writeFile(directory + "/luna-send",
                  "#!/bin/sh\nfor a in \"$@\"; do case \"$a\" in *getSystemTime) cat '" + timeFile +
                      "'; exit 0;; esac; done\necho '{\"returnValue\":true}'\n", true);
        setTime("{\"returnValue\":true,\"timeValid\":false,\"systemTimeSource\":\"system\"}");
        LunaClientOptions options;
        options.executable = directory + "/luna-send";
        options.timeout = std::chrono::milliseconds(8000);
        luna.reset(new LunaClient(loop, children, nullptr, options));
        manager.reset(new AdBlockManager(loop, *luna, data, system, nullptr, copyingMounts()));
        loopThread = std::thread([this]() { loop.run(); });
    }
    ~Rig() {
        manager->stop();
        loop.stop();
        loopThread.join();
        shell("rm -rf '" + directory + "'");
    }
    void setTime(const std::string &reply) { writeFile(timeFile, reply + "\n"); }
    bool tableBlocksClockHost() const {
        return readFile(data + "/adblock_hosts").find("nextlgsdp.com") != std::string::npos;
    }
    // The table is rewritten just after the grace period is marked closed.
    bool blocksClockHostSoon() const {
        for (int waited = 0; waited < 3000 && !tableBlocksClockHost(); waited += 50)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        return tableBlocksClockHost();
    }
    // True once the grace period has closed, within the time given.
    bool closesWithin(int milliseconds) const {
        for (int waited = 0; waited < milliseconds; waited += 50) {
            if (!manager->sdpGraceActive()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        return !manager->sdpGraceActive();
    }
};

void testRestartWithValidTime() {
    Rig rig;
    rig.setTime("{\"returnValue\":true,\"timeValid\":true,\"systemTimeSource\":\"sdp\"}");
    check(rig.manager->start(), "the manager starts");
    check(rig.closesWithin(3000), "a clock already valid skips the grace period");
    check(rig.blocksClockHostSoon(), "nextlgsdp.com is blocked once the window is skipped");
}

void testColdBootWaitsForSdp() {
    Rig rig;
    check(rig.manager->start(), "the manager starts");
    check(rig.manager->sdpGraceActive() && !rig.tableBlocksClockHost(),
          "the grace period is open at boot, with the clock host reachable");
    std::this_thread::sleep_for(std::chrono::milliseconds(700));
    check(rig.manager->sdpGraceActive(), "it stays open while the time is invalid");
    rig.setTime("{\"returnValue\":true,\"timeValid\":true,\"systemTimeSource\":\"sdp\"}");
    check(rig.closesWithin(7000), "it closes once SDP has set a valid time");
    check(rig.blocksClockHostSoon(), "nextlgsdp.com is blocked once synced");
}

void testWebOs4OmitsTimeValid() {
    Rig rig;
    rig.setTime("{\"returnValue\":true,\"utc\":1000,\"systemTimeSource\":\"system\"}");
    check(rig.manager->start(), "the manager starts");
    std::this_thread::sleep_for(std::chrono::milliseconds(700));
    check(rig.manager->sdpGraceActive(), "an epoch utc is not a valid time");
    rig.setTime("{\"returnValue\":true,\"utc\":1791222699,\"systemTimeSource\":\"sdp\"}");
    check(rig.closesWithin(7000), "a current utc from SDP closes the grace period");
}

void testAdsTierDoesNotWait() {
    Rig rig;
    writeFile(rig.data + "/adblock_enabled", "ads");
    check(rig.manager->start(), "the manager starts");
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    check(!rig.tableBlocksClockHost(), "the ads tier never blocks the clock host");
}
} // namespace

int main() {
    try {
        testRestartWithValidTime();
        testColdBootWaitsForSdp();
        testWebOs4OmitsTimeValid();
        testAdsTierDoesNotWait();
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "ad-block manager test failed: %s\n", error.what());
        return 1;
    }
}
