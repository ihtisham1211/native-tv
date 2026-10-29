#include "daemon/install_jobs.hpp"

#include "daemon/app_elevation_executor.hpp"
#include "daemon/app_elevation_store.hpp"
#include "daemon/app_install_executor.hpp"
#include "daemon/install_job_store.hpp"
#include "domain/say.hpp"
#include "service/app_ids.hpp"
#include "service/app_install_preview.hpp"
#include "service/app_install_space.hpp"
#include "service/app_package_inspector.hpp"
#include "service/app_package_policy.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <dirent.h>
#include <fcntl.h>
#include <initializer_list>
#include <mutex>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace nativetv {
namespace daemon {

namespace {
// legacy lib/installer.js is the reference: the messages and the order of the
// steps follow it.
std::string msg(const char *key, const char *english,
                std::initializer_list<std::pair<const char *, std::string>> values = {}) {
    Json variables = Json::object();
    for (const auto &value : values) variables.set(value.first, Json::text(value.second));
    return say::msg(key, english, variables);
}

std::string formatMb(double bytes) {
    std::string value = compactNumber(std::round(bytes / 104857.6) / 10.0, 1);
    if (value.size() >= 2 && value.compare(value.size() - 2, 2, ".0") == 0)
        value.erase(value.size() - 2);
    return value + " MB";
}

std::string unknownError() { return msg("srv.install.unknownError", "unknown error"); }
std::string installFailed(const std::string &text) {
    return msg("srv.install.failed", "the install failed: {error}", {{"error", text}});
}
std::string unreadable(const std::string &text) {
    return msg("srv.install.ipk.unreadable", "the ipk could not be read: {error}",
               {{"error", text}});
}
std::string noSpace(double need, double have) {
    return msg("srv.install.noSpace", "not enough free space: {need} needed, {free} free",
               {{"need", formatMb(need)}, {"free", formatMb(have)}});
}
std::string tooBig(uint64_t limit) {
    return msg("srv.install.tooBig", "the package is larger than the {limit} limit",
               {{"limit", formatMb(static_cast<double>(limit))}});
}
std::string noJobText() {
    return msg("srv.install.noJob", "there is no install waiting for confirmation");
}

// installer.js short(): a bounded slice of a string, never half a UTF-8 character.
std::string shortText(const Json &value, size_t limit = 200) {
    if (!value.isString()) return std::string();
    std::string text = value.string();
    if (text.size() <= limit) return text;
    text.resize(limit);
    while (!text.empty() && (static_cast<unsigned char>(text.back()) & 0xC0) == 0x80)
        text.pop_back();
    if (!text.empty() && static_cast<unsigned char>(text.back()) >= 0xC0) text.pop_back();
    return text;
}

bool truthy(const Json &value) {
    if (!value.valid()) return false;
    if (value.isBoolean()) return value.boolean();
    if (value.isNumber()) return value.number() != 0;
    if (value.isString()) return !value.string().empty();
    return value.isArray() || value.isObject();
}

Json copyOf(const Json &value) {
    return value.valid() ? Json::parse(value.dump()) : Json();
}

std::string lowerHex(std::string text) {
    for (char &character : text)
        if (character >= 'A' && character <= 'F') character = static_cast<char>(character - 'A' + 'a');
    return text;
}

bool isSha256(const std::string &text) {
    if (text.size() != 64) return false;
    for (unsigned char character : text)
        if (!std::isxdigit(character)) return false;
    return true;
}

bool nameLike(const std::string &id) {
    if (id.empty() || id.size() > 128 || !std::isalnum(static_cast<unsigned char>(id[0])))
        return false;
    for (unsigned char character : id)
        if (!std::isalnum(character) && character != '.' && character != '_' &&
            character != '+' && character != '-')
            return false;
    return true;
}

bool uploadFileName(const std::string &name) {
    if (name.size() != 27 || name.compare(0, 7, "upload-") != 0 ||
        name.compare(23, 4, ".ipk") != 0)
        return false;
    for (size_t index = 7; index < 23; ++index) {
        const char character = name[index];
        if (!((character >= '0' && character <= '9') || (character >= 'a' && character <= 'f')))
            return false;
    }
    return true;
}

// fetch.js: where a download came from, for the words the page shows.
std::string hostOf(const std::string &url) {
    const size_t scheme = url.find("://");
    if (scheme == std::string::npos) return "the server";
    size_t begin = scheme + 3;
    const size_t at = url.find('@', begin);
    const size_t slash = url.find_first_of("/?#", begin);
    if (at != std::string::npos && (slash == std::string::npos || at < slash)) begin = at + 1;
    size_t end = url.find_first_of("/:?#", begin);
    if (end == std::string::npos) end = url.size();
    if (end == begin) return "the server";
    std::string host = url.substr(begin, end - begin);
    for (char &character : host)
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    return host;
}

bool isGithub(const std::string &url) {
    const std::string host = hostOf(url);
    return host == "github.com" ||
           (host.size() > 10 && host.compare(host.size() - 11, 11, ".github.com") == 0);
}

std::string pathOf(const std::string &url) {
    const size_t scheme = url.find("://");
    const size_t slash = scheme == std::string::npos ? std::string::npos
                                                     : url.find('/', scheme + 3);
    return slash == std::string::npos ? std::string() : url.substr(slash);
}

std::string hostSaid(long status, const std::string &url, const std::string &name = "") {
    const std::string host = name.empty() ? hostOf(url) : name;
    const std::string where = pathOf(url);
    if (status > 0)
        return msg("srv.fetch.hostStatus", "{host} returned {status} for {where}",
                   {{"host", host}, {"status", std::to_string(status)}, {"where", where}});
    return msg("srv.fetch.hostError", "{host} answered with an error for {where}",
               {{"host", host}, {"where", where}});
}

std::string githubSaid(long status, const std::string &url) {
    if (status == 404)
        return msg("srv.fetch.github404",
                   "GitHub returned 404 for {where} - no release published yet, or the repository is not visible",
                   {{"where", pathOf(url)}});
    if (status == 403 || status == 429)
        return msg("srv.fetch.githubLimit",
                   "GitHub refused the request ({status}). It allows 60 checks an hour from one address, so try again in an hour; if it keeps happening, something on the network may be blocking it.",
                   {{"status", std::to_string(status)}});
    if (status >= 500)
        return msg("srv.fetch.githubDown",
                   "GitHub is not answering at the moment ({status}). Try again later.",
                   {{"status", std::to_string(status)}});
    return hostSaid(status, url, "GitHub");
}

const char *const kUnfinished[] = {"downloading", "verifying", "awaiting-confirm", "installing",
                                   "elevating"};
bool isUnfinished(const std::string &state) {
    for (const char *name : kUnfinished)
        if (state == name) return true;
    return false;
}
bool isWorkingState(const std::string &state) {
    return state == "downloading" || state == "verifying" || state == "installing" ||
           state == "elevating";
}

struct InstallJob {
    std::string id;
    std::string state;
    std::string source;
    std::string file;
    std::string expectedSha;
    std::string digest;
    std::string error;
    bool hasError = false;
    bool cancelled = false;
    bool autoInstall = false;
    uint64_t size = 0;
    Json pkg;
    Json info;
    Json progress;
    Json preview;
    Json result;
    std::shared_ptr<std::atomic<bool>> cancelFlag = std::make_shared<std::atomic<bool>>(false);
};
using JobPointer = std::shared_ptr<InstallJob>;
} // namespace

struct InstallJobsCore : std::enable_shared_from_this<InstallJobsCore> {
    InstallJobsDeps deps;
    mutable std::mutex mutex;
    std::condition_variable condition;
    JobPointer job;
    JobPointer previewJob;
    std::chrono::steady_clock::time_point previewDeadline;
    int workers = 0;
    bool shutdown = false;
    bool timerStarted = false;

    explicit InstallJobsCore(InstallJobsDeps d) : deps(std::move(d)) {}

    using Lock = std::unique_lock<std::mutex>;

    void warn(const std::string &text) const {
        if (deps.warn) deps.warn(text);
    }
    long long now() const { return deps.nowMs ? deps.nowMs() : 0; }

    bool isCurrent(const JobPointer &j) const { return job == j && !j->cancelled; }
    bool busyLocked() const { return job && isUnfinished(job->state); }
    bool workingLocked() const { return job && isWorkingState(job->state); }
    bool vetted(const JobPointer &j) const {
        return j->source == "catalog" && truthy(j->pkg["vetted"]);
    }

    Json snapshotLocked() const {
        Json snapshot = Json::object();
        if (!job) {
            snapshot.set("state", Json::text("idle"));
            snapshot.set("jobId", Json());
            snapshot.set("source", Json());
            snapshot.set("appId", Json());
            snapshot.set("progress", Json());
            snapshot.set("preview", Json());
            snapshot.set("error", Json());
            snapshot.set("result", Json());
            return snapshot;
        }
        snapshot.set("state", Json::text(job->state));
        snapshot.set("jobId", Json::text(job->id));
        if (!job->source.empty()) snapshot.set("source", Json::text(job->source));
        snapshot.set("appId", job->pkg.valid() ? job->pkg["id"] : Json());
        snapshot.set("repo", job->pkg.valid() && truthy(job->pkg["repo"]) ? job->pkg["repo"] : Json());
        snapshot.set("progress", copyOf(job->progress));
        snapshot.set("preview", copyOf(job->preview));
        snapshot.set("error", job->hasError ? Json::text(job->error) : Json());
        snapshot.set("result", copyOf(job->result));
        return Json::parse(snapshot.dump());
    }

    Json recordFor(const JobPointer &j, const std::string &state) const {
        Json record = Json::object();
        record.set("jobId", Json::text(j->id));
        record.set("state", Json::text(state));
        if (j->info.valid()) record.set("package", j->info["package"]);
        else record.set("package", j->pkg.valid() && j->pkg["id"].isString() ? j->pkg["id"] : Json());
        record.set("version", j->info.valid() ? j->info["version"] : Json());
        record.set("updated", Json::numeric(static_cast<double>(now())));
        return record;
    }

    // Writes the record and publishes the snapshot, optionally as it will be
    // once `state` has been reached.
    bool persistLocked(const JobPointer &j, std::string &error, const std::string *state = nullptr,
                       const std::string *errorText = nullptr) {
        const std::string previousState = j->state;
        const std::string previousError = j->error;
        const bool previousHas = j->hasError;
        if (state) {
            j->state = *state;
            j->hasError = errorText && !errorText->empty();
            j->error = j->hasError ? *errorText : std::string();
        }
        const Json snapshot = snapshotLocked();
        const std::string written = state ? *state : j->state;
        if (state) {
            j->state = previousState;
            j->error = previousError;
            j->hasError = previousHas;
        }
        if (!deps.writeJob || !deps.writeJob(recordFor(j, written), error)) {
            if (error.empty()) error = "installer state could not be saved";
            return false;
        }
        if (deps.publish) deps.publish(snapshot);
        return true;
    }

    void setStateLocked(const JobPointer &j, const std::string &state, const std::string &errorText = "") {
        j->state = state;
        j->hasError = !errorText.empty();
        j->error = errorText;
        std::string error;
        if (!persistLocked(j, error)) {
            warn("installer: native state synchronization failed: " + error);
            if (deps.publish) deps.publish(snapshotLocked());
        }
    }

    // The state is reached only once it has been saved.
    bool transitionLocked(const JobPointer &j, const std::string &state, std::string &error) {
        const std::string none;
        if (!persistLocked(j, error, &state, &none)) return false;
        if (!isCurrent(j)) return true;
        j->state = state;
        j->hasError = false;
        j->error.clear();
        return true;
    }

    void clearPreviewTimerLocked() { previewJob.reset(); condition.notify_all(); }

    void failLocked(const JobPointer &j, const std::string &text) {
        if (!isCurrent(j)) return;
        clearPreviewTimerLocked();
        if (deps.clearStaging) deps.clearStaging();
        setStateLocked(j, "error", text);
    }

    void startTimerLocked() {
        if (timerStarted) return;
        timerStarted = true;
        const std::shared_ptr<InstallJobsCore> self = shared_from_this();
        std::thread([self]() { self->timerLoop(); }).detach();
    }

    void timerLoop() {
        Lock lock(mutex);
        while (!shutdown) {
            if (!previewJob) {
                condition.wait(lock);
                continue;
            }
            const auto deadline = previewDeadline;
            if (condition.wait_until(lock, deadline) == std::cv_status::timeout &&
                previewJob && std::chrono::steady_clock::now() >= previewDeadline) {
                const JobPointer j = previewJob;
                previewJob.reset();
                if (isCurrent(j) && j->state == "awaiting-confirm")
                    failLocked(j, msg("srv.install.expired",
                                      "the preview expired; start the install again"));
            }
        }
    }

    void armPreviewTimeoutLocked(const JobPointer &j) {
        if (!isCurrent(j) || j->state != "awaiting-confirm") return;
        startTimerLocked();
        previewJob = j;
        previewDeadline = std::chrono::steady_clock::now() + deps.previewTimeout;
        condition.notify_all();
    }

    void spawn(std::function<void(Lock &)> work) {
        ++workers;
        const std::shared_ptr<InstallJobsCore> self = shared_from_this();
        std::thread([self, work]() {
            Lock lock(self->mutex);
            try {
                work(lock);
            } catch (const std::exception &error) {
                self->warn(std::string("installer: ") + error.what());
            }
            if (!lock.owns_lock()) lock.lock();
            --self->workers;
            self->condition.notify_all();
        }).detach();
    }

    /* ---------------------------------------------------------- start */

    InstallReply refuse(const std::string &text) const {
        InstallReply reply;
        reply.error = text;
        return reply;
    }

    InstallReply start(const Json &request) {
        Lock lock(mutex);
        if (busyLocked()) return refuse(msg("srv.install.busy", "an install is already in progress"));

        const std::string source = request["source"].string();
        Json pkg;
        std::string url;
        std::string sha;
        std::string file;
        uint64_t listedSize = 0;
        if (source == "catalog") {
            pkg = request["pkg"];
            if (!pkg.isObject() || !pkg["id"].isString() || !pkg["version"].isString() ||
                !pkg["ipkUrl"].isString())
                return refuse(msg("srv.install.catalogIncomplete", "the catalog entry is incomplete"));
            url = pkg["ipkUrl"].string();
            sha = pkg["sha256"].isString() ? pkg["sha256"].string() : std::string();
            if (pkg["size"].isNumber() && pkg["size"].number() > 0)
                listedSize = static_cast<uint64_t>(pkg["size"].number());
        } else if (source == "url") {
            if (!request["url"].isString())
                return refuse(msg("srv.fetch.badScheme",
                                  "the address must start with http:// or https://"));
            url = request["url"].string();
            sha = request["sha256"].isString() ? request["sha256"].string() : std::string();
        } else if (source == "file") {
            file = request["file"].string();
            if (!uploadFileName(file))
                return refuse(msg("srv.install.notStaged", "the file is not in the staging directory"));
        } else {
            return refuse(msg("srv.install.badSource", "unknown install source"));
        }

        if (deps.ensureStaging) deps.ensureStaging();
        const JobPointer j = std::make_shared<InstallJob>();
        j->id = deps.newJobId();
        j->state = url.empty() ? "verifying" : "downloading";
        j->source = source;
        j->pkg = pkg;
        j->expectedSha = sha;
        j->file = file.empty() ? j->id + ".ipk" : file;
        j->progress = Json::object();
        j->progress.set("bytes", Json::numeric(0));
        j->progress.set("total", listedSize ? Json::numeric(static_cast<double>(listedSize)) : Json());
        // URL and file installs always stop at the preview: nothing else vouches for them.
        j->autoInstall = source == "catalog" && request["auto"].isBoolean() && request["auto"].boolean();
        job = j;
        std::string error;
        if (!persistLocked(j, error)) {
            if (job == j) job.reset();
            return refuse(error);
        }
        InstallReply reply;
        reply.ok = true;
        reply.install = snapshotLocked();
        if (url.empty())
            spawn([this, j](Lock &lock) { proceedVerify(lock, j); });
        else
            spawn([this, j, url, listedSize](Lock &lock) { runDownload(lock, j, url, listedSize); });
        return reply;
    }

    void runDownload(Lock &lock, const JobPointer &j, const std::string &url, uint64_t listedSize) {
        if (!isCurrent(j)) return;
        lock.unlock();
        const InstallDownloadOutcome outcome = deps.download(
            url, j->file, listedSize,
            [this, j](uint64_t bytes) {
                Lock guard(mutex);
                if (!isCurrent(j) || !j->progress.isObject()) return;
                j->progress.set("bytes", Json::numeric(static_cast<double>(bytes)));
                if (deps.publish) deps.publish(snapshotLocked());
            },
            [j]() { return j->cancelFlag->load(); });
        lock.lock();
        if (!isCurrent(j)) return;
        if (!outcome.ok) {
            failLocked(j, downloadError(outcome, url));
            return;
        }
        proceedVerify(lock, j);
    }

    std::string downloadError(const InstallDownloadOutcome &outcome, const std::string &url) const {
        // A cut-off is the size limit speaking, whatever status the remote sent.
        if (outcome.maxBytesExceeded) {
            if (outcome.noSpace)
                return msg("srv.install.noSpace.download",
                           "not enough free space for this download: {free} free, and the package needs about three times its size",
                           {{"free", formatMb(static_cast<double>(outcome.freeBytes))}});
            return tooBig(deps.uploadBytes);
        }
        std::string text;
        if (outcome.remoteStatus >= 400) {
            text = isGithub(url) ? githubSaid(outcome.remoteStatus, url)
                                 : hostSaid(outcome.remoteStatus, url);
        } else if (outcome.timedOut) {
            text = msg("srv.fetch.stalled",
                       "the download from {host} stalled or ran past {minutes} minutes",
                       {{"host", hostOf(url)}, {"minutes", "30"}});
        } else if (outcome.offline) {
            text = msg("srv.fetch.offline",
                       "The TV could not reach {host}. Check it is connected to the internet.",
                       {{"host", isGithub(url) ? "GitHub" : hostOf(url)}});
        } else {
            text = outcome.message.empty() ? unknownError() : outcome.message;
        }
        return msg("srv.install.downloadFailed", "the download failed: {error}", {{"error", text}});
    }

    /* --------------------------------------------------------- verify */

    void proceedVerify(Lock &lock, const JobPointer &j) {
        std::string error;
        if (!transitionLocked(j, "verifying", error)) {
            failLocked(j, error);
            return;
        }
        if (!isCurrent(j)) return;
        uint64_t bytes = 0;
        lock.unlock();
        const bool statted = deps.fileSize && deps.fileSize(j->file, bytes);
        lock.lock();
        if (!isCurrent(j)) return;
        if (!statted) {
            failLocked(j, unreadable("the staged file could not be read"));
            return;
        }
        if (bytes > deps.uploadBytes) {
            failLocked(j, tooBig(deps.uploadBytes));
            return;
        }
        j->size = bytes;
        const std::string expectedId = j->pkg.valid() ? j->pkg["id"].string() : std::string();
        lock.unlock();
        const Json inspected = deps.inspect(j->file, expectedId,
                                            isSha256(j->expectedSha) ? j->expectedSha : std::string());
        lock.lock();
        if (!isCurrent(j)) return;
        finishInspection(lock, j, inspected);
    }

    void finishInspection(Lock &lock, const JobPointer &j, const Json &inspected) {
        if (inspected["ok"].isBoolean() && !inspected["ok"].boolean()) {
            const std::string code = inspected["code"].string();
            const std::string id = inspected["id"].string();
            const std::string package = inspected["package"].string();
            if (code == "hash-mismatch")
                return failLocked(j, msg("srv.install.hashMismatch",
                                         "the download does not match its sha256 hash"));
            if (code == "catalog-id-mismatch") {
                std::string expected = inspected["expected"].isString() ? inspected["expected"].string() : id;
                if (expected.empty() && j->pkg.valid()) expected = j->pkg["id"].string();
                const std::string found = inspected["actual"].isString() ? inspected["actual"].string() : package;
                return failLocked(j, msg("srv.install.idMismatch",
                                         "the catalog lists it as {expected}, but what it downloads is a different app, {found}. Whoever runs the catalog needs to correct it.",
                                         {{"expected", expected}, {"found", found}}));
            }
            if (code == "id-prefix")
                return failLocked(j, msg("srv.install.id.prefix",
                                         "{id} does not begin with the package name {package}",
                                         {{"id", id}, {"package", package}}));
            if (code == "self-updating")
                return failLocked(j, msg("srv.install.id.self",
                                         "{id} must be updated from the Homebrew Channel", {{"id", id}}));
            if (code == "protected")
                return failLocked(j, msg("srv.install.id.protected",
                                         "{id} belongs to the TV system and cannot be installed",
                                         {{"id", id}}));
            if (code == "system-existing")
                return failLocked(j, msg("srv.install.id.exists",
                                         "{id} is already part of the TV system", {{"id", id}}));
            if (code == "invalid-metadata")
                return failLocked(j, unreadable("invalid package policy metadata"));
            if (!code.empty())
                return failLocked(j, "native app package policy rejected " +
                                         (id.empty() ? std::string("package metadata") : id));
            return failLocked(j, inspected["error"].isString() && !inspected["error"].string().empty()
                                     ? inspected["error"].string()
                                     : std::string("native app package inspection returned invalid metadata"));
        }
        const Json info = inspected["info"];
        if (!inspected["ok"].boolean() || !info.isObject() || !isSha256(info["sha256"].string()) ||
            !info["apps"].isArray() || !info["services"].isArray() || !info["machines"].isArray())
            return failLocked(j, inspected["error"].isString() && !inspected["error"].string().empty()
                                     ? inspected["error"].string()
                                     : std::string("native app package inspection returned invalid metadata"));
        if (!info["package"].isString() || !info["version"].isString())
            return failLocked(j, unreadable("native inspector returned invalid metadata"));
        j->digest = lowerHex(info["sha256"].string());
        bool cpuMismatch = false;
        if (!deps.tvMachine.empty() && info["machines"].size() > 0) {
            cpuMismatch = true;
            for (const Json &machine : info["machines"].items())
                if (machine.string() == deps.tvMachine) cpuMismatch = false;
        }
        Json stored = copyOf(info);
        stored.set("cpuMismatch", Json::flag(cpuMismatch));
        j->info = stored;
        buildPreview(lock, j);
    }

    /* -------------------------------------------------------- preview */

    void buildPreview(Lock &lock, const JobPointer &j) {
        const Json info = j->info;
        std::vector<std::string> previous;
        if (vetted(j) && deps.readElevated) {
            std::vector<std::string> saved;
            std::string error;
            lock.unlock();
            const bool read = deps.readElevated(info["package"].string(), saved, error);
            lock.lock();
            if (!isCurrent(j)) return;
            if (!read)
                return failLocked(j, msg("srv.install.elevationState",
                                         "could not read saved service access for this package"));
            for (const Json &service : info["services"].items())
                if (std::find(saved.begin(), saved.end(), service.string()) != saved.end())
                    previous.push_back(service.string());
        }

        Json installedList = Json::array();
        lock.unlock();
        Json listed;
        const bool haveList = deps.installedApps && deps.installedApps(listed);
        lock.lock();
        if (!isCurrent(j)) return;
        if (haveList && listed.isArray()) {
            for (const Json &item : listed.items()) {
                if (!item["id"].isString() || !nameLike(item["id"].string()) ||
                    item["id"].string().size() > 128)
                    continue;
                Json app = Json::object();
                app.set("id", item["id"]);
                app.set("version", Json::text(item["version"].isString() ? item["version"].string()
                                                                         : std::string()));
                installedList.add(app);
            }
        }

        Json reelevate = Json::array();
        for (const std::string &name : previous) reelevate.add(Json::text(name));
        Json input = Json::object();
        input.set("jobId", Json::text(j->id));
        input.set("source", Json::text(j->source));
        input.set("package", info["package"]);
        input.set("version", info["version"]);
        input.set("apps", info["apps"]);
        input.set("installedApps", installedList);
        input.set("services", info["services"]);
        input.set("rootRequired", Json::flag(j->pkg.valid() && truthy(j->pkg["rootRequired"])));
        input.set("pkgVetted", Json::flag(j->pkg.valid() && truthy(j->pkg["vetted"])));
        input.set("reelevate", reelevate);
        input.set("cpuMismatch", Json::flag(info["cpuMismatch"].boolean()));
        input.set("packageBytes", Json::numeric(static_cast<double>(j->size)));
        input.set("sha256", Json::text(j->digest));

        lock.unlock();
        const Json built = deps.buildPreview(input);
        lock.lock();
        if (!isCurrent(j)) return;
        const Json preview = built["preview"];
        if (!built.isObject() || !built["ok"].boolean() || !preview.isObject() ||
            !built["needsReview"].isBoolean() || !built["elevationAvailable"].isBoolean() ||
            !built["capacityOkay"].isBoolean() || preview["jobId"].string() != j->id ||
            !preview["apps"].isArray() || !preview["services"].isArray() ||
            !preview["reelevate"].isArray())
            return failLocked(j, installFailed(unknownError()));
        if (!built["capacityOkay"].boolean()) {
            if (!preview["needBytes"].isNumber() || !preview["freeBytes"].isNumber())
                return failLocked(j, installFailed(unknownError()));
            return failLocked(j, noSpace(preview["needBytes"].number(), preview["freeBytes"].number()));
        }
        finishPreview(lock, j, copyOf(preview), built["needsReview"].boolean(),
                      built["elevationAvailable"].boolean());
    }

    void finishPreview(Lock &, const JobPointer &j, const Json &preview, bool reviewRequired,
                       bool elevationAvailable) {
        j->preview = preview;
        std::string error;
        if (!transitionLocked(j, "awaiting-confirm", error)) {
            failLocked(j, error);
            return;
        }
        if (!isCurrent(j)) return;
        if (j->autoInstall && !reviewRequired) {
            Json autoRequest = Json::object();
            autoRequest.set("jobId", Json::text(j->id));
            const Json asked = j->preview["willElevate"];
            autoRequest.set("nativeValidated", Json::flag(true));
            if (truthy(j->preview["rootRequired"]) && elevationAvailable) {
                autoRequest.set("elevate", Json::flag(true));
                j->preview.set("willElevate", Json::flag(true));
            }
            const InstallReply reply = confirmLocked(autoRequest);
            if (!reply.ok && isCurrent(j)) {
                j->preview.set("willElevate", asked);
                armPreviewTimeoutLocked(j);
            }
            return;
        }
        armPreviewTimeoutLocked(j);
    }

    /* -------------------------------------------------------- confirm */

    static bool sameNames(const Json &provided, const Json &expected) {
        if (!provided.isArray() || !expected.isArray() || provided.size() != expected.size())
            return false;
        std::vector<std::string> left, right;
        for (const Json &value : provided.items()) left.push_back(value.string());
        for (const Json &value : expected.items()) right.push_back(value.string());
        std::sort(left.begin(), left.end());
        std::sort(right.begin(), right.end());
        return left == right;
    }

    InstallReply confirmLocked(const Json &request) {
        const JobPointer j = job;
        if (!j || !request["jobId"].isString() || j->id != request["jobId"].string() ||
            j->state != "awaiting-confirm")
            return refuse(noJobText());
        const bool wantRoot = request["elevate"].isBoolean() && request["elevate"].boolean();
        const bool validated = request["nativeValidated"].isBoolean() && request["nativeValidated"].boolean();
        if (!validated) {
            if (truthy(j->preview["storeInstalled"]) &&
                !(request["replaceStore"].isBoolean() && request["replaceStore"].boolean()))
                return refuse(msg("srv.install.storeReplace",
                                  "this app came from the LG store; replacing it stops store updates and needs separate confirmation"));
            if (wantRoot && !(deps.elevationAvailable && deps.elevationAvailable()))
                return refuse(msg("srv.install.noElevate",
                                  "root access needs the Homebrew Channel, which is not installed"));
            // Root for a package nobody vetted has to be confirmed for the
            // services the preview listed, so a client cannot ask for it blind.
            if (wantRoot && !vetted(j) && !sameNames(request["confirmRoot"], j->info["services"]))
                return refuse(msg("srv.install.rootConfirm",
                                  "root access for a package from outside the catalog must be confirmed for the services listed in its preview"));
        }
        // A reinstall rewrites the service files that elevation changed.
        std::vector<std::string> services;
        Json source;
        if (validated && request["nativeServices"].isArray()) source = request["nativeServices"];
        else if (wantRoot) source = j->info["services"];
        else if (j->preview.isObject() && j->preview["reelevate"].isArray()) source = j->preview["reelevate"];
        if (source.isArray())
            for (const Json &name : source.items()) services.push_back(name.string());

        clearPreviewTimerLocked();
        std::string error;
        if (!transitionLocked(j, "installing", error)) {
            armPreviewTimeoutLocked(j);
            return refuse(error);
        }
        if (!isCurrent(j)) return refuse(noJobText());
        InstallReply reply;
        reply.ok = true;
        reply.install = snapshotLocked();
        if (deps.publish) deps.publish(reply.install);
        spawn([this, j, services](Lock &lock) { runInstall(lock, j, services); });
        return reply;
    }

    static std::string installOutcomeError(const Json &result, bool &installed) {
        installed = false;
        if (!result.isObject() || !result["ok"].isBoolean() || !result["ok"].boolean()) {
            const std::string text = shortText(result["error"]);
            return installFailed(text.empty() ? unknownError() : text);
        }
        const std::string outcome = result["outcome"].string();
        if (outcome == "installed") {
            installed = true;
            return std::string();
        }
        if (outcome == "failed") {
            const std::string text = shortText(result["error"]);
            return installFailed(text.empty() ? unknownError() : text);
        }
        if (outcome == "timeout")
            return installFailed(msg("srv.install.timeout",
                                     "the install did not finish in time; check the app list"));
        if (outcome == "closed")
            return installFailed(msg("srv.install.closed", "the install service stopped answering"));
        return installFailed(unknownError());
    }

    void runInstall(Lock &lock, const JobPointer &j, const std::vector<std::string> &services) {
        const std::string package = j->info["package"].string();
        const std::string digest = j->digest;
        lock.unlock();
        const Json result = deps.install(package, j->file, digest, deps.installTimeout.count());
        lock.lock();
        if (!isCurrent(j)) return;
        bool installed = false;
        const std::string problem = installOutcomeError(result, installed);
        if (!installed) {
            failLocked(j, problem);
            return;
        }
        if (services.empty()) {
            finishJob(lock, j, Json());
            return;
        }
        setStateLocked(j, "elevating");
        lock.unlock();
        const Json elevation = deps.elevate(package, services);
        lock.lock();
        if (!isCurrent(j)) return;
        if (elevation["done"].isArray() && elevation["done"].size() > 0) {
            std::string error;
            lock.unlock();
            const bool saved = deps.writeElevated && deps.writeElevated(package, elevation["done"], error);
            lock.lock();
            if (!saved) warn("installer: could not persist elevated service state: " + error);
        }
        finishJob(lock, j, elevation);
    }

    static std::string appTitle(const JobPointer &j) {
        const Json first = j->info["apps"][static_cast<size_t>(0)];
        if (first["title"].isString() && !first["title"].string().empty() &&
            first["title"].string() != first["id"].string())
            return first["title"].string();
        if (j->pkg.valid() && truthy(j->pkg["title"])) return j->pkg["title"].string();
        return j->info["package"].string();
    }

    void finishJob(Lock &lock, const JobPointer &j, const Json &elevation) {
        if (!isCurrent(j)) return;
        if (deps.clearStaging) deps.clearStaging();
        Json apps = Json::array();
        for (const Json &app : j->info["apps"].items()) apps.add(app["id"]);
        Json result = Json::object();
        result.set("package", j->info["package"]);
        result.set("title", Json::text(appTitle(j)));
        result.set("version", j->info["version"]);
        result.set("apps", apps);
        result.set("elevation", elevation.valid() ? copyOf(elevation) : Json());
        j->result = result;
        setStateLocked(j, "installed");
        if (deps.onInstalled) {
            lock.unlock();
            deps.onInstalled();
            lock.lock();
        }
    }

    /* --------------------------------------------------------- cancel */

    InstallReply cancel(const std::string &jobId) {
        Lock lock(mutex);
        const JobPointer j = job;
        if (!j || j->id != jobId) return refuse(noJobText());
        if (j->state != "downloading" && j->state != "verifying" && j->state != "awaiting-confirm")
            return refuse(msg("srv.install.cannotCancel",
                              "the install is already running and cannot be cancelled"));
        j->cancelled = true;
        j->cancelFlag->store(true);
        clearPreviewTimerLocked();
        if (deps.clearStaging) deps.clearStaging();
        job.reset();
        std::string error;
        if (!deps.clearJob || !deps.clearJob(error)) return refuse(error.empty() ? "installer state could not be cleared" : error);
        if (deps.publish) deps.publish(snapshotLocked());
        InstallReply reply;
        reply.ok = true;
        return reply;
    }

    void recover() {
        if (deps.clearStaging) deps.clearStaging();
        Json record;
        bool found = false;
        std::string error;
        if (!deps.recoverJob || !deps.recoverJob(record, found, error)) {
            warn("installer: job recovery failed: " + error);
            found = false;
        }
        // A preview left waiting by the last run lost its staged file with the
        // clear above; its record would refuse every new job as still active.
        if (!found && deps.clearJob && !deps.clearJob(error))
            warn("installer: could not clear a stale job record: " + error);
        Lock lock(mutex);
        if (found && record["state"].string() == "interrupted") {
            const JobPointer j = std::make_shared<InstallJob>();
            j->id = record["jobId"].isString() ? record["jobId"].string() : std::string();
            j->state = "interrupted";
            j->error = msg("srv.install.interrupted",
                           "the TV restarted during an install; check the app list");
            j->hasError = true;
            job = j;
        }
        if (deps.publish) deps.publish(snapshotLocked());
    }
};

InstallJobs::InstallJobs(InstallJobsDeps deps)
    : core_(std::make_shared<InstallJobsCore>(std::move(deps))) {}

InstallJobs::~InstallJobs() {
    std::lock_guard<std::mutex> lock(core_->mutex);
    core_->shutdown = true;
    if (core_->job) core_->job->cancelFlag->store(true);
    core_->condition.notify_all();
}

InstallReply InstallJobs::start(const Json &request) { return core_->start(request); }

InstallReply InstallJobs::confirm(const Json &request) {
    std::lock_guard<std::mutex> lock(core_->mutex);
    return core_->confirmLocked(request);
}

InstallReply InstallJobs::cancel(const std::string &jobId) { return core_->cancel(jobId); }

Json InstallJobs::snapshot() const {
    std::lock_guard<std::mutex> lock(core_->mutex);
    return core_->snapshotLocked();
}

bool InstallJobs::busy() const {
    std::lock_guard<std::mutex> lock(core_->mutex);
    return core_->busyLocked();
}

bool InstallJobs::working() const {
    std::lock_guard<std::mutex> lock(core_->mutex);
    return core_->workingLocked();
}

void InstallJobs::recover() { core_->recover(); }

void InstallJobs::waitIdle() const {
    std::unique_lock<std::mutex> lock(core_->mutex);
    core_->condition.wait(lock, [this]() { return core_->workers == 0; });
}

/* -------------------------------------------------------------------------
 * The daemon's services behind the coordinator
 * ---------------------------------------------------------------------- */

namespace {
std::string joinPath(const std::string &directory, const std::string &name) {
    if (!directory.empty() && directory[directory.size() - 1] == '/') return directory + name;
    return directory + "/" + name;
}

std::string rootedPath(const std::string &root, const std::string &path) {
    if (root.empty() || root == "/") return path;
    size_t end = root.size();
    while (end > 1 && root[end - 1] == '/') --end;
    return root.substr(0, end) + path;
}

std::string elevateServicePath(const std::string &systemRoot) {
    const char *configured = std::getenv("TVWEB_ELEVATE_SERVICE");
    return configured && configured[0]
        ? std::string(configured)
        : rootedPath(systemRoot,
            "/media/developer/apps/usr/palm/services/org.webosbrew.hbchannel.service/elevate-service");
}

// The TV's own CPU is read from one of its binaries, not uname: some models
// report aarch64 with a 32-bit armhf userspace.
std::string machineOf(const std::string &path) {
    const int descriptor = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (descriptor < 0) return std::string();
    unsigned char header[20] = {};
    const ssize_t count = pread(descriptor, header, sizeof(header), 0);
    close(descriptor);
    if (count != 20 || header[0] != 0x7f || header[1] != 'E' || header[2] != 'L' || header[3] != 'F')
        return std::string();
    const unsigned code = header[5] == 2 ? (header[18] << 8 | header[19]) : (header[19] << 8 | header[18]);
    switch (code) {
    case 3: return "x86";
    case 8: return "mips";
    case 40: return "arm";
    case 62: return "x86_64";
    case 183: return "aarch64";
    case 243: return "riscv";
    default: return "e_machine " + std::to_string(code);
    }
}

Json listApps(const InstallJobsEnvironment &environment) {
    Json reply;
    if (!environment.luna ||
        !environment.luna("com.webos.applicationManager/listApps", Json::object(), reply))
        return Json();
    const Json rows = reply["apps"].isArray() ? reply["apps"] : reply["launchPoints"];
    if (!rows.isArray()) return Json();
    Json apps = Json::array();
    for (const Json &item : rows.items()) {
        if (!item["id"].isString() || item["id"].string().empty()) continue;
        Json app = Json::object();
        app.set("id", item["id"]);
        app.set("version", item["version"].isString() ? item["version"] : Json::text(""));
        apps.add(app);
    }
    return apps;
}

Json errorReply(const std::string &text) {
    Json reply = Json::object();
    reply.set("ok", Json::flag(false));
    reply.set("error", Json::text(text));
    return reply;
}
} // namespace

InstallJobsDeps productionInstallJobsDeps(const InstallJobsEnvironment &environment) {
    InstallJobsDeps deps;
    const InstallJobsEnvironment env = environment;
    deps.writeJob = [env](const Json &record, std::string &error) {
        return transitionInstallerJob(env.jobPath, record, &error);
    };
    deps.clearJob = [env](std::string &error) { return clearInstallerJob(env.jobPath, &error); };
    deps.recoverJob = [env](Json &record, bool &found, std::string &error) {
        return recoverInstallerJob(env.jobPath, record, found, &error);
    };
    deps.publish = env.publish;
    deps.warn = env.warn;
    deps.onInstalled = env.onInstalled;
    deps.nowMs = []() {
        return static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    };
    deps.newJobId = []() {
        static std::atomic<unsigned long long> sequence{0};
        unsigned char bytes[8] = {};
        const int descriptor = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
        const ssize_t count = descriptor >= 0 ? read(descriptor, bytes, sizeof(bytes)) : -1;
        if (descriptor >= 0) close(descriptor);
        if (count != static_cast<ssize_t>(sizeof(bytes))) {
            // ponytail: a clock-and-counter id if urandom is unreadable; unique is all a job id needs.
            const unsigned long long fallback =
                static_cast<unsigned long long>(std::chrono::steady_clock::now().time_since_epoch().count()) ^
                (++sequence * 0x9E3779B97F4A7C15ULL);
            for (size_t index = 0; index < sizeof(bytes); ++index)
                bytes[index] = static_cast<unsigned char>(fallback >> (index * 8));
        }
        static const char digits[] = "0123456789abcdef";
        std::string id;
        for (unsigned char byte : bytes) {
            id.push_back(digits[byte >> 4]);
            id.push_back(digits[byte & 15]);
        }
        return id;
    };
    deps.tvMachine = machineOf(env.lunaSendPath);
    if (deps.tvMachine.empty()) deps.tvMachine = machineOf("/proc/self/exe");

    deps.ensureStaging = [env]() {
        // ponytail: one level; the staging folder's parent is the TV's temp area.
        (void)mkdir(env.stagingDirectory.c_str(), 0700);
    };
    deps.clearStaging = [env]() {
        DIR *directory = opendir(env.stagingDirectory.c_str());
        if (!directory) return;
        std::vector<std::string> names;
        while (const dirent *entry = readdir(directory)) {
            const std::string name = entry->d_name;
            if (name != "." && name != "..") names.push_back(name);
        }
        closedir(directory);
        for (const std::string &name : names) (void)unlink(joinPath(env.stagingDirectory, name).c_str());
    };
    deps.fileSize = [env](const std::string &name, uint64_t &bytes) {
        struct stat info = {};
        if (stat(joinPath(env.stagingDirectory, name).c_str(), &info) != 0) return false;
        bytes = static_cast<uint64_t>(info.st_size);
        return true;
    };
    deps.elevationAvailable = [env]() { return access(elevateServicePath(env.systemRoot).c_str(), F_OK) == 0; };

    deps.download = [env](const std::string &url, const std::string &fileName, uint64_t expectedBytes,
                          const std::function<void(uint64_t)> &progress,
                          const std::function<bool()> &cancelled) {
        InstallDownloadOutcome outcome;
        std::string urlError;
        if (!service::AppPackageFetcher::validUrl(url, urlError)) {
            outcome.message = urlError;
            return outcome;
        }
        uint64_t maximumBytes = 512ULL * 1024 * 1024;
        uint64_t freeBytes = 0;
        const bool freeKnown = service::AppInstallSpace::availableBytes(env.stagingDirectory, freeBytes);
        service::AppInstallDownloadCapacity capacity;
        std::string capacityError;
        if (!service::AppInstallSpace::downloadCapacity(maximumBytes, expectedBytes, freeKnown,
                                                        freeBytes, capacity, &capacityError)) {
            outcome.message = capacityError.empty() ? "invalid app download capacity request" : capacityError;
            return outcome;
        }
        outcome.freeBytesKnown = capacity.freeBytesKnown;
        outcome.freeBytes = capacity.freeBytes;
        if (!capacity.capacityOkay) {
            outcome.message = "not enough free space for this download";
            outcome.maxBytesExceeded = true;
            outcome.noSpace = true;
            return outcome;
        }
        maximumBytes = capacity.maxDownloadBytes;
        const int directory = open(env.stagingDirectory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (directory < 0) {
            outcome.message = "could not open app install staging directory";
            return outcome;
        }
        const int output = openat(directory, fileName.c_str(),
                                  O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (output < 0) {
            close(directory);
            outcome.message = "could not create staged app package";
            return outcome;
        }
        uint64_t received = 0;
        uint64_t reported = 0;
        auto lastReport = std::chrono::steady_clock::now();
        std::string sinkError;
        const service::AppPackageDownloadSink sink = [&](const char *bytes, size_t length) {
            if (received > maximumBytes || length > maximumBytes - received) {
                sinkError = "download exceeds the allowed size";
                return false;
            }
            size_t offset = 0;
            while (offset < length) {
                const ssize_t written = write(output, bytes + offset, length - offset);
                if (written < 0 && errno == EINTR) continue;
                if (written <= 0) {
                    sinkError = "could not write staged app package";
                    return false;
                }
                offset += static_cast<size_t>(written);
            }
            received += length;
            const auto now = std::chrono::steady_clock::now();
            if (received - reported >= 64 * 1024 || now - lastReport >= std::chrono::milliseconds(250)) {
                progress(received);
                reported = received;
                lastReport = now;
            }
            return true;
        };
        long remoteStatus = 0;
        service::AppPackageDownloadFailure failure;
        bool downloaded = env.fetcher(url, env.userAgent, env.ipPreference ? env.ipPreference() : std::string(),
                                      maximumBytes, sink, remoteStatus, failure, cancelled);
        if (cancelled()) {
            downloaded = false;
            failure.message = "cancelled";
            outcome.cancelled = true;
        }
        if (!sinkError.empty()) failure.message = sinkError;
        if (downloaded && fsync(output) != 0) {
            failure.message = "could not flush staged app package";
            downloaded = false;
        }
        if (close(output) != 0 && downloaded) {
            failure.message = "could not close staged app package";
            downloaded = false;
        }
        if (!downloaded) {
            (void)unlinkat(directory, fileName.c_str(), 0);
            (void)fsync(directory);
            close(directory);
            outcome.message = failure.message.empty() ? "package download failed" : failure.message;
            outcome.offline = failure.offline;
            outcome.timedOut = failure.timedOut;
            outcome.remoteStatus = remoteStatus;
            outcome.maxBytesExceeded = failure.message == "download exceeds the allowed size";
            outcome.noSpace = outcome.maxBytesExceeded && capacity.spaceLimited;
            return outcome;
        }
        close(directory);
        progress(received);
        outcome.ok = true;
        return outcome;
    };

    deps.inspect = [env](const std::string &fileName, const std::string &expectedPackageId,
                         const std::string &expectedSha256) {
        Json metadata;
        std::string error;
        if (!service::AppPackageInspector::inspect(env.stagingDirectory, fileName, metadata, error))
            return errorReply(error.empty() ? "the ipk could not be read" : error);
        service::AppPackagePolicyFailure failure;
        const auto rejected = [&failure]() {
            Json reply = Json::object();
            reply.set("ok", Json::flag(false));
            reply.set("code", Json::text(failure.code));
            reply.set("id", Json::text(failure.id));
            reply.set("package", Json::text(failure.package));
            if (!failure.expected.empty()) reply.set("expected", Json::text(failure.expected));
            if (!failure.actual.empty()) reply.set("actual", Json::text(failure.actual));
            return reply;
        };
        if (!service::AppPackagePolicy::validate(metadata, service::AppPackagePolicy::systemAppDirectories(),
                                                 failure))
            return rejected();
        if (!service::AppPackagePolicy::validateCatalogSelection(metadata, expectedPackageId,
                                                                 expectedSha256, failure))
            return rejected();
        Json reply = Json::object();
        reply.set("ok", Json::flag(true));
        reply.set("info", metadata);
        return reply;
    };

    deps.buildPreview = [env](const Json &input) {
        const auto safe = [](const Json &value) {
            if (!value.isObject() || !value["package"].isString() ||
                !service::appPackageIdIsSafe(value["package"].string()) ||
                !value["apps"].isArray() || value["apps"].size() > 20000)
                return false;
            for (const Json &app : value["apps"].items())
                if (!app.isObject() || !app["id"].isString() ||
                    !service::appPackageIdIsSafe(app["id"].string()))
                    return false;
            return true;
        };
        if (!safe(input)) return Json();
        Json trusted = copyOf(input);
        uint64_t available = 0;
        if (service::AppInstallSpace::availableBytes(env.stagingDirectory, available))
            trusted.set("freeBytes", Json::numeric(static_cast<double>(available)));
        else
            trusted.set("freeBytes", Json());
        const auto storeApp = [&env](const std::string &id) {
            return access(rootedPath(env.systemRoot,
                                     "/media/cryptofs/apps/usr/palm/applications/" + id + "/appinfo.json")
                              .c_str(), F_OK) == 0;
        };
        bool storeInstalled = storeApp(input["package"].string());
        for (const Json &app : input["apps"].items())
            if (storeApp(app["id"].string())) storeInstalled = true;
        const bool elevationAvailable = access(elevateServicePath(env.systemRoot).c_str(), F_OK) == 0;
        trusted.set("storeInstalled", Json::flag(storeInstalled));
        trusted.set("elevationAvailable", Json::flag(elevationAvailable));
        Json preview;
        bool needsReview = false;
        bool capacityOkay = false;
        std::string error;
        if (!service::AppInstallPreview::build(trusted, preview, needsReview, capacityOkay, error))
            return Json();
        Json reply = Json::object();
        reply.set("ok", Json::flag(true));
        reply.set("preview", preview);
        reply.set("needsReview", Json::flag(needsReview));
        reply.set("elevationAvailable", Json::flag(elevationAvailable));
        reply.set("capacityOkay", Json::flag(capacityOkay));
        return reply;
    };

    deps.installedApps = [env](Json &apps) {
        apps = listApps(env);
        return apps.valid();
    };

    deps.readElevated = [env](const std::string &packageId, std::vector<std::string> &services,
                              std::string &error) {
        Json stored;
        if (!readElevatedServices(env.elevatedPath, packageId, stored, &error)) return false;
        if (access(elevateServicePath(env.systemRoot).c_str(), F_OK) != 0) return true;
        if (stored.isArray())
            for (const Json &name : stored.items()) services.push_back(name.string());
        return true;
    };
    deps.writeElevated = [env](const std::string &packageId, const Json &services, std::string &error) {
        return recordElevatedServices(env.elevatedPath, packageId, services, &error);
    };

    deps.install = [env](const std::string &packageId, const std::string &fileName,
                         const std::string &sha256, long long timeoutMs) {
        AppInstallResult result;
        std::string error;
        bool accepted = false;
        if (env.children) {
            AppInstallExecutor executor(*env.children, env.stagingDirectory, env.lunaSendPath);
            accepted = executor.run(packageId, fileName, sha256, std::chrono::milliseconds(timeoutMs),
                                    result, &error);
        } else {
            error = "child processes are unavailable";
        }
        if (!accepted) return errorReply(error.empty() ? "invalid app install request" : error);
        // The install service carries on after luna-send gives up, so a slow
        // eMMC can finish after the timeout; ask the app list before calling it failed.
        if (result.outcome == AppInstallOutcome::Closed || result.outcome == AppInstallOutcome::TimedOut) {
            const Json apps = listApps(env);
            if (apps.isArray()) {
                for (const Json &installed : apps.items()) {
                    const std::string id = installed["id"].string();
                    const std::string version = installed["version"].string();
                    bool matches = id == packageId && version == result.packageVersion;
                    for (const AppInstallAppVersion &app : result.apps)
                        if (id == app.id && (version == result.packageVersion ||
                                             (!app.version.empty() && version == app.version)))
                            matches = true;
                    if (matches) {
                        result.outcome = AppInstallOutcome::Installed;
                        result.error.clear();
                        break;
                    }
                }
            }
        }
        const char *outcome = "closed";
        switch (result.outcome) {
        case AppInstallOutcome::Installed: outcome = "installed"; break;
        case AppInstallOutcome::Failed: outcome = "failed"; break;
        case AppInstallOutcome::Closed: outcome = "closed"; break;
        case AppInstallOutcome::TimedOut: outcome = "timeout"; break;
        }
        Json reply = Json::object();
        reply.set("ok", Json::flag(true));
        reply.set("outcome", Json::text(outcome));
        if (!result.error.empty()) reply.set("error", Json::text(result.error));
        return reply;
    };

    deps.elevate = [env](const std::string &packageId, const std::vector<std::string> &services) {
        Json done = Json::array();
        Json failed = Json::array();
        AppElevationResult result;
        std::string error;
        bool ran = false;
        if (env.children) {
            AppElevationExecutor executor(*env.children, env.systemRoot);
            ran = executor.run(packageId, services, result, &error);
        }
        if (!ran) {
            for (const std::string &name : services) {
                Json item = Json::object();
                item.set("service", Json::text(name));
                item.set("error", Json::text(msg("srv.install.elevateFailed",
                                                  "could not give {service} root access: {error}",
                                                  {{"service", name}, {"error", error.empty() ? unknownError() : error}})));
                failed.add(item);
            }
        } else {
            for (const std::string &name : result.done) done.add(Json::text(name));
            for (const AppElevationFailure &failure : result.failed) {
                Json item = Json::object();
                item.set("service", Json::text(failure.service));
                item.set("error", Json::text(failure.error));
                failed.add(item);
            }
        }
        Json reply = Json::object();
        reply.set("done", done);
        reply.set("failed", failed);
        return reply;
    };
    return deps;
}

} // namespace daemon
} // namespace nativetv
