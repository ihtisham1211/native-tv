#pragma once

#include "service/background_services.hpp"
#include <chrono>
#include <functional>
#include <string>
#include <vector>

namespace nativetv {
namespace daemon {

struct ServicesKeeperDeps {
    std::string systemRoot = "/";
    std::string dataRoot;
    std::string procRoot = "/proc";
    // Runs a program to its end; true when it exited 0.
    std::function<bool(const std::string &program, const std::vector<std::string> &arguments)> run;
    // Binds the stub over the service's programs (and its jail's copy).
    std::function<bool(const service::BackgroundServiceInfo &info, const std::string &id)> bindStubs;
    std::function<void(int pid)> terminate;
    // Calls the task after the delay, on whatever thread the daemon runs timers on.
    std::function<void(std::chrono::milliseconds, std::function<void()>)> later;
    std::function<void(const std::string &message)> log;
};

// What legacy lib/services.js does besides the toggle route: at start it
// retires old ids, drops the old init script, rewrites the boot hook and binds
// the stubs of the services switched off, and every few minutes it stops one
// that came back (the service hub launches some on demand, outside their unit).
class ServicesKeeper {
  public:
    explicit ServicesKeeper(ServicesKeeperDeps deps);

    // services.init: returns the list of services switched off.
    std::vector<std::string> start();
    // services.enforce: returns how many switched-off services were found running.
    size_t enforce();

  private:
    bool infoFor(const std::string &id, service::BackgroundServiceInfo &info) const;

    ServicesKeeperDeps deps_;
};

} // namespace daemon
} // namespace nativetv
