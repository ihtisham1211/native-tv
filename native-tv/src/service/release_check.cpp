#include "service/release_check.hpp"
#include "service/app_package_fetcher.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <utility>

namespace nativetv {
namespace service {
namespace {

const char *const kRepository = "https://github.com/ihtisham1211/native-tv";
// scripts/build-ipk.py's APP_ID; a published release is what "latest" means,
// a pre-release never is.
const char *const kAppId = "io.github.ihtisham1211.nativetv";
const size_t kManifestBytes = 64 * 1024;

// The releases come from GitHub over HTTPS. Tests point them at a server on
// the same computer through TVWEB_RELEASE_BASE; any other value, which a TV
// never has, is ignored, so the variable cannot send an update elsewhere.
std::string repository() {
    const char *override = std::getenv("TVWEB_RELEASE_BASE");
    const std::string base = override ? override : "";
    if (base.compare(0, 17, "http://127.0.0.1:") == 0 || base.compare(0, 18, "https://127.0.0.1:") == 0)
        return base;
    return kRepository;
}

// Digits only, at most nine each: a longer number is not a release this
// server could have been built from.
bool splitVersion(const std::string &version, unsigned long parts[3]) {
    size_t index = 0;
    for (int part = 0; part < 3; ++part) {
        const size_t begin = index;
        while (index < version.size() && std::isdigit(static_cast<unsigned char>(version[index])))
            ++index;
        if (index == begin || index - begin > 9) return false;
        parts[part] = std::strtoul(version.substr(begin, index - begin).c_str(), nullptr, 10);
        if (part < 2) {
            if (index >= version.size() || version[index] != '.') return false;
            ++index;
        }
    }
    return index == version.size();
}

bool isSha256(const std::string &value) {
    return value.size() == 64 &&
           std::all_of(value.begin(), value.end(), [](unsigned char character) {
               return std::isxdigit(character) != 0;
           });
}

} // namespace

std::string releaseVersionOf(const std::string &serverVersion) {
    return serverVersion.substr(0, serverVersion.find('+'));
}

bool releaseIsNewer(const std::string &candidate, const std::string &current) {
    unsigned long a[3], b[3];
    if (!splitVersion(candidate, a) || !splitVersion(current, b)) return false;
    return std::lexicographical_compare(b, b + 3, a, a + 3);
}

bool parseLatestRelease(const std::string &manifestBody, ReleaseOffer &offer,
                        std::string &error) {
    offer = ReleaseOffer();
    const Json manifest = Json::parse(manifestBody);
    unsigned long parts[3];
    if (!manifest.isObject() || manifest["id"].string() != kAppId ||
        !splitVersion(manifest["version"].string(), parts) ||
        !isSha256(manifest["ipkHash"]["sha256"].string())) {
        error = "the latest release's manifest is not one this server understands";
        return false;
    }
    offer.version = manifest["version"].string();
    offer.sha256 = manifest["ipkHash"]["sha256"].string();
    std::transform(offer.sha256.begin(), offer.sha256.end(), offer.sha256.begin(),
                   [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    const std::string tag = repository() + "/releases/";
    offer.releaseUrl = tag + "tag/v" + offer.version;
    offer.ipkUrl = tag + "download/v" + offer.version + "/" + releasePackageFile(offer.version);
    return true;
}

std::string releasePackageFile(const std::string &version) {
    return std::string(kAppId) + "_" + version + "_all.ipk";
}

std::string releaseManifestUrl(const std::string &version) {
    return repository() + "/releases/" +
           (version.empty() ? "latest/download/" : "download/v" + version + "/") +
           kAppId + ".manifest.json";
}

bool parseReleaseManifest(const std::string &manifestBody, const std::string &version,
                          ReleaseOffer &offer, std::string &error) {
    if (!parseLatestRelease(manifestBody, offer, error)) return false;
    // The file the manifest names is the one build-ipk.py wrote beside it, which
    // the link above points at; any other name is not this release's package.
    if (offer.version != version ||
        Json::parse(manifestBody)["ipkUrl"].string() != releasePackageFile(version)) {
        offer = ReleaseOffer();
        error = "the release's manifest does not describe version " + version;
        return false;
    }
    return true;
}

ReleaseFetch fetchOverCurl(const std::string &userAgent,
                           std::function<std::string()> ipPreference) {
    return [userAgent, ipPreference](const std::string &url, std::string &body,
                                     std::string &error) {
        body.clear();
        long status = 0;
        AppPackageDownloadFailure failure;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        const bool fetched = AppPackageFetcher::downloadHttps(
            url, userAgent, ipPreference ? ipPreference() : std::string(), kManifestBytes,
            [&body](const char *bytes, size_t count) { body.append(bytes, count); return true; },
            status, failure,
            [deadline]() { return std::chrono::steady_clock::now() > deadline; });
        if (fetched) return true;
        error = failure.offline ? "GitHub could not be reached"
              : failure.message == "cancelled" || failure.timedOut ? "GitHub did not answer in time"
              : failure.message;
        return false;
    };
}

ReleaseChecker::ReleaseChecker(std::string serverVersion, ReleaseFetch fetch, Seconds now)
    : serverVersion_(std::move(serverVersion)), fetch_(std::move(fetch)), now_(std::move(now)) {}

bool ReleaseChecker::check(std::string &error) {
    error.clear();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (checking_) return true;
        checking_ = true;
    }
    std::string body;
    ReleaseOffer offer;
    bool ok = fetch_(releaseManifestUrl(std::string()), body, error) &&
              parseLatestRelease(body, offer, error);
    std::lock_guard<std::mutex> lock(mutex_);
    checking_ = false;
    everChecked_ = true;
    checkedAt_ = now_();
    error_ = ok ? std::string() : error;
    // The last good answer stays shown while a later check fails.
    if (ok) { haveOffer_ = true; offer_ = offer; }
    return ok;
}

bool ReleaseChecker::dailyCheckDue() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (checking_) return false;
    if (!everChecked_) return true;
    return now_() - checkedAt_ >= (error_.empty() ? 24 * 3600 : 3600);
}

bool ReleaseChecker::newerOffer(ReleaseOffer &offer) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!haveOffer_ || !releaseIsNewer(offer_.version, releaseVersionOf(serverVersion_))) return false;
    offer = offer_;
    return true;
}

Json ReleaseChecker::status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    Json status = Json::object();
    status.set("current", Json::text(releaseVersionOf(serverVersion_)));
    // A clone's build is updated by pulling and deploying, not from a package.
    status.set("fromSource", Json::flag(serverVersion_.find('+') != std::string::npos));
    status.set("checking", Json::flag(checking_));
    status.set("checkedAgoS", everChecked_ ? Json::numeric(static_cast<double>(now_() - checkedAt_)) : Json());
    status.set("error", error_.empty() ? Json() : Json::text(error_));
    status.set("releasesUrl", Json::text(repository() + "/releases"));
    Json latest;
    if (haveOffer_) {
        latest = Json::object();
        latest.set("version", Json::text(offer_.version));
        latest.set("sha256", Json::text(offer_.sha256));
        latest.set("releaseUrl", Json::text(offer_.releaseUrl));
        latest.set("ipkUrl", Json::text(offer_.ipkUrl));
    }
    status.set("latest", latest);
    status.set("available", Json::flag(haveOffer_ &&
                                       releaseIsNewer(offer_.version, releaseVersionOf(serverVersion_))));
    return status;
}

} // namespace service
} // namespace nativetv
