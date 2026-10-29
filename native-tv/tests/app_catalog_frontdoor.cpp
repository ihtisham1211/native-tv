#include "daemon/child_process.hpp"
#include "daemon/event_loop.hpp"
#include "daemon/http_frontdoor.hpp"
#include "daemon/http_server.hpp"
#include "daemon/luna.hpp"
#include <arpa/inet.h>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <mutex>
#include <netinet/in.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace nativetv;
using namespace nativetv::daemon;

namespace {
unsigned short unusedPort() {
    const int descriptor = socket(AF_INET, SOCK_STREAM, 0);
    if (descriptor < 0) throw std::runtime_error("could not create test socket");
    sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(descriptor, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) {
        close(descriptor);
        throw std::runtime_error("could not reserve test port");
    }
    socklen_t length = sizeof(address);
    if (getsockname(descriptor, reinterpret_cast<sockaddr *>(&address), &length) != 0) {
        close(descriptor);
        throw std::runtime_error("could not read test port");
    }
    const unsigned short port = ntohs(address.sin_port);
    close(descriptor);
    return port;
}

std::string get(unsigned short port, const std::string &path) {
    const int descriptor = socket(AF_INET, SOCK_STREAM, 0);
    if (descriptor < 0) throw std::runtime_error("could not create request socket");
    timeval timeout = {5, 0};
    (void)setsockopt(descriptor, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(descriptor, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) {
        close(descriptor);
        throw std::runtime_error("could not connect to app catalog front door");
    }
    const std::string request = "GET " + path + " HTTP/1.1\r\nHost: localhost\r\n"
                                "Authorization: Bearer catalog-route-token\r\n"
                                "Connection: close\r\n\r\n";
    size_t offset = 0;
    while (offset < request.size()) {
        const ssize_t count = send(descriptor, request.data() + offset,
                                   request.size() - offset, 0);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) {
            close(descriptor);
            throw std::runtime_error("could not send app catalog request");
        }
        offset += static_cast<size_t>(count);
    }
    std::string response;
    char buffer[4096];
    for (;;) {
        const ssize_t count = recv(descriptor, buffer, sizeof(buffer), 0);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) {
            close(descriptor);
            throw std::runtime_error("could not read app catalog response");
        }
        if (count == 0) break;
        response.append(buffer, static_cast<size_t>(count));
    }
    close(descriptor);
    return response;
}

std::string post(unsigned short port, const std::string &path,
                 const std::string &body, const std::string &secret) {
    const int descriptor = socket(AF_INET, SOCK_STREAM, 0);
    if (descriptor < 0) throw std::runtime_error("could not create request socket");
    timeval timeout = {5, 0};
    (void)setsockopt(descriptor, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(descriptor, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) {
        close(descriptor);
        throw std::runtime_error("could not connect to app catalog front door");
    }
    const std::string request = "POST " + path + " HTTP/1.1\r\nHost: localhost\r\n"
                                "Content-Type: application/json\r\n" +
                                "Content-Length: " + std::to_string(body.size()) + "\r\n"
                                "X-NativeTv-Proxy-Secret: " + secret + "\r\n"
                                "Connection: close\r\n\r\n" + body;
    size_t offset = 0;
    while (offset < request.size()) {
        const ssize_t count = send(descriptor, request.data() + offset,
                                   request.size() - offset, 0);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) {
            close(descriptor);
            throw std::runtime_error("could not send app catalog request");
        }
        offset += static_cast<size_t>(count);
    }
    std::string response;
    char buffer[4096];
    for (;;) {
        const ssize_t count = recv(descriptor, buffer, sizeof(buffer), 0);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) {
            close(descriptor);
            throw std::runtime_error("could not read app catalog response");
        }
        if (count == 0) break;
        response.append(buffer, static_cast<size_t>(count));
    }
    close(descriptor);
    return response;
}

std::string publicPost(unsigned short port, const std::string &path,
                       const std::string &body, const std::string &origin = "",
                       const std::string &host = "localhost",
                       const std::string &contentType = "application/json") {
    const int descriptor = socket(AF_INET, SOCK_STREAM, 0);
    if (descriptor < 0) throw std::runtime_error("could not create request socket");
    timeval timeout = {5, 0};
    (void)setsockopt(descriptor, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(descriptor, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) {
        close(descriptor);
        throw std::runtime_error("could not connect to app catalog front door");
    }
    std::string request = "POST " + path + " HTTP/1.1\r\nHost: " + host +
                          "\r\nAuthorization: Bearer catalog-route-token\r\n"
                          "Content-Type: " + contentType + "\r\n";
    if (!origin.empty()) request += "Origin: " + origin + "\r\n";
    request += "Content-Length: " + std::to_string(body.size()) +
               "\r\nConnection: close\r\n\r\n" + body;
    size_t offset = 0;
    while (offset < request.size()) {
        const ssize_t count = send(descriptor, request.data() + offset,
                                   request.size() - offset, 0);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) {
            close(descriptor);
            throw std::runtime_error("could not send public app catalog request");
        }
        offset += static_cast<size_t>(count);
    }
    std::string response;
    char buffer[4096];
    for (;;) {
        const ssize_t count = recv(descriptor, buffer, sizeof(buffer), 0);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) {
            close(descriptor);
            throw std::runtime_error("could not read public app catalog response");
        }
        if (count == 0) break;
        response.append(buffer, static_cast<size_t>(count));
    }
    close(descriptor);
    return response;
}

class NodeReloadServer {
  public:
    NodeReloadServer() {
        descriptor_ = socket(AF_INET, SOCK_STREAM, 0);
        if (descriptor_ < 0) throw std::runtime_error("could not create reload socket");
        int reuse = 1;
        (void)setsockopt(descriptor_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        sockaddr_in address = {};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        if (bind(descriptor_, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0 ||
            listen(descriptor_, 8) != 0) {
            close(descriptor_);
            descriptor_ = -1;
            throw std::runtime_error("could not start config reload stub");
        }
        socklen_t length = sizeof(address);
        if (getsockname(descriptor_, reinterpret_cast<sockaddr *>(&address), &length) != 0) {
            close(descriptor_);
            descriptor_ = -1;
            throw std::runtime_error("could not read config reload stub port");
        }
        port_ = ntohs(address.sin_port);
        thread_ = std::thread([this] { run(); });
    }

    ~NodeReloadServer() {
        stopping_.store(true);
        if (descriptor_ >= 0) {
            const int wake = socket(AF_INET, SOCK_STREAM, 0);
            if (wake >= 0) {
                sockaddr_in address = {};
                address.sin_family = AF_INET;
                address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                address.sin_port = htons(port_);
                (void)connect(wake, reinterpret_cast<sockaddr *>(&address), sizeof(address));
                close(wake);
            }
            (void)shutdown(descriptor_, SHUT_RDWR);
        }
        if (thread_.joinable()) thread_.join();
        if (descriptor_ >= 0) close(descriptor_);
    }

    unsigned short port() const { return port_; }
    unsigned requests() const { return requests_.load(); }
    unsigned installStarts() const { return installStarts_.load(); }
    unsigned installActions() const { return installActions_.load(); }
    std::string installBody() const {
        std::lock_guard<std::mutex> lock(installBodyMutex_);
        return installBody_;
    }
    std::string installActionBody() const {
        std::lock_guard<std::mutex> lock(installBodyMutex_);
        return installActionBody_;
    }

  private:
    void run() {
        while (!stopping_.load()) {
            const int client = accept(descriptor_, nullptr, nullptr);
            if (client < 0) {
                if (stopping_.load()) break;
                continue;
            }
            std::string request;
            char buffer[1024];
            size_t headerEnd = std::string::npos;
            size_t expectedSize = 0;
            while (headerEnd == std::string::npos || request.size() < expectedSize) {
                headerEnd = request.find("\r\n\r\n");
                if (headerEnd != std::string::npos && expectedSize == 0) {
                    expectedSize = headerEnd + 4;
                    const size_t lengthHeader = request.find("Content-Length:");
                    if (lengthHeader != std::string::npos) {
                        const size_t valueStart = request.find_first_not_of(" \t",
                            lengthHeader + std::strlen("Content-Length:"));
                        if (valueStart != std::string::npos)
                            expectedSize += static_cast<size_t>(std::strtoul(
                                request.c_str() + valueStart, nullptr, 10));
                    }
                }
                if (headerEnd != std::string::npos && request.size() >= expectedSize)
                    break;
                const ssize_t count = recv(client, buffer, sizeof(buffer), 0);
                if (count <= 0) break;
                request.append(buffer, static_cast<size_t>(count));
            }
            if (headerEnd != std::string::npos) {
                const size_t requestLineEnd = request.find("\r\n");
                const bool installStart = request.compare(0, requestLineEnd,
                    "POST /_internal/apps/install-start HTTP/1.1") == 0;
                const bool installAction = request.compare(0, requestLineEnd,
                    "POST /_internal/apps/install-action HTTP/1.1") == 0;
                std::string responseBody = "{\"ok\":true}";
                if (installStart) {
                    const std::string installReply =
                        "{\"ok\":true,\"install\":{\"jobId\":\"fixture-job\","
                        "\"state\":\"awaiting-confirm\",\"source\":\"catalog\","
                        "\"appId\":\"org.example.youtube\",\"repo\":null,"
                        "\"progress\":{\"bytes\":100,\"total\":100},"
                        "\"preview\":{\"storeInstalled\":false,\"vetted\":true,"
                        "\"services\":[]},\"error\":null,\"result\":null}}";
                    responseBody = installReply;
                    installStarts_.fetch_add(1);
                    std::lock_guard<std::mutex> lock(installBodyMutex_);
                    installBody_ = request.substr(headerEnd + 4, expectedSize - headerEnd - 4);
                } else if (installAction) {
                    const std::string body = request.substr(
                        headerEnd + 4, expectedSize - headerEnd - 4);
                    const Json payload = Json::parse(body);
                    if (payload["action"].string() == "confirm")
                        responseBody =
                            "{\"ok\":true,\"install\":{\"jobId\":\"fixture-job\","
                            "\"state\":\"installing\"}}";
                    installActions_.fetch_add(1);
                    std::lock_guard<std::mutex> lock(installBodyMutex_);
                    installActionBody_ = body;
                } else {
                    requests_.fetch_add(1);
                }
                const std::string response = "HTTP/1.1 200 OK\r\nContent-Length: " +
                    std::to_string(responseBody.size()) + "\r\nConnection: close\r\n\r\n" +
                    responseBody;
                size_t offset = 0;
                while (offset < response.size()) {
                    const ssize_t count = send(client, response.data() + offset,
                                               response.size() - offset, 0);
                    if (count <= 0) break;
                    offset += static_cast<size_t>(count);
                }
            }
            close(client);
        }
    }

    int descriptor_ = -1;
    unsigned short port_ = 0;
    std::atomic<bool> stopping_{false};
    std::atomic<unsigned> requests_{0};
    std::atomic<unsigned> installStarts_{0};
    std::atomic<unsigned> installActions_{0};
    mutable std::mutex installBodyMutex_;
    std::string installBody_;
    std::string installActionBody_;
    std::thread thread_;
};

std::string bodyOf(const std::string &response) {
    const size_t split = response.find("\r\n\r\n");
    if (split == std::string::npos) throw std::runtime_error("invalid HTTP response");
    return response.substr(split + 4);
}

std::string decodeChunked(const std::string &body) {
    std::string result;
    size_t offset = 0;
    for (;;) {
        const size_t lineEnd = body.find("\r\n", offset);
        if (lineEnd == std::string::npos) throw std::runtime_error("invalid chunked response");
        const std::string line = body.substr(offset, lineEnd - offset);
        char *end = nullptr;
        const unsigned long length = std::strtoul(line.c_str(), &end, 16);
        if (end == line.c_str() || *end != '\0')
            throw std::runtime_error("invalid chunk length");
        offset = lineEnd + 2;
        if (!length) return result;
        if (length > body.size() - offset || body.size() - offset - length < 2 ||
            body.compare(offset + length, 2, "\r\n") != 0)
            throw std::runtime_error("truncated chunked response");
        result.append(body, offset, length);
        offset += length + 2;
    }
}

Json package(const std::string &id, const std::string &version) {
    Json manifest = Json::object();
    manifest.set("id", Json::text(id));
    manifest.set("version", Json::text(version));
    manifest.set("ipkUrl", Json::text("https://cdn.example/app.ipk"));
    Json hash = Json::object();
    hash.set("sha256", Json::text(std::string(64, 'b')));
    manifest.set("ipkHash", hash);
    Json item = Json::object();
    item.set("id", Json::text(id));
    item.set("title", Json::text("YouTube Catalog"));
    item.set("manifest", manifest);
    return item;
}
} // namespace

int main(int argc, char **argv) {
    assert(argc == 2);
    setenv("TVWEB_PROXY_SECRET", "catalog-route-secret-0123456789abcdef", 1);
    const std::string stagingDirectory = "/tmp/nativetv-app-inspect-frontdoor-" +
                                         std::to_string(getpid());
    (void)rmdir(stagingDirectory.c_str());
    if (mkdir(stagingDirectory.c_str(), 0700) != 0)
        throw std::runtime_error("could not create package inspection fixture directory");
    if (setenv("TVWEB_INSTALL_STAGING", stagingDirectory.c_str(), 1) != 0)
        throw std::runtime_error("could not set package inspection fixture directory");
    EventLoop loop;
    std::thread eventThread([&loop] { loop.run(); });
    ChildProcessRunner children;
    LunaClientOptions lunaOptions;
    lunaOptions.executable = argv[1];
    lunaOptions.environment.push_back("NATIVETV_FAKE_LUNA_MODE=apps-list");
    LunaClient luna(loop, children, nullptr, lunaOptions);

    ServerConfig config;
    config.port = unusedPort();
    config.token = "catalog-route-token";
    config.allowControl = true;
    Json document = Json::object();
    Json appsConfig = Json::object();
    appsConfig.set("repos", Json::array());
    appsConfig.set("sideload", Json::flag(true));
    document.set("apps", appsConfig);
    Json fetchConfig = Json::object();
    fetchConfig.set("ip", Json::text("6"));
    document.set("fetch", fetchConfig);
    config.document = document;

    const std::string defaultRepo = "https://repo.webosbrew.org/api/apps.json";
    const std::string customRepo = "https://repos.example.org/apps.json";
    const std::string invalidRepo = "https://repos.example.org/not-a-catalog.json";
    const std::string githubRepo =
        "https://raw.githubusercontent.com/Owner/Project/HEAD/repo.json";
    Json catalogDocument = Json::object();
    Json packages = Json::array();
    packages.add(package("org.example.youtube", "3.0"));
    catalogDocument.set("packages", packages);
    Json paging = Json::object();
    paging.set("maxPage", Json::numeric(1));
    catalogDocument.set("paging", paging);
    Json customDocument = Json::object();
    Json customPackages = Json::array();
    customPackages.add(package("org.example.custom", "1.0"));
    customDocument.set("packages", customPackages);
    Json invalidDocument = Json::object();
    invalidDocument.set("message", Json::text("not a catalog"));
    unsigned fetches = 0;
    std::vector<std::string> fetchedUrls;
    service::AppCatalogFetcher fetcher = [&fetches, &catalogDocument, &customDocument,
                                          &invalidDocument, &defaultRepo, &customRepo,
                                          &invalidRepo, &githubRepo, &fetchedUrls](
        const std::string &url, const std::string &userAgent, Json &result,
        std::string &error) {
        ++fetches;
        fetchedUrls.push_back(url);
        if (userAgent != "tvweb/4.2.0") {
            error = "unexpected catalog request";
            return false;
        }
        if (url == defaultRepo) result = catalogDocument;
        else if (url == customRepo || url == githubRepo) result = customDocument;
        else if (url == invalidRepo) result = invalidDocument;
        else {
            error = "unexpected catalog request";
            return false;
        }
        error.clear();
        return true;
    };
    unsigned downloads = 0;
    std::atomic<bool> cancellationStarted(false);
    std::atomic<bool> cancellationObserved(false);
    service::AppPackageDownload packageFetcher = [&downloads, &cancellationStarted,
                                                   &cancellationObserved](
        const std::string &url, const std::string &userAgent,
        const std::string &ipPreference, uint64_t maximumBytes,
        const service::AppPackageDownloadSink &sink, long &status,
        service::AppPackageDownloadFailure &failure,
        const service::AppPackageDownloadCancelled &cancelled) {
        ++downloads;
        assert(!cancelled || !cancelled());
        assert(userAgent == "tvweb/4.2.0");
        assert(ipPreference == "6");
        if (url == "https://cdn.example/missing.ipk") {
            status = 404;
            failure.message = "remote server returned HTTP 404";
            return false;
        }
        if (url == "https://cdn.example/offline.ipk") {
            failure.message = "could not connect to remote server";
            failure.offline = true;
            return false;
        }
        if (url == "https://cdn.example/cancellable.ipk") {
            cancellationStarted.store(true);
            while (!cancelled || !cancelled())
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            cancellationObserved.store(true);
            failure.message = "cancelled";
            return false;
        }
        if (url == "https://cdn.example/oversize.ipk") {
            status = 200;
            failure.message = "download exceeds the allowed size";
            const std::string partial(64 * 1024, 'p');
            assert(sink(partial.data(), partial.size()));
            return false;
        }
        assert(url == "https://cdn.example/package.ipk");
        assert(maximumBytes == 4096);
        status = 200;
        return sink("ipk-", 4) && sink("payload", 7);
    };
    const std::string configPath = stagingDirectory + "/config.json";
    {
        std::ofstream configFile(configPath.c_str(), std::ios::out | std::ios::trunc);
        if (!configFile) throw std::runtime_error("could not write config fixture");
        configFile << document.dump();
    }
    if (chmod(configPath.c_str(), 0600) != 0)
        throw std::runtime_error("could not set config fixture permissions");
    NodeReloadServer reloadServer;
    HttpProxyOptions proxyOptions;
    proxyOptions.host = "127.0.0.1";
    proxyOptions.port = reloadServer.port();
    proxyOptions.sharedSecret = "catalog-route-secret-0123456789abcdef";
    HttpFrontDoor frontDoor(config, "4.2.0", "/tmp/nativetv-catalog-data",
                            configPath,
                            HttpProxy(proxyOptions), HttpProxy(proxyOptions), luna, children,
                            "/tmp/nativetv-catalog-assets", "/proc", "/", 5000,
                            nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                            nullptr, nullptr, nullptr, nullptr, nullptr,
                            std::move(fetcher), std::move(packageFetcher));
    HttpServer server;
    HttpServerOptions options;
    options.host = "127.0.0.1";
    options.port = static_cast<unsigned short>(config.port);
    options.workerCount = 2;
    options.connectionQueue = 8;
    std::string error;
    if (!server.start(options, [&frontDoor](HttpExchange &exchange) {
            frontDoor.handle(exchange);
        }, error)) {
        loop.stop();
        eventThread.join();
        throw std::runtime_error("could not start app catalog HTTP fixture: " + error);
    }

    try {
        const std::string firstResponse = get(server.boundPort(), "/api/apps/catalog");
        assert(firstResponse.find(" 200 ") != std::string::npos);
        Json first = Json::parse(bodyOf(firstResponse));
        assert(first["ok"].boolean());
        assert(first["apps"].size() == 1);
        assert(first["apps"][0]["id"].string() == "org.example.youtube");
        assert(first["apps"][0]["installedVersion"].string() == "2.1");
        assert(first["apps"][0]["state"].string() == "update");
        assert(fetches == 1);

        const std::string secondResponse = get(server.boundPort(), "/api/apps/catalog?force=1");
        assert(secondResponse.find(" 200 ") != std::string::npos);
        Json second = Json::parse(bodyOf(secondResponse));
        assert(second["ok"].boolean());
        assert(fetches == 1);

        const std::string lookup = post(server.boundPort(),
                                        "/_internal/apps/catalog/find",
                                        "{\"id\":\"org.example.youtube\"}",
                                        "catalog-route-secret-0123456789abcdef");
        assert(lookup.find(" 200 ") != std::string::npos);
        Json selected = Json::parse(bodyOf(lookup));
        assert(selected["ok"].boolean());
        assert(selected["pkg"]["id"].string() == "org.example.youtube");
        assert(fetches == 1);

        const std::string missingInstallId = publicPost(
            server.boundPort(), "/api/apps/install/fetch", "{}");
        assert(missingInstallId.find(" 400 ") != std::string::npos);
        assert(Json::parse(bodyOf(missingInstallId))["error"].string() ==
               "an app id is required");
        const std::string crossOriginInstall = publicPost(
            server.boundPort(), "/api/apps/install/fetch",
            "{\"id\":\"org.example.youtube\"}", "http://attacker.example");
        assert(crossOriginInstall.find(" 403 ") != std::string::npos);
        const std::string wrongHostInstall = publicPost(
            server.boundPort(), "/api/apps/install/fetch",
            "{\"id\":\"org.example.youtube\"}", "", "unlisted.example");
        assert(wrongHostInstall.find(" 403 ") != std::string::npos);
        const std::string wrongTypeInstall = publicPost(
            server.boundPort(), "/api/apps/install/fetch",
            "{\"id\":\"org.example.youtube\"}", "", "localhost", "text/plain");
        assert(wrongTypeInstall.find(" 415 ") != std::string::npos);
        assert(reloadServer.installStarts() == 0);

        const std::string fetchInstall = publicPost(
            server.boundPort(), "/api/apps/install/fetch",
            "{\"id\":\"org.example.youtube\",\"auto\":true}");
        assert(fetchInstall.find(" 200 ") != std::string::npos);
        Json installResponse = Json::parse(bodyOf(fetchInstall));
        assert(installResponse["ok"].boolean());
        assert(installResponse["install"]["jobId"].string() == "fixture-job");
        assert(reloadServer.installStarts() == 1);
        const Json installPayload = Json::parse(reloadServer.installBody());
        assert(installPayload["source"].string() == "catalog");
        assert(installPayload["pkg"]["id"].string() == "org.example.youtube");
        assert(installPayload["auto"].boolean());
        assert(installPayload["nativeValidated"].boolean());
        assert(fetches == 1);

        const std::string concurrentInstallUrl = publicPost(
            server.boundPort(), "/api/apps/install/url",
            "{\"url\":\"https://packages.example/app.ipk\"}");
        assert(concurrentInstallUrl.find(" 409 ") != std::string::npos);
        assert(reloadServer.installStarts() == 1 &&
               "an active catalog install must block a second start");
        const std::string clearDownloadProgressSnapshot = post(
            server.boundPort(), "/_internal/apps/install-job", "{\"action\":\"clear\"}",
            "catalog-route-secret-0123456789abcdef");
        assert(clearDownloadProgressSnapshot.find(" 200 ") != std::string::npos);

        const std::string invalidInstallUrl = publicPost(
            server.boundPort(), "/api/apps/install/url",
            "{\"url\":\"ftp://packages.example/app.ipk\"}");
        assert(invalidInstallUrl.find(" 400 ") != std::string::npos);
        assert(Json::parse(bodyOf(invalidInstallUrl))["error"].string() ==
               "the address must start with http:// or https://");
        const std::string invalidInstallHash = publicPost(
            server.boundPort(), "/api/apps/install/url",
            "{\"url\":\"https://packages.example/app.ipk\",\"sha256\":\"bad\"}");
        assert(invalidInstallHash.find(" 400 ") != std::string::npos);
        assert(Json::parse(bodyOf(invalidInstallHash))["error"].string() ==
               "the sha256 hash must be 64 hexadecimal digits");
        const std::string blockedInstallUrlHost = publicPost(
            server.boundPort(), "/api/apps/install/url",
            "{\"url\":\"https://packages.example/app.ipk\"}", "",
            "unlisted.example");
        assert(blockedInstallUrlHost.find(" 403 ") != std::string::npos);
        const std::string crossOriginInstallUrl = publicPost(
            server.boundPort(), "/api/apps/install/url",
            "{\"url\":\"https://packages.example/app.ipk\"}",
            "http://attacker.example");
        assert(crossOriginInstallUrl.find(" 403 ") != std::string::npos);
        assert(reloadServer.installStarts() == 1 &&
               "invalid URL requests must not start an install job");

        const std::string validInstallUrl = publicPost(
            server.boundPort(), "/api/apps/install/url",
            "{\"url\":\"https://packages.example/app.ipk\",\"sha256\":\"" +
                std::string(64, 'a') + "\"}");
        assert(validInstallUrl.find(" 200 ") != std::string::npos);
        assert(reloadServer.installStarts() == 2);
        const Json urlPayload = Json::parse(reloadServer.installBody());
        assert(urlPayload["source"].string() == "url");
        assert(urlPayload["url"].string() == "https://packages.example/app.ipk");
        assert(urlPayload["sha256"].string() == std::string(64, 'a'));
        assert(urlPayload["nativeValidated"].boolean());

        const std::string confirmRequest =
            "{\"jobId\":\"fixture-job\",\"elevate\":false,"
            "\"confirmRoot\":[],\"replaceStore\":true}";
        const std::string confirmResponse = publicPost(
            server.boundPort(), "/api/apps/install/confirm", confirmRequest);
        assert(confirmResponse.find(" 200 ") != std::string::npos);
        assert(Json::parse(bodyOf(confirmResponse))["install"]["state"].string() ==
               "installing");
        assert(reloadServer.installActions() == 1);
        const Json confirmPayload = Json::parse(reloadServer.installActionBody());
        assert(confirmPayload["action"].string() == "confirm");
        assert(confirmPayload["nativeValidated"].boolean());
        assert(confirmPayload["body"]["jobId"].string() == "fixture-job");
        assert(confirmPayload["body"]["replaceStore"].boolean());

        const std::string crossOriginConfirm = publicPost(
            server.boundPort(), "/api/apps/install/confirm", confirmRequest,
            "http://attacker.example");
        assert(crossOriginConfirm.find(" 403 ") != std::string::npos);
        const std::string blockedCancelHost = publicPost(
            server.boundPort(), "/api/apps/install/cancel",
            "{\"jobId\":\"fixture-job\"}", "", "unlisted.example");
        assert(blockedCancelHost.find(" 403 ") != std::string::npos);
        assert(reloadServer.installActions() == 1);

        const std::string cancelResponse = publicPost(
            server.boundPort(), "/api/apps/install/cancel",
            "{\"jobId\":\"fixture-job\"}");
        assert(cancelResponse.find(" 200 ") != std::string::npos);
        assert(Json::parse(bodyOf(cancelResponse))["ok"].boolean());
        assert(reloadServer.installActions() == 2);
        const Json cancelPayload = Json::parse(reloadServer.installActionBody());
        assert(cancelPayload["action"].string() == "cancel");
        assert(!cancelPayload["nativeValidated"].boolean());
        assert(cancelPayload["body"]["jobId"].string() == "fixture-job");

        const std::string checked = post(server.boundPort(),
                                         "/_internal/apps/catalog/check",
                                         "{\"url\":\"" + customRepo + "\"}",
                                         "catalog-route-secret-0123456789abcdef");
        assert(checked.find(" 200 ") != std::string::npos);
        assert(Json::parse(bodyOf(checked))["ok"].boolean());
        assert(fetches == 2);

        const std::string rejectedCatalog = post(server.boundPort(),
                                                 "/_internal/apps/catalog/check",
                                                 "{\"url\":\"" + invalidRepo + "\"}",
                                                 "catalog-route-secret-0123456789abcdef");
        assert(rejectedCatalog.find(" 200 ") != std::string::npos);
        Json rejected = Json::parse(bodyOf(rejectedCatalog));
        assert(!rejected["ok"].boolean());
        assert(rejected["error"].string() == "not a package catalog: " + invalidRepo);
        assert(fetches == 3);

        const std::string addRepoBody =
            "{\"action\":\"add\",\"url\":\"  https://github.com/Owner/Project.git/?source=dashboard  \"}";
        const std::string crossOriginRepo = publicPost(
            server.boundPort(), "/api/apps/install/repos", addRepoBody,
            "http://attacker.example");
        assert(crossOriginRepo.find(" 403 ") != std::string::npos);
        const std::string wrongHostRepo = publicPost(
            server.boundPort(), "/api/apps/install/repos", addRepoBody, "",
            "unlisted.example");
        assert(wrongHostRepo.find(" 403 ") != std::string::npos);
        const std::string wrongTypeRepo = publicPost(
            server.boundPort(), "/api/apps/install/repos", addRepoBody, "",
            "localhost", "text/plain");
        assert(wrongTypeRepo.find(" 415 ") != std::string::npos);
        const std::string insecureRepo = publicPost(
            server.boundPort(), "/api/apps/install/repos",
            "{\"action\":\"add\",\"url\":\"http://example.org/apps.json\"}");
        assert(insecureRepo.find(" 400 ") != std::string::npos);
        assert(Json::parse(bodyOf(insecureRepo))["error"].string() ==
               "the address must start with https://");
        const std::string malformedAddress = publicPost(
            server.boundPort(), "/api/apps/install/repos",
            "{\"action\":\"add\",\"url\":\"https://example.org/bad path.json\"}");
        assert(malformedAddress.find(" 400 ") != std::string::npos);
        assert(Json::parse(bodyOf(malformedAddress))["error"].string() ==
               "the address contains spaces or control characters");
        assert(fetches == 3);

        const std::string addedRepoResponse = publicPost(
            server.boundPort(), "/api/apps/install/repos", addRepoBody);
        assert(addedRepoResponse.find(" 200 ") != std::string::npos);
        Json addedRepo = Json::parse(bodyOf(addedRepoResponse));
        assert(addedRepo["ok"].boolean());
        assert(addedRepo["repos"].size() == 1);
        assert(addedRepo["repos"][0].string() == githubRepo);
        assert(fetches == 4);
        assert(fetchedUrls.back() == githubRepo &&
               "GitHub links must be normalized to the raw repo.json URL");
        assert(reloadServer.requests() == 1);

        std::ifstream savedConfigInput(configPath.c_str());
        const std::string savedConfigText((std::istreambuf_iterator<char>(savedConfigInput)),
                                          std::istreambuf_iterator<char>());
        Json savedConfig = Json::parse(savedConfigText);
        assert(savedConfig["apps"]["sideload"].boolean());
        assert(savedConfig["apps"]["repos"].size() == 1);
        assert(savedConfig["apps"]["repos"][0].string() == githubRepo);
        struct stat configInfo = {};
        assert(stat(configPath.c_str(), &configInfo) == 0);
        assert((configInfo.st_mode & 0777) == 0600);

        const std::string afterAdd = get(server.boundPort(), "/api/apps/catalog");
        assert(afterAdd.find(" 200 ") != std::string::npos);
        Json addedCatalog = Json::parse(bodyOf(afterAdd));
        bool hasCustomApp = false;
        for (const Json &app : addedCatalog["others"].items())
            if (app["id"].string() == "org.example.custom") hasCustomApp = true;
        assert(hasCustomApp &&
               "adding a repository must invalidate the native catalog cache");
        assert(fetches == 6);

        const std::string duplicateRepoResponse = publicPost(
            server.boundPort(), "/api/apps/install/repos", addRepoBody);
        assert(duplicateRepoResponse.find(" 400 ") != std::string::npos);
        assert(Json::parse(bodyOf(duplicateRepoResponse))["error"].string() ==
               "that catalog is already listed");
        assert(fetches == 7);

        const std::string removeRepoResponse = publicPost(
            server.boundPort(), "/api/apps/install/repos",
            "{\"action\":\"remove\",\"url\":\"" + githubRepo + "\"}");
        assert(removeRepoResponse.find(" 200 ") != std::string::npos);
        Json removedRepo = Json::parse(bodyOf(removeRepoResponse));
        assert(removedRepo["ok"].boolean());
        assert(removedRepo["repos"].size() == 0);
        assert(reloadServer.requests() == 2);
        const std::string afterRemove = get(server.boundPort(), "/api/apps/catalog");
        assert(afterRemove.find(" 200 ") != std::string::npos);
        Json removedCatalog = Json::parse(bodyOf(afterRemove));
        assert(removedCatalog["apps"].size() == 1);
        assert(removedCatalog["others"].size() == 0);
        assert(removedCatalog["apps"][0]["id"].string() == "org.example.youtube");
        assert(fetches == 8);

        const std::string initialInstallSnapshot = post(
            server.boundPort(), "/_internal/apps/install-job",
            "{\"action\":\"write\",\"job\":{\"jobId\":\"0123456789abcdef\",\"state\":\"downloading\",\"package\":\"org.example.youtube\",\"version\":\"3.0\",\"updated\":1},\"snapshot\":{\"state\":\"downloading\",\"jobId\":\"0123456789abcdef\",\"source\":\"catalog\",\"appId\":\"org.example.youtube\",\"progress\":{\"bytes\":0,\"total\":4096},\"preview\":null,\"error\":null,\"result\":null}}",
            "catalog-route-secret-0123456789abcdef");
        assert(initialInstallSnapshot.find(" 200 ") != std::string::npos);
        const std::string downloadResponse = post(
            server.boundPort(), "/_internal/apps/download",
            "{\"url\":\"https://cdn.example/package.ipk\",\"maxBytes\":4096,\"file\":\"0123456789abcdef.ipk\"}",
            "catalog-route-secret-0123456789abcdef");
        assert(downloadResponse.find(" 200 ") != std::string::npos);
        assert(downloadResponse.find("Transfer-Encoding: chunked") != std::string::npos);
        assert(downloadResponse.find("Content-Type: application/x-ndjson; charset=utf-8") !=
               std::string::npos);
        const std::string downloadRecords = decodeChunked(bodyOf(downloadResponse));
        assert(downloadRecords.find("\"type\":\"progress\"") != std::string::npos);
        assert(downloadRecords.find("\"bytes\":11") != std::string::npos);
        assert(downloadRecords.find("\"type\":\"done\"") != std::string::npos);
        std::ifstream stagedPackage((stagingDirectory + "/0123456789abcdef.ipk").c_str(),
                                    std::ios::in | std::ios::binary);
        const std::string stagedBytes((std::istreambuf_iterator<char>(stagedPackage)),
                                      std::istreambuf_iterator<char>());
        assert(stagedBytes == "ipk-payload");
        struct stat stagedStatus = {};
        assert(stat((stagingDirectory + "/0123456789abcdef.ipk").c_str(),
                    &stagedStatus) == 0);
        assert((stagedStatus.st_mode & 0777) == 0600);
        assert(unlink((stagingDirectory + "/0123456789abcdef.ipk").c_str()) == 0);
        const std::string installStatus = get(server.boundPort(),
                                              "/api/apps/install/status");
        assert(installStatus.find(" 200 ") != std::string::npos);
        const Json cachedInstallStatus = Json::parse(bodyOf(installStatus));
        assert(cachedInstallStatus["progress"]["bytes"].number() == 11);
        assert(cachedInstallStatus["progress"]["total"].number() == 4096);
        const std::string clearInstallSnapshot = post(
            server.boundPort(), "/_internal/apps/install-job", "{\"action\":\"clear\"}",
            "catalog-route-secret-0123456789abcdef");
        assert(clearInstallSnapshot.find(" 200 ") != std::string::npos);
        assert(downloads == 1);

        const std::string cancellableSnapshot = post(
            server.boundPort(), "/_internal/apps/install-job",
            "{\"action\":\"write\",\"job\":{\"jobId\":\"4444444444444444\",\"state\":\"downloading\",\"package\":\"org.example.youtube\",\"version\":\"3.0\",\"updated\":1},\"snapshot\":{\"state\":\"downloading\",\"jobId\":\"4444444444444444\",\"source\":\"url\",\"appId\":\"org.example.youtube\",\"progress\":{\"bytes\":0,\"total\":4096},\"preview\":null,\"error\":null,\"result\":null}}",
            "catalog-route-secret-0123456789abcdef");
        assert(cancellableSnapshot.find(" 200 ") != std::string::npos);
        std::string cancelledDownloadResponse;
        std::thread cancellableDownload([&]() {
            cancelledDownloadResponse = post(
                server.boundPort(), "/_internal/apps/download",
                "{\"url\":\"https://cdn.example/cancellable.ipk\",\"maxBytes\":4096,\"file\":\"4444444444444444.ipk\"}",
                "catalog-route-secret-0123456789abcdef");
        });
        while (!cancellationStarted.load())
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        const std::string managedDownloadCancelResponse = publicPost(
            server.boundPort(), "/api/apps/install/cancel",
            "{\"jobId\":\"4444444444444444\"}");
        assert(managedDownloadCancelResponse.find(" 200 ") != std::string::npos);
        const Json cancelAction = Json::parse(reloadServer.installActionBody());
        assert(cancelAction["action"].string() == "cancel");
        assert(cancelAction["body"]["nativeDownloadCancelled"].boolean());
        cancellableDownload.join();
        assert(cancelledDownloadResponse.find(" 502 ") != std::string::npos);
        assert(cancellationObserved.load());
        assert(access((stagingDirectory + "/4444444444444444.ipk").c_str(), F_OK) != 0);
        const std::string clearCancellableSnapshot = post(
            server.boundPort(), "/_internal/apps/install-job", "{\"action\":\"clear\"}",
            "catalog-route-secret-0123456789abcdef");
        assert(clearCancellableSnapshot.find(" 200 ") != std::string::npos);

        const std::string downloadFailure = post(
            server.boundPort(), "/_internal/apps/download",
            "{\"url\":\"https://cdn.example/missing.ipk\",\"maxBytes\":4096,\"file\":\"1111111111111111.ipk\"}",
            "catalog-route-secret-0123456789abcdef");
        assert(downloadFailure.find(" 502 ") != std::string::npos);
        assert(downloadFailure.find("X-NativeTv-Remote-Status: 404") != std::string::npos);
        assert(!Json::parse(bodyOf(downloadFailure))["ok"].boolean());
        assert(access((stagingDirectory + "/1111111111111111.ipk").c_str(), F_OK) != 0);
        assert(downloads == 3);

        const std::string offlineDownload = post(
            server.boundPort(), "/_internal/apps/download",
            "{\"url\":\"https://cdn.example/offline.ipk\",\"maxBytes\":4096,\"file\":\"2222222222222222.ipk\"}",
            "catalog-route-secret-0123456789abcdef");
        assert(offlineDownload.find(" 502 ") != std::string::npos);
        assert(Json::parse(bodyOf(offlineDownload))["offline"].boolean());
        assert(access((stagingDirectory + "/2222222222222222.ipk").c_str(), F_OK) != 0);
        assert(downloads == 4);

        const std::string sizeLimitedDownload = post(
            server.boundPort(), "/_internal/apps/download",
            "{\"url\":\"https://cdn.example/oversize.ipk\",\"maxBytes\":131072,\"file\":\"3333333333333333.ipk\"}",
            "catalog-route-secret-0123456789abcdef");
        assert(sizeLimitedDownload.find(" 200 ") != std::string::npos);
        const std::string failureRecords = decodeChunked(bodyOf(sizeLimitedDownload));
        assert(failureRecords.find("\"bytes\":65536") != std::string::npos);
        assert(failureRecords.find("\"type\":\"error\"") != std::string::npos);
        assert(failureRecords.find("\"maxBytesExceeded\":true") != std::string::npos);
        assert(access((stagingDirectory + "/3333333333333333.ipk").c_str(), F_OK) != 0);
        assert(downloads == 5);

        const std::string invalidDownload = post(
            server.boundPort(), "/_internal/apps/download",
            "{\"url\":\"https://cdn.example/package.ipk\",\"maxBytes\":0,\"file\":\"4444444444444444.ipk\"}",
            "catalog-route-secret-0123456789abcdef");
        assert(invalidDownload.find(" 400 ") != std::string::npos);
        assert(downloads == 5);

        const std::string invalidFileDownload = post(
            server.boundPort(), "/_internal/apps/download",
            "{\"url\":\"https://cdn.example/package.ipk\",\"maxBytes\":4096,\"file\":\"../escape.ipk\"}",
            "catalog-route-secret-0123456789abcdef");
        assert(invalidFileDownload.find(" 400 ") != std::string::npos);
        assert(downloads == 5);

        const std::string refused = post(server.boundPort(),
                                         "/_internal/apps/catalog/find",
                                         "{\"id\":\"org.example.youtube\"}",
                                         "wrong-secret");
        assert(refused.find(" 403 ") != std::string::npos);
        assert(fetches == 8);
        const std::string downloadRefused = post(
            server.boundPort(), "/_internal/apps/download",
            "{\"url\":\"https://cdn.example/package.ipk\",\"maxBytes\":4096,\"file\":\"5555555555555555.ipk\"}",
            "wrong-secret");
        assert(downloadRefused.find(" 403 ") != std::string::npos);
        assert(downloads == 5);

        const std::string missingInspection = post(
            server.boundPort(), "/_internal/apps/inspect",
            "{\"file\":\"missing.ipk\"}",
            "catalog-route-secret-0123456789abcdef");
        assert(missingInspection.find(" 200 ") != std::string::npos);
        assert(!Json::parse(bodyOf(missingInspection))["ok"].boolean());
        assert(Json::parse(bodyOf(missingInspection))["error"].string().find(
                   "could not be read") != std::string::npos);
        const std::string traversalInspection = post(
            server.boundPort(), "/_internal/apps/inspect",
            "{\"file\":\"../missing.ipk\"}",
            "catalog-route-secret-0123456789abcdef");
        assert(traversalInspection.find(" 200 ") != std::string::npos);
        assert(!Json::parse(bodyOf(traversalInspection))["ok"].boolean());
        assert(Json::parse(bodyOf(traversalInspection))["error"].string() ==
               "the file is not in the staging directory");
        const std::string refusedInspection = post(
            server.boundPort(), "/_internal/apps/inspect",
            "{\"file\":\"missing.ipk\"}", "wrong-secret");
        assert(refusedInspection.find(" 403 ") != std::string::npos);
    } catch (...) {
        server.stop();
        loop.stop();
        eventThread.join();
        throw;
    }
    server.stop();
    loop.stop();
    eventThread.join();
    unsetenv("TVWEB_INSTALL_STAGING");
    rmdir(stagingDirectory.c_str());
    std::cout << "native app catalog front-door assertions passed\n";
    return 0;
}
