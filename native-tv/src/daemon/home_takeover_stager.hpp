#pragma once

#include "core/json.hpp"
#include <string>

namespace nativetv {
namespace daemon {

// Stages Native Tv Home as the app the bind mount puts in LG's Home's place:
// the native client's files (nativetv-tv, its libraries and assets, copied from
// the dashboard app's payload), a start script and a native manifest under LG's
// Home's id. sam then starts the client itself as Home.
class HomeTakeoverStager {
  public:
    HomeTakeoverStager(std::string sourceRoot, std::string stagingRoot);

    // {ok, changed} or {ok: false, error}. Files are replaced by rename, so a
    // client running from the staged copy keeps the files it started with.
    Json stage() const;

  private:
    std::string sourceRoot_;
    std::string stagingRoot_;
};

// The files of the staged app that are not the client's own.
constexpr const char *kHomeStartScript = "nativetv-home";
constexpr const char *kHomeClientExecutable = "nativetv-tv";

} // namespace daemon
} // namespace nativetv
