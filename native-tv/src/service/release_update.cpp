#include "service/release_update.hpp"

#include "domain/say.hpp"

#include <utility>

namespace nativetv {
namespace service {
namespace {

bool isWorking(const std::string &state) {
    return state == "downloading" || state == "verifying" || state == "installing" ||
           state == "restarting";
}

} // namespace

ReleaseUpdate::ReleaseUpdate(ReleaseUpdateSteps steps) : steps_(std::move(steps)) {
    if (steps_.lastFailure && steps_.lastFailure(version_, error_)) state_ = "failed";
}

ReleaseUpdate::~ReleaseUpdate() {
    // The app's installer restarts this server and so ends it while the installer
    // runs on: the wait for it is given up rather than holding the shutdown.
    stopping_ = true;
    waitIdle();
}

bool ReleaseUpdate::start(const ReleaseOffer &checked, std::string &error) {
    const std::string reason = steps_.unavailable ? steps_.unavailable() : std::string();
    if (!reason.empty()) {
        error = reason;
        return false;
    }
    std::thread previous;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (isWorking(state_)) {
            error = say::msg("srv.update.running", "an update is already running");
            return false;
        }
        state_ = "downloading";
        version_ = checked.version;
        error_.clear();
        receivedBytes_ = 0;
        previous = std::move(worker_);
    }
    if (previous.joinable()) previous.join();
    std::lock_guard<std::mutex> lock(mutex_);
    worker_ = std::thread([this, checked]() { run(checked); });
    return true;
}

void ReleaseUpdate::run(ReleaseOffer checked) {
    std::string error;
    ReleaseOffer offer;
    const auto enter = [this](const char *state) {
        std::lock_guard<std::mutex> lock(mutex_);
        state_ = state;
    };
    const auto stop = [this, &error]() {
        if (steps_.discard) steps_.discard();
        std::lock_guard<std::mutex> lock(mutex_);
        state_ = "failed";
        error_ = error;
    };

    // The manifest an update installs from is the release's own, read again
    // now; one that no longer matches what the check showed is not installed.
    if (!steps_.readManifest(checked.version, offer, error)) return stop();
    if (offer.sha256 != checked.sha256) {
        error = say::msg("srv.update.manifestChanged",
                         "the release's manifest changed since it was checked; check for updates again");
        return stop();
    }
    const std::function<bool()> stopping = [this]() { return stopping_.load(); };
    if (!steps_.download(offer, [this](uint64_t bytes) { receivedBytes_ = bytes; }, stopping, error))
        return stop();
    enter("verifying");
    if (!steps_.verify(offer, error)) return stop();
    enter("installing");
    if (!steps_.installPackage(offer, error)) return stop();
    // The file has done its job, and the app's installer needs the room.
    if (steps_.discard) steps_.discard();
    enter("restarting");
    // The app's installer restarts this server, which ends this thread with it:
    // one that comes back, even having moved the files, did not.
    if (steps_.applyPackage(offer, stopping, error)) {
        Json variables = Json::object();
        variables.set("version", Json::text(offer.version));
        error = say::msg("srv.update.notRestarted",
                         "version {version} is installed, but the server did not restart; restart the TV, or run tvwebctl restart over SSH",
                         variables);
    }
    stop();
}

Json ReleaseUpdate::status() const {
    const std::string reason = steps_.unavailable ? steps_.unavailable() : std::string();
    Json status = Json::object();
    std::lock_guard<std::mutex> lock(mutex_);
    status.set("state", Json::text(state_));
    status.set("version", version_.empty() ? Json() : Json::text(version_));
    status.set("receivedBytes", Json::numeric(static_cast<double>(receivedBytes_.load())));
    status.set("error", error_.empty() ? Json() : Json::text(error_));
    status.set("unavailable", reason.empty() ? Json() : Json::text(reason));
    return status;
}

bool ReleaseUpdate::working() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return isWorking(state_);
}

void ReleaseUpdate::waitIdle() {
    std::thread finished;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        finished = std::move(worker_);
    }
    if (finished.joinable()) finished.join();
}

} // namespace service
} // namespace nativetv
