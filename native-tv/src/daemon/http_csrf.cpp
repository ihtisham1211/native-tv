#include "daemon/http_csrf.hpp"
#include <algorithm>
#include <string>

namespace nativetv {
namespace daemon {
namespace {
enum class ContentTypeRule { JsonPrefix, OctetStreamExact, JpegExact };

struct GuardedRoute {
    const char *path;
    ContentTypeRule contentType;
};

const GuardedRoute kGuardedRoutes[] = {
    {"/api/ssh", ContentTypeRule::JsonPrefix},
    {"/api/screentime", ContentTypeRule::JsonPrefix},
    {"/api/apps/install/fetch", ContentTypeRule::JsonPrefix},
    {"/api/apps/install/url", ContentTypeRule::JsonPrefix},
    {"/api/apps/install/confirm", ContentTypeRule::JsonPrefix},
    {"/api/apps/install/cancel", ContentTypeRule::JsonPrefix},
    {"/api/apps/install/repos", ContentTypeRule::JsonPrefix},
    {"/api/apps/install/sideload", ContentTypeRule::JsonPrefix},
    {"/api/apps/install/upload", ContentTypeRule::OctetStreamExact},
    {"/api/apps/add-page", ContentTypeRule::JsonPrefix},
    {"/api/apps/remove-page", ContentTypeRule::JsonPrefix},
    {"/api/apps/edit-page", ContentTypeRule::JsonPrefix},
    {"/api/apps/rename", ContentTypeRule::JsonPrefix},
    {"/api/apps/uninstall", ContentTypeRule::JsonPrefix},
    {"/api/apps/hide", ContentTypeRule::JsonPrefix},
    {"/api/apps/unhide", ContentTypeRule::JsonPrefix},
    {"/api/apps/unhide-all", ContentTypeRule::JsonPrefix},
    {"/api/apps/tile-hiding", ContentTypeRule::JsonPrefix},
    {"/api/services/toggle", ContentTypeRule::JsonPrefix},
    {"/api/homescreen", ContentTypeRule::JsonPrefix},
    {"/api/home/layout", ContentTypeRule::JsonPrefix},
    {"/api/home/profile", ContentTypeRule::JsonPrefix},
    {"/api/home/start-layout", ContentTypeRule::JsonPrefix},
    {"/api/home/startup", ContentTypeRule::JsonPrefix},
    {"/api/home/wallpapers", ContentTypeRule::JsonPrefix},
    {"/api/home/enable", ContentTypeRule::JsonPrefix},
    {"/api/home/disable", ContentTypeRule::JsonPrefix},
    {"/api/home/open", ContentTypeRule::JsonPrefix},
    {"/api/home/recents", ContentTypeRule::JsonPrefix},
    {"/api/home/beat", ContentTypeRule::JsonPrefix},
    {"/api/home/wallpapers/upload", ContentTypeRule::JpegExact},
    {"/api/quickaccess", ContentTypeRule::JsonPrefix},
    {"/api/remotekeys", ContentTypeRule::JsonPrefix},
    {"/api/preload/toggle", ContentTypeRule::JsonPrefix},
    {"/api/settings", ContentTypeRule::JsonPrefix},
    {"/api/backup", ContentTypeRule::JsonPrefix},
    {"/api/control", ContentTypeRule::JsonPrefix},
    {"/api/adblock/custom", ContentTypeRule::JsonPrefix},
    {"/api/services/toggle", ContentTypeRule::JsonPrefix},
    {"/api/setup/network", ContentTypeRule::JsonPrefix},
    {"/api/setup/done", ContentTypeRule::JsonPrefix},
    {"/api/setup/always-ready", ContentTypeRule::JsonPrefix},
};

const GuardedRoute *guardedRoute(const std::string &path) {
    for (const GuardedRoute &route : kGuardedRoutes) {
        if (path == route.path)
            return &route;
    }
    return nullptr;
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(character >= 'A' && character <= 'Z' ? character + ('a' - 'A')
                                                                      : character);
    });
    return value;
}

bool validContentType(ContentTypeRule rule, const std::string &header) {
    const std::string contentType = lower(header);
    switch (rule) {
    case ContentTypeRule::JsonPrefix:
        return contentType.compare(0, 16, "application/json") == 0;
    case ContentTypeRule::OctetStreamExact:
        return contentType == "application/octet-stream";
    case ContentTypeRule::JpegExact:
        return contentType == "image/jpeg";
    }
    return false;
}

bool sameOrigin(const HttpRequest &request) {
    const std::string origin = request.header("origin");
    if (origin.empty())
        return true;
    std::string originHost = origin;
    if (origin.compare(0, 7, "http://") == 0)
        originHost.erase(0, 7);
    else if (origin.compare(0, 8, "https://") == 0)
        originHost.erase(0, 8);
    return originHost == request.header("host");
}

bool sendRefusal(HttpExchange &exchange, int status, const std::string &message) {
    const std::string body = "{\"ok\":false,\"error\":\"" + message + "\"}";
    return exchange.respond(status,
                            {{"Content-Type", "application/json; charset=utf-8"},
                             {"Cache-Control", "no-store"},
                             {"X-Content-Type-Options", "nosniff"},
                             {"Connection", "close"}},
                            body);
}
} // namespace

bool rejectCrossSitePost(HttpExchange &exchange) {
    const HttpRequest &request = exchange.request();
    if (request.method != "POST")
        return false;
    const GuardedRoute *route = guardedRoute(request.uri);
    if (!route)
        return false;

    std::string requiredType;
    if (route->contentType == ContentTypeRule::JsonPrefix)
        requiredType = "application/json";
    else if (route->contentType == ContentTypeRule::OctetStreamExact)
        requiredType = "application/octet-stream";
    else if (route->contentType == ContentTypeRule::JpegExact)
        requiredType = "image/jpeg";
    if (!validContentType(route->contentType, request.header("content-type"))) {
        (void)sendRefusal(exchange, 415, "Content-Type must be " + requiredType);
        return true;
    }
    if (!sameOrigin(request)) {
        (void)sendRefusal(exchange, 403, "cross-origin request refused");
        return true;
    }
    return false;
}
} // namespace daemon
} // namespace nativetv
