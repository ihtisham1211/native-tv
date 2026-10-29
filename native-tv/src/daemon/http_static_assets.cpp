#include "daemon/http_static_assets.hpp"
#include "civetweb.h"
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <fcntl.h>
#include <map>
#include <regex>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>
#include <zlib.h>

namespace nativetv {
namespace daemon {
namespace {
const size_t kMaximumAssetBytes = 8u * 1024u * 1024u;
const size_t kMaximumUiBytes = 8u * 1024u * 1024u;

bool startsWith(const std::string &value, const std::string &prefix) {
    return value.compare(0, prefix.size(), prefix) == 0;
}

bool safeRelativePath(const std::string &path) {
    if (path.empty() || path[0] == '/' || path.find('\\') != std::string::npos ||
        path.find('\0') != std::string::npos)
        return false;
    size_t start = 0;
    while (start <= path.size()) {
        const size_t end = path.find('/', start);
        const size_t limit = end == std::string::npos ? path.size() : end;
        if (limit == start)
            return false;
        const std::string segment = path.substr(start, limit - start);
        if (segment == "." || segment == "..")
            return false;
        for (unsigned char character : segment) {
            const bool valid = (character >= 'a' && character <= 'z') ||
                               (character >= 'A' && character <= 'Z') ||
                               (character >= '0' && character <= '9') ||
                               character == '.' || character == '_' || character == '-';
            if (!valid)
                return false;
        }
        if (end == std::string::npos)
            return true;
        start = end + 1;
    }
    return false;
}

bool decodePath(const std::string &input, std::string &output) {
    if (input.empty() || input.size() > 8192)
        return false;
    std::vector<char> decoded(input.size() + 1, '\0');
    const int length = mg_url_decode(input.c_str(), static_cast<int>(input.size()),
                                     decoded.data(), static_cast<int>(decoded.size()), 0);
    if (length < 0 || static_cast<size_t>(length) > input.size())
        return false;
    if (std::strlen(decoded.data()) != static_cast<size_t>(length))
        return false;
    output.assign(decoded.data(), static_cast<size_t>(length));
    return true;
}

bool readFile(const std::string &root, const std::string &relative, std::string &contents,
              size_t maximumBytes) {
    if (root.empty() || !safeRelativePath(relative))
        return false;
    const std::string candidate = root + "/" + relative;
    char resolved[4096];
    if (!realpath(candidate.c_str(), resolved))
        return false;
    const std::string fullPath(resolved);
    const std::string prefix = root + "/";
    if (!startsWith(fullPath, prefix))
        return false;

    int flags = O_RDONLY;
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    const int descriptor = open(fullPath.c_str(), flags);
    if (descriptor < 0)
        return false;
    struct stat info;
    bool ok = fstat(descriptor, &info) == 0 && S_ISREG(info.st_mode) && info.st_size >= 0 &&
              static_cast<unsigned long long>(info.st_size) <= maximumBytes;
    contents.clear();
    if (ok) {
        contents.reserve(static_cast<size_t>(info.st_size));
        char buffer[16384];
        for (;;) {
            const ssize_t count = read(descriptor, buffer, sizeof(buffer));
            if (count < 0 && errno == EINTR)
                continue;
            if (count < 0) {
                ok = false;
                break;
            }
            if (count == 0)
                break;
            if (static_cast<size_t>(count) > maximumBytes ||
                contents.size() > maximumBytes - static_cast<size_t>(count)) {
                ok = false;
                break;
            }
            contents.append(buffer, static_cast<size_t>(count));
        }
    }
    close(descriptor);
    if (!ok)
        contents.clear();
    return ok;
}

std::string attribute(const std::string &attributes, const std::string &name) {
    const std::regex expression("(^|[ \\t\\r\\n])" + name +
                                "[ \\t\\r\\n]*=[ \\t\\r\\n]*[\\\"']([^\\\"']*)[\\\"']",
                                std::regex::icase);
    std::smatch match;
    return std::regex_search(attributes, match, expression) ? match[2].str() : "";
}

std::string replaceFontVersions(std::string css, const std::string &version) {
    const std::regex expression("(url\\('/assets/fonts/[A-Za-z0-9_.-]+)'\\)");
    return std::regex_replace(css, expression, "$1?v=" + version + "')");
}

std::string cssSections(const std::string &css, const std::string &sections,
                        std::string &missing) {
    if (sections.empty())
        return css;
    const std::regex marker("/\\*\\s*@section\\s+([A-Za-z0-9_-]+)\\s*\\*/");
    std::map<std::string, std::string> byName;
    size_t position = 0;
    std::string current;
    for (std::sregex_iterator item(css.begin(), css.end(), marker), end; item != end; ++item) {
        const size_t start = static_cast<size_t>(item->position());
        if (!current.empty())
            byName[current] = css.substr(position, start - position);
        current = (*item)[1].str();
        position = start + static_cast<size_t>(item->length());
    }
    if (!current.empty())
        byName[current] = css.substr(position);

    std::istringstream names(sections);
    std::string name;
    std::string result;
    while (names >> name) {
        const std::map<std::string, std::string>::const_iterator found = byName.find(name);
        if (found == byName.end()) {
            if (missing.empty())
                missing = name;
            continue;
        }
        if (!result.empty())
            result += '\n';
        result += found->second;
    }
    return result;
}

bool inlineAssetParts(const std::string &root, std::string &html, std::string &missing,
                      const std::string &version) {
    const std::regex linkExpression("<link\\b([^>]*)>[ \\t\\r\\n]*", std::regex::icase);
    std::string rewritten;
    size_t cursor = 0;
    for (std::sregex_iterator item(html.begin(), html.end(), linkExpression), end;
         item != end; ++item) {
        const size_t start = static_cast<size_t>(item->position());
        rewritten.append(html, cursor, start - cursor);
        const std::string tag = item->str();
        const std::string attrs = (*item)[1].str();
        const std::string href = attribute(attrs, "href");
        const std::string rel = attribute(attrs, "rel");
        const std::string sections = attribute(attrs, "data-sections");
        const std::string id = attribute(attrs, "id");
        std::smatch assetMatch;
        const std::regex assetExpression("^/assets/(ui|google-tv)/([A-Za-z0-9_./-]+\\.css)$");
        if (rel != "stylesheet" || !std::regex_match(href, assetMatch, assetExpression)) {
            rewritten += tag;
        } else {
            const std::string relative = assetMatch[1].str() + "/" + assetMatch[2].str();
            std::string css;
            if (!safeRelativePath(relative) || !readFile(root, relative, css, kMaximumUiBytes)) {
                missing = relative;
                return false;
            }
            css = replaceFontVersions(css, version);
            if (!sections.empty()) {
                std::string missingSection;
                css = cssSections(css, sections, missingSection);
                if (!missingSection.empty()) {
                    missing = relative + "#" + missingSection;
                    return false;
                }
            }
            rewritten += "<style";
            if (!id.empty() && std::regex_match(id, std::regex("[A-Za-z0-9_-]+")))
                rewritten += " id=\"" + id + "\"";
            rewritten += ">\n" + css + "</style>\n";
        }
        cursor = start + static_cast<size_t>(item->length());
    }
    rewritten.append(html, cursor, std::string::npos);
    html.swap(rewritten);

    const std::regex scriptRun("(?:[ \\t]*<script src=\"/assets/(?:ui|google-tv)/[A-Za-z0-9_./-]+\\.js\"></script>[ \\t]*\\r?\\n)+");
    rewritten.clear();
    cursor = 0;
    for (std::sregex_iterator item(html.begin(), html.end(), scriptRun), end; item != end; ++item) {
        const size_t start = static_cast<size_t>(item->position());
        rewritten.append(html, cursor, start - cursor);
        const std::string run = item->str();
        const std::regex script("<script src=\"/assets/((?:ui|google-tv)/[A-Za-z0-9_./-]+\\.js)\"></script>");
        std::string scripts;
        bool first = true;
        for (std::sregex_iterator part(run.begin(), run.end(), script), partEnd;
             part != partEnd; ++part) {
            std::string source;
            const std::string path = (*part)[1].str();
            if (!readFile(root, path, source, kMaximumUiBytes)) {
                missing = path;
                return false;
            }
            if (!first)
                scripts += "\n;\n";
            scripts += source;
            first = false;
        }
        rewritten += "<script>\n" + scripts + "</script>\n";
        cursor = start + static_cast<size_t>(item->length());
    }
    rewritten.append(html, cursor, std::string::npos);
    html.swap(rewritten);
    return html.size() <= kMaximumUiBytes;
}

std::string missingAssetsPage(const std::string &missing) {
    return "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
           "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
           "<title>Native Tv &middot; dashboard assets missing</title></head><body>"
           "<h1>Dashboard assets are missing</h1><p><code>" + missing +
           "</code> was not found. The server is running normally, but it has no dashboard to serve.</p>"
           "</body></html>";
}

std::string contentType(const std::string &path) {
    const size_t dot = path.find_last_of('.');
    std::string extension = dot == std::string::npos ? "" : path.substr(dot);
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char value) {
                       return static_cast<char>(value >= 'A' && value <= 'Z'
                                                    ? value + ('a' - 'A')
                                                    : value);
                   });
    if (extension == ".html") return "text/html; charset=utf-8";
    if (extension == ".otf" || extension == ".ttf" || extension == ".woff" ||
        extension == ".woff2") return "application/javascript";
    if (extension == ".css" || extension == ".js") return "application/javascript";
    if (extension == ".json") return "application/json; charset=utf-8";
    if (extension == ".png") return "image/png";
    if (extension == ".svg") return "image/svg+xml";
    return "application/octet-stream";
}

std::string etag(const std::string &body) {
    unsigned long long hash = 14695981039346656037ULL;
    for (unsigned char value : body) {
        hash ^= value;
        hash *= 1099511628211ULL;
    }
    char result[48];
    std::snprintf(result, sizeof(result), "\"gh-%016llx\"", hash);
    return result;
}

std::string cacheControl(const std::string &path, const std::string &query) {
    const size_t dot = path.find_last_of('.');
    std::string extension = dot == std::string::npos ? "" : path.substr(dot);
    if (extension == ".html" || extension == ".json")
        return "no-cache";
    size_t start = 0;
    while (start <= query.size()) {
        const size_t end = query.find('&', start);
        const std::string pair = query.substr(start, end == std::string::npos
                                                       ? std::string::npos
                                                       : end - start);
        if (pair.compare(0, 2, "v=") == 0 && pair.size() > 2)
            return "public, max-age=31536000, immutable";
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    return "public, max-age=86400";
}

bool supportedMethod(const HttpRequest &request) {
    return request.method == "GET" || request.method == "HEAD";
}

bool acceptsGzip(const std::string &value) {
    std::string encodings = value;
    std::transform(encodings.begin(), encodings.end(), encodings.begin(),
                   [](unsigned char character) {
                       return static_cast<char>(character >= 'A' && character <= 'Z'
                                                    ? character + ('a' - 'A')
                                                    : character);
                   });
    size_t start = 0;
    while (start <= encodings.size()) {
        const size_t end = encodings.find(',', start);
        const std::string item = encodings.substr(start, end == std::string::npos
                                                            ? std::string::npos
                                                            : end - start);
        const size_t semicolon = item.find(';');
        std::string name = item.substr(0, semicolon);
        name.erase(std::remove_if(name.begin(), name.end(), [](unsigned char character) {
                       return character == ' ' || character == '\t';
                   }), name.end());
        bool accepted = true;
        size_t parameterStart = semicolon == std::string::npos ? item.size() : semicolon + 1;
        while (parameterStart < item.size()) {
            const size_t parameterEnd = item.find(';', parameterStart);
            std::string parameter = item.substr(parameterStart,
                parameterEnd == std::string::npos ? std::string::npos : parameterEnd - parameterStart);
            const size_t equals = parameter.find('=');
            if (equals != std::string::npos && parameter.substr(0, equals) == "q") {
                char *endPointer = nullptr;
                const double quality = std::strtod(parameter.c_str() + equals + 1, &endPointer);
                accepted = endPointer != parameter.c_str() + equals + 1 &&
                           quality > 0.0 && quality <= 1.0;
                break;
            }
            if (parameterEnd == std::string::npos)
                break;
            parameterStart = parameterEnd + 1;
        }
        if (name == "gzip" && accepted)
            return true;
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    return false;
}

bool gzipEncode(const std::string &input, std::string &output) {
    z_stream stream;
    std::memset(&stream, 0, sizeof(stream));
    if (deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, MAX_WBITS + 16,
                     MAX_MEM_LEVEL, Z_DEFAULT_STRATEGY) != Z_OK)
        return false;
    const uLong bound = deflateBound(&stream, static_cast<uLong>(input.size()));
    if (bound > static_cast<uLong>(kMaximumUiBytes + 65536)) {
        deflateEnd(&stream);
        return false;
    }
    output.resize(static_cast<size_t>(bound));
    stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(input.data()));
    stream.avail_in = static_cast<uInt>(input.size());
    stream.next_out = reinterpret_cast<Bytef *>(&output[0]);
    stream.avail_out = static_cast<uInt>(output.size());
    const int status = deflate(&stream, Z_FINISH);
    const size_t written = static_cast<size_t>(stream.total_out);
    deflateEnd(&stream);
    if (status != Z_STREAM_END) {
        output.clear();
        return false;
    }
    output.resize(written);
    return true;
}

} // namespace

HttpStaticAssets::HttpStaticAssets(std::string root, std::string version)
    : root_(std::move(root)), version_(std::move(version)) {
    char resolved[4096];
    if (realpath(root_.c_str(), resolved))
        root_ = resolved;
    else
        root_.clear();
    if (version_.empty())
        version_ = "1";
}

bool HttpStaticAssets::assemblePage(const std::string &relative, std::string &page,
                                    std::string &missing) const {
    page.clear();
    missing.clear();
    if (!safeRelativePath(relative) || !readFile(root_, relative, page, kMaximumUiBytes)) {
        missing = relative;
        return false;
    }
    if (!inlineAssetParts(root_, page, missing, version_)) {
        page.clear();
        if (missing.empty())
            missing = "assembled page exceeds the size limit";
        return false;
    }
    return true;
}

bool HttpStaticAssets::loadUi(std::shared_ptr<const std::string> &page,
                              std::shared_ptr<const std::string> &gzipPage,
                              std::string &missing) const {
    std::lock_guard<std::mutex> lock(uiMutex_);
    if (!uiLoaded_) {
        uiLoaded_ = true;
        std::string html;
        if (!readFile(root_, "ui.html", html, kMaximumUiBytes)) {
            uiMissing_ = "ui.html";
        } else if (inlineAssetParts(root_, html, uiMissing_, version_)) {
            std::shared_ptr<const std::string> uncompressed(new std::string(std::move(html)));
            uiPage_ = uncompressed;
            std::string compressed;
            if (gzipEncode(*uiPage_, compressed))
                uiGzipPage_.reset(new std::string(std::move(compressed)));
            uiAvailable_ = true;
        }
    }
    page = uiPage_;
    gzipPage = uiGzipPage_;
    missing = uiMissing_.empty() ? "ui.html" : uiMissing_;
    return uiAvailable_;
}

bool HttpStaticAssets::handle(HttpExchange &exchange) const {
    const HttpRequest &request = exchange.request();
    std::string path;
    if (!decodePath(request.uri, path))
        return startsWith(request.uri, "/assets/")
                   ? exchange.respond(400, {{"Content-Type", "text/plain; charset=utf-8"},
                                            {"Cache-Control", "no-store"}},
                                      "Bad Request\n")
                   : false;

    // The dashboard and Home apps installed on a TV open the pages by their flat
    // names (pages/dashboard.html); each page now lives in a folder of its own.
    {
        const std::string pages = "/assets/google-tv/pages/";
        for (const char *name : {"dashboard", "home", "setup", "setup-phone"}) {
            if (path == pages + name + ".html") {
                path = pages + name + "/index.html";
                break;
            }
        }
    }

    if (path == "/" || path == "/index.html") {
        if (!supportedMethod(request))
            return false;
        std::shared_ptr<const std::string> page;
        std::shared_ptr<const std::string> gzipPage;
        std::string missing;
        if (!loadUi(page, gzipPage, missing)) {
            const std::string body = missingAssetsPage(missing);
            return exchange.respond(503,
                                    {{"Content-Type", "text/html; charset=utf-8"},
                                     {"Cache-Control", "no-store"},
                                     {"X-Content-Type-Options", "nosniff"},
                                     {"Referrer-Policy", "no-referrer"}}, body);
        }
        if (gzipPage && acceptsGzip(request.header("accept-encoding"))) {
            return exchange.respond(200,
                                    {{"Content-Type", "text/html; charset=utf-8"},
                                     {"Content-Encoding", "gzip"},
                                     {"Vary", "Accept-Encoding"},
                                     {"Cache-Control", "no-store"},
                                     {"X-Content-Type-Options", "nosniff"},
                                     {"Referrer-Policy", "no-referrer"}}, *gzipPage);
        }
        return exchange.respond(200,
                                {{"Content-Type", "text/html; charset=utf-8"},
                                 {"Vary", "Accept-Encoding"},
                                 {"Cache-Control", "no-store"},
                                 {"X-Content-Type-Options", "nosniff"},
                                 {"Referrer-Policy", "no-referrer"}}, *page);
    }

    if (path == "/assets/dashboard.html" || path == "/assets/setup.html" ||
        path == "/assets/home.html") {
        if (!supportedMethod(request))
            return false;
        std::string destination = path == "/assets/dashboard.html"
                                      ? "/assets/google-tv/pages/dashboard/index.html"
                                      : path == "/assets/setup.html"
                                            ? "/assets/google-tv/pages/setup/index.html"
                                            : "/assets/google-tv/pages/home/index.html";
        if (!request.query.empty())
            destination += "?" + request.query;
        return exchange.respond(302, {{"Location", destination}, {"Cache-Control", "no-store"}}, "");
    }

    if (!startsWith(path, "/assets/") || !supportedMethod(request))
        return false;
    const std::string relative = path.substr(std::string("/assets/").size());
    if (!safeRelativePath(relative))
        return exchange.respond(400, {{"Content-Type", "text/plain; charset=utf-8"},
                                      {"Cache-Control", "no-store"}},
                                "Bad Request\n");

    std::string body;
    if (!readFile(root_, relative, body, kMaximumAssetBytes))
        return false;
    // A TV page naming parts under assets/google-tv/ is served with them inline,
    // so the TV gets one response. A missing part leaves the page as it is,
    // still fetching its parts.
    if (startsWith(relative, "google-tv/") && relative.size() > 5 &&
        relative.compare(relative.size() - 5, 5, ".html") == 0 &&
        body.find("/assets/google-tv/") != std::string::npos) {
        std::string assembled;
        std::string missing;
        if (assemblePage(relative, assembled, missing)) body = std::move(assembled);
    }
    const std::string tag = etag(body);
    const std::string cache = cacheControl(relative, request.query);
    if (request.header("if-none-match") == tag)
        return exchange.respond(304, {{"ETag", tag}, {"Cache-Control", cache}}, "");
    return exchange.respond(200,
                            {{"Content-Type", contentType(relative)},
                             {"Cache-Control", cache},
                             {"ETag", tag}}, body);
}

} // namespace daemon
} // namespace nativetv
