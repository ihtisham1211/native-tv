#pragma once

#include "core/json.hpp"
#include <functional>
#include <mutex>
#include <string>

namespace nativetv {
namespace service {

// The newest published release, as the release manifest that scripts/build-ipk.py
// writes and the Homebrew Channel workflow attaches describes it.
struct ReleaseOffer {
    std::string version;    // "0.119.0"
    std::string sha256;     // of the .ipk, lower case
    std::string releaseUrl; // the release's page
    std::string ipkUrl;     // the .ipk on that page
};

// "0.118.0+55e51e5.dirty" is "0.118.0": a server built from a clone says which
// commit after its version.
std::string releaseVersionOf(const std::string &serverVersion);
// Whether x.y.z `candidate` is newer than `current`. False when either is not x.y.z.
bool releaseIsNewer(const std::string &candidate, const std::string &current);
// The manifest of the latest release. The links are built from the validated
// version, never taken from the manifest.
bool parseLatestRelease(const std::string &manifestBody, ReleaseOffer &offer,
                        std::string &error);

// GET one small file; false with `error` set when it could not be had.
using ReleaseFetch = std::function<bool(const std::string &url, std::string &body,
                                        std::string &error)>;
// libcurl, as installs from a URL use; gives up after 20 seconds.
ReleaseFetch fetchOverCurl(const std::string &userAgent,
                           std::function<std::string()> ipPreference);

// Asks GitHub whether a newer release exists. It only reports: installing
// stays with the Homebrew Channel, since the installer refuses Native Tv's own
// package.
class ReleaseChecker {
  public:
    using Seconds = std::function<long long()>; // monotonic, so a clock step cannot age a check

    ReleaseChecker(std::string serverVersion, ReleaseFetch fetch, Seconds now);

    // Records the answer or the reason there is none. A check already running
    // is left to finish. False when this check failed.
    bool check(std::string &error);
    // Never checked, or the last answer is a day old, or the last try failed
    // an hour ago.
    bool dailyCheckDue() const;
    Json status() const;

  private:
    const std::string serverVersion_;
    const ReleaseFetch fetch_;
    const Seconds now_;
    mutable std::mutex mutex_;
    bool checking_ = false;
    bool everChecked_ = false;
    long long checkedAt_ = 0;
    std::string error_;
    bool haveOffer_ = false;
    ReleaseOffer offer_;
};

} // namespace service
} // namespace nativetv
