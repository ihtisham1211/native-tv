#pragma once

#include "core/json.hpp"
#include <string>

namespace nativetv {
namespace daemon {

// Stages the hidden webOS Home app that is temporarily mounted over LG Home.
// Mount and SAM lifecycle ownership remains transitional while the native TV
// entry point is migrated.
class HomeTakeoverStager {
  public:
    HomeTakeoverStager(std::string sourceRoot, std::string stagingRoot, int port);

    Json stage() const;

  private:
    std::string sourceRoot_;
    std::string stagingRoot_;
    int port_;
};

} // namespace daemon
} // namespace nativetv
