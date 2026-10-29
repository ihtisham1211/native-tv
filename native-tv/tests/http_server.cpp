#include "daemon/http_server.hpp"
#include "daemon/child_process.hpp"
#include "daemon/http_auth.hpp"
#include "daemon/http_caps.hpp"
#include "daemon/http_proxy.hpp"
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <netinet/in.h>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace nativetv::daemon;
using nativetv::Json;

namespace {
void check(bool condition, const std::string &message) {
    if (!condition)
        throw std::runtime_error(message);
}

int connectTo(unsigned short port) {
    const int socketFd = socket(AF_INET, SOCK_STREAM, 0);
    if (socketFd < 0)
        throw std::runtime_error("could not create a test socket");
    timeval timeout;
    timeout.tv_sec = 3;
    timeout.tv_usec = 0;
    (void)setsockopt(socketFd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    sockaddr_in address;
    std::memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(socketFd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) {
        close(socketFd);
        throw std::runtime_error("could not connect to the embedded HTTP server");
    }
    return socketFd;
}

int connectToV6(unsigned short port) {
    const int socketFd = socket(AF_INET6, SOCK_STREAM, 0);
    if (socketFd < 0)
        throw std::runtime_error("could not create an IPv6 test socket");
    timeval timeout;
    timeout.tv_sec = 3;
    timeout.tv_usec = 0;
    (void)setsockopt(socketFd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    sockaddr_in6 address;
    std::memset(&address, 0, sizeof(address));
    address.sin6_family = AF_INET6;
    address.sin6_port = htons(port);
    address.sin6_addr = in6addr_loopback;
    if (connect(socketFd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) {
        close(socketFd);
        throw std::runtime_error("could not connect to the embedded HTTP server over IPv6");
    }
    return socketFd;
}

unsigned long long maximumRssBytes() {
    rusage usage;
    if (getrusage(RUSAGE_SELF, &usage) != 0)
        throw std::runtime_error("could not read process peak RSS");
#if defined(__APPLE__)
    return static_cast<unsigned long long>(usage.ru_maxrss);
#else
    return static_cast<unsigned long long>(usage.ru_maxrss) * 1024;
#endif
}

bool closesWithin(int socketFd, int timeoutMs) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    char bytes[1024];
    while (std::chrono::steady_clock::now() < deadline) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   deadline - std::chrono::steady_clock::now())
                                   .count();
        pollfd descriptor;
        descriptor.fd = socketFd;
        descriptor.events = POLLIN | POLLHUP | POLLERR;
        descriptor.revents = 0;
        const int ready = poll(&descriptor, 1, static_cast<int>(remaining));
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready <= 0)
            return false;
        const ssize_t count = recv(socketFd, bytes, sizeof(bytes), 0);
        if (count == 0)
            return true;
        if (count < 0 && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)
            return true;
    }
    return false;
}

unsigned short unusedPort() {
    const int socketFd = socket(AF_INET, SOCK_STREAM, 0);
    if (socketFd < 0)
        throw std::runtime_error("could not allocate a test port");
    sockaddr_in address;
    std::memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(socketFd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) {
        close(socketFd);
        throw std::runtime_error("could not reserve a test port");
    }
    socklen_t length = sizeof(address);
    if (getsockname(socketFd, reinterpret_cast<sockaddr *>(&address), &length) != 0) {
        close(socketFd);
        throw std::runtime_error("could not read the reserved test port");
    }
    const unsigned short port = ntohs(address.sin_port);
    close(socketFd);
    return port;
}

int reserveIpv6OnlyPort(unsigned short port) {
    const int socketFd = socket(AF_INET6, SOCK_STREAM, 0);
    if (socketFd < 0)
        return -1;
    const int enabled = 1;
    if (setsockopt(socketFd, IPPROTO_IPV6, IPV6_V6ONLY, &enabled, sizeof(enabled)) != 0) {
        close(socketFd);
        return -1;
    }
    sockaddr_in6 address;
    std::memset(&address, 0, sizeof(address));
    address.sin6_family = AF_INET6;
    address.sin6_port = htons(port);
    address.sin6_addr = in6addr_any;
    if (bind(socketFd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) {
        close(socketFd);
        return -1;
    }
    return socketFd;
}

void sendAll(int socketFd, const std::string &request) {
    size_t offset = 0;
    while (offset < request.size()) {
        const ssize_t count = send(socketFd, request.data() + offset, request.size() - offset, 0);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            throw std::runtime_error("could not write the test request");
        offset += static_cast<size_t>(count);
    }
}

std::string receive(int socketFd, size_t minimumBytes) {
    std::string result;
    char bytes[4096];
    while (result.size() < minimumBytes) {
        const ssize_t count = recv(socketFd, bytes, sizeof(bytes), 0);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            throw std::runtime_error("HTTP response ended early");
        result.append(bytes, static_cast<size_t>(count));
    }
    return result;
}

size_t contentLength(const std::string &headers) {
    const std::string key = "Content-Length: ";
    const size_t start = headers.find(key);
    if (start == std::string::npos)
        return 0;
    return static_cast<size_t>(std::strtoul(headers.c_str() + start + key.size(), nullptr, 10));
}

std::string readResponse(int socketFd) {
    std::string response;
    const size_t headerEndSize = 4;
    for (;;) {
        const size_t end = response.find("\r\n\r\n");
        if (end != std::string::npos) {
            const bool chunked =
                response.substr(0, end).find("Transfer-Encoding: chunked") != std::string::npos;
            if (chunked && response.find("0\r\n\r\n", end + headerEndSize) != std::string::npos)
                return response;
            const size_t expected =
                end + headerEndSize + contentLength(response.substr(0, end + 2));
            if (chunked) {
                response += receive(socketFd, 1);
                continue;
            }
            if (response.size() >= expected)
                return response;
        }
        response += receive(socketFd, 1);
    }
}

std::string readHeadersOnly(int socketFd) {
    std::string response;
    while (response.find("\r\n\r\n") == std::string::npos)
        response += receive(socketFd, 1);
    return response;
}

std::string get(unsigned short port, const std::string &path, bool keepAlive = false) {
    const int socketFd = connectTo(port);
    try {
        sendAll(socketFd, "GET " + path + " HTTP/1.1\r\nHost: localhost\r\nConnection: " +
                              (keepAlive ? "keep-alive" : "close") + "\r\n\r\n");
        std::string response = readResponse(socketFd);
        close(socketFd);
        return response;
    } catch (...) {
        close(socketFd);
        throw;
    }
}

std::string request(unsigned short port, const std::string &wireRequest) {
    const int socketFd = connectTo(port);
    try {
        sendAll(socketFd, wireRequest);
        std::string response = readResponse(socketFd);
        close(socketFd);
        return response;
    } catch (...) {
        close(socketFd);
        throw;
    }
}

void testRequestsAndPersistentConnections() {
    HttpServer server;
    HttpServerOptions options;
    options.host = "::";
    options.port = unusedPort();
    options.workerCount = 32;
    options.keepAliveTimeoutMs = 3000;
    options.requestTimeoutMs = 1000;
    std::string error;
    const bool started = server.start(
        options,
        [](HttpExchange &exchange) {
            const HttpRequest &request = exchange.request();
            if (request.uri == "/echo" && request.method == "POST") {
                std::string body;
                char buffer[256];
                while (body.size() < static_cast<size_t>(request.contentLength)) {
                    const int count = exchange.readBody(buffer, sizeof(buffer));
                    if (count <= 0)
                        break;
                    body.append(buffer, static_cast<size_t>(count));
                }
                (void)exchange.respond(200, {{"Content-Type", "text/plain"}}, body);
            } else if (request.uri == "/limited" && request.method == "POST") {
                bool accepted = exchange.setBodyLimit(5);
                char body[2];
                while (accepted) {
                    const int count = exchange.readBody(body, sizeof(body));
                    if (count == 0)
                        break;
                    if (count < 0) {
                        accepted = false;
                        break;
                    }
                }
                if (!accepted || exchange.bodyLimitExceeded()) {
                    (void)exchange.respond(
                        413, {{"Content-Type", "text/plain"}, {"Connection", "close"}},
                        "too large");
                } else {
                    (void)exchange.respond(200, {{"Content-Type", "text/plain"}}, "accepted");
                }
            } else if (request.uri == "/stream") {
                if (exchange.beginResponse(200, {{"Content-Type", "text/plain"}}, -1)) {
                    (void)exchange.writeBody("A", 1);
                    (void)exchange.writeBody("BC", 2);
                    (void)exchange.finishResponse();
                }
            } else {
                (void)exchange.respond(request.uri == "/health" ? 200 : 404,
                                       {{"Content-Type", "text/plain"}},
                                       request.uri == "/health" ? "ok" : "not found");
            }
        },
        error);
    check(started, "HTTP server failed to start: " + error);
    check(server.boundPort() == options.port, "server should report the bound listener port");

    ChildRunnerOptions launchOptions;
    launchOptions.startupGap = std::chrono::milliseconds(0);
    launchOptions.regularGap = std::chrono::milliseconds(0);
    launchOptions.startupWindow = std::chrono::milliseconds(0);
    ChildProcessRunner childRunner(launchOptions);
    ChildOptions child;
    child.arguments = {"/bin/echo", "runner-survived"};
    const ChildResult childResult = childRunner.run(child);
    check(childResult.succeeded() && childResult.standardOutput == "runner-survived\n",
          "CivetWeb startup must preserve SIGCHLD so the child runner can reap children");

    std::string response = get(server.boundPort(), "/health");
    check(response.find("HTTP/1.1 200 OK") == 0 && response.find("\r\n\r\nok") != std::string::npos,
          "health response did not contain the expected status and body");
    response = get(server.boundPort(), "/a-private-file");
    check(
        response.find("HTTP/1.1 404 Not Found") == 0 &&
            response.find("not found") != std::string::npos,
        "unmapped paths should stay inside the request handler, not fall through to file serving");
    const int ipv6 = connectToV6(server.boundPort());
    sendAll(ipv6, "GET /health HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
    response = readResponse(ipv6);
    close(ipv6);
    check(response.find("HTTP/1.1 200 OK") == 0, "dual-stack listener did not answer on IPv6");
    response = get(server.boundPort(), "/stream");
    check(response.find("Transfer-Encoding: chunked") != std::string::npos &&
              response.find("1\r\nA\r\n2\r\nBC\r\n0\r\n\r\n") != std::string::npos,
          "streamed response did not use valid HTTP chunks");

    const int post = connectTo(server.boundPort());
    sendAll(post, "POST /echo HTTP/1.1\r\nHost: localhost\r\nContent-Length: 6\r\n"
                  "Connection: close\r\n\r\nhello!");
    response = readResponse(post);
    close(post);
    check(response.find("\r\n\r\nhello!") != std::string::npos,
          "request body reader did not deliver the exact body");

    const std::string multipart = "--boundary\r\nContent-Disposition: form-data; name=\"file\"; "
                                  "filename=\"upload.ipk\"\r\n\r\npkg\r\n--boundary--\r\n";
    const int upload = connectTo(server.boundPort());
    sendAll(upload, "POST /echo HTTP/1.1\r\nHost: localhost\r\n"
                    "Content-Type: multipart/form-data; boundary=boundary\r\nContent-Length: " +
                        std::to_string(multipart.size()) + "\r\nConnection: close\r\n\r\n" +
                        multipart);
    response = readResponse(upload);
    close(upload);
    check(response.find("\r\n\r\n" + multipart) != std::string::npos,
          "multipart request body was not streamed intact");

    const int oversized = connectTo(server.boundPort());
    sendAll(oversized, "POST /limited HTTP/1.1\r\nHost: localhost\r\nContent-Length: 6\r\n"
                       "Connection: close\r\n\r\n123456");
    response = readResponse(oversized);
    close(oversized);
    check(response.find("HTTP/1.1 413") == 0,
          "per-route request body limits should reject an oversized body");

    const int chunkedOversized = connectTo(server.boundPort());
    sendAll(chunkedOversized, "POST /limited HTTP/1.1\r\nHost: localhost\r\n"
                              "Transfer-Encoding: chunked\r\nConnection: close\r\n\r\n"
                              "6\r\n123456\r\n0\r\n\r\n");
    response = readResponse(chunkedOversized);
    close(chunkedOversized);
    check(response.find("HTTP/1.1 413") == 0,
          "route body limit should also reject an oversized chunked body");

    const int slow = connectTo(server.boundPort());
    sendAll(slow, "GET /health HTTP/1.1\r\nHost:");
    response = get(server.boundPort(), "/health");
    check(response.find("HTTP/1.1 200 OK") == 0,
          "one slow header should not prevent healthy requests from being served");
    check(closesWithin(slow, 2500),
          "incomplete request headers should be closed by the configured timeout");
    close(slow);

    std::vector<int> idle;
    const unsigned long long rssBeforeIdle = maximumRssBytes();
    for (int index = 0; index < 20; ++index) {
        const int socketFd = connectTo(server.boundPort());
        idle.push_back(socketFd);
        sendAll(socketFd,
                "GET /health HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\n");
        check(readResponse(socketFd).find("HTTP/1.1 200 OK") == 0,
              "one of twenty idle keep-alive connections failed");
    }
    const int afterIdle = connectTo(server.boundPort());
    sendAll(afterIdle, "GET /health HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
    response = readResponse(afterIdle);
    close(afterIdle);
    check(response.find("HTTP/1.1 200 OK") == 0,
          "twenty idle keep-alive clients starved a new request");
    const unsigned long long rssAfterIdle = maximumRssBytes();
    check(rssAfterIdle <= rssBeforeIdle + 16ULL * 1024 * 1024,
          "twenty idle keep-alive clients added more than 16 MiB of peak RSS");
    for (int socketFd : idle)
        close(socketFd);
    server.stop();
    check(!server.running() && server.boundPort() == 0,
          "stopping the HTTP server should release its listener state");
}

void testOptionBounds() {
    HttpServer server;
    HttpServerOptions options;
    options.workerCount = 0;
    std::string error;
    check(!server.start(
              options, [](HttpExchange &) {}, error),
          "zero HTTP worker count should be rejected");
    check(error.find("between 1 and 64") != std::string::npos,
          "HTTP server worker limit should have a clear error");
}

void testNativeCapabilities() {
    char pattern[] = "/tmp/nativetv-caps-XXXXXX";
    char *created = mkdtemp(pattern);
    check(created != nullptr, "could not create native caps test directory");
    const std::string root(created);
    {
        std::ofstream setup((root + "/.setup-pending").c_str());
        std::ofstream homebrew((root + "/.from-homebrew-channel").c_str());
        check(setup.good() && homebrew.good(), "could not create native caps marker files");
    }

    ServerConfig config;
    config.host = "::";
    config.port = 8123;
    config.token = "native-caps-secret";
    config.allowControl = false;
    config.allowPower = true;
    const Json tv =
        nativeCapabilities(config, "0.118.0", root, "::ffff:127.0.0.1", "192.168.10.22");
    check(tv["ok"].boolean() && !tv["allowControl"].boolean() && tv["allowPower"].boolean() &&
              tv["origin"].string() == "http://192.168.10.22:8123" &&
              tv["version"].string() == "0.118.0" && tv["fromHomebrewChannel"].boolean() &&
              tv["setupNeeded"].boolean() && tv["key"].string() == config.token,
          "native caps must preserve configuration, origin, setup flags and TV token");

    const Json remote = nativeCapabilities(config, "0.118.0", root, "192.0.2.20", "192.168.10.22");
    check(!remote.has("key"), "native caps must never disclose the token to a remote peer");
    (void)std::remove((root + "/.setup-pending").c_str());
    config.document = Json::parse("{\"emulateSetup\":true}");
    const Json emulatedSetup =
        nativeCapabilities(config, "0.118.0", root, "127.0.0.1", "192.168.10.22");
    check(emulatedSetup["setupNeeded"].boolean(),
          "native caps must preserve the configured setup demo flag");
    config.host = "127.0.0.1";
    const Json loopbackOnly =
        nativeCapabilities(config, "0.118.0", root, "127.0.0.1", "192.168.10.22");
    check(loopbackOnly.dump().find("\"origin\":null") != std::string::npos,
          "a loopback-only server must return a null LAN origin");

    (void)std::remove((root + "/.from-homebrew-channel").c_str());
    check(rmdir(root.c_str()) == 0, "could not remove native caps test directory");
}

void testIpv4OnlyAndDualStackFallback() {
    const unsigned short port = unusedPort();
    const int ipv6Only = reserveIpv6OnlyPort(port);
    if (ipv6Only < 0)
        return; // The host does not provide bindable IPv6 sockets.

    std::string error;
    HttpServer ipv4Only;
    HttpServerOptions ipv4Options;
    ipv4Options.host = "0.0.0.0";
    ipv4Options.port = port;
    ipv4Options.workerCount = 2;
    check(ipv4Only.start(
              ipv4Options,
              [](HttpExchange &exchange) {
                  (void)exchange.respond(200, {{"Content-Type", "text/plain"}}, "ipv4");
              },
              error),
          "IPv4-only listener should coexist with an IPv6-only bind: " + error);
    check(!ipv4Only.usingIpv4Fallback(),
          "an explicitly IPv4-only listener must not be reported as a fallback");
    check(get(port, "/health").find("HTTP/1.1 200 OK") == 0,
          "explicit IPv4 listener did not accept an IPv4 request");
    ipv4Only.stop();

    HttpServer fallback;
    HttpServerOptions dualStackOptions;
    dualStackOptions.host = "::";
    dualStackOptions.port = port;
    dualStackOptions.workerCount = 2;
    check(fallback.start(
              dualStackOptions,
              [](HttpExchange &exchange) {
                  (void)exchange.respond(200, {{"Content-Type", "text/plain"}}, "fallback");
              },
              error),
          "dual-stack listener should fall back to IPv4 when its port is occupied on IPv6: " +
              error);
    check(fallback.usingIpv4Fallback(),
          "a failed dual-stack bind should be reported as IPv4 fallback");
    check(get(port, "/health").find("HTTP/1.1 200 OK") == 0,
          "IPv4 fallback listener did not accept an IPv4 request");
    fallback.stop();
    close(ipv6Only);
}

void testNodeProxy() {
    const std::string secret = "native-node-proxy-test-secret-0123456789";
    bool restartCheckSeen = false;
    bool restartRequestSeen = false;
    bool setupContextSeen = false;
    bool nativeStatsSeen = false;
    HttpServer node;
    HttpServerOptions nodeOptions;
    nodeOptions.host = "127.0.0.1";
    nodeOptions.port = unusedPort();
    nodeOptions.workerCount = 4;
    nodeOptions.keepAliveTimeoutMs = 100;
    nodeOptions.requestTimeoutMs = 1000;
    std::string error;
    check(
        node.start(
            nodeOptions,
            [&secret, &restartCheckSeen, &restartRequestSeen,
             &setupContextSeen, &nativeStatsSeen](HttpExchange &exchange) {
                const HttpRequest &incoming = exchange.request();
                std::string body;
                char buffer[1024];
                while (incoming.contentLength < 0 ||
                       body.size() < static_cast<size_t>(incoming.contentLength)) {
                    const int count = exchange.readBody(buffer, sizeof(buffer));
                    if (count <= 0)
                        break;
                    body.append(buffer, static_cast<size_t>(count));
                }
                if (incoming.uri == "/_internal/reload-config") {
                    const bool trusted = incoming.method == "POST" &&
                                         incoming.header("x-nativetv-proxy-secret") == secret &&
                                         incoming.header("x-nativetv-original-remote").empty() &&
                                         incoming.contentLength == 0;
                    (void)exchange.respond(trusted ? 200 : 403,
                                           {{"Content-Type", "application/json"}},
                                           trusted ? "{\"ok\":true}" : "{\"ok\":false}");
                    return;
                }
                if (incoming.uri == "/_internal/setup-context") {
                    const bool trusted = incoming.method == "POST" &&
                                         incoming.header("x-nativetv-proxy-secret") == secret &&
                                         incoming.header("x-nativetv-original-remote").empty() &&
                                         incoming.contentLength == 0 &&
                                         incoming.query == "demo=1";
                    setupContextSeen = trusted;
                    (void)exchange.respond(
                        trusted ? 200 : 403, {{"Content-Type", "application/json"}},
                        trusted ? "{\"homeAssistant\":{\"configured\":false,\"state\":\"disabled\"},\"handoff\":\"http://192.0.2.1:8081/?c=demo\",\"hostname\":null}"
                                : "{\"ok\":false}");
                    return;
                }
                if (incoming.uri == "/_internal/restart") {
                    const bool trusted = incoming.method == "POST" &&
                                         incoming.header("x-nativetv-proxy-secret") == secret &&
                                         incoming.header("x-nativetv-original-remote").empty() &&
                                         incoming.contentLength == 0;
                    if (incoming.query == "check=1")
                        restartCheckSeen = trusted;
                    else if (incoming.query.empty())
                        restartRequestSeen = trusted;
                    (void)exchange.respond(trusted ? 200 : 403,
                                           {{"Content-Type", "application/json"}},
                                           trusted ? "{\"ok\":true}" : "{\"ok\":false}");
                    return;
                }
                if (incoming.uri == "/_internal/native-stats") {
                    const Json payload = Json::parse(body);
                    const bool trusted = incoming.method == "POST" &&
                                         incoming.header("x-nativetv-proxy-secret") == secret &&
                                         incoming.header("x-nativetv-original-remote").empty() &&
                                         incoming.header("content-type") == "application/json" &&
                                         payload.isObject() && payload["system"]["ok"].boolean() &&
                                         payload["withSettings"].boolean();
                    nativeStatsSeen = trusted;
                    (void)exchange.respond(trusted ? 200 : 403,
                                           {{"Content-Type", "application/json"}},
                                           trusted ? "{\"ok\":true,\"withSettings\":true}"
                                                   : "{\"ok\":false}");
                    return;
                }
                if (incoming.uri == "/api/setup") {
                    Json setup = Json::object();
                    setup.set("body", Json::parse(body));
                    setup.set("peer", Json::text(incoming.header(
                                             "x-nativetv-original-remote")));
                    setup.set("secret", Json::text(incoming.header(
                                               "x-nativetv-proxy-secret")));
                    (void)exchange.respond(200, {{"Content-Type", "application/json"}},
                                           setup.dump());
                    return;
                }
                std::vector<HttpHeader> headers;
                headers.push_back({"Content-Type", "application/octet-stream"});
                headers.push_back(
                    {"X-Original-Peer", incoming.header("x-nativetv-original-remote")});
                headers.push_back({"X-Proxy-Secret", incoming.header("x-nativetv-proxy-secret")});
                headers.push_back({"X-Forwarded-Range", incoming.header("range")});
                headers.push_back(
                    {"X-Request-Target",
                     incoming.uri + (incoming.query.empty() ? "" : "?" + incoming.query)});
                if (incoming.uri == "/not-modified") {
                    if (exchange.beginResponse(
                            304, {{"ETag", "\"cached\""}, {"Content-Length", "9"}}, -1))
                        (void)exchange.finishResponse();
                } else if (incoming.method == "HEAD")
                    (void)exchange.respond(200, headers, "head-body");
                else if (incoming.uri == "/partial")
                    (void)exchange.respond(206, headers, "partial");
                else
                    (void)exchange.respond(200, headers, body);
            },
            error),
        "Node proxy fixture failed to start: " + error);

    HttpProxy proxy({"127.0.0.1", node.boundPort(), secret, 3000});
    check(proxy.valid(error), "Node proxy rejected a valid loopback configuration: " + error);
    Json setupContext;
    check(proxy.requestNodeSetupContext(true, setupContext, error) &&
              setupContextSeen &&
              setupContext["homeAssistant"]["state"].string() == "disabled" &&
              setupContext["handoff"].string() == "http://192.0.2.1:8081/?c=demo",
          "native setup read should use the secret-only context bridge: " + error);
    Json nativeSystem = Json::object();
    nativeSystem.set("ok", Json::flag(true));
    std::string nativeStats;
    check(proxy.requestNodeStats(nativeSystem, true, nativeStats, error) &&
              Json::parse(nativeStats)["withSettings"].boolean() && nativeStatsSeen,
          "native stats should send the system snapshot through the secret-only Node bridge: " +
              error);
    check(proxy.requestNodeConfigReload(error),
          "native config reload bridge should call Node directly with only its shared secret: " +
              error);
    int restartStatus = 0;
    std::string restartBody;
    check(proxy.requestNodeRestart(true, restartStatus, restartBody, error) &&
              restartStatus == 200 && Json::parse(restartBody)["ok"].boolean() &&
              restartCheckSeen,
          "native restart preflight should use the secret-only Node bridge: " + error);
    check(proxy.requestNodeRestart(false, restartStatus, restartBody, error) &&
              restartStatus == 200 && Json::parse(restartBody)["ok"].boolean() &&
              restartRequestSeen,
          "native restart request should use the secret-only Node bridge: " + error);
    HttpServer front;
    HttpServerOptions frontOptions;
    frontOptions.host = "::";
    frontOptions.port = unusedPort();
    frontOptions.workerCount = 4;
    frontOptions.keepAliveTimeoutMs = 100;
    frontOptions.requestTimeoutMs = 1000;
    check(front.start(
              frontOptions, [&proxy](HttpExchange &exchange) { (void)proxy.forward(exchange); },
              error),
          "front door fixture failed to start: " + error);

    const std::string upload("multipart\r\n\0binary", 18);
    std::string response =
        request(front.boundPort(),
                "POST /upload?mode=full HTTP/1.1\r\nHost: nativetv.local\r\n"
                "Content-Type: application/octet-stream\r\nRange: bytes=0-4\r\nContent-Length: " +
                    std::to_string(upload.size()) + "\r\nConnection: close\r\n\r\n" + upload);
    const size_t bodyStart = response.find("\r\n\r\n");
    check(bodyStart != std::string::npos && response.substr(bodyStart + 4) == upload,
          "Node proxy should preserve streamed binary request bodies");
    check(response.find("X-Original-Peer: ") != std::string::npos &&
              response.find("X-Proxy-Secret: " + secret) != std::string::npos &&
              response.find("X-Forwarded-Range: bytes=0-4") != std::string::npos &&
              response.find("X-Request-Target: /upload?mode=full") != std::string::npos,
          "Node proxy should forward trusted peer metadata, range and query");

    response = request(front.boundPort(), "POST /chunked HTTP/1.1\r\nHost: nativetv.local\r\n"
                                          "Transfer-Encoding: chunked\r\nConnection: close\r\n\r\n"
                                          "4\r\nbody\r\n0\r\n\r\n");
    check(response.find("\r\n\r\nbody") != std::string::npos,
          "Node proxy should de-frame and stream chunked request bodies");

    response = request(front.boundPort(),
                       "GET /partial HTTP/1.1\r\nHost: nativetv.local\r\nRange: bytes=0-6\r\n"
                       "Connection: close\r\n\r\n");
    check(response.find("HTTP/1.1 206 Partial Content") == 0 &&
              response.find("X-Forwarded-Range: bytes=0-6") != std::string::npos &&
              response.find("\r\n\r\npartial") != std::string::npos,
          "Node proxy should preserve partial response status, range and body");

    const int head = connectTo(front.boundPort());
    sendAll(head, "HEAD /head HTTP/1.1\r\nHost: nativetv.local\r\nConnection: close\r\n\r\n");
    const std::string headResponse = readHeadersOnly(head);
    close(head);
    check(headResponse.find("HTTP/1.1 200 OK") == 0 &&
              contentLength(headResponse.substr(0, headResponse.find("\r\n\r\n"))) == 9,
          "Node proxy should preserve HEAD status and representation length without a body");

    const int notModified = connectTo(front.boundPort());
    sendAll(notModified, "GET /not-modified HTTP/1.1\r\nHost: nativetv.local\r\n"
                         "Connection: close\r\n\r\n");
    const std::string notModifiedResponse = readHeadersOnly(notModified);
    close(notModified);
    check(notModifiedResponse.find("HTTP/1.1 304 Not Modified") == 0 &&
              notModifiedResponse.find("Content-Length: 9\r\n") != std::string::npos,
          "Node proxy should preserve 304 representation length without sending a body");

    front.stop();

    HttpProxy unavailable({"127.0.0.1", unusedPort(), secret, 1000});
    HttpServer failureFront;
    frontOptions.port = unusedPort();
    check(failureFront.start(
              frontOptions,
              [&unavailable](HttpExchange &exchange) { (void)unavailable.forward(exchange); },
              error),
          "failure fixture failed to start: " + error);
    response = get(failureFront.boundPort(), "/api/stats");
    check(response.find("HTTP/1.1 502 Bad Gateway") == 0 &&
              response.find("\"ok\":false") != std::string::npos,
          "a stopped Node backend should return the established JSON 502 shape");
    failureFront.stop();

    HttpProxy limited({"127.0.0.1", node.boundPort(), secret, 3000, 5});
    HttpServer limitedFront;
    frontOptions.port = unusedPort();
    check(limitedFront.start(
              frontOptions, [&limited](HttpExchange &exchange) { (void)limited.forward(exchange); },
              error),
          "body-limit fixture failed to start: " + error);
    response =
        request(limitedFront.boundPort(), "POST /chunked HTTP/1.1\r\nHost: nativetv.local\r\n"
                                          "Transfer-Encoding: chunked\r\nConnection: close\r\n\r\n"
                                          "6\r\n123456\r\n0\r\n\r\n");
    check(response.find("HTTP/1.1 413") == 0 &&
              response.find("request body too large") != std::string::npos,
          "Node proxy should return 413 when a streamed chunked upload exceeds its limit");
    limitedFront.stop();
    node.stop();

    HttpProxy external({"192.0.2.1", 8081, secret, 1000});
    check(!external.valid(error) && error.find("loopback") != std::string::npos,
          "Node proxy should reject a non-loopback upstream");
    HttpProxy weakSecret({"127.0.0.1", 8081, "short", 1000});
    check(!weakSecret.valid(error) && error.find("32 safe characters") != std::string::npos,
          "Node proxy should reject a short trust-bridge secret");
}

void testHttpAuthentication() {
    HttpRequest requestValue;
    requestValue.remoteAddress = "192.0.2.15";
    const std::string token = "shared-api-token";
    check(!requestIsAuthorized(requestValue, token),
          "a remote API request without a token should be denied");

    requestValue.query = "mode=dashboard&k=shared-api-token";
    check(requestIsAuthorized(requestValue, token),
          "the legacy query token should authorize an API request");
    requestValue.query = "mode=dashboard&k=shared%2Dapi%2Dtoken";
    check(requestIsAuthorized(requestValue, token),
          "percent-encoded query tokens should be decoded like Node URL queries");

    requestValue.query.clear();
    requestValue.headers = {{"Authorization", "bEaReR shared-api-token"}};
    check(requestIsAuthorized(requestValue, token),
          "bearer token scheme should be case-insensitive");
    requestValue.headers = {{"Authorization", "Bearer wrong"}};
    requestValue.query = "k=shared-api-token";
    check(requestIsAuthorized(requestValue, token),
          "a valid query token should remain accepted when another bearer value is invalid");

    requestValue.headers.clear();
    requestValue.query = "k=shared-api-token&k=wrong";
    check(!requestIsAuthorized(requestValue, token),
          "duplicate query tokens should not be interpreted ambiguously");
    requestValue.query.clear();
    requestValue.remoteAddress = "::ffff:127.0.0.1";
    check(isLoopbackPeer(requestValue.remoteAddress) && requestIsAuthorized(requestValue, token),
          "the TV's IPv4-mapped loopback address should retain its token bypass");
    requestValue.remoteAddress = "192.0.2.15";
    check(requestIsAuthorized(requestValue, ""),
          "an unset token should preserve the legacy open API behavior");
}

void testBufferedExchange() {
    HttpRequest request;
    request.method = "POST";
    request.uri = "/api/control";
    request.version = "1.1";
    request.contentLength = 5;
    request.headers.push_back({"Content-Type", "application/json"});
    HttpExchange exchange(request, "hello");
    check(exchange.setBodyLimit(8), "buffered exchange rejected a valid body limit");
    char buffer[8] = {};
    check(exchange.readBody(buffer, 3) == 3 && std::string(buffer, 3) == "hel",
          "buffered exchange did not stream the request body");
    check(exchange.readBody(buffer, sizeof(buffer)) == 2 && std::string(buffer, 2) == "lo",
          "buffered exchange did not return the final request bytes");
    check(exchange.readBody(buffer, sizeof(buffer)) == 0,
          "buffered exchange did not end at the request content length");
    check(exchange.respond(202, {{"Content-Type", "application/json"}}, "{\"ok\":true}"),
          "buffered exchange could not capture a route response");
    check(exchange.responseStatus() == 202 && exchange.responseBody() == "{\"ok\":true}",
          "buffered exchange did not expose its route response");

    request.contentLength = 5;
    HttpExchange oversized(request, "hello");
    check(!oversized.setBodyLimit(4) && oversized.bodyLimitExceeded(),
          "buffered exchange did not reject a body beyond its limit");
}
} // namespace

int main() {
    try {
        testRequestsAndPersistentConnections();
        testOptionBounds();
        testNativeCapabilities();
        testIpv4OnlyAndDualStackFallback();
        testNodeProxy();
        testHttpAuthentication();
        testBufferedExchange();
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "native HTTP server test failed: %s\n", error.what());
        return 1;
    }
}
