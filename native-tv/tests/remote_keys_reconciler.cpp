#include "daemon/child_process.hpp"
#include "daemon/event_loop.hpp"
#include "daemon/luna.hpp"
#include "daemon/remote_keys_reconciler.hpp"
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

using namespace nativetv::daemon;

namespace {
void check(bool condition, const std::string &message) {
    if (!condition) throw std::runtime_error(message);
}

ChildRunnerOptions fastStarts() {
    ChildRunnerOptions options;
    options.startupGap = std::chrono::milliseconds(0);
    options.regularGap = std::chrono::milliseconds(0);
    options.startupWindow = std::chrono::milliseconds(0);
    return options;
}
} // namespace

int main() {
    try {
        check(setenv("NATIVETV_FAKE_LUNA_MODE",
                     "native-remote-keys-reconciler", 1) == 0,
              "could not select the fake Luna mode");
        EventLoop loop(4, 32, 64);
        ChildProcessRunner children(fastStarts());
        LunaClientOptions lunaOptions;
        lunaOptions.executable = NATIVETV_FAKE_LUNA_SEND;
        LunaClient luna(loop, children, nullptr, lunaOptions);

        std::mutex mutex;
        std::condition_variable changed;
        unsigned calls = 0;
        RemoteKeysReconciler::Options options;
        options.startupDelayMilliseconds = 260;
        options.settingsDebounceMilliseconds = 60;
        options.periodicMilliseconds = 5000;
        options.powerRechecksMilliseconds = {100, 180};
        RemoteKeysReconciler reconciler(
            loop, luna,
            [&mutex, &changed, &calls](bool &wrote, std::string &) {
                wrote = false;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    ++calls;
                }
                changed.notify_all();
                return true;
            }, nullptr, options);

        check(loop.post([&reconciler]() {
                  if (!reconciler.start())
                      throw std::runtime_error("could not start native key reconciliation");
                  reconciler.powerChanged();
              }), "could not queue reconciliation startup");
        std::thread loopThread([&loop]() { loop.run(); });
        bool observedAllTriggers = false;
        {
            std::unique_lock<std::mutex> lock(mutex);
            observedAllTriggers = changed.wait_for(
                lock, std::chrono::seconds(3), [&calls]() { return calls >= 4; });
        }
        const bool shutdownPosted = loop.post([&loop, &reconciler]() {
            reconciler.stop();
            loop.stop();
        });
        if (!shutdownPosted) loop.stop();
        loopThread.join();
        check(shutdownPosted, "could not queue reconciliation shutdown");
        check(observedAllTriggers,
              "settings changes, delayed power checks and startup did not reconcile");
        std::cout << "native remote-key reconciliation lifecycle passed\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
