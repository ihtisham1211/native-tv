#include "service/home_discover.hpp"
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {
class TemporaryDirectory {
  public:
    TemporaryDirectory() {
        char pattern[] = "/tmp/nativetv-home-discover-XXXXXX";
        char *created = mkdtemp(pattern);
        assert(created);
        path_ = created;
    }
    ~TemporaryDirectory() {
        (void)unlink((path_ + "/home-discover.json").c_str());
        (void)unlink((path_ + "/home-discover.json.tmp").c_str());
        (void)rmdir(path_.c_str());
    }
    const std::string &path() const { return path_; }

  private:
    std::string path_;
};

nativetv::Json title(int id, const std::string &type = "movie") {
    using nativetv::Json;
    Json item = Json::object();
    item.set("id", Json::numeric(id));
    item.set("media_type", Json::text(type));
    item.set("title", Json::text("Title " + std::to_string(id)));
    item.set("name", Json::text("Show " + std::to_string(id)));
    item.set("release_date", Json::text("2026-04-03"));
    item.set("vote_average", Json::numeric(8.26));
    item.set("vote_count", Json::numeric(200));
    item.set("overview", Json::text("A description."));
    item.set("poster_path", Json::text("/poster-abcdefghijkl.jpg"));
    item.set("backdrop_path", Json::text("/backdrop-abcdefghi.jpg"));
    return item;
}

nativetv::Json titleReply(int firstId, const std::string &type = "movie") {
    using nativetv::Json;
    Json results = Json::array();
    results.add(title(firstId, type));
    results.add(title(firstId, type)); // Each row de-duplicates its own titles.
    results.add(title(firstId + 1, type));
    results.add(title(firstId + 2, type));
    Json missing = title(firstId + 3, type);
    missing.set("poster_path", Json::parse("null"));
    missing.set("backdrop_path", Json::parse("null"));
    results.add(missing);
    Json document = Json::object();
    document.set("results", results);
    return document;
}

nativetv::Json youtubeReply() {
    using nativetv::Json;
    Json items = Json::array();
    items.add(Json::parse(
        "{\"id\":\"dQw4w9WgXcQ\",\"snippet\":{\"title\":\"Video\",\"channelTitle\":\"Channel\"}}"));
    items.add(Json::parse(
        "{\"id\":\"invalid id\",\"snippet\":{\"title\":\"Invalid\"}}"));
    Json document = Json::object();
    document.set("items", items);
    return document;
}

int64_t localMilliseconds(int year, int month, int day, int hour) {
    std::tm date = {};
    date.tm_year = year - 1900;
    date.tm_mon = month - 1;
    date.tm_mday = day;
    date.tm_hour = hour;
    return static_cast<int64_t>(std::mktime(&date)) * 1000;
}
} // namespace

int main() {
    using namespace nativetv;
    using namespace nativetv::service;
    TemporaryDirectory directory;
    int64_t clock = localMilliseconds(2026, 10, 20, 9);
    std::mutex askedMutex;
    std::condition_variable askedChanged;
    std::vector<std::string> asked;
    bool failTrending = false;
    bool failAll = false;
    auto fetcher = [&](const std::string &url, Json &document, std::string &error) {
        std::lock_guard<std::mutex> lock(askedMutex);
        asked.push_back(url);
        askedChanged.notify_all();
        if (failAll || (failTrending && url.find("trending/all/day") != std::string::npos)) {
            error = "offline";
            return false;
        }
        if (url.find("googleapis.com") != std::string::npos) {
            document = youtubeReply();
        } else if (url.find("trending/all/day") != std::string::npos) {
            document = titleReply(1);
        } else if (url.find("trending/movie/week") != std::string::npos) {
            document = titleReply(11);
        } else if (url.find("trending/tv/week") != std::string::npos) {
            document = titleReply(21, "tv");
        } else {
            document = titleReply(31, "tv");
        }
        return true;
    };
    const auto waitForAsked = [&](size_t count) {
        std::unique_lock<std::mutex> lock(askedMutex);
        return askedChanged.wait_for(lock, std::chrono::seconds(2),
                                     [&asked, count] { return asked.size() >= count; });
    };

    {
        HomeDiscover discover(directory.path(), fetcher, [&clock] { return clock; });
        const Json disabled = discover.feed("", "");
        assert(disabled["ok"].boolean() && !disabled["enabled"].boolean());
        assert(disabled["rows"].isArray() && disabled["rows"].size() == 0);
        assert(asked.empty());

        const Json first = discover.feed("private-tmdb-key", "youtube-key");
        assert(first["enabled"].boolean() && first["titlesEnabled"].boolean());
        assert(first["rows"].size() == 0); // A cache miss never waits for network.
        (void)discover.feed("private-tmdb-key", "youtube-key");
        assert(waitForAsked(5));
    } // Joining the worker makes the completed refresh observable below.
    assert(asked.size() == 5);
    for (const std::string &url : asked)
        assert(url.find("private-tmdb-key") == std::string::npos ||
               url.find("https://api.themoviedb.org/3/") == 0);
    assert(asked[0].find("api_key=private-tmdb-key") != std::string::npos);
    assert(asked[3].find("air_date.gte=") != std::string::npos &&
           asked[3].find("air_date.lte=") != std::string::npos);
    assert(asked[4].find("key=youtube-key") != std::string::npos);

    struct stat cacheInfo = {};
    assert(stat((directory.path() + "/home-discover.json").c_str(), &cacheInfo) == 0);
    assert((cacheInfo.st_mode & 0777) == 0600);
    {
        std::ifstream input((directory.path() + "/home-discover.json").c_str());
        std::string saved((std::istreambuf_iterator<char>(input)),
                          std::istreambuf_iterator<char>());
        assert(saved.find("private-tmdb-key") == std::string::npos);
        assert(saved.find("youtube-key") == std::string::npos);
        const Json cache = Json::parse(saved);
        assert(!cache["partial"].boolean());
        assert(cache["rows"].size() == 5);
    }

    {
        HomeDiscover cached(directory.path(), fetcher, [&clock] { return clock; });
        const Json feed = cached.feed("private-tmdb-key", "youtube-key");
        assert(feed["rows"].size() == 7); // Four source rows, YouTube, and two derived rows.
        assert(feed["rows"][0]["key"].string() == "trending");
        assert(feed["rows"][0]["items"].size() == 3);
        assert(feed["rows"][0]["items"][0]["rating"].number() == 8.3);
        assert(feed["rows"][4]["key"].string() == "youtube");
        assert(feed["rows"][4]["items"][0]["image"].string() ==
               "https://i.ytimg.com/vi/dQw4w9WgXcQ/mqdefault.jpg");
        assert(feed["rows"][5]["key"].string() == "toprated");
        assert(feed["rows"][6]["key"].string() == "recent");
    }
    assert(asked.size() == 5); // A fresh cache does not refetch.

    failTrending = true;
    const std::string partialDir = directory.path() + "/partial";
    assert(mkdir(partialDir.c_str(), 0700) == 0);
    const size_t beforePartial = asked.size();
    {
        HomeDiscover partial(partialDir, fetcher, [&clock] { return clock; });
        assert(partial.feed("tmdb", "")["rows"].size() == 0);
        assert(waitForAsked(beforePartial + 4));
    }
    Json partialCache;
    {
        std::ifstream input((partialDir + "/home-discover.json").c_str());
        std::string saved((std::istreambuf_iterator<char>(input)),
                          std::istreambuf_iterator<char>());
        partialCache = Json::parse(saved);
    }
    assert(partialCache["partial"].boolean());
    assert(partialCache["rows"].size() == 3);
    failTrending = false;
    clock += 11 * 60 * 1000;
    const size_t beforeRetry = asked.size();
    {
        HomeDiscover retry(partialDir, fetcher, [&clock] { return clock; });
        const Json stale = retry.feed("tmdb", "");
        assert(stale["rows"].size() >= 3); // Existing rows are returned during refresh.
        assert(waitForAsked(beforeRetry + 4));
    }
    {
        HomeDiscover recovered(partialDir, fetcher, [&clock] { return clock; });
        const Json feed = recovered.feed("tmdb", "");
        assert(feed["rows"][0]["key"].string() == "trending");
        assert(feed["rows"].size() == 6); // Four TMDB rows plus derived rows.
    }

    failAll = true;
    clock = localMilliseconds(2026, 10, 21, 6);
    const size_t beforeStaleRefresh = asked.size();
    {
        HomeDiscover staleService(directory.path(), fetcher, [&clock] { return clock; });
        const Json stale = staleService.feed("private-tmdb-key", "youtube-key");
        assert(stale["rows"].size() == 7);
        assert(waitForAsked(beforeStaleRefresh + 5));
    }
    const size_t beforeStaleRetry = asked.size();
    {
        HomeDiscover staleCache(directory.path(), fetcher, [&clock] { return clock; });
        const Json stale = staleCache.feed("private-tmdb-key", "youtube-key");
        assert(stale["rows"].size() == 7);
        assert(waitForAsked(beforeStaleRetry + 5));
    }

    const std::string youtubeDir = directory.path() + "/youtube";
    assert(mkdir(youtubeDir.c_str(), 0700) == 0);
    asked.clear();
    failAll = false;
    {
        HomeDiscover youtubeOnly(youtubeDir, fetcher, [&clock] { return clock; });
        const Json first = youtubeOnly.feed("", "youtube only");
        assert(!first["titlesEnabled"].boolean());
        assert(first["rows"].size() == 0);
        std::unique_lock<std::mutex> lock(askedMutex);
        assert(askedChanged.wait_for(lock, std::chrono::seconds(2),
                                     [&asked] { return !asked.empty(); }));
    }
    assert(asked.size() == 1 && asked[0].find("googleapis.com") != std::string::npos);
    assert(asked[0].find("youtube%20only") != std::string::npos);
    {
        HomeDiscover youtubeCache(youtubeDir, fetcher, [&clock] { return clock; });
        const Json feed = youtubeCache.feed("", "youtube only");
        assert(feed["rows"].size() == 1);
        assert(feed["rows"][0]["key"].string() == "youtube");
        assert(feed["rows"][0]["items"].size() == 1);
    }
    assert(asked.size() == 1);

    (void)unlink((partialDir + "/home-discover.json").c_str());
    (void)unlink((partialDir + "/home-discover.json.tmp").c_str());
    (void)rmdir(partialDir.c_str());
    (void)unlink((youtubeDir + "/home-discover.json").c_str());
    (void)unlink((youtubeDir + "/home-discover.json.tmp").c_str());
    (void)rmdir(youtubeDir.c_str());
    return 0;
}
