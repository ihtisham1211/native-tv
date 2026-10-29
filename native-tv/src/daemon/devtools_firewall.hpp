#pragma once

#include "core/json.hpp"
#include <mutex>
#include <string>

namespace nativetv {
namespace daemon {
class ChildProcessRunner;
class Logger;

// Owns the WebAppMgr inspector firewall rule that limits TCP/9998 to loopback.
class DevtoolsFirewall {
  public:
    DevtoolsFirewall(ChildProcessRunner &children, std::string systemRoot,
                     std::string procRoot, Logger *logger = nullptr);

    Json setNetworkAllowed(bool allowed);
    std::string status() const;

  private:
    ChildProcessRunner &children_;
    std::string systemRoot_;
    std::string procRoot_;
    Logger *logger_;
    mutable std::mutex mutex_;
    std::string status_ = "pending";
};
} // namespace daemon
} // namespace nativetv
