#pragma once

#include "core/json.hpp"
#include "service/release_check.hpp"
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace nativetv {
namespace service {

// What an update does to a TV, one blocking step after another; a step that
// returns false has set `error`, and the update stops there. They are given
// to ReleaseUpdate so the order, and what each failure leaves behind, can be
// tested without a TV or GitHub (the daemon's own are release_update_steps).
struct ReleaseUpdateSteps {
    // Why the update cannot be installed from here, or empty when it can.
    std::function<std::string()> unavailable;
    // An update the app's installer put back, which the server it restored
    // starts to find: the version it was for and why it did not hold.
    std::function<bool(std::string &version, std::string &error)> lastFailure;
    // Release `version`'s own manifest, read and checked.
    std::function<bool(const std::string &version, ReleaseOffer &offer, std::string &error)> readManifest;
    // The package, to a file of the daemon's own; `progress` takes the bytes so
    // far, and `stopping` turns true when this server is shutting down.
    std::function<bool(const ReleaseOffer &offer, const std::function<void(uint64_t)> &progress,
                       const std::function<bool()> &stopping, std::string &error)> download;
    // The file's SHA-256 is the manifest's.
    std::function<bool(const ReleaseOffer &offer, std::string &error)> verify;
    // The TV's app installer replaces the app with the package.
    std::function<bool(const ReleaseOffer &offer, std::string &error)> installPackage;
    // The app's own installer moves the server over and restarts it. It does
    // not return when it works, as the restart ends the server that runs it:
    // true is an installer that moved the files but left this server running.
    // On `stopping` the step stops waiting for the installer, which carries on.
    std::function<bool(const ReleaseOffer &offer, const std::function<bool()> &stopping,
                       std::string &error)> applyPackage;
    // The downloaded file is removed, whatever came of the update.
    std::function<void()> discard;
};

// Installs a release from a worker thread: one update at a time, its
// progress in status().
class ReleaseUpdate {
  public:
    explicit ReleaseUpdate(ReleaseUpdateSteps steps);
    ~ReleaseUpdate();
    ReleaseUpdate(const ReleaseUpdate &) = delete;
    ReleaseUpdate &operator=(const ReleaseUpdate &) = delete;

    // Starts installing `checked`, the newer release a check found. False,
    // with `error`, when an update is already working or this install cannot
    // take one.
    bool start(const ReleaseOffer &checked, std::string &error);
    // {state, version, receivedBytes, error, unavailable}; state is idle,
    // downloading, verifying, installing, restarting or failed.
    Json status() const;
    // Downloading, verifying, installing or restarting: nothing else that
    // installs or restarts the server may run.
    bool working() const;
    // Tests: wait until the worker has finished.
    void waitIdle();

  private:
    void run(ReleaseOffer checked);

    const ReleaseUpdateSteps steps_;
    mutable std::mutex mutex_;
    std::thread worker_;
    std::string state_ = "idle";
    std::string version_;
    std::string error_;
    std::atomic<uint64_t> receivedBytes_{0};
    std::atomic<bool> stopping_{false};
};

} // namespace service
} // namespace nativetv
