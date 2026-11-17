#include "service/release_update.hpp"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace nativetv;
using namespace nativetv::service;

namespace {
unsigned checks = 0;

void check(bool value, const char *message) {
    ++checks;
    if (!value)
        throw std::runtime_error(message);
}

ReleaseOffer offerOf(const std::string &version, char sha = 'a') {
    ReleaseOffer offer;
    offer.version = version;
    offer.sha256 = std::string(64, sha);
    return offer;
}

// Steps that all work and say which ran; `failAt` names the one that fails.
struct Script {
    std::vector<std::string> ran;
    std::string failAt;
    std::string manifestSha = std::string(64, 'a');
    std::string unavailable;
    std::string lastFailureVersion;

    ReleaseUpdateSteps steps() {
        ReleaseUpdateSteps steps;
        const auto step = [this](const std::string &name, std::string &error) {
            ran.push_back(name);
            if (failAt != name) return true;
            error = name + " failed";
            return false;
        };
        steps.unavailable = [this]() { return unavailable; };
        steps.lastFailure = [this](std::string &version, std::string &error) {
            version = lastFailureVersion;
            error = "the new server did not start";
            return !lastFailureVersion.empty();
        };
        steps.readManifest = [this, step](const std::string &version, ReleaseOffer &offer, std::string &error) {
            offer = offerOf(version);
            offer.sha256 = manifestSha;
            return step("manifest", error);
        };
        steps.download = [step](const ReleaseOffer &, const std::function<void(uint64_t)> &progress, const std::function<bool()> &,
                                std::string &error) {
            progress(1234);
            return step("download", error);
        };
        steps.verify = [step](const ReleaseOffer &, std::string &error) { return step("verify", error); };
        steps.installPackage = [step](const ReleaseOffer &, std::string &error) { return step("install", error); };
        steps.applyPackage = [step](const ReleaseOffer &, const std::function<bool()> &, std::string &error) {
            return step("apply", error);
        };
        steps.discard = [this]() { ran.push_back("discard"); };
        return steps;
    }
};

std::string joined(const std::vector<std::string> &names) {
    std::string text;
    for (const std::string &name : names) text += (text.empty() ? "" : " ") + name;
    return text;
}

// Runs one update to its end and reports what ran and where it stopped.
Json run(Script &script, std::string &ran) {
    ReleaseUpdate update(script.steps());
    std::string error;
    check(update.start(offerOf("0.125.0"), error), "an update starts");
    update.waitIdle();
    ran = joined(script.ran);
    return update.status();
}
} // namespace

int main() {
    try {
        std::string ran;

        // An installer that works restarts the server, so it never returns; one that
        // does return, here with the files moved, is a server that did not restart.
        Script working;
        Json status = run(working, ran);
        check(ran == "manifest download verify install discard apply discard", "the steps run in order, the file going before the restart");
        check(status["state"].string() == "failed" && status["version"].string() == "0.125.0" &&
                  static_cast<int>(status["receivedBytes"].number()) == 1234 &&
                  status["error"].string().find("did not restart") != std::string::npos,
              "an installer that returned is reported, with the version and progress");

        // A failure at each step stops there: nothing after it runs, and the file is removed.
        const struct { const char *failAt; const char *expectedRan; } failures[] = {
            {"manifest", "manifest discard"},
            {"download", "manifest download discard"},
            {"verify", "manifest download verify discard"},
            {"install", "manifest download verify install discard"},
            {"apply", "manifest download verify install discard apply discard"},
        };
        for (const auto &failure : failures) {
            Script script;
            script.failAt = failure.failAt;
            status = run(script, ran);
            check(ran == failure.expectedRan, failure.failAt);
            check(status["state"].string() == "failed" &&
                      status["error"].string() == std::string(failure.failAt) + " failed",
                  "a failed step is shown with its reason");
        }

        // A package that is not the one the check showed is never installed.
        Script changed;
        changed.manifestSha = std::string(64, 'b');
        status = run(changed, ran);
        check(ran == "manifest discard" && status["state"].string() == "failed" &&
                  status["error"].string().find("manifest changed") != std::string::npos,
              "a manifest that differs from the checked one stops the update before the download");

        // An install that cannot take it refuses to start, and runs nothing.
        Script unavailable;
        unavailable.unavailable = "installed with deploy.sh";
        ReleaseUpdate refused(unavailable.steps());
        std::string error;
        check(!refused.start(offerOf("0.125.0"), error) && error == "installed with deploy.sh" &&
                  unavailable.ran.empty() && refused.status()["unavailable"].string() == "installed with deploy.sh" &&
                  refused.status()["state"].string() == "idle",
              "an install that is not the package's is refused, naming why");

        // One update at a time; a failed one can be tried again.
        std::mutex lock;
        std::condition_variable wake;
        bool release = false;
        Script slow;
        ReleaseUpdateSteps blocking = slow.steps();
        blocking.download = [&](const ReleaseOffer &, const std::function<void(uint64_t)> &,
                                const std::function<bool()> &, std::string &) {
            std::unique_lock<std::mutex> hold(lock);
            wake.wait(hold, [&]() { return release; });
            return true;
        };
        ReleaseUpdate busy(blocking);
        check(busy.start(offerOf("0.125.0"), error) && busy.working() &&
                  busy.status()["state"].string() == "downloading", "an update is working");
        check(!busy.start(offerOf("0.125.0"), error) && error == "an update is already running",
              "a second update is refused while one works");
        {
            std::lock_guard<std::mutex> hold(lock);
            release = true;
        }
        wake.notify_all();
        busy.waitIdle();
        check(!busy.working(), "the first one finished");

        // The installer is the last step, and the one the server is restarted in.
        bool applying = false, applied = false;
        Script last;
        ReleaseUpdateSteps restarting = last.steps();
        restarting.applyPackage = [&](const ReleaseOffer &, const std::function<bool()> &, std::string &) {
            std::unique_lock<std::mutex> hold(lock);
            applying = true;
            wake.notify_all();
            wake.wait(hold, [&]() { return applied; });
            return false;
        };
        ReleaseUpdate installing(restarting);
        check(installing.start(offerOf("0.125.0"), error), "start");
        {
            std::unique_lock<std::mutex> hold(lock);
            wake.wait(hold, [&]() { return applying; });
        }
        check(installing.working() && installing.status()["state"].string() == "restarting",
              "while the app's installer runs the update is restarting, and nothing else may restart the server");
        {
            std::lock_guard<std::mutex> hold(lock);
            applied = true;
        }
        wake.notify_all();
        installing.waitIdle();
        check(!installing.working(), "an installer that failed leaves the server free again");

        // A server that shuts down while the installer runs does not wait for it: the
        // installer restarts the server, so the wait would be for its own end.
        {
            Script script;
            ReleaseUpdateSteps waiting = script.steps();
            std::atomic<bool> waited(false);
            waiting.applyPackage = [&](const ReleaseOffer &, const std::function<bool()> &stopping, std::string &) {
                waited = true;
                while (!stopping()) std::this_thread::sleep_for(std::chrono::milliseconds(2));
                return false;
            };
            ReleaseUpdate shuttingDown(waiting);
            check(shuttingDown.start(offerOf("0.125.0"), error), "start");
            while (!waited) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        } // the destructor returns only because the step saw `stopping`
        check(true, "a shutdown does not wait for the installer");

        Script retried;
        retried.failAt = "download";
        ReleaseUpdate again(retried.steps());
        check(again.start(offerOf("0.125.0"), error), "start");
        again.waitIdle();
        check(again.status()["state"].string() == "failed", "the first try failed");
        retried.failAt.clear();
        check(again.start(offerOf("0.125.0"), error), "a failed update can be tried again");
        again.waitIdle();
        check(again.status()["error"].string().find("did not restart") != std::string::npos,
              "the second try clears the first one's error and goes on to the end");

        // A server that was put back after an update did not hold tells why.
        Script restored;
        restored.lastFailureVersion = "0.125.0";
        Json found = ReleaseUpdate(restored.steps()).status();
        check(found["state"].string() == "failed" && found["version"].string() == "0.125.0" &&
                  found["error"].string() == "the new server did not start",
              "an update the app's installer rolled back is reported after the restart");

        std::printf("%u release update checks passed\n", checks);
        return 0;
    } catch (const std::exception &failure) {
        std::fprintf(stderr, "%s\n", failure.what());
        return 1;
    }
}
