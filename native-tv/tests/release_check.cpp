#include "service/release_check.hpp"
#include <cstdio>
#include <stdexcept>
#include <string>

using namespace nativetv;
using namespace nativetv::service;

namespace {
unsigned checks = 0;

void check(bool value, const char *message) {
    ++checks;
    if (!value)
        throw std::runtime_error(message);
}

const std::string kSha(64, 'A');

std::string manifest(const std::string &version, const std::string &sha = kSha,
                     const std::string &id = "io.github.ihtisham1211.nativetv") {
    return "{\"id\":\"" + id + "\",\"version\":\"" + version +
           "\",\"ipkUrl\":\"https://evil.example/x.ipk\",\"ipkHash\":{\"sha256\":\"" + sha + "\"}}";
}
} // namespace

int main() {
    try {
        check(releaseVersionOf("0.80.1+55e51e5.dirty") == "0.80.1", "a clone's build keeps its release version");
        check(releaseVersionOf("0.118.0") == "0.118.0", "a release is its own version");

        check(releaseIsNewer("0.119.0", "0.118.0"), "a later minor is newer");
        check(releaseIsNewer("0.118.10", "0.118.9"), "versions compare as numbers, not text");
        check(releaseIsNewer("1.0.0", "0.999.999"), "a later major is newer");
        check(!releaseIsNewer("0.118.0", "0.118.0"), "the same version is not newer");
        check(!releaseIsNewer("0.117.0", "0.118.0"), "an earlier version is not newer");
        check(!releaseIsNewer("0.119", "0.118.0") && !releaseIsNewer("0.119.0-rc1", "0.118.0") &&
                  !releaseIsNewer("0.1234567890.0", "0.118.0") && !releaseIsNewer("", "0.118.0"),
              "anything but x.y.z is never newer");

        ReleaseOffer offer;
        std::string error;
        check(parseLatestRelease(manifest("0.119.0"), offer, error), "a release manifest parses");
        check(offer.version == "0.119.0" && offer.sha256 == std::string(64, 'a'),
              "the hash is kept in lower case");
        check(offer.releaseUrl == "https://github.com/ihtisham1211/native-tv/releases/tag/v0.119.0" &&
                  offer.ipkUrl == "https://github.com/ihtisham1211/native-tv/releases/download/v0.119.0/"
                                  "io.github.ihtisham1211.nativetv_0.119.0_all.ipk",
              "the links are built from the version, not read from the manifest");
        check(!parseLatestRelease(manifest("0.119.0", kSha, "org.example.other"), offer, error) &&
                  !parseLatestRelease(manifest("latest"), offer, error) &&
                  !parseLatestRelease(manifest("0.119.0", "abc"), offer, error) &&
                  !parseLatestRelease("not json", offer, error) && offer.version.empty(),
              "another app's, a malformed or a hashless manifest is refused");

        long long clock = 1000;
        std::string fetched;
        bool reachable = true;
        ReleaseChecker checker("0.118.0+55e51e5", [&](const std::string &url, std::string &body,
                                                      std::string &failure) {
            fetched = url;
            if (!reachable) { failure = "GitHub could not be reached"; return false; }
            body = manifest("0.119.0");
            return true;
        }, [&]() { return clock; });

        Json status = checker.status();
        check(status["current"].string() == "0.118.0" && status["fromSource"].boolean() &&
                  !status["available"].boolean() && !status["checkedAgoS"].isNumber() &&
                  checker.dailyCheckDue(),
              "before a check nothing is known and one is due");

        check(checker.check(error) && error.empty(), "a check succeeds");
        check(fetched == "https://github.com/ihtisham1211/native-tv/releases/latest/download/"
                         "io.github.ihtisham1211.nativetv.manifest.json",
              "the latest release's manifest is what is asked for");
        clock += 90;
        status = checker.status();
        check(status["available"].boolean() && status["latest"]["version"].string() == "0.119.0" &&
                  static_cast<int>(status["checkedAgoS"].number()) == 90 && !status["error"].isString(),
              "a newer release is reported with its age");
        check(!checker.dailyCheckDue(), "a fresh answer is not checked again");
        clock += 24 * 3600;
        check(checker.dailyCheckDue(), "a day-old answer is checked again");

        reachable = false;
        check(!checker.check(error) && error == "GitHub could not be reached", "an unreachable GitHub is an error");
        status = checker.status();
        check(status["error"].string() == "GitHub could not be reached" && status["available"].boolean(),
              "the error is shown beside the last good answer");
        check(!checker.dailyCheckDue(), "a failed check is not retried at once");
        clock += 3600;
        check(checker.dailyCheckDue(), "a failed check is retried after an hour");

        ReleaseChecker current("0.119.0", [](const std::string &, std::string &body, std::string &) {
            body = manifest("0.119.0");
            return true;
        }, [&]() { return clock; });
        check(current.check(error) && !current.status()["available"].boolean(),
              "the same release is not an update");

        std::printf("%u release check checks passed\n", checks);
        return 0;
    } catch (const std::exception &failure) {
        std::fprintf(stderr, "%s\n", failure.what());
        return 1;
    }
}
