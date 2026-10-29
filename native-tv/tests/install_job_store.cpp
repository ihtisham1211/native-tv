#include "daemon/install_job_store.hpp"
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace {
void check(bool condition, const std::string &message) {
    if (!condition) throw std::runtime_error(message);
}
nativetv::Json record(const std::string &state,
                        const std::string &jobId = "7e48a92c") {
    nativetv::Json job = nativetv::Json::object();
    job.set("jobId", nativetv::Json::text(jobId));
    job.set("state", nativetv::Json::text(state));
    job.set("package", nativetv::Json::text("org.example.app"));
    job.set("version", nativetv::Json::text("1.2.3"));
    job.set("updated", nativetv::Json::numeric(1700000000000.0));
    return job;
}
}

int main() {
    char folder[] = "/tmp/nativetv-install-job-test-XXXXXX";
    char *root = mkdtemp(folder);
    check(root != nullptr, "could not create installer job test directory");
    const std::string path = std::string(root) + "/install-job.json";
    bool found = true;
    nativetv::Json job;
    std::string error;
    check(nativetv::daemon::readInstallerJob(path, job, found, &error) && !found,
          "missing job state should be reported as missing");

    const std::string activeStates[] = {
        "downloading", "verifying", "installing", "elevating"
    };
    for (const std::string &state : activeStates) {
        check(nativetv::daemon::recordInstallerJob(path, record(state), &error),
              "valid install job should persist: " + error);
        check(nativetv::daemon::recoverInstallerJob(path, job, found, &error) && found &&
                  job["state"].string() == "interrupted" &&
                  job["package"].string() == "org.example.app" &&
                  job["updated"].number() >= 1700000000000.0,
              "an active install should be marked interrupted during recovery");
        check(nativetv::daemon::recoverInstallerJob(path, job, found, &error) && !found,
              "an interrupted job should be reported only once");
        check(access(path.c_str(), F_OK) != 0,
              "consuming an interrupted job should remove the record");
    }

    check(nativetv::daemon::recordInstallerJob(path, record("awaiting-confirm"), &error),
          "a preview waiting for confirmation should persist: " + error);
    check(nativetv::daemon::recoverInstallerJob(path, job, found, &error) && !found,
          "a preview waiting for confirmation must not be reported as interrupted");
    check(nativetv::daemon::readInstallerJob(path, job, found, &error) && found &&
              job["state"].string() == "awaiting-confirm",
          "non-working jobs should remain available to the normal reader");

    check(nativetv::daemon::recordInstallerJob(path, record("installed"), &error),
          "a finished install job should persist: " + error);
    check(nativetv::daemon::recoverInstallerJob(path, job, found, &error) && !found,
          "a finished job must not be reported as interrupted");
    struct stat info = {};
    check(stat(path.c_str(), &info) == 0 && (info.st_mode & 0777) == 0600,
          "installer job state must have mode 0600");

    nativetv::Json malformed = record("installing");
    malformed.set("jobId", nativetv::Json::text(""));
    check(!nativetv::daemon::recordInstallerJob(path, malformed, &error),
          "empty job ids should be rejected");
    check(nativetv::daemon::readInstallerJob(path, job, found, &error) && found &&
              job["state"].string() == "installed",
          "invalid updates must preserve the last good job");

    const std::string transitionPath = std::string(root) + "/transition-job.json";
    check(nativetv::daemon::transitionInstallerJob(
              transitionPath, record("downloading"), &error),
          "a new install should start in downloading: " + error);
    check(!nativetv::daemon::transitionInstallerJob(
              transitionPath, record("installing"), &error),
          "an install cannot skip download and verification states");
    check(nativetv::daemon::readInstallerJob(transitionPath, job, found, &error) &&
              found && job["state"].string() == "downloading",
          "a refused transition must preserve the last accepted state");
    check(nativetv::daemon::transitionInstallerJob(
              transitionPath, record("verifying"), &error),
          "a completed download should advance to verification: " + error);
    check(nativetv::daemon::transitionInstallerJob(
              transitionPath, record("awaiting-confirm"), &error),
          "a verified package should advance to its preview: " + error);
    check(nativetv::daemon::transitionInstallerJob(
              transitionPath, record("installing"), &error),
          "a confirmed preview should advance to installation: " + error);
    check(nativetv::daemon::transitionInstallerJob(
              transitionPath, record("elevating"), &error),
          "a successful install may advance to elevation: " + error);
    check(nativetv::daemon::transitionInstallerJob(
              transitionPath, record("installed"), &error),
          "a completed install should become terminal: " + error);
    check(nativetv::daemon::transitionInstallerJob(
              transitionPath, record("downloading", "next-job"), &error),
          "a terminal job may be replaced by a new job: " + error);
    check(!nativetv::daemon::transitionInstallerJob(
              transitionPath, record("downloading", "replacement-job"), &error),
          "a different job cannot replace one that is still active");
    check(nativetv::daemon::transitionInstallerJob(
              transitionPath, record("error", "next-job"), &error),
          "an active job may end with an error: " + error);
    check(!nativetv::daemon::transitionInstallerJob(
              transitionPath, record("invented", "next-job"), &error),
          "unknown install states should be rejected");
    check(nativetv::daemon::readInstallerJob(transitionPath, job, found, &error) &&
              found && job["state"].string() == "error",
          "an invalid state must preserve the last accepted terminal record");
    check(nativetv::daemon::clearInstallerJob(transitionPath, &error),
          "the transition fixture should clear cleanly: " + error);

    const std::string backup = path + ".last-good";
    FILE *sidecar = std::fopen(backup.c_str(), "w");
    check(sidecar != nullptr, "could not create installer job backup fixture");
    std::fputs("{}", sidecar);
    std::fclose(sidecar);
    check(nativetv::daemon::clearInstallerJob(path, &error),
          "installer job state should clear: " + error);
    check(nativetv::daemon::readInstallerJob(path, job, found, &error) && !found,
          "cleared job state should not recover from its previous copy");
    check(access(path.c_str(), F_OK) != 0 && access(backup.c_str(), F_OK) != 0,
          "clear must remove both the current job and its last-good copy");
    unlink(path.c_str());
    unlink((path + ".last-good").c_str());
    unlink(transitionPath.c_str());
    unlink((transitionPath + ".last-good").c_str());
    rmdir(root);
    std::cout << "native install job store tests passed\n";
    return 0;
}
