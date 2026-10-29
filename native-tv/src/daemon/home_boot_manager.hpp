#pragma once

#include "core/json.hpp"
#include <string>

namespace nativetv {
namespace daemon {
class Logger;
class LunaClient;

// Applies the boot policy for opening Native Tv Home over the input selected
// by webOS. This runs as a short-lived, background boot-hook operation.
class HomeBootManager {
  public:
    HomeBootManager(LunaClient &luna, std::string dataRoot,
                    std::string systemRoot, Logger *logger = nullptr);

    Json openAtBoot();
    Json showStartingNotice();

  private:
    LunaClient &luna_;
    std::string dataRoot_;
    std::string systemRoot_;
    Logger *logger_;
};
} // namespace daemon
} // namespace nativetv
