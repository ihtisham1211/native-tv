#include "service/home_discover.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>
#include <curl/curl.h>

namespace nativetv {
namespace service {
namespace {
const int64_t kRetryMs = 10LL * 60 * 1000;
const int kMorningHour = 5;
const size_t kPerRow = 16;
const size_t kMaximumResponseBytes = 4 * 1024 * 1024;

struct RowSpec {
    const char *key;
    const char *path;
    const char *type;
};

// Only what is trending this week.
const RowSpec kRows[] = {
    {"trending", "trending/all/week", ""},
    {"movies", "trending/movie/week", "movie"},
    {"series", "trending/tv/week", "tv"}
};

bool initializeCurl() {
    static std::once_flag once;
    static CURLcode result = CURLE_FAILED_INIT;
    std::call_once(once, []() { result = curl_global_init(CURL_GLOBAL_DEFAULT); });
    return result == CURLE_OK;
}

size_t appendBody(char *data, size_t size, size_t count, void *context) {
    if (size && count > std::numeric_limits<size_t>::max() / size) return 0;
    const size_t bytes = size * count;
    std::string *body = static_cast<std::string *>(context);
    if (bytes > kMaximumResponseBytes - std::min(body->size(), kMaximumResponseBytes))
        return 0;
    body->append(data, bytes);
    return bytes;
}

int cancelTransfer(void *context, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    const std::atomic<bool> *stopping =
        static_cast<const std::atomic<bool> *>(context);
    return stopping->load() ? 1 : 0;
}

bool localTime(int64_t milliseconds, std::tm &result) {
    const std::time_t seconds = static_cast<std::time_t>(milliseconds / 1000);
    return localtime_r(&seconds, &result) != nullptr;
}

int dayOf(int64_t milliseconds) {
    std::tm value = {};
    if (!localTime(milliseconds, value)) return 0;
    return (value.tm_year + 1900) * 400 + value.tm_mon * 31 + value.tm_mday;
}

bool truthy(const Json &value) {
    if (!value.valid()) return false;
    if (value.isBoolean()) return value.boolean();
    if (value.isNumber()) return value.number() != 0 && !std::isnan(value.number());
    if (value.isString()) return !value.string().empty();
    return true;
}

std::string jsString(const Json &value) {
    if (!value.valid()) return "";
    if (value.isString()) return value.string();
    if (value.isBoolean()) return value.boolean() ? "true" : "false";
    if (value.isNumber()) return value.dump();
    if (value.isObject()) return "[object Object]";
    std::string result;
    const std::vector<Json> values = value.items();
    for (size_t index = 0; index < values.size(); ++index) {
        if (index) result.push_back(',');
        if (values[index].valid()) result += jsString(values[index]);
    }
    return result;
}

std::string utf16Prefix(const std::string &text, size_t maximumUnits) {
    size_t offset = 0;
    size_t units = 0;
    while (offset < text.size()) {
        const unsigned char first = static_cast<unsigned char>(text[offset]);
        size_t width = 1;
        size_t nextUnits = 1;
        if ((first & 0xe0) == 0xc0 && offset + 1 < text.size()) width = 2;
        else if ((first & 0xf0) == 0xe0 && offset + 2 < text.size()) width = 3;
        else if ((first & 0xf8) == 0xf0 && offset + 3 < text.size()) {
            width = 4;
            nextUnits = 2;
        }
        if (units + nextUnits > maximumUnits) break;
        units += nextUnits;
        offset += width;
    }
    return text.substr(0, offset);
}

std::string urlEncode(const std::string &text) {
    std::ostringstream encoded;
    encoded << std::uppercase << std::hex;
    for (unsigned char value : text) {
        if ((value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
            (value >= '0' && value <= '9') || value == '-' || value == '_' ||
            value == '.' || value == '!' || value == '~' || value == '*' ||
            value == '\'' || value == '(' || value == ')') {
            encoded << static_cast<char>(value);
        } else {
            encoded << '%' << std::setw(2) << std::setfill('0')
                    << static_cast<unsigned>(value);
        }
    }
    return encoded.str();
}

std::string idString(const Json &value) {
    if (value.isString()) return value.string();
    // 42, whether the reply wrote it as 42 or 42.0
    if (value.isNumber()) return compactNumber(value.number());
    return "";
}

Json trimmedTitle(const Json &raw, const std::string &rowType) {
    const Json mediaType = raw["media_type"];
    const std::string type = truthy(mediaType) ? jsString(mediaType) : rowType;
    if (type != "movie" && type != "tv") return Json();
    const Json id = raw["id"];
    const Json posterValue = raw["poster_path"];
    const Json backdropValue = raw["backdrop_path"];
    if (!truthy(id) || (!truthy(posterValue) && !truthy(backdropValue))) return Json();

    std::string date = truthy(raw["release_date"])
        ? jsString(raw["release_date"]) : jsString(raw["first_air_date"]);
    std::string year;
    if (date.size() >= 4 && std::all_of(date.begin(), date.begin() + 4,
            [](unsigned char value) { return value >= '0' && value <= '9'; }))
        year = date.substr(0, 4);

    Json result = Json::object();
    result.set("id", id);
    result.set("type", Json::text(type));
    const Json title = truthy(raw["title"]) ? raw["title"] :
                       truthy(raw["name"]) ? raw["name"] : Json::text("");
    result.set("title", Json::text(jsString(title)));
    result.set("year", Json::text(year));
    const Json average = raw["vote_average"];
    const Json votes = raw["vote_count"];
    if (average.isNumber() && votes.number(-1) >= 20)
        result.set("rating", Json::numeric(std::floor(average.number() * 10 + 0.5) / 10));
    else
        result.set("rating", Json::parse("null"));
    result.set("overview", Json::text(utf16Prefix(jsString(raw["overview"]), 280)));
    result.set("poster", truthy(posterValue) ? Json::text(jsString(posterValue))
                                                : Json::text(""));
    result.set("backdrop", truthy(backdropValue) ? Json::text(jsString(backdropValue))
                                                    : Json::text(""));
    return result;
}

Json makeRow(const std::string &key, const std::vector<Json> &items) {
    Json row = Json::object();
    row.set("key", Json::text(key));
    Json list = Json::array();
    for (const Json &item : items) list.add(item);
    row.set("items", list);
    return row;
}

Json youtubeRow(const Json &document) {
    std::vector<Json> items;
    for (const Json &video : document["items"].items()) {
        const std::string id = jsString(video["id"]);
        const Json snippet = video["snippet"];
        const bool validId = id.size() == 11 && std::all_of(id.begin(), id.end(),
            [](unsigned char value) {
                return (value >= 'a' && value <= 'z') ||
                       (value >= 'A' && value <= 'Z') ||
                       (value >= '0' && value <= '9') || value == '_' || value == '-';
            });
        if (!validId || !snippet.isObject()) continue;
        Json item = Json::object();
        item.set("id", Json::text(id));
        item.set("type", Json::text("youtube"));
        item.set("title", Json::text(utf16Prefix(jsString(snippet["title"]), 120)));
        item.set("year", Json::text(""));
        item.set("rating", Json::parse("null"));
        item.set("overview", Json::text(utf16Prefix(jsString(snippet["channelTitle"]), 120)));
        item.set("poster", Json::text(""));
        item.set("backdrop", Json::text(""));
        item.set("image", Json::text("https://i.ytimg.com/vi/" + id + "/mqdefault.jpg"));
        item.set("wide", Json::text("https://i.ytimg.com/vi/" + id + "/maxresdefault.jpg"));
        items.push_back(item);
        if (items.size() == kPerRow) break;
    }
    return items.empty() ? Json() : makeRow("youtube", items);
}

Json responseRows(const Json &rows, const std::string &tmdbKey,
                  const std::string &youtubeKey) {
    Json visible = Json::array();
    for (const Json &row : rows.items()) {
        const std::string key = row["key"].string();
        if ((key == "youtube" && !youtubeKey.empty()) ||
            (key != "youtube" && !tmdbKey.empty()))
            visible.add(row);
    }
    return visible;
}

int64_t systemNow() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}
} // namespace

HomeDiscover::HomeDiscover(std::string dataRoot, Fetcher fetcher, Clock clock)
    : cachePath_((dataRoot.empty() ? "/var/lib/tvweb" : std::move(dataRoot)) +
                 "/home-discover.json"),
      fetcher_(std::move(fetcher)), clock_(std::move(clock)) {
    if (!clock_) clock_ = systemNow;
    worker_ = std::thread(&HomeDiscover::workerLoop, this);
}

HomeDiscover::~HomeDiscover() {
    stopping_.store(true);
    changed_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void HomeDiscover::setLinkCheck(LinkCheck check) {
    std::lock_guard<std::mutex> lock(mutex_);
    linkCheck_ = std::move(check);
}

void HomeDiscover::setNetflixCheck(LinkCheck check) {
    std::lock_guard<std::mutex> lock(mutex_);
    netflixCheck_ = std::move(check);
}

Json HomeDiscover::feed(const std::string &tmdbKey, const std::string &youtubeKey) {
    Json response = Json::object();
    response.set("ok", Json::flag(true));
    if (tmdbKey.empty() && youtubeKey.empty()) {
        response.set("enabled", Json::flag(false));
        response.set("rows", Json::array());
        return response;
    }

    Json rows = Json::array();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!cacheLoaded_) loadCacheLocked();
        const int64_t nowMs = clock_();
        if (dueLocked(nowMs) && !refreshing_ &&
            (!hasFailed_ || nowMs - failedAtMs_ >= kRetryMs)) {
            requestedTmdbKey_ = tmdbKey;
            requestedYoutubeKey_ = youtubeKey;
            refreshRequested_ = true;
            refreshing_ = true;
            changed_.notify_one();
        }
        if (cache_.isObject()) rows = cache_["rows"];
    }

    response.set("enabled", Json::flag(true));
    response.set("titlesEnabled", Json::flag(!tmdbKey.empty()));
    response.set("rows", responseRows(rows, tmdbKey, youtubeKey));
    return response;
}

void HomeDiscover::workerLoop() {
    for (;;) {
        std::string tmdbKey;
        std::string youtubeKey;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            changed_.wait(lock, [this]() { return stopping_.load() || refreshRequested_; });
            if (stopping_.load()) return;
            tmdbKey = requestedTmdbKey_;
            youtubeKey = requestedYoutubeKey_;
            refreshRequested_ = false;
        }
        refresh(tmdbKey, youtubeKey);
    }
}

void HomeDiscover::refresh(const std::string &tmdbKey,
                           const std::string &youtubeKey) {
    Json previous;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        previous = cache_;
    }

    LinkCheck linkCheck, netflixCheck;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        linkCheck = linkCheck_;
        netflixCheck = netflixCheck_;
    }
    std::map<std::string, bool> linked; // a title is asked about once, whichever rows it is in
    const auto hasLink = [&](const Json &item) {
        if (!linkCheck) return true;
        const std::string key = item["type"].string() + ":" + idString(item["id"]);
        const auto known = linked.find(key);
        if (known != linked.end()) return known->second;
        bool answer = false;
        try { answer = linkCheck(item["type"].string(), idString(item["id"])); } catch (...) { answer = false; }
        linked[key] = answer;
        return answer;
    };
    std::vector<Json> rows;
    const size_t asked = (tmdbKey.empty() ? 0 : 3) + (youtubeKey.empty() ? 0 : 1);
    size_t missed = asked;
    for (const RowSpec &row : kRows) {
        if (tmdbKey.empty()) break;
        std::string url = "https://api.themoviedb.org/3/" + std::string(row.path) +
            "?api_key=" + urlEncode(tmdbKey) + "&language=en-US";
        Json document;
        std::string error;
        bool fetched = false;
        try { fetched = fetch(url, document, error); } catch (...) { fetched = false; }
        std::vector<Json> items;
        std::set<std::string> seen;
        if (fetched && document["results"].isArray()) {
            for (const Json &raw : document["results"].items()) {
                Json item = trimmedTitle(raw, row.type);
                if (!item.valid()) continue;
                const std::string key = item["type"].string() + idString(item["id"]);
                if (!seen.insert(key).second) continue;
                if (!hasLink(item)) continue;
                items.push_back(item);
                if (items.size() == kPerRow) break;
            }
        }
        if (!items.empty()) {
            rows.push_back(makeRow(row.key, items));
            --missed;
        }
    }

    // The titles above that open on Netflix, in the order of the rows they came from.
    if (netflixCheck) {
        std::vector<Json> onNetflix;
        std::set<std::string> seenOnNetflix;
        for (const Json &row : rows)
            for (const Json &item : row["items"].items()) {
                const std::string key = item["type"].string() + ":" + idString(item["id"]);
                if (onNetflix.size() == kPerRow || !seenOnNetflix.insert(key).second) continue;
                bool answer = false;
                try { answer = netflixCheck(item["type"].string(), idString(item["id"])); } catch (...) { answer = false; }
                if (answer) onNetflix.push_back(item);
            }
        if (!onNetflix.empty()) rows.push_back(makeRow("netflix", onNetflix));
    }

    if (!youtubeKey.empty()) {
        const std::string url = "https://www.googleapis.com/youtube/v3/videos?part=snippet&chart=mostPopular&maxResults=16&key=" +
                                urlEncode(youtubeKey);
        Json document;
        std::string error;
        bool fetched = false;
        try { fetched = fetch(url, document, error); } catch (...) { fetched = false; }
        Json row = fetched ? youtubeRow(document) : Json();
        if (row.valid()) {
            rows.push_back(row);
            --missed;
        }
    }

    if (rows.empty()) {
        std::lock_guard<std::mutex> lock(mutex_);
        failedAtMs_ = clock_();
        hasFailed_ = true;
        refreshing_ = false;
        changed_.notify_all();
        return;
    }

    if (missed && previous.isObject()) {
        std::set<std::string> have;
        for (const Json &row : rows) have.insert(row["key"].string());
        for (const Json &row : previous["rows"].items()) {
            const std::string key = row["key"].string();
            if (have.insert(key).second) {
                rows.push_back(row);
                --missed;
            }
        }
    }

    Json updated = Json::object();
    updated.set("at", Json::numeric(static_cast<double>(clock_())));
    Json rowList = Json::array();
    for (const Json &row : rows) rowList.add(row);
    updated.set("rows", rowList);
    updated.set("partial", Json::flag(missed > 0));
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cache_ = updated;
        failedAtMs_ = 0;
        hasFailed_ = false;
        saveCacheLocked();
        refreshing_ = false;
        changed_.notify_all();
    }
}

bool HomeDiscover::fetch(const std::string &url, Json &document,
                         std::string &error) {
    if (fetcher_) return fetcher_(url, document, error);
    error.clear();
    if (stopping_.load()) {
        error = "service is stopping";
        return false;
    }
    if (!initializeCurl()) {
        error = "HTTP initialization failed";
        return false;
    }
    CURL *curl = curl_easy_init();
    if (!curl) {
        error = "Could not create a connection";
        return false;
    }
    std::string body;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 5000L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 15000L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS, CURLPROTO_HTTPS);
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTPS);
#pragma GCC diagnostic pop
    curl_easy_setopt(curl, CURLOPT_PROXY, "");
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "tvweb");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, appendBody);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, cancelTransfer);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &stopping_);
    const CURLcode code = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    if (code != CURLE_OK) {
        error = code == CURLE_WRITE_ERROR && body.size() >= kMaximumResponseBytes
            ? "discovery response exceeded 4 MiB" : curl_easy_strerror(code);
        return false;
    }
    if (status < 200 || status >= 300) {
        error = "remote server returned HTTP " + std::to_string(status);
        return false;
    }
    document = Json::parse(body);
    if (!document.valid()) {
        error = "remote server did not return JSON";
        return false;
    }
    return true;
}

void HomeDiscover::loadCacheLocked() {
    cacheLoaded_ = true;
    std::ifstream input(cachePath_.c_str(), std::ios::binary);
    if (!input) return;
    input.seekg(0, std::ios::end);
    const std::streamoff length = input.tellg();
    if (length <= 0 || length > static_cast<std::streamoff>(kMaximumResponseBytes)) return;
    input.seekg(0, std::ios::beg);
    std::string contents(static_cast<size_t>(length), '\0');
    input.read(&contents[0], length);
    if (!input) return;
    Json parsed = Json::parse(contents);
    if (!parsed.isObject() || !parsed["rows"].isArray()) return;
    cache_ = parsed;
}

void HomeDiscover::saveCacheLocked() {
    const std::string temporary = cachePath_ + ".tmp";
    const std::string contents = cache_.dump();
    const int fd = open(temporary.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
    if (fd < 0) return;
    size_t written = 0;
    bool ok = true;
    while (written < contents.size()) {
        const ssize_t count = write(fd, contents.data() + written,
                                    contents.size() - written);
        if (count <= 0) { ok = false; break; }
        written += static_cast<size_t>(count);
    }
    if (close(fd) != 0) ok = false;
    if (ok) {
        if (rename(temporary.c_str(), cachePath_.c_str()) != 0)
            (void)unlink(temporary.c_str());
    } else {
        (void)unlink(temporary.c_str());
    }
}

bool HomeDiscover::dueLocked(int64_t nowMs) const {
    if (!cache_.isObject()) return true;
    const int64_t cachedAt = static_cast<int64_t>(cache_["at"].number());
    if (cache_["partial"].boolean() && nowMs - cachedAt >= kRetryMs) return true;
    std::tm nowLocal = {};
    if (!localTime(nowMs, nowLocal)) return false;
    return dayOf(cachedAt) != dayOf(nowMs) && nowLocal.tm_hour >= kMorningHour;
}

} // namespace service
} // namespace nativetv
