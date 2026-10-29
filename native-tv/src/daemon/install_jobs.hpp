#pragma once

#include "core/json.hpp"
#include "daemon/child_process.hpp"
#include "service/app_package_fetcher.hpp"
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace nativetv {
namespace daemon {

struct InstallDownloadOutcome {
    bool ok = false;
    std::string message;
    bool offline = false;
    bool timedOut = false;
    bool maxBytesExceeded = false;
    bool noSpace = false;
    bool cancelled = false;
    long remoteStatus = 0;
    bool freeBytesKnown = false;
    uint64_t freeBytes = 0;
};

// Everything the install job coordinator reads or does outside its own
// decisions, so the whole flow runs against scripted answers in tests. The
// calls block; the coordinator runs each job on its own worker thread.
struct InstallJobsDeps {
    // The job record (install-job.json) and the status route's snapshot cache.
    std::function<bool(const Json &record, std::string &error)> writeJob;
    std::function<bool(std::string &error)> clearJob;
    std::function<bool(Json &record, bool &found, std::string &error)> recoverJob;
    std::function<void(const Json &snapshot)> publish;

    std::function<void()> ensureStaging;
    std::function<void()> clearStaging;
    std::function<bool(const std::string &fileName, uint64_t &bytes)> fileSize;
    std::function<InstallDownloadOutcome(
        const std::string &url, const std::string &fileName, uint64_t expectedBytes,
        const std::function<void(uint64_t)> &progress,
        const std::function<bool()> &cancelled)> download;
    // {ok:true, info} or {ok:false, code, id, package, expected, actual} or {ok:false, error}.
    std::function<Json(const std::string &fileName, const std::string &expectedPackageId,
                       const std::string &expectedSha256)> inspect;
    // {ok, preview, needsReview, elevationAvailable, capacityOkay}, or invalid Json.
    std::function<Json(const Json &input)> buildPreview;
    std::function<bool(Json &apps)> installedApps;
    std::function<bool()> elevationAvailable;
    std::function<bool(const std::string &packageId, std::vector<std::string> &services,
                       std::string &error)> readElevated;
    std::function<bool(const std::string &packageId, const Json &services,
                       std::string &error)> writeElevated;
    // {ok, outcome: installed|failed|timeout|closed, error?}; ok false is a refusal.
    std::function<Json(const std::string &packageId, const std::string &fileName,
                       const std::string &sha256, long long timeoutMs)> install;
    // {done: [...], failed: [{service, error}]}
    std::function<Json(const std::string &packageId,
                       const std::vector<std::string> &services)> elevate;
    std::function<void()> onInstalled;
    std::function<void(const std::string &)> warn;
    std::function<long long()> nowMs;
    std::function<std::string()> newJobId;

    std::string tvMachine;
    std::chrono::milliseconds previewTimeout{10 * 60 * 1000};
    std::chrono::milliseconds installTimeout{120000};
    uint64_t uploadBytes = 512ULL * 1024 * 1024;
};

struct InstallReply {
    bool ok = false;
    std::string error;
    Json install;
};

struct InstallJobsCore;

// Installing an .ipk: stage, verify, inspect, preview, install through the
// developer install service, optionally elevate (legacy lib/installer.js).
// One job at a time; later steps show in snapshot().
class InstallJobs {
  public:
    explicit InstallJobs(InstallJobsDeps deps);
    ~InstallJobs();

    // {source:'catalog', pkg, auto}, {source:'url', url, sha256} or
    // {source:'file', file} (a name under staging). The caller has validated
    // the catalog row and the address.
    InstallReply start(const Json &request);
    // {jobId, elevate, confirmRoot, replaceStore, nativeServices?, nativeValidated?}
    InstallReply confirm(const Json &request);
    InstallReply cancel(const std::string &jobId);
    Json snapshot() const;
    // A job is working or waiting for confirmation / is downloading, verifying, installing.
    bool busy() const;
    bool working() const;
    // At start: a job the last run left unfinished is reported, not resumed.
    void recover();
    // Tests: wait until no worker thread runs.
    void waitIdle() const;

  private:
    std::shared_ptr<InstallJobsCore> core_;
};

struct InstallJobsEnvironment {
    std::string stagingDirectory;
    std::string systemRoot;
    std::string jobPath;
    std::string elevatedPath;
    std::string lunaSendPath;
    std::string userAgent;
    std::function<std::string()> ipPreference;
    service::AppPackageDownload fetcher;
    ChildProcessRunner *children = nullptr;
    std::function<bool(const std::string &uri, const Json &payload, Json &reply)> luna;
    std::function<void(const Json &snapshot)> publish;
    std::function<void()> onInstalled;
    std::function<void(const std::string &)> warn;
};

// The daemon's own services behind the coordinator's calls.
InstallJobsDeps productionInstallJobsDeps(const InstallJobsEnvironment &environment);

} // namespace daemon
} // namespace nativetv
