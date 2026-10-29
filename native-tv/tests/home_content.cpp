#include "service/home_content.hpp"
#include <cassert>
#include <cstdio>
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
        char pattern[] = "/tmp/nativetv-home-content-XXXXXX";
        char *created = mkdtemp(pattern);
        assert(created);
        path_ = created;
    }
    ~TemporaryDirectory() {
        (void)unlink((path_ + "/home-content.json").c_str());
        (void)unlink((path_ + "/home-content.json.tmp").c_str());
        (void)rmdir(path_.c_str());
    }
    const std::string &path() const { return path_; }

  private:
    std::string path_;
};

nativetv::Json rawTitle(int id, const std::string &type = "movie") {
    using nativetv::Json;
    Json raw = Json::object();
    raw.set("id", Json::numeric(id));
    raw.set("media_type", Json::text(type));
    raw.set("title", Json::text("Film " + std::to_string(id)));
    raw.set("name", Json::text("Series " + std::to_string(id)));
    raw.set("poster_path", Json::text("/abcdefghijklmnop.jpg"));
    raw.set("backdrop_path", Json::text("/ponmlkjihgfedcba.jpg"));
    raw.set("release_date", Json::text("2026-01-01"));
    raw.set("vote_average", Json::numeric(8.04));
    raw.set("vote_count", Json::numeric(100));
    raw.set("overview", Json::text("A story."));
    return raw;
}

nativetv::Json responseFor(const std::string &url) {
    using nativetv::Json;
    if (url.find("wikidata.org") != std::string::npos) {
        Json claims = Json::array();
        claims.add(Json::parse("null"));
        Json deprecated = Json::object();
        deprecated.set("rank", Json::text("deprecated"));
        deprecated.set("mainsnak", Json::parse(
            "{\"datavalue\":{\"value\":\"999999\"}}"));
        claims.add(deprecated);
        Json accepted = Json::object();
        accepted.set("mainsnak", Json::parse(
            "{\"datavalue\":{\"value\":\"80057281\"}}"));
        claims.add(accepted);
        Json entity = Json::object();
        entity.set("claims", Json::object());
        entity["claims"].set("P1874", claims);
        Json entities = Json::object();
        entities.set("Q42", entity);
        Json result = Json::object();
        result.set("entities", entities);
        return result;
    }
    if (url.find("search/multi") != std::string::npos) {
        Json results = Json::array();
        results.add(rawTitle(5));
        results.add(rawTitle(5));
        results.add(rawTitle(8, "tv"));
        Json person = rawTitle(9);
        person.set("media_type", Json::text("person"));
        results.add(person);
        Json adult = rawTitle(10);
        adult.set("adult", Json::flag(true));
        results.add(adult);
        Json document = Json::object();
        document.set("results", results);
        return document;
    }
    const size_t slash = url.find_last_of('/', url.find('?'));
    const size_t question = url.find('?', slash);
    const int id = std::atoi(url.substr(slash + 1, question - slash - 1).c_str());
    const std::string type = url.find("/tv/") != std::string::npos ? "tv" : "movie";
    Json document = rawTitle(id, type);
    if (type == "movie") document.set("runtime", Json::numeric(121));
    else document.set("episode_run_time", Json::parse("[44]"));
    document.set("genres", Json::parse("[{\"name\":\"Drama\"}]"));
    document.set("credits", Json::parse(
        "{\"cast\":[{\"name\":\"<Actor>\",\"character\":\"A role\"}]}"));
    document.set("external_ids", Json::parse("{\"wikidata_id\":\"Q42\"}"));
    document.set("videos", Json::parse(
        "{\"results\":[{\"site\":\"YouTube\",\"type\":\"Trailer\",\"key\":\"bad\"},"
        "{\"site\":\"Other\",\"type\":\"Trailer\",\"key\":\"dQw4w9WgXcQ\"},"
        "{\"site\":\"YouTube\",\"type\":\"Trailer\",\"official\":true,\"key\":\"dQw4w9WgXcQ\"}]}"));
    document.set("similar", Json::parse("{\"results\":["
        "{\"id\":7,\"media_type\":\"movie\",\"poster_path\":\"/abcdefghijklmnop.jpg\"},"
        "{\"id\":20,\"media_type\":\"movie\",\"poster_path\":\"/abcdefghijklmnop.jpg\"},"
        "{\"id\":21,\"media_type\":\"movie\",\"poster_path\":\"/abcdefghijklmnop.jpg\"}]}"));
    return document;
}
} // namespace

int main() {
    using namespace nativetv;
    using namespace nativetv::service;
    TemporaryDirectory directory;
    int64_t clock = 1792930000000LL;
    std::mutex askedMutex;
    std::vector<std::string> asked;
    bool fail = false;
    auto fetcher = [&](const std::string &url, Json &document, std::string &error) {
        std::lock_guard<std::mutex> lock(askedMutex);
        asked.push_back(url);
        if (fail) {
            error = "offline";
            return false;
        }
        document = responseFor(url);
        return true;
    };

    HomeContent content(directory.path(), fetcher, [&clock] { return clock; });
    assert(!content.details("", false, "movie", "7")["enabled"].boolean());
    assert(!content.search("", "film")["enabled"].boolean());
    assert(!content.netflix("", true, "movie", "7")["enabled"].boolean());
    assert(asked.empty());

    assert(!content.details("key", false, "person", "7")["ok"].boolean());
    assert(!content.details("key", false, "movie", "../7")["ok"].boolean());
    assert(!content.details("key", false, "movie", "2147483648")["ok"].boolean());
    assert(!content.search("key", "a\nfilm")["ok"].boolean());
    assert(!content.search("key", std::string(81, 'x'))["ok"].boolean());
    assert(content.search("key", " a ")["items"].size() == 0);
    assert(asked.empty());

    const Json details = content.details("private-test-key", false, "movie", "7");
    assert(details["ok"].boolean() && details["enabled"].boolean());
    assert(!details["netflixEnabled"].boolean());
    const Json item = details["item"];
    assert(item["id"].number() == 7);
    assert(item["runtime"].number() == 121);
    assert(item["rating"].number() == 8.0);
    assert(item["genres"][0].string() == "Drama");
    assert(item["cast"][0]["name"].string() == "<Actor>");
    assert(item["trailer"].string() == "dQw4w9WgXcQ");
    assert(item["wikidataId"].string() == "Q42");
    assert(item["similar"].size() == 2 && item["similar"][0]["id"].number() == 20);
    const size_t afterDetails = asked.size();
    assert(content.details("private-test-key", true, "movie", "7")["netflixEnabled"].boolean());
    assert(asked.size() == afterDetails);

    const Json search = content.search("private-test-key", "secret phrase & /?");
    assert(search["items"].size() == 2);
    assert(search["items"][0]["id"].number() == 5);
    assert(search["items"][1]["id"].number() == 8);
    assert(asked.back().find("query=secret%20phrase%20%26%20%2F%3F") != std::string::npos);
    struct stat cacheInfo = {};
    assert(stat((directory.path() + "/home-content.json").c_str(), &cacheInfo) == 0);
    assert((cacheInfo.st_mode & 0777) == 0600);
    {
        std::ifstream input((directory.path() + "/home-content.json").c_str());
        std::string saved((std::istreambuf_iterator<char>(input)),
                          std::istreambuf_iterator<char>());
        assert(saved.find("secret phrase") == std::string::npos);
        assert(saved.find("private-test-key") == std::string::npos);
        const Json cache = Json::parse(saved);
        bool matchingSearchKey = false;
        for (const Json &entry : cache["entries"].items())
            matchingSearchKey = matchingSearchKey ||
                entry["key"].string() == "267247212e9822f0fe8a7eef55bb694a";
        assert(matchingSearchKey);
    }

    const size_t beforeNetflix = asked.size();
    const Json netflix = content.netflix("private-test-key", true, "movie", "7");
    assert(netflix["ok"].boolean() && netflix["netflixId"].string() == "80057281");
    assert(asked.size() == beforeNetflix + 1);
    assert(asked.back() == "https://www.wikidata.org/wiki/Special:EntityData/Q42.json");

    const size_t beforeRestart = asked.size();
    HomeContent restarted(directory.path(), fetcher, [&clock] { return clock; });
    assert(restarted.details("private-test-key", false, "movie", "7")["ok"].boolean());
    assert(restarted.netflix("private-test-key", true, "movie", "7")["netflixId"].string() == "80057281");
    assert(asked.size() == beforeRestart);

    clock += 24 * 60 * 60 * 1000LL + 1;
    fail = true;
    const size_t beforeFailure = asked.size();
    const Json stale = content.details("private-test-key", false, "movie", "7");
    assert(stale["ok"].boolean() && stale["stale"].boolean());
    assert(asked.size() == beforeFailure + 1);
    (void)content.details("private-test-key", false, "movie", "7");
    assert(asked.size() == beforeFailure + 1);
    fail = false;

    for (int index = 0; index < 45; ++index)
        assert(content.search("private-test-key", "query " + std::to_string(index))["ok"].boolean());
    {
        std::ifstream input((directory.path() + "/home-content.json").c_str());
        std::string saved((std::istreambuf_iterator<char>(input)),
                          std::istreambuf_iterator<char>());
        const Json cache = Json::parse(saved);
        assert(cache["version"].number() == 1);
        assert(cache["entries"].size() == 40);
    }
    return 0;
}
