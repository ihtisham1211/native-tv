#include "service/home_image_proxy.hpp"
#include "core/md5.hpp"
#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cstdio>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <iomanip>
#include <limits>
#include <sstream>
#include <set>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <curl/curl.h>

namespace nativetv {
namespace service {
namespace {
const size_t kMaximumImageBytes = 8 * 1024 * 1024;
const int64_t kKeepImageMs = 7LL * 24 * 60 * 60 * 1000;
const unsigned kBackgroundPriority = 0;
const unsigned kForegroundPriority = 1;
const unsigned kFocusPriority = 2;
const unsigned kHeroPriority = 3;

bool initializeCurl() {
    static std::once_flag once;
    static CURLcode result = CURLE_FAILED_INIT;
    std::call_once(once, []() { result = curl_global_init(CURL_GLOBAL_DEFAULT); });
    return result == CURLE_OK;
}

size_t receiveImage(char *data, size_t size, size_t count, void *context) {
    if (size && count > std::numeric_limits<size_t>::max() / size) return 0;
    const size_t bytes = size * count;
    std::string *output = static_cast<std::string *>(context);
    if (bytes > kMaximumImageBytes - std::min(output->size(), kMaximumImageBytes))
        return 0;
    output->append(data, bytes);
    return bytes;
}

int cancelTransfer(void *context, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    const std::atomic<bool> *stopping =
        static_cast<const std::atomic<bool> *>(context);
    return stopping->load() ? 1 : 0;
}

bool imageFileExists(const std::string &path) {
    struct stat info = {};
    return lstat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode) && info.st_size > 0;
}

bool ensureDirectory(const std::string &path) {
    if (mkdir(path.c_str(), 0755) == 0 || errno == EEXIST) {
        struct stat info = {};
        return stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
    }
    return false;
}

std::string etagFor(const struct stat &info) {
#if defined(__APPLE__)
    const int64_t modified = static_cast<int64_t>(info.st_mtimespec.tv_sec) * 1000 +
                             info.st_mtimespec.tv_nsec / 1000000;
#else
    const int64_t modified = static_cast<int64_t>(info.st_mtim.tv_sec) * 1000 +
                             info.st_mtim.tv_nsec / 1000000;
#endif
    std::ostringstream etag;
    etag << '"' << std::hex << static_cast<uint64_t>(info.st_size) << '-'
         << static_cast<uint64_t>(modified) << '"';
    return etag.str();
}

int64_t systemNow() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string imageUrlFor(const Json &row, const Json &item) {
    const Json explicitImage = item["image"];
    if (explicitImage.isString() && !explicitImage.string().empty())
        return explicitImage.string();
    if (row["key"].string() == "trending") {
        const std::string backdrop = item["backdrop"].string();
        return backdrop.empty() ? "" :
            "https://image.tmdb.org/t/p/w500" + backdrop;
    }
    const std::string poster = item["poster"].string();
    return poster.empty() ? "" : "https://image.tmdb.org/t/p/w342" + poster;
}
} // namespace

HomeImageProxy::HomeImageProxy(std::string dataRoot, Fetcher fetcher, Clock clock)
    : imageDir_((dataRoot.empty() ? "/var/lib/tvweb" : std::move(dataRoot)) +
                "/home-images"),
      fetcher_(std::move(fetcher)), clock_(std::move(clock)) {
    if (!clock_) clock_ = systemNow;
    (void)ensureDirectory(imageDir_);
    workers_.emplace_back(&HomeImageProxy::workerLoop, this);
    workers_.emplace_back(&HomeImageProxy::workerLoop, this);
}

HomeImageProxy::~HomeImageProxy() {
    stopping_.store(true);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const std::shared_ptr<Job> &job : queued_) {
            job->done = true;
            job->succeeded = false;
            job->changed.notify_all();
        }
        queued_.clear();
    }
    changed_.notify_all();
    for (std::thread &worker : workers_)
        if (worker.joinable()) worker.join();
}

bool HomeImageProxy::allowedUrl(const std::string &url) {
    const std::string tmdb = "https://image.tmdb.org/t/p/w";
    const std::string youtube = "https://i.ytimg.com/vi/";
    const auto isName = [](const std::string &name) {
        return !name.empty() && std::all_of(name.begin(), name.end(),
            [](unsigned char value) {
                return (value >= 'a' && value <= 'z') ||
                       (value >= 'A' && value <= 'Z') ||
                       (value >= '0' && value <= '9') || value == '_' || value == '-';
            });
    };
    if (url.compare(0, tmdb.size(), tmdb) == 0) {
        const size_t slash = url.find('/', tmdb.size());
        if (slash == std::string::npos) return false;
        const std::string width = url.substr(tmdb.size(), slash - tmdb.size());
        if (width != "185" && width != "342" && width != "500" &&
            width != "780" && width != "1280") return false;
        const std::string path = url.substr(slash + 1);
        if (path.size() < 14 || path.size() > 44 ||
            path.compare(path.size() - 4, 4, ".jpg") != 0) return false;
        return isName(path.substr(0, path.size() - 4));
    }
    if (url.compare(0, youtube.size(), youtube) == 0) {
        const size_t slash = url.find('/', youtube.size());
        if (slash == std::string::npos) return false;
        const std::string id = url.substr(youtube.size(), slash - youtube.size());
        const std::string size = url.substr(slash + 1);
        return id.size() == 11 && isName(id) &&
               (size == "mqdefault.jpg" || size == "maxresdefault.jpg");
    }
    return false;
}

std::string HomeImageProxy::cacheFilename(const std::string &url) {
    if (!allowedUrl(url)) return "";
    return md5Hex(url) + ".jpg";
}

HomeImage HomeImageProxy::image(const std::string &url,
                                const std::string &priority) {
    HomeImage result;
    if (!allowedUrl(url) || stopping_.load() || !ensureDirectory(imageDir_))
        return result;
    unsigned rank = kForegroundPriority;
    if (priority == "hero") rank = kHeroPriority;
    else if (priority == "focus") rank = kFocusPriority;
    const std::string path = imageDir_ + "/" + cacheFilename(url);
    std::unique_lock<std::mutex> lock(mutex_);
    if (readCache(path, result)) return result;
    const std::shared_ptr<Job> job = enqueueLocked(url, rank);
    if (!job) return result;
    job->changed.wait(lock, [&]() { return job->done || stopping_.load(); });
    if (!job->succeeded || stopping_.load()) return result;
    lock.unlock();
    return readCache(path, result) ? result : HomeImage();
}

void HomeImageProxy::retainAndWarm(const Json &rows) {
    if (!rows.isArray() || stopping_.load() || !ensureDirectory(imageDir_)) return;
    std::vector<std::string> usedFiles;
    std::set<std::string> used;
    for (const Json &row : rows.items()) {
        for (const Json &item : row["items"].items()) {
            const std::string url = imageUrlFor(row, item);
            if (allowedUrl(url)) {
                const std::string filename = cacheFilename(url);
                if (used.insert(filename).second) {
                    usedFiles.push_back(filename);
                    warmUrl(url);
                }
            }
            if (row["key"].string() == "trending") {
                const std::string backdrop = item["backdrop"].string();
                if (!backdrop.empty()) {
                    const std::string url =
                        "https://image.tmdb.org/t/p/w1280" + backdrop;
                    if (allowedUrl(url)) {
                        const std::string filename = cacheFilename(url);
                        if (used.insert(filename).second) {
                            usedFiles.push_back(filename);
                            warmUrl(url);
                        }
                    }
                }
            }
            const std::string wide = item["wide"].string();
            if (allowedUrl(wide)) {
                const std::string filename = cacheFilename(wide);
                if (used.insert(filename).second) {
                    usedFiles.push_back(filename);
                    warmUrl(wide);
                }
            }
        }
    }
    cleanUnused(usedFiles);
}

void HomeImageProxy::workerLoop() {
    for (;;) {
        std::shared_ptr<Job> job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            changed_.wait(lock, [&]() { return stopping_.load() || !queued_.empty(); });
            if (stopping_.load()) return;
            size_t best = 0;
            for (size_t index = 1; index < queued_.size(); ++index)
                if (queued_[index]->priority > queued_[best]->priority) best = index;
            job = queued_[best];
            queued_.erase(queued_.begin() + static_cast<std::ptrdiff_t>(best));
        }

        std::string bytes, error;
        bool succeeded = false;
        try {
            succeeded = fetch(job->url, bytes, error) &&
                        !bytes.empty() && writeCache(job->path, bytes);
        } catch (...) {
            succeeded = false;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            job->succeeded = succeeded;
            job->done = true;
            const auto found = jobs_.find(job->url);
            if (found != jobs_.end() && found->second == job) jobs_.erase(found);
            job->changed.notify_all();
        }
    }
}

bool HomeImageProxy::fetch(const std::string &url, std::string &bytes,
                           std::string &error) {
    if (fetcher_) return fetcher_(url, bytes, error);
    error.clear();
    if (stopping_.load() || !initializeCurl()) return false;
    CURL *curl = curl_easy_init();
    if (!curl) return false;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 5000L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 20000L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS, CURLPROTO_HTTPS);
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTPS);
#pragma GCC diagnostic pop
    curl_easy_setopt(curl, CURLOPT_PROXY, "");
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "tvweb");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, receiveImage);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &bytes);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, cancelTransfer);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &stopping_);
    const CURLcode code = curl_easy_perform(curl);
    long status = 0;
    char *effectiveUrl = nullptr;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &effectiveUrl);
    const bool safeDestination = effectiveUrl && allowedUrl(effectiveUrl);
    curl_easy_cleanup(curl);
    if (code != CURLE_OK || status < 200 || status >= 300 || !safeDestination) {
        error = code == CURLE_OK ? "image source returned an invalid response"
                                 : curl_easy_strerror(code);
        return false;
    }
    return !bytes.empty();
}

bool HomeImageProxy::writeCache(const std::string &path,
                               const std::string &bytes) {
    const std::string temporary = path + ".part";
    const int fd = open(temporary.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
    if (fd < 0) return false;
    size_t written = 0;
    bool ok = true;
    while (written < bytes.size()) {
        const ssize_t count = write(fd, bytes.data() + written,
                                    bytes.size() - written);
        if (count <= 0) { ok = false; break; }
        written += static_cast<size_t>(count);
    }
    if (close(fd) != 0) ok = false;
    if (ok) {
        if (rename(temporary.c_str(), path.c_str()) == 0) return true;
    }
    (void)unlink(temporary.c_str());
    return false;
}

bool HomeImageProxy::readCache(const std::string &path,
                               HomeImage &image) const {
    struct stat info = {};
    if (lstat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode) ||
        info.st_size <= 0 || info.st_size > static_cast<off_t>(kMaximumImageBytes))
        return false;
#ifdef O_NOFOLLOW
    const int fd = open(path.c_str(), O_RDONLY | O_NOFOLLOW);
#else
    const int fd = open(path.c_str(), O_RDONLY);
#endif
    if (fd < 0) return false;
    struct stat opened = {};
    if (fstat(fd, &opened) != 0 || !S_ISREG(opened.st_mode) ||
        opened.st_dev != info.st_dev || opened.st_ino != info.st_ino ||
        opened.st_size <= 0 ||
        opened.st_size > static_cast<off_t>(kMaximumImageBytes)) {
        close(fd);
        return false;
    }
    std::vector<unsigned char> contents(static_cast<size_t>(opened.st_size));
    size_t offset = 0;
    while (offset < contents.size()) {
        const ssize_t count = read(fd, contents.data() + offset,
                                   contents.size() - offset);
        if (count <= 0) { close(fd); return false; }
        offset += static_cast<size_t>(count);
    }
    close(fd);
    image.found = true;
    image.bytes = std::move(contents);
    image.etag = etagFor(opened);
    return true;
}

std::shared_ptr<HomeImageProxy::Job> HomeImageProxy::enqueueLocked(
    const std::string &url, unsigned priority) {
    const auto found = jobs_.find(url);
    if (found != jobs_.end()) {
        if (priority > found->second->priority)
            found->second->priority = priority;
        return found->second;
    }
    if (stopping_.load()) return std::shared_ptr<Job>();
    std::shared_ptr<Job> job = std::make_shared<Job>();
    job->url = url;
    job->path = imageDir_ + "/" + cacheFilename(url);
    job->priority = priority;
    jobs_[url] = job;
    queued_.push_back(job);
    changed_.notify_one();
    return job;
}

void HomeImageProxy::warmUrl(const std::string &url) {
    if (!allowedUrl(url) || stopping_.load()) return;
    const std::string path = imageDir_ + "/" + cacheFilename(url);
    if (imageFileExists(path)) return;
    std::lock_guard<std::mutex> lock(mutex_);
    if (imageFileExists(path)) return;
    (void)enqueueLocked(url, kBackgroundPriority);
}

void HomeImageProxy::cleanUnused(const std::vector<std::string> &usedFiles) {
    DIR *directory = opendir(imageDir_.c_str());
    if (!directory) return;
    std::set<std::string> used(usedFiles.begin(), usedFiles.end());
    const int64_t nowMs = clock_();
    struct dirent *entry = nullptr;
    while ((entry = readdir(directory)) != nullptr) {
        const std::string name(entry->d_name);
        if (name == "." || name == ".." || used.find(name) != used.end()) continue;
        const std::string path = imageDir_ + "/" + name;
        struct stat info = {};
        if (lstat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode)) continue;
#if defined(__APPLE__)
        const int64_t modified = static_cast<int64_t>(info.st_mtimespec.tv_sec) * 1000 +
                                 info.st_mtimespec.tv_nsec / 1000000;
#else
        const int64_t modified = static_cast<int64_t>(info.st_mtim.tv_sec) * 1000 +
                                 info.st_mtim.tv_nsec / 1000000;
#endif
        if (nowMs - modified > kKeepImageMs) (void)unlink(path.c_str());
    }
    closedir(directory);
}

} // namespace service
} // namespace nativetv
