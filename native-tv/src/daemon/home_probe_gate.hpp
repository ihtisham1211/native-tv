#pragma once

#include "core/json.hpp"
#include <chrono>
#include <mutex>
#include <string>
#include <vector>

namespace nativetv {
namespace daemon {
class ChildProcessRunner;

// How the gate runs `nativetv-tv --probe`. The defaults suit a TV; config.json's
// "nativeClient" section ({path, probeCommand, probeTimeoutMs}) overrides them
// and a test points `path` at a script.
struct HomeProbeOptions {
    // argv of the probe, flag included. Empty: `clientPath --probe`.
    std::vector<std::string> command;
    // Added to the daemon's own environment.
    std::vector<std::string> environment;
    std::string clientPath;
    // What else a verdict depends on besides the client file (the firmware).
    std::string identity;
    // Where a verdict that cannot change by itself is kept across restarts, so
    // the probe's window is not opened at every boot. "" keeps it in memory only.
    std::string cachePath;
    std::chrono::milliseconds timeout{15000};
    // Exit 10 and 11 (no display, no window yet) are tried again: the compositor
    // may still be starting.
    int attempts = 3;
    std::chrono::milliseconds retryGap{2000};
};

HomeProbeOptions homeProbeOptions(const Json &config, const std::string &systemRoot,
                                  const std::string &dataRoot);

// Runs the native client's probe and says whether Native Tv Home may take LG's
// Home's place: the stub that replaces it has no page to fall back to.
//
// A verdict is {ok, exit, class, cached, report?, error?}. `class` names the
// exit: ok, display (10), context (11), gl (12), readback (13), failed (any
// other exit), crashed (a signal), timeout, missing (no client file).
class HomeProbeGate {
  public:
    HomeProbeGate(ChildProcessRunner &children, HomeProbeOptions options);

    // Runs the probe unless this client on this firmware already has a verdict
    // that a rerun would not change (ok, gl, readback); `force` always runs it.
    // Blocking.
    Json verdict(bool force);
    // The last verdict while it still holds for the client file as it is now,
    // never running the probe; invalid before there is one, and after the file
    // has been replaced.
    Json known() const;

  private:
    Json run();
    std::string clientKey() const;

    ChildProcessRunner &children_;
    const HomeProbeOptions options_;
    // verdict() holds runMutex_ for its whole run, so known() is never kept
    // waiting on a probe by mutex_, which only guards the two fields.
    std::mutex runMutex_;
    mutable std::mutex mutex_;
    Json last_;
    std::string lastKey_;
};

// `supported` as service::homeStatusSupported answers it, with the verdict
// applied: unchanged when the verdict is ok or absent, else not ok, with the
// reason and the verdict under `probe`.
Json homeSupportedWithProbe(const Json &supported, const Json &verdict);
} // namespace daemon
} // namespace nativetv
