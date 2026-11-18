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
    // Whether the table sinkholes exactly this name.
    bool tableBlocks(const std::string &host) const {
        return readFile(data + "/adblock_hosts").find("0.0.0.0\t" + host + "\n") != std::string::npos;
    }
    bool tableBlocksClockHost() const { return tableBlocks("nextlgsdp.com"); }
    // The table is rewritten just after the grace period is marked closed.
    bool blocksSoon(const std::string &host) const {
        for (int waited = 0; waited < 3000 && !tableBlocks(host); waited += 50)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        return tableBlocks(host);
    }
    bool blocksClockHostSoon() const { return blocksSoon("nextlgsdp.com"); }
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
    check(rig.manager->sdpGraceActive() && !rig.tableBlocksClockHost() &&
              !rig.tableBlocks("tv.wiselg.com") && !rig.tableBlocks("us.tv.wiselg.com"),
          "the grace period is open at boot, with the clock hosts reachable (tv.wiselg.com "
          "too, which webOS 11 sets its clock from)");
    std::this_thread::sleep_for(std::chrono::milliseconds(700));
    check(rig.manager->sdpGraceActive(), "it stays open while the time is invalid");
    rig.setTime("{\"returnValue\":true,\"timeValid\":true,\"systemTimeSource\":\"sdp\"}");
    check(rig.closesWithin(7000), "it closes once SDP has set a valid time");
    check(rig.blocksClockHostSoon(), "nextlgsdp.com is blocked once synced");
    check(rig.tableBlocks("tv.wiselg.com") && rig.tableBlocks("ngfts.tv.wiselg.com"),
          "tv.wiselg.com is blocked with it");
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

// The ads tier blocks one name under the clock servers' domain, the report upload sdx
// hands out, and so waits for the clock too, but never blocks the clock hosts themselves.
void testAdsTierWaitsForItsReportHostOnly() {
    Rig rig;
    writeFile(rig.data + "/adblock_enabled", "ads");
    check(rig.manager->start(), "the manager starts");
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    check(rig.manager->sdpGraceActive() && !rig.tableBlocks("rdx2.nextlgsdp.com") &&
              rig.tableBlocks("ad.lgsmartad.com"),
          "the ads tier is up at once, less its report host while the clock is not valid");
    rig.setTime("{\"returnValue\":true,\"timeValid\":true,\"systemTimeSource\":\"sdp\"}");
    check(rig.closesWithin(7000), "it closes once SDP has set a valid time");
    check(rig.blocksSoon("rdx2.nextlgsdp.com") && !rig.tableBlocksClockHost() &&
              !rig.tableBlocks("us.nextlgsdp.com") && !rig.tableBlocks("tv.wiselg.com"),
          "the report host is blocked once synced, and the clock hosts never are");
}

// AirPlay's key check goes through the country's clock server: while the table blocks it a
// folder is kept in /tmp/mfi_key_requested so the TV's daemon skips the check.
void testAirplayKeyCheckFollowsTheClockServerBlock() {
    Rig rig;
    shell("mkdir -p '" + rig.system + "/tmp'");
    writeFile(rig.data + "/country", "gb");
    const std::string directory = rig.system + "/tmp/mfi_key_requested";
    const std::string folder = directory + "/nativetv";
    const auto exists = [](const std::string &path) { return access(path.c_str(), F_OK) == 0; };
    check(rig.manager->start(), "the manager starts");
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    check(rig.manager->sdpGraceActive() && !exists(folder),
          "nothing is held while the clock server is still reachable");
    rig.setTime("{\"returnValue\":true,\"timeValid\":true,\"systemTimeSource\":\"sdp\"}");
    check(rig.closesWithin(7000) && rig.blocksClockHostSoon(),
          "the clock servers are blocked once synced");
    for (int waited = 0; waited < 3000 && !exists(folder); waited += 50)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    check(exists(folder), "AirPlay's key check is skipped while gb.nextlgsdp.com is blocked");
    check(rig.manager->setMode("ads")["ok"].boolean(), "the tier is switched to Ads & telemetry");
    check(!exists(folder) && !exists(directory),
          "the folder goes, and its directory, with the clock server back in reach");
    check(rig.manager->setMode("full")["ok"].boolean() && exists(folder),
          "and comes back with the Everything tier");
    check(rig.manager->setMode("off")["ok"].boolean() && !exists(folder),
          "and goes again with the blocker switched off");
    // A file the TV left there is replaced by the directory, and one holding anything else is kept.
    check(rig.manager->setMode("ads")["ok"].boolean() && !exists(directory), "back to nothing held");
    writeFile(directory, "");
    check(rig.manager->setMode("full")["ok"].boolean() && exists(folder),
          "the TV's own marker, a file, makes way for the directory");
    shell("mkdir '" + directory + "/other'");
    check(rig.manager->setMode("ads")["ok"].boolean() && !exists(folder) && exists(directory + "/other"),
          "a directory with something else in it is left in place");
}
} // namespace

int main() {
    try {
        testRestartWithValidTime();
        testColdBootWaitsForSdp();
        testWebOs4OmitsTimeValid();
        testAdsTierWaitsForItsReportHostOnly();
        testAirplayKeyCheckFollowsTheClockServerBlock();
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "ad-block manager test failed: %s\n", error.what());
        return 1;
    }
}
