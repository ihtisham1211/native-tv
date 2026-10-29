#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace nativetv {
namespace service {

using AppPackageDownloadSink = std::function<bool(const char *, size_t)>;
using AppPackageDownloadCancelled = std::function<bool()>;
struct AppPackageDownloadFailure {
    std::string message;
    bool offline = false;
    bool timedOut = false;
};
using AppPackageDownload = std::function<bool(
    const std::string &, const std::string &, const std::string &, uint64_t,
    const AppPackageDownloadSink &, long &, AppPackageDownloadFailure &,
    const AppPackageDownloadCancelled &)>;

class AppPackageFetcher {
  public:
    static bool download(const std::string &url, const std::string &userAgent,
                         const std::string &ipPreference, uint64_t maximumBytes,
                         const AppPackageDownloadSink &sink,
                         long &httpStatus, AppPackageDownloadFailure &failure,
                         const AppPackageDownloadCancelled &cancelled = {});
    static bool validUrl(const std::string &url, std::string &error);
};

} // namespace service
} // namespace nativetv
