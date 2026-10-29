#include "service/app_catalog.hpp"
#include "service/app_ids.hpp"
#include "service/curl_global.hpp"

#include <curl/curl.h>
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <iomanip>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <utility>

namespace nativetv {
namespace service {
namespace {

const char kDefaultRepo[] = "https://repo.webosbrew.org/api/apps.json";
const size_t kMaximumUrlBytes = 2048;
const size_t kMaximumDocumentBytes = 8 * 1024 * 1024;
const int kMaximumPages = 50;
const int64_t kCacheMs = 60LL * 60 * 1000;
const int64_t kRetryMs = 10LL * 1000;

bool startsWith(const std::string &value, const std::string &prefix) {
    return value.compare(0, prefix.size(), prefix) == 0;
}

bool startsWithHttps(const std::string &value);

bool validUrl(const std::string &url, std::string &error) {
    if (!startsWithHttps(url)) {
        error = "the address must start with https://";
        return false;
    }
    if (url.size() > kMaximumUrlBytes) {
        error = "the address is longer than 2048 characters";
        return false;
    }
    for (unsigned char character : url) {
        if (character <= 0x20 || (character >= 0x7f && character <= 0x9f)) {
            error = "the address contains spaces or control characters";
            return false;
        }
    }
    return true;
}

std::string lowerAscii(std::string value) {
    for (char &character : value)
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    return value;
}

std::string trimAscii(const std::string &value) {
    size_t first = 0;
    while (first < value.size() &&
           std::isspace(static_cast<unsigned char>(value[first])))
        ++first;
    size_t last = value.size();
    while (last > first &&
           std::isspace(static_cast<unsigned char>(value[last - 1])))
        --last;
    return value.substr(first, last - first);
}

bool asciiEqualAt(const std::string &value, size_t offset,
                  const std::string &expected) {
    if (offset > value.size() || value.size() - offset < expected.size())
        return false;
    for (size_t index = 0; index < expected.size(); ++index) {
        const unsigned char actual = static_cast<unsigned char>(value[offset + index]);
        const unsigned char wanted = static_cast<unsigned char>(expected[index]);
        if (std::tolower(actual) != std::tolower(wanted))
            return false;
    }
    return true;
}

bool githubRepoPart(const std::string &value) {
    if (value.empty()) return false;
    for (unsigned char character : value) {
        if (character > 0x7f ||
            !(std::isalnum(character) || character == '_' || character == '.' ||
              character == '-'))
            return false;
    }
    return true;
}

bool exceedsUtf16Length(const std::string &value, size_t maximumCodeUnits) {
    size_t codeUnits = 0;
    for (size_t index = 0; index < value.size();) {
        const unsigned char lead = static_cast<unsigned char>(value[index]);
        size_t byteCount = 1;
        unsigned long codePoint = lead;
        if (lead >= 0xc2 && lead <= 0xdf && index + 1 < value.size()) {
            const unsigned char second = static_cast<unsigned char>(value[index + 1]);
            if ((second & 0xc0) == 0x80) {
                byteCount = 2;
                codePoint = ((lead & 0x1f) << 6) | (second & 0x3f);
            }
        } else if (lead >= 0xe0 && lead <= 0xef && index + 2 < value.size()) {
            const unsigned char second = static_cast<unsigned char>(value[index + 1]);
            const unsigned char third = static_cast<unsigned char>(value[index + 2]);
            if ((second & 0xc0) == 0x80 && (third & 0xc0) == 0x80 &&
                !(lead == 0xe0 && second < 0xa0) &&
                !(lead == 0xed && second >= 0xa0)) {
                byteCount = 3;
                codePoint = ((lead & 0x0f) << 12) | ((second & 0x3f) << 6) |
                            (third & 0x3f);
            }
        } else if (lead >= 0xf0 && lead <= 0xf4 && index + 3 < value.size()) {
            const unsigned char second = static_cast<unsigned char>(value[index + 1]);
            const unsigned char third = static_cast<unsigned char>(value[index + 2]);
            const unsigned char fourth = static_cast<unsigned char>(value[index + 3]);
            if ((second & 0xc0) == 0x80 && (third & 0xc0) == 0x80 &&
                (fourth & 0xc0) == 0x80 &&
                !(lead == 0xf0 && second < 0x90) &&
                !(lead == 0xf4 && second >= 0x90)) {
                byteCount = 4;
                codePoint = ((lead & 0x07) << 18) | ((second & 0x3f) << 12) |
                            ((third & 0x3f) << 6) | (fourth & 0x3f);
            }
        }
        codeUnits += codePoint > 0xffff ? 2 : 1;
        if (codeUnits > maximumCodeUnits) return true;
        index += byteCount;
    }
    return false;
}

bool hasUrlControlCharacter(const std::string &value) {
    for (size_t index = 0; index < value.size(); ++index) {
        const unsigned char character = static_cast<unsigned char>(value[index]);
        if (character <= 0x20 || character == 0x7f ||
            (character == 0xc2 && index + 1 < value.size() &&
             static_cast<unsigned char>(value[index + 1]) >= 0x80 &&
             static_cast<unsigned char>(value[index + 1]) <= 0x9f))
            return true;
    }
    return false;
}

bool startsWithHttps(const std::string &value) {
    return value.size() >= 8 && lowerAscii(value.substr(0, 8)) == "https://";
}

std::string hostOf(const std::string &url) {
    const size_t scheme = url.find("://");
    const size_t start = scheme == std::string::npos ? 0 : scheme + 3;
    const size_t end = url.find_first_of("/:?#", start);
    std::string authority = url.substr(start, end == std::string::npos ? end : end - start);
    const size_t at = authority.rfind('@');
    if (at != std::string::npos) authority.erase(0, at + 1);
    const size_t port = authority.find(':');
    if (port != std::string::npos) authority.erase(port);
    return authority.empty() ? "the server" : lowerAscii(authority);
}

std::string pathOf(const std::string &url) {
    const size_t scheme = url.find("://");
    const size_t start = scheme == std::string::npos ? 0 : scheme + 3;
    const size_t path = url.find('/', start);
    if (path == std::string::npos) return "/";
    const size_t end = url.find_first_of("?#", path);
    return url.substr(path, end == std::string::npos ? end : end - path);
}

std::string hostError(const std::string &url, long status) {
    const std::string host = hostOf(url);
    const std::string path = pathOf(url);
    if (status > 0)
        return host + " returned " + std::to_string(status) + " for " + path;
    return host + " answered with an error for " + path;
}

size_t appendBody(char *bytes, size_t size, size_t count, void *context) {
    std::string *body = static_cast<std::string *>(context);
    if (!size || count > (kMaximumDocumentBytes - body->size()) / size)
        return 0;
    body->append(bytes, size * count);
    return size * count;
}

bool isNameCharacter(unsigned char character) {
    return (character >= 'a' && character <= 'z') ||
           (character >= 'A' && character <= 'Z') ||
           (character >= '0' && character <= '9') || character == '.' ||
           character == '_' || character == '+' || character == '-';
}

bool validPackageName(const std::string &name) {
    if (name.empty() || name.size() > 128 ||
        !((name[0] >= 'a' && name[0] <= 'z') ||
          (name[0] >= 'A' && name[0] <= 'Z') ||
          (name[0] >= '0' && name[0] <= '9')))
        return false;
    for (size_t index = 1; index < name.size(); ++index)
        if (!isNameCharacter(static_cast<unsigned char>(name[index]))) return false;
    return true;
}

std::string packageString(const Json &primary, const Json &secondary = Json()) {
    if (primary.isString()) return primary.string();
    if (secondary.isString()) return secondary.string();
    return std::string();
}

std::string jsonString(const Json &value) {
    if (value.isString()) return value.string();
    if (value.isNumber()) {
        const double number = value.number();
        std::ostringstream output;
        output << std::setprecision(15) << number;
        return output.str();
    }
    if (value.isBoolean()) return value.boolean() ? "true" : "false";
    return std::string();
}

bool isHbcPackage(const std::string &id) {
    if (appIdIsProtected(id)) return true;
    static const char *updating[] = {
        "org.webosbrew.hbchannel",
        "io.github.ihtisham1211.nativetv"
    };
    for (const char *base : updating) {
        const std::string prefix = std::string(base) + ".";
        if (id == base || startsWith(id, prefix)) return true;
    }
    return false;
}

int compareVersions(const std::string &left, const std::string &right) {
    std::vector<unsigned long long> a, b;
    const auto parts = [](const std::string &version, std::vector<unsigned long long> &out) {
        size_t index = 0;
        while (index < version.size()) {
            while (index < version.size() &&
                   !(version[index] >= '0' && version[index] <= '9'))
                ++index;
            if (index == version.size()) break;
            unsigned long long number = 0;
            while (index < version.size() && version[index] >= '0' && version[index] <= '9') {
                const unsigned digit = static_cast<unsigned>(version[index] - '0');
                if (number <= (static_cast<unsigned long long>(-1) - digit) / 10)
                    number = number * 10 + digit;
                else
                    number = static_cast<unsigned long long>(-1);
                ++index;
            }
            out.push_back(number);
        }
    };
    parts(left, a);
    parts(right, b);
    const size_t count = std::max(a.size(), b.size());
    for (size_t index = 0; index < count; ++index) {
        const unsigned long long x = index < a.size() ? a[index] : 0;
        const unsigned long long y = index < b.size() ? b[index] : 0;
        if (x != y) return x > y ? 1 : -1;
    }
    return 0;
}

std::string pageUrl(const std::string &base, int page) {
    if (page <= 1) return base;
    if (base.size() >= 5 && base.compare(base.size() - 5, 5, ".json") == 0)
        return base.substr(0, base.size() - 5) + "/" + std::to_string(page) + ".json";
    return base;
}

int maxPage(const Json &document) {
    const Json raw = document["paging"]["maxPage"];
    std::string value = jsonString(raw);
    if (value.empty()) return 1;
    char *end = nullptr;
    errno = 0;
    const long parsed = std::strtol(value.c_str(), &end, 10);
    if (errno || end == value.c_str() || parsed <= 0) return 1;
    return static_cast<int>(std::min<long>(parsed, kMaximumPages));
}

std::vector<std::string> pathSegments(const std::string &url) {
    std::vector<std::string> result;
    const size_t scheme = url.find("://");
    const size_t start = scheme == std::string::npos ? 0 : scheme + 3;
    size_t path = url.find('/', start);
    if (path == std::string::npos) return result;
    size_t end = url.find_first_of("?#", path);
    const std::string value = url.substr(path, end == std::string::npos ? end : end - path);
    size_t offset = 1;
    while (offset <= value.size()) {
        end = value.find('/', offset);
        const std::string part = value.substr(
            offset, end == std::string::npos ? end : end - offset);
        if (!part.empty()) result.push_back(part);
        if (end == std::string::npos) break;
        offset = end + 1;
    }
    return result;
}

std::string repoLabel(const std::string &url) {
    const std::string host = hostOf(url);
    const std::vector<std::string> path = pathSegments(url);
    if (host == "raw.githubusercontent.com" && path.size() >= 2) {
        if (lowerAscii(path[1]) == lowerAscii(path[0] + ".github.io")) return path[1];
        return path[0] + "/" + path[1];
    }
    if (host == "github.com" && path.size() >= 2) {
        if (lowerAscii(path[1]) == lowerAscii(path[0] + ".github.io")) return path[1];
        return path[0] + "/" + path[1];
    }
    const std::string suffix = ".github.io";
    if (host.size() > suffix.size() &&
        host.compare(host.size() - suffix.size(), suffix.size(), suffix) == 0 &&
        !path.empty()) {
        const std::string owner = host.substr(0, host.size() - suffix.size());
        return owner + "/" + path[0];
    }
    return host;
}

} // namespace

AppCatalog::AppCatalog(std::string version, AppCatalogFetcher fetcher)
    : version_(std::move(version)), fetcher_(std::move(fetcher)) {
    if (!fetcher_) fetcher_ = fetchJson;
}

bool AppCatalog::validPackageId(const std::string &id) {
    return validPackageName(id);
}

bool AppCatalog::fetchJson(const std::string &url, const std::string &userAgent,
                           Json &document, std::string &error) {
    error.clear();
    if (!validUrl(url, error)) return false;
    if (!ensureCurlGlobal()) {
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
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 50L);
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
#pragma GCC diagnostic pop
    curl_easy_setopt(curl, CURLOPT_PROXY, "");
    curl_easy_setopt(curl, CURLOPT_USERAGENT,
                     userAgent.empty() ? "tvweb" : userAgent.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, appendBody);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    const CURLcode code = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    if (code != CURLE_OK) {
        if (code == CURLE_WRITE_ERROR && body.size() >= kMaximumDocumentBytes)
            error = "catalog response exceeded 8 MiB";
        else if (code == CURLE_COULDNT_RESOLVE_HOST || code == CURLE_COULDNT_CONNECT ||
                 code == CURLE_OPERATION_TIMEDOUT)
            error = "The TV could not reach " + hostOf(url) +
                    ". Check it is connected to the internet.";
        else
            error = curl_easy_strerror(code);
        return false;
    }
    if (status < 200 || status >= 300) {
        error = hostError(url, status);
        return false;
    }
    document = Json::parse(body);
    if (!document.valid()) {
        error = hostOf(url) + " did not return JSON";
        return false;
    }
    return true;
}

std::string AppCatalog::resolveUrl(const std::string &base, const std::string &relative) {
    if (relative.empty()) return base;
    const size_t colon = relative.find(':');
    const size_t firstDelimiter = relative.find_first_of("/?#");
    if (colon != std::string::npos &&
        (firstDelimiter == std::string::npos || colon < firstDelimiter))
        return relative;

    const size_t schemeEnd = base.find("://");
    if (schemeEnd == std::string::npos) return relative;
    const std::string scheme = base.substr(0, schemeEnd);
    if (startsWith(relative, "//")) return scheme + ":" + relative;

    const size_t authorityStart = schemeEnd + 3;
    const size_t authorityEnd = base.find_first_of("/?#", authorityStart);
    const std::string origin = base.substr(
        0, authorityEnd == std::string::npos ? base.size() : authorityEnd);
    const std::string basePath = authorityEnd == std::string::npos ? "/" :
        (base[authorityEnd] == '/' ? base.substr(authorityEnd) : "/");
    const size_t baseSuffix = basePath.find_first_of("?#");
    const std::string cleanBasePath = basePath.substr(0, baseSuffix);

    std::string referencePath = relative;
    std::string suffix;
    const size_t suffixAt = relative.find_first_of("?#");
    if (suffixAt != std::string::npos) {
        referencePath = relative.substr(0, suffixAt);
        suffix = relative.substr(suffixAt);
    }

    std::string combined;
    if (!referencePath.empty() && referencePath[0] == '/') {
        combined = referencePath;
    } else if (referencePath.empty()) {
        combined = cleanBasePath;
    } else {
        const size_t slash = cleanBasePath.rfind('/');
        combined = (slash == std::string::npos ? std::string("/") :
                    cleanBasePath.substr(0, slash + 1)) + referencePath;
    }

    const bool trailingSlash = !combined.empty() && combined.back() == '/';
    std::vector<std::string> segments;
    size_t start = 0;
    while (start <= combined.size()) {
        const size_t end = combined.find('/', start);
        const std::string segment = combined.substr(
            start, end == std::string::npos ? end : end - start);
        if (segment.empty() || segment == ".") {
            // Ignore empty and current-directory path components.
        } else if (segment == "..") {
            if (!segments.empty()) segments.pop_back();
        } else {
            segments.push_back(segment);
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    std::string normalized = "/";
    for (size_t index = 0; index < segments.size(); ++index) {
        if (index) normalized += "/";
        normalized += segments[index];
    }
    if (trailingSlash && normalized.back() != '/') normalized += "/";
    return origin + normalized + suffix;
}

Json AppCatalog::parsePackage(const Json &item, const std::string &page,
                              const std::string &repoUrl) {
    if (!item.isObject()) return Json();
    const Json manifest = item["manifest"];
    if (!manifest.isObject()) return Json();
    const std::string id = packageString(item["id"], manifest["id"]);
    const std::string version = manifest["version"].string();
    const std::string ipkPath = manifest["ipkUrl"].string();
    const std::string hash = lowerAscii(manifest["ipkHash"]["sha256"].string());
    if (!validPackageName(id) || version.empty() || ipkPath.empty() || hash.size() != 64)
        return Json();
    for (char character : hash)
        if (!((character >= '0' && character <= '9') ||
              (character >= 'a' && character <= 'f')))
            return Json();

    const std::string manifestUrl = item["manifestUrl"].string().empty()
                                        ? page : item["manifestUrl"].string();
    const std::string resolvedIpk = resolveUrl(manifestUrl, ipkPath);
    if (!startsWithHttps(resolvedIpk)) return Json();

    const std::string rawIcon = packageString(item["iconUri"], manifest["iconUri"]);
    const Json itemSize = item["ipkSize"];
    const Json manifestSize = manifest["ipkSize"];
    const Json genericSize = manifest["size"];
    Json size;
    if (manifestSize.isNumber() && manifestSize.number() > 0)
        size = manifestSize;
    else if (itemSize.isNumber() && itemSize.number() > 0)
        size = itemSize;
    else if (genericSize.isNumber() && genericSize.number() > 0)
        size = genericSize;

    Json app = Json::object();
    app.set("id", Json::text(id));
    app.set("title", Json::text(packageString(item["title"], manifest["title"]).empty()
                                     ? id : packageString(item["title"], manifest["title"])));
    app.set("description", Json::text(packageString(item["shortDescription"],
                                                     manifest["appDescription"])));
    app.set("iconUri", Json::text(startsWithHttps(rawIcon) ? rawIcon : ""));
    app.set("version", Json::text(version));
    app.set("ipkUrl", Json::text(resolvedIpk));
    app.set("sha256", Json::text(hash));
    app.set("size", size);
    app.set("category", Json::text(packageString(item["category"], manifest["category"])));
    const Json rootRequired = manifest["rootRequired"];
    if (rootRequired.isBoolean() && rootRequired.boolean())
        app.set("rootRequired", Json::flag(true));
    else if (rootRequired.isString() && rootRequired.string() == "optional")
        app.set("rootRequired", Json::text("optional"));
    else
        app.set("rootRequired", Json::flag(false));
    app.set("vetted", Json::flag(repoUrl == kDefaultRepo));
    app.set("repo", Json::text(repoUrl));
    return app;
}

Json AppCatalog::fetchRepository(const std::string &base, std::string &error) const {
    Json apps = Json::array();
    std::map<std::string, bool> seen;
    for (int page = 1; page <= kMaximumPages; ++page) {
        const std::string url = pageUrl(base, page);
        Json document;
        if (!fetcher_(url, "tvweb/" + version_, document, error)) return Json();
        const Json packages = document["packages"];
        if (!document.isObject() || !packages.isArray()) {
            error = "not a package catalog: " + url;
            return Json();
        }
        for (const Json &item : packages.items()) {
            Json app = parsePackage(item, url, base);
            if (!app.isObject()) continue;
            const std::string id = app["id"].string();
            if (seen[id]) continue;
            seen[id] = true;
            apps.add(app);
        }
        if (page >= maxPage(document)) break;
    }
    error.clear();
    return apps;
}

void AppCatalog::refresh(const std::vector<std::string> &urls, int64_t nowMs) {
    std::map<std::string, Repository> previous;
    for (const Repository &repo : repositories_) previous[repo.url] = repo;
    std::vector<Repository> next;
    std::vector<std::string> errors;
    bool any = false;
    bool fresh = false;
    for (const std::string &url : urls) {
        Repository repo;
        repo.url = url;
        repo.apps = fetchRepository(url, repo.error);
        repo.hasApps = repo.apps.isArray();
        if (!repo.hasApps) {
            const auto old = previous.find(url);
            if (old != previous.end() && old->second.hasApps) {
                repo.apps = old->second.apps;
                repo.hasApps = true;
            }
            if (repo.error.empty()) repo.error = "catalog unavailable";
            errors.push_back(repo.error);
        } else {
            fresh = true;
        }
        any = any || repo.hasApps;
        next.push_back(std::move(repo));
    }
    repositories_ = std::move(next);
    hasCache_ = true;
    hasAnyApps_ = any;
    attemptedAtMs_ = nowMs;
    if (fresh) fetchedAtMs_ = nowMs;
    error_.clear();
    for (const std::string &error : errors) {
        if (!error_.empty()) error_ += "; ";
        error_ += error;
    }
}

Json AppCatalog::response(const Json &installedReply) const {
    Json installed = Json::object();
    Json store = Json::object();
    Json rawApps = installedReply["apps"];
    if (!rawApps.isArray()) rawApps = installedReply["launchPoints"];
    for (const Json &app : rawApps.items()) {
        const std::string id = app["id"].string();
        if (id.empty()) continue;
        installed.set(id, Json::text(jsonString(app["version"])));
        if (startsWith(app["folderPath"].string(), "/media/cryptofs/"))
            store.set(id, Json::flag(true));
    }

    Json result = Json::object();
    const Repository *main = repositories_.empty() ? nullptr : &repositories_[0];
    result.set("ok", Json::flag(main && main->hasApps));
    Json apps = Json::array();
    if (main && main->hasApps) {
        for (const Json &catalogApp : main->apps.items()) {
            Json app = catalogApp;
            const std::string id = app["id"].string();
            const bool isInstalled = installed.has(id);
            if (isInstalled) app.set("installedVersion", installed[id]);
            else app.set("installedVersion", Json());
            if (isHbcPackage(id))
                app.set("state", Json::text("hbc"));
            else if (!isInstalled)
                app.set("state", Json::text("none"));
            else if (store.has(id))
                app.set("state", Json::text("store"));
            else if (compareVersions(app["version"].string(), installed[id].string()) > 0)
                app.set("state", Json::text("update"));
            else
                app.set("state", Json::text("installed"));
            apps.add(app);
        }
    }
    result.set("apps", apps);
    result.set("fetchedAt", Json::numeric(static_cast<double>(fetchedAtMs_)));
    if (!error_.empty()) result.set("error", Json::text(error_));

    Json others = Json::array();
    Json repos = Json::array();
    for (size_t index = 1; index < repositories_.size(); ++index) {
        const Repository &repo = repositories_[index];
        if (repo.hasApps) {
            for (const Json &catalogApp : repo.apps.items()) {
                Json app = catalogApp;
                const std::string id = app["id"].string();
                const bool isInstalled = installed.has(id);
                if (isInstalled) app.set("installedVersion", installed[id]);
                else app.set("installedVersion", Json());
                if (isHbcPackage(id))
                    app.set("state", Json::text("hbc"));
                else if (!isInstalled)
                    app.set("state", Json::text("none"));
                else if (store.has(id))
                    app.set("state", Json::text("store"));
                else if (compareVersions(app["version"].string(), installed[id].string()) > 0)
                    app.set("state", Json::text("update"));
                else
                    app.set("state", Json::text("installed"));
                others.add(app);
            }
        }
        Json info = Json::object();
        info.set("url", Json::text(repo.url));
        info.set("label", Json::text(repoLabel(repo.url)));
        info.set("count", Json::numeric(static_cast<double>(repo.hasApps ? repo.apps.size() : 0)));
        if (!repo.error.empty()) info.set("error", Json::text(repo.error));
        repos.add(info);
    }
    result.set("others", others);
    result.set("repos", repos);
    return result;
}

Json AppCatalog::get(const Json &configuredRepos, const Json &installedReply, bool force,
                     int64_t nowMs) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> urls(1, kDefaultRepo);
    if (configuredRepos.isArray()) {
        for (const Json &repo : configuredRepos.items()) {
            if (!repo.isString()) continue;
            const std::string url = repo.string();
            if (std::find(urls.begin(), urls.end(), url) == urls.end()) urls.push_back(url);
        }
    }
    if (urls != cachedUrls_) {
        cachedUrls_ = urls;
        repositories_.clear();
        hasCache_ = false;
        hasAnyApps_ = false;
        fetchedAtMs_ = 0;
        attemptedAtMs_ = 0;
        error_.clear();
    }
    const int64_t minimumAge = force ? kRetryMs : kCacheMs;
    const bool failed = !hasCache_ || !hasAnyApps_ || !error_.empty();
    const int64_t retryAge = std::min(minimumAge, failed ? kRetryMs : minimumAge);
    const bool shouldFetch = !hasCache_ ||
        (!failed && nowMs - fetchedAtMs_ >= minimumAge) ||
        (failed && nowMs - attemptedAtMs_ >= retryAge);
    if (shouldFetch) refresh(urls, nowMs);
    return response(installedReply);
}

Json AppCatalog::findPackage(const Json &configuredRepos, const Json &installedReply,
                             const std::string &id, const std::string &repoUrl,
                             int64_t nowMs) {
    if (!validPackageName(id)) {
        Json failure = Json::object();
        failure.set("ok", Json::flag(false));
        failure.set("error", Json::text("not in the catalog: " + id));
        return failure;
    }
    const std::string selectedRepo = repoUrl.empty() ? kDefaultRepo : repoUrl;
    const Json catalog = get(configuredRepos, installedReply, false, nowMs);
    const bool defaultSelected = selectedRepo == kDefaultRepo;
    if (defaultSelected && !catalog["ok"].boolean()) {
        Json failure = Json::object();
        failure.set("ok", Json::flag(false));
        failure.set("error", Json::text(catalog["error"].string("catalog unavailable")));
        return failure;
    }
    const Json apps = defaultSelected ? catalog["apps"] : catalog["others"];
    for (const Json &app : apps.items()) {
        if (app["id"].string() == id && app["repo"].string() == selectedRepo) {
            Json result = Json::object();
            result.set("ok", Json::flag(true));
            result.set("pkg", app);
            return result;
        }
    }
    Json failure = Json::object();
    failure.set("ok", Json::flag(false));
    failure.set("error", Json::text("not in the catalog: " + id));
    return failure;
}

const std::string &AppCatalog::defaultRepository() {
    static const std::string repository = kDefaultRepo;
    return repository;
}

Json AppCatalog::customRepositories(const Json &configuredRepos) {
    Json repositories = Json::array();
    std::set<std::string> seen;
    seen.insert(kDefaultRepo);
    if (!configuredRepos.isArray()) return repositories;
    for (const Json &entry : configuredRepos.items()) {
        if (!entry.isString()) continue;
        if (seen.insert(entry.string()).second)
            repositories.add(entry);
    }
    return repositories;
}

std::string AppCatalog::normalizeRepositoryCandidate(const std::string &input) {
    const std::string value = trimAscii(input);
    static const std::string prefix = "https://github.com/";
    if (!asciiEqualAt(value, 0, prefix)) return value;

    const size_t pathStart = prefix.size();
    const size_t suffix = value.find_first_of("?#", pathStart);
    std::string path = value.substr(pathStart, suffix == std::string::npos
                                                  ? std::string::npos
                                                  : suffix - pathStart);
    if (!path.empty() && path.back() == '/') path.pop_back();
    const size_t slash = path.find('/');
    if (slash == std::string::npos || path.find('/', slash + 1) != std::string::npos)
        return value;
    const size_t repositoryStart = slash + 1;
    const size_t repositoryLength = path.size() - repositoryStart;
    if (repositoryLength > 4 && asciiEqualAt(path, path.size() - 4, ".git"))
        path.erase(path.size() - 4);
    const std::string owner = path.substr(0, slash);
    const std::string repository = path.substr(slash + 1);
    if (!githubRepoPart(owner) || !githubRepoPart(repository)) return value;
    return "https://raw.githubusercontent.com/" + owner + "/" + repository +
           "/HEAD/repo.json";
}

std::string AppCatalog::repositoryInputError(const std::string &url) {
    const bool http = asciiEqualAt(url, 0, "http://");
    const bool https = startsWithHttps(url);
    if (!http && !https)
        return "the address must start with http:// or https://";
    if (exceedsUtf16Length(url, kMaximumUrlBytes))
        return "the address is longer than 2048 characters";
    if (hasUrlControlCharacter(url))
        return "the address contains spaces or control characters";
    if (!https)
        return "the address must start with https://";
    return std::string();
}

Json AppCatalog::checkRepository(const std::string &url) const {
    std::string error = repositoryInputError(url);
    if (!error.empty()) {
        Json failure = Json::object();
        failure.set("ok", Json::flag(false));
        failure.set("error", Json::text(error));
        return failure;
    }
    if (!validUrl(url, error)) {
        Json failure = Json::object();
        failure.set("ok", Json::flag(false));
        failure.set("error", Json::text(error));
        return failure;
    }
    Json document;
    if (!fetcher_(url, "tvweb/" + version_, document, error)) {
        Json failure = Json::object();
        failure.set("ok", Json::flag(false));
        failure.set("error", Json::text(error.empty() ? hostError(url, 0) : error));
        return failure;
    }
    if (!document.isObject() || !document["packages"].isArray()) {
        Json failure = Json::object();
        failure.set("ok", Json::flag(false));
        failure.set("error", Json::text("not a package catalog: " + url));
        return failure;
    }
    Json result = Json::object();
    result.set("ok", Json::flag(true));
    return result;
}

void AppCatalog::invalidate() {
    std::lock_guard<std::mutex> lock(mutex_);
    hasCache_ = false;
    hasAnyApps_ = false;
    fetchedAtMs_ = 0;
    attemptedAtMs_ = 0;
    error_.clear();
    repositories_.clear();
    cachedUrls_.clear();
}

} // namespace service
} // namespace nativetv
