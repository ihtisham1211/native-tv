#include "daemon/install_jobs.hpp"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace nativetv;
using namespace nativetv::daemon;

namespace {
void check(bool value, const std::string &message) {
    if (!value) throw std::runtime_error(message);
}

std::string readFile(const std::string &path) {
    std::ifstream stream(path);
    std::stringstream text;
    text << stream.rdbuf();
    return text.str();
}

bool sameJson(const Json &left, const Json &right) {
    if (!left.valid() || !right.valid()) return !left.valid() && !right.valid();
    if (left.isObject() && right.isObject()) {
        std::vector<std::string> keys = left.keys();
        if (keys.size() != right.keys().size()) return false;
        for (const std::string &key : keys)
            if (!right.has(key) || !sameJson(left[key], right[key])) return false;
        return true;
    }
    if (left.isArray() && right.isArray()) {
        if (left.size() != right.size()) return false;
        for (size_t index = 0; index < left.size(); ++index)
            if (!sameJson(left[index], right[index])) return false;
        return true;
    }
    if (left.isNumber() && right.isNumber()) return left.number() == right.number();
    if (left.isString() && right.isString()) return left.string() == right.string();
    if (left.isBoolean() && right.isBoolean()) return left.boolean() == right.boolean();
    return false;
}

std::string replaceAll(std::string text, const std::string &from, const std::string &to) {
    for (size_t at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size()))
        text.replace(at, from.size(), to);
    return text;
}

// Scripted answers for every call the coordinator makes.
struct Script {
    Json scenario;
    std::vector<std::string> states;
    std::vector<Json> published;
    std::mutex mutex;
    int cleared = 0;
    int installed = 0;
    int elevated = 0;
    std::vector<std::string> savedElevation;

    InstallJobsDeps deps() {
        InstallJobsDeps d;
        d.writeJob = [this](const Json &record, std::string &) {
            std::lock_guard<std::mutex> lock(mutex);
            states.push_back(record["state"].string());
            return true;
        };
        d.clearJob = [this](std::string &) {
            std::lock_guard<std::mutex> lock(mutex);
            states.push_back("cleared");
            ++cleared;
            return true;
        };
        d.recoverJob = [](Json &, bool &found, std::string &) { found = false; return true; };
        d.publish = [this](const Json &snapshot) {
            std::lock_guard<std::mutex> lock(mutex);
            published.push_back(snapshot);
        };
        d.ensureStaging = []() {};
        d.clearStaging = []() {};
        d.fileSize = [this](const std::string &, uint64_t &bytes) {
            bytes = scenario["fileBytes"].isNumber() ? static_cast<uint64_t>(scenario["fileBytes"].number()) : 4096;
            return true;
        };
        d.download = [this](const std::string &, const std::string &, uint64_t,
                            const std::function<void(uint64_t)> &progress,
                            const std::function<bool()> &) {
            InstallDownloadOutcome outcome;
            const Json answer = scenario["download"];
            const Json failure = answer["error"];
            if (failure.isObject()) {
                outcome.message = failure["message"].string();
                outcome.remoteStatus = static_cast<long>(failure["remoteStatus"].number());
                outcome.offline = failure["offline"].boolean();
                outcome.timedOut = failure["timedOut"].boolean();
                outcome.maxBytesExceeded = failure["maxBytesExceeded"].boolean();
                outcome.noSpace = failure["noSpace"].boolean();
                outcome.freeBytesKnown = failure["freeBytes"].isNumber();
                outcome.freeBytes = static_cast<uint64_t>(failure["freeBytes"].number());
                return outcome;
            }
            progress(static_cast<uint64_t>(answer["bytes"].number()));
            outcome.ok = true;
            return outcome;
        };
        d.inspect = [this](const std::string &, const std::string &, const std::string &) {
            return scenario["inspect"];
        };
        d.buildPreview = [this](const Json &input) {
            return Json::parse(replaceAll(scenario["preview"].dump(), "JOB", input["jobId"].string()));
        };
        d.installedApps = [](Json &apps) { apps = Json::array(); return true; };
        d.elevationAvailable = [this]() {
            return !(scenario["elevateAvailable"].isBoolean() && !scenario["elevateAvailable"].boolean());
        };
        d.readElevated = [this](const std::string &, std::vector<std::string> &services, std::string &) {
            for (const Json &name : scenario["elevatedSaved"].items()) services.push_back(name.string());
            return true;
        };
        d.writeElevated = [this](const std::string &, const Json &services, std::string &) {
            std::lock_guard<std::mutex> lock(mutex);
            for (const Json &name : services.items()) savedElevation.push_back(name.string());
            return true;
        };
        d.install = [this](const std::string &, const std::string &, const std::string &, long long) {
            std::lock_guard<std::mutex> lock(mutex);
            ++installed;
            return scenario["install"].valid() ? scenario["install"]
                                               : Json::parse("{\"ok\":true,\"outcome\":\"installed\"}");
        };
        d.elevate = [this](const std::string &, const std::vector<std::string> &) {
            std::lock_guard<std::mutex> lock(mutex);
            ++elevated;
            return scenario["elevate"].valid() ? scenario["elevate"] : Json::parse("{\"done\":[],\"failed\":[]}");
        };
        d.nowMs = []() { return 1700000000000LL; };
        d.newJobId = []() { return std::string("feedfacecafebeef"); };
        d.tvMachine = "arm";
        return d;
    }
};

std::vector<std::string> dedupe(const std::vector<std::string> &states) {
    std::vector<std::string> result;
    for (const std::string &state : states)
        if (result.empty() || result.back() != state) result.push_back(state);
    return result;
}

// What the Node coordinator did for the same scripted answers, recorded in
// tests/fixtures/install-jobs.json from runs of the Node installer.
void scenarios(const std::string &fixturePath) {
    const Json fixture = Json::parse(readFile(fixturePath));
    check(fixture["scenarios"].isArray() && fixture["scenarios"].size() > 20, "fixture has scenarios");
    for (const Json &scenario : fixture["scenarios"].items()) {
        const std::string name = scenario["name"].string();
        const Json expected = scenario["expected"];
        Script script;
        script.scenario = scenario;
        InstallJobs jobs(script.deps());
        const InstallReply started = jobs.start(scenario["request"]);
        check(started.ok == !expected["startError"].isString(), name + ": start outcome");
        if (!started.ok) {
            check(started.error == expected["startError"].string(),
                  name + ": start refusal '" + started.error + "'");
            continue;
        }
        jobs.waitIdle();
        const std::string jobId = jobs.snapshot()["jobId"].string();
        if (scenario["confirm"].isObject() && jobs.snapshot()["state"].string() == "awaiting-confirm") {
            Json request = Json::parse(scenario["confirm"].dump());
            if (!request.has("jobId")) request.set("jobId", Json::text(jobId));
            const InstallReply confirmed = jobs.confirm(request);
            check(confirmed.ok == !expected["confirmError"].isString(), name + ": confirm outcome");
            if (!confirmed.ok)
                check(confirmed.error == expected["confirmError"].string(),
                      name + ": confirm refusal '" + confirmed.error + "'");
            jobs.waitIdle();
        }
        if (scenario["cancel"].boolean()) {
            const InstallReply cancelled = jobs.cancel(scenario["cancelJobId"].isString()
                                                           ? scenario["cancelJobId"].string() : jobId);
            check(cancelled.ok == !expected["cancelError"].isString(), name + ": cancel outcome");
            if (!cancelled.ok)
                check(cancelled.error == expected["cancelError"].string(), name + ": cancel refusal");
        }
        std::vector<std::string> states = dedupe(script.states);
        Json wanted = expected["states"];
        check(states.size() == wanted.size(), name + ": states " + std::to_string(states.size()) + " vs " +
                                                  std::to_string(wanted.size()));
        for (size_t index = 0; index < states.size(); ++index)
            check(states[index] == wanted[index].string(), name + ": state " + states[index]);
        const Json final = Json::parse(replaceAll(jobs.snapshot().dump(), jobId, "JOB"));
        check(sameJson(final, expected["final"]),
              name + ": snapshot\n native " + final.dump() + "\n node   " + expected["final"].dump());
    }
}

void waitFor(const std::function<bool()> &condition, const std::string &what) {
    for (int attempt = 0; attempt < 400; ++attempt) {
        if (condition()) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    throw std::runtime_error("timed out waiting for " + what);
}

Json catalogRequest() {
    return Json::parse("{\"source\":\"catalog\",\"auto\":false,\"pkg\":{\"id\":\"com.example.app\","
                       "\"version\":\"1.0.0\",\"ipkUrl\":\"https://repo.example/a.ipk\","
                       "\"sha256\":\"" + std::string(64, 'a') + "\",\"size\":4096}}");
}

void prepare(Script &script) {
    script.scenario = Json::parse(R"({
      "download": {"bytes": 4096},
      "inspect": {"ok": true, "info": {"package": "com.example.app", "version": "1.0.0",
        "sha256": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "machines": ["arm"],
        "services": [], "apps": [{"id": "com.example.app", "title": "Example", "version": "1.0.0"}]}},
      "preview": {"ok": true, "needsReview": false, "elevationAvailable": true, "capacityOkay": true,
        "preview": {"jobId": "JOB", "apps": [], "services": [], "reelevate": [], "willElevate": false,
                    "rootRequired": false}}
    })");
}

// A job in the middle of its download is held there until it is cancelled,
// and nothing else may start meanwhile.
void cancelDuringDownload() {
    Script script;
    prepare(script);
    InstallJobsDeps deps = script.deps();
    std::atomic<bool> inDownload{false};
    deps.download = [&inDownload](const std::string &, const std::string &, uint64_t,
                                  const std::function<void(uint64_t)> &progress,
                                  const std::function<bool()> &cancelled) {
        progress(1024);
        inDownload = true;
        while (!cancelled()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        InstallDownloadOutcome outcome;
        outcome.cancelled = true;
        outcome.message = "cancelled";
        return outcome;
    };
    InstallJobs jobs(deps);
    const InstallReply first = jobs.start(catalogRequest());
    check(first.ok && first.install["state"].string() == "downloading", "download starts");
    waitFor([&]() { return inDownload.load(); }, "the download");
    check(jobs.busy() && jobs.working(), "a download is busy and working");
    const InstallReply second = jobs.start(catalogRequest());
    check(!second.ok && second.error == "an install is already in progress", "second start is refused");
    waitFor([&]() { return jobs.snapshot()["progress"]["bytes"].number() == 1024; }, "progress in the snapshot");
    check(!jobs.cancel("not-the-job").ok, "a stranger's job id is refused");
    check(jobs.cancel(first.install["jobId"].string()).ok, "cancel is accepted");
    check(jobs.snapshot()["state"].string() == "idle", "cancelled job is gone");
    check(script.cleared == 1, "the job record is cleared");
    jobs.waitIdle();
    check(jobs.snapshot()["state"].string() == "idle", "the cancelled download does not come back");
    check(!jobs.busy(), "nothing is busy after a cancel");
    check(jobs.start(catalogRequest()).ok, "a new job starts after a cancel");
    jobs.cancel(jobs.snapshot()["jobId"].string());
    jobs.waitIdle();
}

void previewExpires() {
    Script script;
    prepare(script);
    InstallJobsDeps deps = script.deps();
    deps.previewTimeout = std::chrono::milliseconds(80);
    InstallJobs jobs(deps);
    check(jobs.start(catalogRequest()).ok, "start");
    jobs.waitIdle();
    check(jobs.snapshot()["state"].string() == "awaiting-confirm", "waits for confirmation");
    check(jobs.busy() && !jobs.working(), "an open preview is busy but not working");
    waitFor([&]() { return jobs.snapshot()["state"].string() == "error"; }, "the preview to expire");
    check(jobs.snapshot()["error"].string() == "the preview expired; start the install again", "expiry text");
    check(!jobs.confirm(Json::parse("{\"jobId\":\"feedfacecafebeef\"}")).ok, "an expired preview cannot be confirmed");
}

void confirmStopsTheTimer() {
    Script script;
    prepare(script);
    InstallJobsDeps deps = script.deps();
    deps.previewTimeout = std::chrono::milliseconds(60);
    InstallJobs jobs(deps);
    check(jobs.start(catalogRequest()).ok, "start");
    jobs.waitIdle();
    check(jobs.confirm(Json::parse("{\"jobId\":\"feedfacecafebeef\",\"nativeValidated\":true}")).ok, "confirm");
    jobs.waitIdle();
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    check(jobs.snapshot()["state"].string() == "installed", "the timer does not touch an installed job");
}

void recovery() {
    {
        Script script;
    prepare(script);
        InstallJobsDeps deps = script.deps();
        deps.recoverJob = [](Json &record, bool &found, std::string &) {
            record = Json::parse("{\"jobId\":\"abc123\",\"state\":\"interrupted\"}");
            found = true;
            return true;
        };
        InstallJobs jobs(deps);
        jobs.recover();
        const Json snapshot = jobs.snapshot();
        check(snapshot["state"].string() == "interrupted" && snapshot["jobId"].string() == "abc123",
              "an interrupted job is reported");
        check(snapshot["error"].string() == "the TV restarted during an install; check the app list",
              "interrupted text");
        check(!snapshot.has("source") && snapshot["appId"].valid() == false, "an interrupted job has no source");
        check(!jobs.busy(), "an interrupted job does not block a new one");
        check(!script.published.empty() && script.published.back()["state"].string() == "interrupted",
              "the interrupted snapshot is published");
        check(jobs.start(catalogRequest()).ok, "a new job starts over an interrupted one");
        jobs.waitIdle();
    }
    {
        Script script;
    prepare(script);
        InstallJobs jobs(script.deps());
        jobs.recover();
        check(script.cleared == 1, "a record that is not an interrupted job is cleared at start");
        check(jobs.snapshot()["state"].string() == "idle", "no record, no job");
        check(!script.published.empty() && script.published.back()["state"].string() == "idle",
              "the idle snapshot is published");
    }
}

void persistenceFailures() {
    Script script;
    prepare(script);
    InstallJobsDeps deps = script.deps();
    std::atomic<int> allowed{0};
    deps.writeJob = [&allowed](const Json &, std::string &error) {
        if (allowed-- > 0) return true;
        error = "disk is read-only";
        return false;
    };
    allowed = 0;
    InstallJobs refused(deps);
    const InstallReply reply = refused.start(catalogRequest());
    check(!reply.ok && reply.error == "disk is read-only", "start reports a failed first write");
    check(refused.snapshot()["state"].string() == "idle" && !refused.busy(), "nothing was started");

    // The first write (downloading) works; the move to verifying does not.
    allowed = 1;
    InstallJobs failing(deps);
    check(failing.start(catalogRequest()).ok, "the first write works");
    failing.waitIdle();
    check(failing.snapshot()["state"].string() == "error" ||
              failing.snapshot()["state"].string() == "downloading",
          "a failed transition does not leave the job moving on");
}

void auxiliaryWords() {
    Script script;
    prepare(script);
    InstallJobs jobs(script.deps());
    check(!jobs.confirm(Json::parse("{\"jobId\":\"nothing\"}")).ok, "no job to confirm");
    check(jobs.confirm(Json::parse("{\"jobId\":\"nothing\"}")).error ==
              "there is no install waiting for confirmation", "no-job text");
    check(jobs.cancel("nothing").error == "there is no install waiting for confirmation", "no-job cancel text");
}
} // namespace

int main(int argc, char **argv) {
    try {
        check(argc > 1, "the fixture path is expected");
        scenarios(argv[1]);
        cancelDuringDownload();
        previewExpires();
        confirmStopsTheTimer();
        recovery();
        persistenceFailures();
        auxiliaryWords();
    } catch (const std::exception &error) {
        std::fprintf(stderr, "install jobs test failed: %s\n", error.what());
        return 1;
    }
    std::puts("install jobs tests passed");
    return 0;
}
