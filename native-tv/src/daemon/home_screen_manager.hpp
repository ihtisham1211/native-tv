#pragma once

#include "core/json.hpp"
#include <mutex>
#include <string>

namespace nativetv {
namespace daemon {
class ChildProcessRunner;

// Owns LG Home layout files, process management, and the webOS 10 overlay
// mount used by the Home takeover lifecycle.
class HomeScreenManager {
  public:
    HomeScreenManager(ChildProcessRunner &children, std::string systemRoot,
                      std::string dataRoot, std::string procRoot,
                      bool allowExperimental);

    bool isWebos9() const;
    bool supportsTakeover(bool allowExperimentalHome) const;
    Json state() const;
    Json setPart(const std::string &id, bool shown);
    Json startLayout(bool blank);
    Json restore();
    Json setAllowed(bool allowed);
    Json reapply();
    Json setAddedInputs(const Json &inputs);
    bool stopHome(bool &stopped, std::string *error = nullptr);
    bool restartHome(std::string *error = nullptr);

  private:
    ChildProcessRunner &children_;
    std::string systemRoot_;
    std::string dataRoot_;
    std::string procRoot_;
    bool allowExperimental_;
    mutable std::mutex mutex_;
};
} // namespace daemon
} // namespace nativetv
