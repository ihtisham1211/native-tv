#include "daemon/http_server.hpp"
#include "service/app_package_fetcher.hpp"
#include <arpa/inet.h>
#include <atomic>
#include <cassert>
#include <chrono>
#include <iostream>
#include <netinet/in.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

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
} // namespace

int main() {
    const std::string package("ipk\0binary\xffpayload", 18);
    const std::string large(128, 'x');
    const std::string prefix = "https://example.org/";
    const std::string emoji = "\xf0\x9f\x98\x80";
    std::string validationError;
    const std::string exactUrlLimit = prefix +
        std::string(2048 - prefix.size() - 2, 'x') + emoji;
    assert(exactUrlLimit.size() > 2048);
    assert(service::AppPackageFetcher::validUrl(exactUrlLimit, validationError));
    const std::string overUrlLimit = exactUrlLimit + "x";
    assert(!service::AppPackageFetcher::validUrl(overUrlLimit, validationError));
    assert(validationError == "the address is longer than 2048 characters");
    const std::string c1Control = prefix + "\xc2\x85";
    assert(!service::AppPackageFetcher::validUrl(c1Control, validationError));
    assert(validationError == "the address contains spaces or control characters");

    std::string receivedUserAgent;
    std::atomic<bool> releaseStall(false);
    HttpServer server;
    HttpServerOptions options;
    options.host = "127.0.0.1";
    options.port = unusedPort();
    options.workerCount = 2;
    std::string startError;
    assert(server.start(options, [&package, &large, &receivedUserAgent,
                                  &releaseStall](HttpExchange &exchange) {
        const HttpRequest &request = exchange.request();
        receivedUserAgent = request.header("user-agent");
        if (request.uri == "/redirect") {
            (void)exchange.respond(302, {{"Location", "/package"}}, "");
        } else if (request.uri == "/package") {
            (void)exchange.respond(200, {{"Content-Type", "application/octet-stream"}}, package);
        } else if (request.uri == "/large") {
            (void)exchange.respond(200, {{"Content-Type", "application/octet-stream"}}, large);
        } else if (request.uri == "/stall") {
            if (exchange.beginResponse(200, {{"Content-Type", "application/octet-stream"}}, -1)) {
                while (!releaseStall.load())
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                (void)exchange.writeBody("late", 4);
                (void)exchange.finishResponse();
            }
        } else {
            (void)exchange.respond(404, {{"Content-Type", "text/plain"}}, "missing");
        }
    }, startError));

    const auto url = [&server](const std::string &path) {
        return "http://127.0.0.1:" + std::to_string(server.boundPort()) + path;
    };
    try {
        std::string bytes;
        long status = 0;
        service::AppPackageDownloadFailure failure;
        const bool downloaded = service::AppPackageFetcher::download(
            url("/redirect"), "tvweb/4.2.0", "", 1024,
            [&bytes](const char *data, size_t size) {
                bytes.append(data, size);
                return true;
            }, status, failure);
        assert(downloaded);
        assert(status == 200);
        assert(bytes == package);
        assert(receivedUserAgent == "tvweb/4.2.0");

        bytes.clear();
        const std::string localhostUrl = "http://localhost:" +
                                         std::to_string(server.boundPort()) + "/package";
        const bool familyRetry = service::AppPackageFetcher::download(
            localhostUrl, "tvweb/4.2.0", "6", 1024,
            [&bytes](const char *data, size_t size) {
                bytes.append(data, size);
                return true;
            }, status, failure);
        assert(familyRetry);
        assert(bytes == package);

        bytes.clear();
        const bool missing = service::AppPackageFetcher::download(
            url("/missing"), "tvweb/4.2.0", "", 1024,
            [&bytes](const char *data, size_t size) {
                bytes.append(data, size);
                return true;
            }, status, failure);
        assert(!missing);
        assert(status == 404);
        assert(bytes.empty());

        bytes.clear();
        const bool tooLarge = service::AppPackageFetcher::download(
            url("/large"), "tvweb/4.2.0", "", 16,
            [&bytes](const char *data, size_t size) {
                bytes.append(data, size);
                return true;
            }, status, failure);
        assert(!tooLarge);
        assert(status == 200);
        assert(failure.message == "download exceeds the allowed size");
        assert(bytes.empty());

        const bool cancelled = service::AppPackageFetcher::download(
            url("/package"), "tvweb/4.2.0", "", 1024,
            [](const char *, size_t) { return false; }, status, failure);
        assert(!cancelled);
        assert(failure.message == "package download stream was interrupted");

        std::atomic<bool> cancelRequested(false);
        bool transferCompleted = true;
        service::AppPackageDownloadFailure nativeCancelFailure;
        const auto cancelStarted = std::chrono::steady_clock::now();
        std::thread stalledDownload([&]() {
            transferCompleted = service::AppPackageFetcher::download(
                url("/stall"), "tvweb/4.2.0", "", 1024,
                [](const char *, size_t) { return true; }, status,
                nativeCancelFailure, [&cancelRequested]() {
                    return cancelRequested.load();
                });
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        cancelRequested.store(true);
        stalledDownload.join();
        assert(!transferCompleted);
        assert(nativeCancelFailure.message == "cancelled");
        assert(std::chrono::steady_clock::now() - cancelStarted <
               std::chrono::seconds(2));
        releaseStall.store(true);

        const bool invalid = service::AppPackageFetcher::download(
            "file:///tmp/app.ipk", "tvweb/4.2.0", "", 1024,
            [](const char *, size_t) { return true; }, status, failure);
        assert(!invalid);
        assert(failure.message.find("http:// or https://") != std::string::npos);

        const std::string closedUrl = "http://127.0.0.1:" +
                                      std::to_string(unusedPort()) + "/unreachable";
        const bool offline = service::AppPackageFetcher::download(
            closedUrl, "tvweb/4.2.0", "", 1024,
            [](const char *, size_t) { return true; }, status, failure);
        assert(!offline);
        assert(failure.offline);
    } catch (...) {
        server.stop();
        throw;
    }
    server.stop();
    std::cout << "native app package fetcher assertions passed\n";
    return 0;
}
