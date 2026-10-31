#include "adapters/legacy_routes.hpp"
#include "service/home_image_proxy.hpp"
#include "core/md5.hpp"
#include <cassert>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <sys/stat.h>
#include <sys/time.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
class TemporaryDirectory {
  public:
    TemporaryDirectory() {
        char pattern[] = "/tmp/nativetv-home-image-XXXXXX";
        char *created = mkdtemp(pattern);
        assert(created);
        path_ = created;
    }
    ~TemporaryDirectory() {
        const std::string images = path_ + "/home-images";
        DIR *directory = opendir(images.c_str());
        if (directory) {
            struct dirent *entry = nullptr;
            while ((entry = readdir(directory)) != nullptr) {
                if (entry->d_name[0] == '.') continue;
                (void)unlink((images + "/" + entry->d_name).c_str());
            }
            closedir(directory);
            (void)rmdir(images.c_str());
        }
        (void)rmdir(path_.c_str());
    }
    const std::string &path() const { return path_; }

  private:
    std::string path_;
};

struct ControlledFetch {
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<std::string> started;
    std::map<std::string, size_t> releases;
    std::set<std::string> failures;
    bool blocked = false;
    bool releaseEverything = false;
    size_t active = 0;
    size_t peak = 0;

    bool operator()(const std::string &url, std::string &bytes,
                    std::string &error) {
        std::unique_lock<std::mutex> lock(mutex);
        started.push_back(url);
        ++active;
        if (active > peak) peak = active;
        changed.notify_all();
        if (blocked) {
            changed.wait(lock, [&]() {
                return releaseEverything || releases[url] > 0;
            });
            if (!releaseEverything) --releases[url];
        }
        --active;
        const bool fail = failures.find(url) != failures.end();
        changed.notify_all();
        lock.unlock();
        if (fail) {
            error = "offline";
            return false;
        }
        bytes = "jpeg bytes for " + url;
        return true;
    }

    bool waitForStarts(size_t count) {
        std::unique_lock<std::mutex> lock(mutex);
        return changed.wait_for(lock, std::chrono::seconds(2), [&]() {
            return started.size() >= count;
        });
    }

    void release(const std::string &url) {
        std::lock_guard<std::mutex> lock(mutex);
        ++releases[url];
        changed.notify_all();
    }

    void releaseAll() {
        std::lock_guard<std::mutex> lock(mutex);
        releaseEverything = true;
        changed.notify_all();
    }
};

std::string tmdbUrl(const std::string &name) {
    return "https://image.tmdb.org/t/p/w342/" + name + ".jpg";
}

nativetv::Json warmRows(const std::vector<std::string> &names) {
    using nativetv::Json;
    Json items = Json::array();
    for (const std::string &name : names) {
        Json item = Json::object();
        item.set("poster", Json::text("/" + name + ".jpg"));
        items.add(item);
    }
    Json row = Json::object();
    row.set("key", Json::text("movies"));
    row.set("items", items);
    Json rows = Json::array();
    rows.add(row);
    return rows;
}
} // namespace

std::string percentDecoded(const std::string &text) {
    std::string result;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '%' && i + 2 < text.size() + 0 && std::isxdigit(static_cast<unsigned char>(text[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(text[i + 2]))) {
            result += static_cast<char>(std::strtol(text.substr(i + 1, 2).c_str(), nullptr, 16));
            i += 2;
        } else
            result += text[i];
    }
    return result;
}

// What the client asks the server for must be a picture the server serves: a YouTube thumbnail at a
// size it refuses (hqdefault) is a card that never gets its picture.
void testTheClientAsksForPicturesTheServerServes() {
    const std::string youtube = nativetv::legacyAssetRoute("youtube:dQw4w9WgXcQ");
    const std::string prefix = "/api/home/image?u=";
    assert(youtube.compare(0, prefix.size(), prefix) == 0);
    assert(nativetv::service::HomeImageProxy::allowedUrl(
        percentDecoded(youtube.substr(prefix.size()))));
    for (const char *size : {"w342", "w500", "w1280"}) {
        const std::string tmdb = nativetv::legacyAssetRoute(std::string("tmdb:") + size + ":/abcdefghij1234567890.jpg");
        assert(nativetv::service::HomeImageProxy::allowedUrl(
            percentDecoded(tmdb.substr(prefix.size(), tmdb.find('&') == std::string::npos ? std::string::npos : tmdb.find('&') - prefix.size()))));
    }
}

int main() {
    using namespace nativetv;
    using namespace nativetv::service;
    testTheClientAsksForPicturesTheServerServes();
    const std::string valid = tmdbUrl("abcdefghijklmnop");
    assert(HomeImageProxy::allowedUrl(valid));
    assert(HomeImageProxy::allowedUrl(
        "https://image.tmdb.org/t/p/w1280/abcdefghij.jpg"));
    assert(HomeImageProxy::allowedUrl(
        "https://i.ytimg.com/vi/dQw4w9WgXcQ/mqdefault.jpg"));
    assert(!HomeImageProxy::allowedUrl(
        "https://evil.example/t/p/w342/abcdefghijklmnop.jpg"));
    assert(!HomeImageProxy::allowedUrl(
        "https://image.tmdb.org/t/p/w400/abcdefghijklmnop.jpg"));
    assert(!HomeImageProxy::allowedUrl(
        "https://image.tmdb.org/t/p/w342/../../secret.jpg"));
    assert(!HomeImageProxy::allowedUrl(
        "https://image.tmdb.org/t/p/w342/abcdefghijklmnop.jpg?x=1"));
    assert(!HomeImageProxy::allowedUrl(
        "https://i.ytimg.com/vi/short/mqdefault.jpg"));
    assert(HomeImageProxy::cacheFilename(valid) == md5Hex(valid) + ".jpg");

    TemporaryDirectory cachedDirectory;
    size_t fetchCount = 0;
    {
        HomeImageProxy proxy(cachedDirectory.path(),
            [&](const std::string &url, std::string &bytes, std::string &) {
                ++fetchCount;
                assert(url == valid);
                bytes = "jpeg fixture bytes";
                return true;
            });
        const HomeImage first = proxy.image(valid);
        assert(first.found && first.bytes.size() == 18);
        assert(!first.etag.empty());
        const HomeImage second = proxy.image(valid, "hero");
        assert(second.found && second.bytes == first.bytes);
        assert(fetchCount == 1);
        const HomeImage refused = proxy.image(
            "https://evil.example/image.jpg", "hero");
        assert(!refused.found && fetchCount == 1);
    }
    struct stat cacheInfo = {};
    assert(stat((cachedDirectory.path() + "/home-images/" +
                HomeImageProxy::cacheFilename(valid)).c_str(), &cacheInfo) == 0);
    assert((cacheInfo.st_mode & 0777) == 0600);

    TemporaryDirectory dedupeDirectory;
    ControlledFetch dedupe;
    dedupe.blocked = true;
    HomeImage dedupedFirst, dedupedSecond;
    {
        HomeImageProxy proxy(dedupeDirectory.path(),
            [&](const std::string &url, std::string &bytes, std::string &error) {
                return dedupe(url, bytes, error);
            });
        std::thread first([&]() { dedupedFirst = proxy.image(valid); });
        std::thread second([&]() { dedupedSecond = proxy.image(valid, "focus"); });
        assert(dedupe.waitForStarts(1));
        dedupe.release(valid);
        first.join();
        second.join();
        assert(dedupedFirst.found && dedupedSecond.found);
        assert(dedupe.started.size() == 1);
        assert(dedupe.peak == 1);
    }

    TemporaryDirectory priorityDirectory;
    ControlledFetch priority;
    priority.blocked = true;
    const std::string firstUrl = tmdbUrl("background0001");
    const std::string secondUrl = tmdbUrl("background0002");
    const std::string thirdUrl = tmdbUrl("background0003");
    const std::string heroUrl = tmdbUrl("background0004");
    HomeImage heroResult;
    {
        HomeImageProxy proxy(priorityDirectory.path(),
            [&](const std::string &url, std::string &bytes, std::string &error) {
                return priority(url, bytes, error);
            });
        proxy.retainAndWarm(warmRows({"background0001", "background0002",
                                      "background0003", "background0004"}));
        assert(priority.waitForStarts(2));
        std::thread visible([&]() { heroResult = proxy.image(heroUrl, "hero"); });
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        priority.release(firstUrl);
        assert(priority.waitForStarts(3));
        {
            std::lock_guard<std::mutex> lock(priority.mutex);
            assert(priority.started[0] == firstUrl);
            assert(priority.started[1] == secondUrl);
            assert(priority.started[2] == heroUrl);
        }
        priority.release(heroUrl);
        visible.join();
        assert(heroResult.found);
        assert(priority.waitForStarts(4));
        {
            std::lock_guard<std::mutex> lock(priority.mutex);
            assert(priority.started[3] == thirdUrl);
        }
        priority.releaseAll();
    }
    assert(priority.peak == 2);

    TemporaryDirectory sweepDirectory;
    const int64_t nowMs = 2000000000000LL;
    const std::string shownUrl = tmdbUrl("shownposter12345");
    const std::string shownPath = sweepDirectory.path() + "/home-images/" +
                                  HomeImageProxy::cacheFilename(shownUrl);
    const std::string gonePath = sweepDirectory.path() +
        "/home-images/oldoldoldoldoldoldoldoldoldold.jpg";
    assert(mkdir((sweepDirectory.path() + "/home-images").c_str(), 0700) == 0);
    for (const std::string &path : {shownPath, gonePath}) {
        std::ofstream output(path.c_str(), std::ios::binary);
        output << "jpeg";
    }
    struct timeval oldTimes[2] = {};
    oldTimes[0].tv_sec = static_cast<time_t>((nowMs - 30LL * 24 * 60 * 60 * 1000) / 1000);
    oldTimes[1] = oldTimes[0];
    assert(utimes(shownPath.c_str(), oldTimes) == 0);
    assert(utimes(gonePath.c_str(), oldTimes) == 0);
    {
        HomeImageProxy proxy(sweepDirectory.path(),
            [](const std::string &, std::string &bytes, std::string &) {
                bytes = "unexpected warm";
                return true;
            }, [] { return nowMs; });
        proxy.retainAndWarm(warmRows({"shownposter12345"}));
        assert(access(shownPath.c_str(), F_OK) == 0);
        assert(access(gonePath.c_str(), F_OK) != 0);
    }
    return 0;
}
