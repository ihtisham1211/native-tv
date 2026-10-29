#include "daemon/http_proxy.hpp"
#include "civetweb.h"
#include "core/json.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace nativetv {
namespace daemon {
namespace {
std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c);
    });
    return value;
}

bool hasToken(const std::string &value, const std::string &token) {
    size_t start = 0;
    while (start <= value.size()) {
        const size_t end = value.find(',', start);
        size_t first = start;
        size_t last = end == std::string::npos ? value.size() : end;
        while (first < last && (value[first] == ' ' || value[first] == '\t'))
            ++first;
        while (last > first && (value[last - 1] == ' ' || value[last - 1] == '\t'))
            --last;
        if (lower(value.substr(first, last - first)) == token)
            return true;
        if (end == std::string::npos)
            return false;
        start = end + 1;
    }
    return false;
}

bool hopByHop(const std::string &name, const std::string &connectionTokens) {
    const std::string normalized = lower(name);
    return normalized == "connection" || normalized == "keep-alive" ||
           normalized == "proxy-authenticate" || normalized == "proxy-authorization" ||
           normalized == "proxy-connection" || normalized == "te" || normalized == "trailer" ||
           normalized == "transfer-encoding" || normalized == "upgrade" ||
           hasToken(connectionTokens, normalized);
}

bool writeAll(mg_connection *connection, const void *data, size_t size) {
    const char *bytes = static_cast<const char *>(data);
    size_t offset = 0;
    while (offset < size) {
        const int written = mg_write(connection, bytes + offset, size - offset);
        if (written <= 0)
            return false;
        offset += static_cast<size_t>(written);
    }
    return true;
}

bool requestHasBody(const HttpRequest &request) {
    return request.contentLength > 0 || !request.header("transfer-encoding").empty();
}

bool safeRequestTarget(const HttpRequest &request, std::string &target) {
    if (request.uri.empty() || request.uri[0] != '/' ||
        request.uri.find_first_of("\r\n\0 ", 0, 4) != std::string::npos ||
        request.query.find_first_of("\r\n\0 ", 0, 4) != std::string::npos)
        return false;
    target = request.uri;
    if (!request.query.empty())
        target += "?" + request.query;
    return true;
}

bool sendFailure(HttpExchange &exchange) {
    const std::string body = "{\"ok\":false,\"error\":\"native service unavailable\"}";
    return exchange.respond(502,
                            {{"Content-Type", "application/json; charset=utf-8"},
                             {"Cache-Control", "no-store"},
                             {"X-Content-Type-Options", "nosniff"}},
                            body);
}

bool responseHasNoBody(int status) {
    return (status >= 100 && status < 200) || status == 204 || status == 304;
}

bool sendTooLarge(HttpExchange &exchange) {
    return exchange.respond(413,
                            {{"Content-Type", "application/json; charset=utf-8"},
                             {"Cache-Control", "no-store"},
                             {"Connection", "close"}},
                            "{\"ok\":false,\"error\":\"request body too large\"}");
}
} // namespace

HttpProxy::HttpProxy(HttpProxyOptions options, std::shared_ptr<NodeFreeHooks> local)
    : options_(std::move(options)), local_(std::move(local)) {}

bool HttpProxy::valid(std::string &error) const {
    if (options_.host != "127.0.0.1" && options_.host != "::1") {
        error = "Node proxy host must be a loopback address";
        return false;
    }
    if (!options_.port || options_.responseTimeoutMs == 0 || options_.responseTimeoutMs > 120000) {
        error = "Node proxy port must be non-zero and response timeout must be 1 to 120000 ms";
        return false;
    }
    if (!options_.maximumBodyBytes || options_.maximumBodyBytes > 536870912) {
        error = "Node proxy body limit must be between 1 byte and 512 MiB";
        return false;
    }
    if (options_.sharedSecret.size() < 32 ||
        options_.sharedSecret.find_first_of("\r\n\0", 0, 3) != std::string::npos) {
        error = "Node proxy secret must contain at least 32 safe characters";
        return false;
    }
    return true;
}

bool HttpProxy::forward(HttpExchange &exchange) const {
    return forwardImpl(exchange, nullptr);
}

bool HttpProxy::forwardBufferedBody(HttpExchange &exchange,
                                    const std::string &body) const {
    return forwardImpl(exchange, &body);
}

bool HttpProxy::forwardImpl(HttpExchange &exchange,
                            const std::string *bufferedBody) const {
    if (local_)   // no Node: what is not served here is not found
        return exchange.respond(
            404,
            {{"Content-Type", "application/json; charset=utf-8"}, {"Cache-Control", "no-store"}},
            "{\"ok\":false,\"error\":\"not found\"}");
    const HttpRequest &request = exchange.request();
    std::string error;
    if (!valid(error))
        return sendFailure(exchange);

    const std::string connectionTokens = request.header("connection");
    if (hasToken(connectionTokens, "upgrade") || !request.header("upgrade").empty()) {
        return exchange.respond(
            501,
            {{"Content-Type", "application/json; charset=utf-8"}, {"Cache-Control", "no-store"}},
            "{\"ok\":false,\"error\":\"protocol upgrade is unsupported\"}");
    }

    std::string target;
    if (!safeRequestTarget(request, target))
        return exchange.respond(400, {{"Content-Type", "text/plain; charset=utf-8"}},
                                "Bad Request\n");

    const bool bodyExpected = bufferedBody ? !bufferedBody->empty() : requestHasBody(request);
    if (!bufferedBody && bodyExpected && request.contentLength >= 0 &&
        !exchange.setBodyLimit(options_.maximumBodyBytes)) {
        return sendTooLarge(exchange);
    }
    if (!bufferedBody && bodyExpected && request.contentLength < 0 &&
        !exchange.setBodyLimit(options_.maximumBodyBytes)) {
        return sendTooLarge(exchange);
    }

    char connectError[256] = {};
    mg_connection *upstream = mg_connect_client(options_.host.c_str(), options_.port, 0,
                                                connectError, sizeof(connectError));
    if (!upstream)
        return sendFailure(exchange);

    bool requestSent = false;
    bool bodyTooLarge = false;
    do {
        std::string outbound = request.method + " " + target + " HTTP/1.1\r\n";
        bool sawHost = false;
        for (const HttpHeader &header : request.headers) {
            const std::string name = lower(header.name);
            if (name == "host") {
                if (!sawHost) {
                    outbound += header.name + ": " + header.value + "\r\n";
                    sawHost = true;
                }
                continue;
            }
            if (hopByHop(name, connectionTokens) || name == "content-length" || name == "expect" ||
                name == "x-nativetv-original-remote" || name == "x-nativetv-proxy-secret" ||
                name == "x-forwarded-for" || name == "x-real-ip")
                continue;
            outbound += header.name + ": " + header.value + "\r\n";
        }
        if (!sawHost)
            outbound += "Host: 127.0.0.1:" + std::to_string(options_.port) + "\r\n";
        if (bufferedBody)
            outbound += "Content-Length: " + std::to_string(bufferedBody->size()) + "\r\n";
        else if (request.contentLength >= 0)
            outbound += "Content-Length: " + std::to_string(request.contentLength) + "\r\n";
        else if (bodyExpected)
            outbound += "Transfer-Encoding: chunked\r\n";
        outbound += "X-NativeTv-Original-Remote: " + request.remoteAddress + "\r\n";
        outbound += "X-NativeTv-Proxy-Secret: " + options_.sharedSecret + "\r\n";
        outbound += "Connection: close\r\n\r\n";

        if (!writeAll(upstream, outbound.data(), outbound.size()))
            break;

        char buffer[16384];
        if (bufferedBody) {
            requestSent = bufferedBody->empty() ||
                          writeAll(upstream, bufferedBody->data(), bufferedBody->size());
        } else if (bodyExpected) {
            for (;;) {
                const int count = exchange.readBody(buffer, sizeof(buffer));
                if (count < 0) {
                    bodyTooLarge = exchange.bodyLimitExceeded();
                    break;
                }
                if (count == 0) {
                    if (request.contentLength < 0 && !writeAll(upstream, "0\r\n\r\n", 5))
                        break;
                    requestSent = true;
                    break;
                }
                if (request.contentLength < 0) {
                    char chunkSize[32];
                    const int digits = std::snprintf(chunkSize, sizeof(chunkSize), "%x\r\n",
                                                     static_cast<unsigned int>(count));
                    if (digits <= 0 || static_cast<size_t>(digits) >= sizeof(chunkSize) ||
                        !writeAll(upstream, chunkSize, static_cast<size_t>(digits)) ||
                        !writeAll(upstream, buffer, static_cast<size_t>(count)) ||
                        !writeAll(upstream, "\r\n", 2))
                        break;
                } else if (!writeAll(upstream, buffer, static_cast<size_t>(count))) {
                    break;
                }
            }
        } else {
            requestSent = true;
        }
    } while (false);

    if (!requestSent) {
        mg_close_connection(upstream);
        if (bodyTooLarge)
            return sendTooLarge(exchange);
        return sendFailure(exchange);
    }

    char responseError[256] = {};
    if (mg_get_response(upstream, responseError, sizeof(responseError),
                        static_cast<int>(options_.responseTimeoutMs)) < 0) {
        mg_close_connection(upstream);
        return sendFailure(exchange);
    }

    const mg_response_info *info = mg_get_response_info(upstream);
    if (!info || info->status_code < 100 || info->status_code > 599) {
        mg_close_connection(upstream);
        return sendFailure(exchange);
    }

    std::vector<HttpHeader> responseHeaders;
    std::string responseConnectionTokens;
    std::string responseContentLength;
    for (int index = 0; index < info->num_headers; ++index) {
        const mg_header &header = info->http_headers[index];
        if (header.name && lower(header.name) == "connection" && header.value)
            responseConnectionTokens = header.value;
        if (header.name && lower(header.name) == "content-length" && header.value)
            responseContentLength = header.value;
    }
    for (int index = 0; index < info->num_headers; ++index) {
        const mg_header &header = info->http_headers[index];
        if (!header.name || !header.value || hopByHop(header.name, responseConnectionTokens) ||
            lower(header.name) == "content-length")
            continue;
        responseHeaders.push_back({header.name, header.value});
    }

    long long responseLength = info->content_length;
    if (info->status_code == 204 || info->status_code == 304) {
        if (info->status_code == 304 && !responseContentLength.empty())
            responseHeaders.push_back({"Content-Length", responseContentLength});
        responseLength = -1;
    } else if (request.method == "HEAD" && responseLength < 0 && !responseContentLength.empty()) {
        responseHeaders.push_back({"Content-Length", responseContentLength});
    }
    if (!exchange.beginResponse(info->status_code, responseHeaders, responseLength)) {
        mg_close_connection(upstream);
        return sendFailure(exchange);
    }

    bool completed = true;
    if (!responseHasNoBody(info->status_code) && request.method != "HEAD") {
        char buffer[16384];
        for (;;) {
            const int count = mg_read(upstream, buffer, sizeof(buffer));
            if (count < 0) {
                completed = false;
                break;
            }
            if (count == 0)
                break;
            if (!exchange.writeBody(buffer, static_cast<size_t>(count))) {
                completed = false;
                break;
            }
        }
    }
    mg_close_connection(upstream);
    return completed && exchange.finishResponse();
}

bool HttpProxy::requestNodeConfigReload(std::string &error) const {
    if (local_) return true;   // nothing to tell
    if (!valid(error))
        return false;

    char connectError[256] = {};
    mg_connection *upstream = mg_connect_client(options_.host.c_str(), options_.port, 0,
                                                connectError, sizeof(connectError));
    if (!upstream) {
        error = connectError[0] ? connectError : "could not connect to Node";
        return false;
    }

    const std::string request = "POST /_internal/reload-config HTTP/1.1\r\n"
                                "Host: localhost\r\n"
                                "X-NativeTv-Proxy-Secret: " +
                                options_.sharedSecret +
                                "\r\n"
                                "Content-Length: 0\r\n"
                                "Connection: close\r\n\r\n";
    if (!writeAll(upstream, request.data(), request.size())) {
        error = "could not send config reload request to Node";
        mg_close_connection(upstream);
        return false;
    }

    char responseError[256] = {};
    if (mg_get_response(upstream, responseError, sizeof(responseError), 5000) < 0) {
        error = responseError[0] ? responseError : "Node config reload timed out";
        mg_close_connection(upstream);
        return false;
    }

    const mg_response_info *info = mg_get_response_info(upstream);
    if (!info || info->status_code != 200) {
        error = info ? "Node config reload returned HTTP " + std::to_string(info->status_code)
                     : "Node config reload returned no HTTP response";
        mg_close_connection(upstream);
        return false;
    }
    if (info->content_length > 4096) {
        error = "Node config reload response exceeded the size limit";
        mg_close_connection(upstream);
        return false;
    }

    std::string body;
    char buffer[1024];
    for (;;) {
        const int count = mg_read(upstream, buffer, sizeof(buffer));
        if (count < 0) {
            error = "could not read Node config reload response";
            mg_close_connection(upstream);
            return false;
        }
        if (count == 0)
            break;
        if (body.size() + static_cast<size_t>(count) > 4096) {
            error = "Node config reload response exceeded the size limit";
            mg_close_connection(upstream);
            return false;
        }
        body.append(buffer, static_cast<size_t>(count));
    }
    mg_close_connection(upstream);

    const Json response = Json::parse(body);
    if (!response.valid() || !response["ok"].isBoolean() || !response["ok"].boolean()) {
        error = "Node rejected the config reload request";
        return false;
    }
    error.clear();
    return true;
}

bool HttpProxy::requestNodeAppUploadStart(const std::string &file, int &status,
                                          Json &response,
                                          std::string &error) const {
    if (local_) {
        error = "there is no Node to ask";
        return false;
    }
    status = 0;
    response = Json();
    error.clear();
    if (file.empty()) {
        error = "invalid Node app upload parameters";
        return false;
    }
    if (!valid(error))
        return false;

    Json payload = Json::object();
    payload.set("action", Json::text("start"));
    payload.set("file", Json::text(file));
    const std::string body = payload.dump();

    char connectError[256] = {};
    mg_connection *upstream = mg_connect_client(options_.host.c_str(), options_.port, 0,
                                                connectError, sizeof(connectError));
    if (!upstream) {
        error = connectError[0] ? connectError : "could not connect to Node";
        return false;
    }

    const std::string request = "POST /_internal/apps/upload HTTP/1.1\r\n"
                                "Host: localhost\r\n"
                                "Content-Type: application/json\r\n"
                                "X-NativeTv-Proxy-Secret: " + options_.sharedSecret +
                                "\r\nContent-Length: " + std::to_string(body.size()) +
                                "\r\nConnection: close\r\n\r\n" + body;
    if (!writeAll(upstream, request.data(), request.size())) {
        error = "could not send app upload request to Node";
        mg_close_connection(upstream);
        return false;
    }

    char responseError[256] = {};
    if (mg_get_response(upstream, responseError, sizeof(responseError), 30000) < 0) {
        error = responseError[0] ? responseError : "Node app upload request timed out";
        mg_close_connection(upstream);
        return false;
    }

    const mg_response_info *info = mg_get_response_info(upstream);
    if (!info || info->status_code < 100 || info->status_code > 599) {
        error = "Node app upload request returned no valid HTTP response";
        mg_close_connection(upstream);
        return false;
    }
    status = info->status_code;
    if (info->content_length > 1024 * 1024) {
        error = "Node app upload response exceeded the size limit";
        mg_close_connection(upstream);
        return false;
    }

    std::string responseBody;
    char buffer[4096];
    for (;;) {
        const int count = mg_read(upstream, buffer, sizeof(buffer));
        if (count < 0) {
            error = "could not read Node app upload response";
            mg_close_connection(upstream);
            return false;
        }
        if (count == 0)
            break;
        if (responseBody.size() + static_cast<size_t>(count) > 1024 * 1024) {
            error = "Node app upload response exceeded the size limit";
            mg_close_connection(upstream);
            return false;
        }
        responseBody.append(buffer, static_cast<size_t>(count));
    }
    mg_close_connection(upstream);

    response = Json::parse(responseBody);
    if (!response.isObject()) {
        error = "Node app upload response was not valid JSON";
        return false;
    }
    return true;
}

bool HttpProxy::requestNodeAppInstallStart(const Json &payload,
                                           int &status, Json &response,
                                           std::string &error) const {
    if (local_) {
        error = "there is no Node to ask";
        return false;
    }
    if (!payload.isObject()) {
        status = 0;
        response = Json();
        error = "invalid app install start payload";
        return false;
    }
    if (!requestNodeAppInstallBridge("/_internal/apps/install-start", payload,
                                    status, response, error))
        return false;
    if (status == 200 &&
        (!response["ok"].isBoolean() || !response["ok"].boolean() ||
         !response["install"].isObject())) {
        error = "Node app install start returned an invalid install snapshot";
        response = Json();
        return false;
    }
    if (status < 200 || status >= 300) {
        if (!response["ok"].isBoolean() || response["ok"].boolean() ||
            !response["error"].isString()) {
            error = "Node app install start returned an invalid error response";
            response = Json();
            return false;
        }
    }
    return true;
}

bool HttpProxy::requestNodeAppInstallAction(const std::string &action,
                                            const Json &payload, bool nativeValidated,
                                            int &status, Json &response,
                                            std::string &error) const {
    if (local_) {
        error = "there is no Node to ask";
        return false;
    }
    if ((action != "confirm" && action != "cancel") ||
        (!payload.isObject() && !payload.isArray())) {
        status = 0;
        response = Json();
        error = "invalid app install action payload";
        return false;
    }
    Json request = Json::object();
    request.set("action", Json::text(action));
    request.set("body", payload);
    request.set("nativeValidated", Json::flag(nativeValidated));
    if (!requestNodeAppInstallBridge("/_internal/apps/install-action", request,
                                    status, response, error))
        return false;
    if (status == 200 &&
        (!response["ok"].isBoolean() || !response["ok"].boolean() ||
         (action == "confirm" && !response["install"].isObject()))) {
        error = "Node app install action returned an invalid response";
        response = Json();
        return false;
    }
    if (status < 200 || status >= 300) {
        if (!response["ok"].isBoolean() || response["ok"].boolean() ||
            !response["error"].isString()) {
            error = "Node app install action returned an invalid error response";
            response = Json();
            return false;
        }
    }
    return true;
}

bool HttpProxy::requestNodeHomeEscape(bool escaped, std::string &error) const {
    if (local_) return true;   // nothing to tell
    Json payload = Json::object();
    payload.set("escaped", Json::flag(escaped));
    int status = 0;
    Json response;
    if (!requestNodeAppInstallBridge("/_internal/home-escape", payload,
                                     status, response, error))
        return false;
    if (status != 200 || !response["ok"].isBoolean() ||
        !response["ok"].boolean()) {
        error = response["error"].isString() ? response["error"].string()
                                             : "Node Home escape bridge refused";
        return false;
    }
    return true;
}

bool HttpProxy::requestNodeHomeForegroundFallback(
    const std::string &appId, std::string &error) const {
    if (local_) {
        error = "there is no Node to ask";
        return false;
    }
    Json payload = Json::object();
    payload.set("foregroundFallback", Json::flag(true));
    payload.set("appId", Json::text(appId));
    int status = 0;
    Json response;
    if (!requestNodeAppInstallBridge("/_internal/home-escape", payload,
                                     status, response, error))
        return false;
    if (status != 200 || !response["ok"].isBoolean() ||
        !response["ok"].boolean()) {
        error = response["error"].isString() ? response["error"].string()
                                             : "Node Home foreground fallback refused";
        return false;
    }
    return true;
}

bool HttpProxy::requestNodeAppInstallBridge(const std::string &path,
                                            const Json &payload, int &status,
                                            Json &response,
                                            std::string &error) const {
    if (local_) {
        error = "there is no Node to ask";
        return false;
    }
    status = 0;
    response = Json();
    error.clear();
    if (!valid(error))
        return false;

    const std::string body = payload.dump();
    if (body.empty() || body.size() > 65536) {
        error = "Node app install bridge request exceeded the size limit";
        return false;
    }

    char connectError[256] = {};
    mg_connection *upstream = mg_connect_client(options_.host.c_str(), options_.port, 0,
                                                connectError, sizeof(connectError));
    if (!upstream) {
        error = connectError[0] ? connectError : "could not connect to Node";
        return false;
    }

    const std::string request = "POST " + path + " HTTP/1.1\r\n"
                                "Host: localhost\r\n"
                                "Content-Type: application/json\r\n"
                                "X-NativeTv-Proxy-Secret: " + options_.sharedSecret +
                                "\r\nContent-Length: " + std::to_string(body.size()) +
                                "\r\nConnection: close\r\n\r\n" + body;
    if (!writeAll(upstream, request.data(), request.size())) {
        error = "could not send app install bridge request to Node";
        mg_close_connection(upstream);
        return false;
    }

    char responseError[256] = {};
    if (mg_get_response(upstream, responseError, sizeof(responseError), 10000) < 0) {
        error = responseError[0] ? responseError : "Node app install bridge request timed out";
        mg_close_connection(upstream);
        return false;
    }
    const mg_response_info *info = mg_get_response_info(upstream);
    if (!info || info->status_code < 100 || info->status_code > 599) {
        error = "Node app install bridge returned no valid HTTP response";
        mg_close_connection(upstream);
        return false;
    }
    status = info->status_code;
    if (info->content_length > 1024 * 1024) {
        error = "Node app install bridge response exceeded the size limit";
        mg_close_connection(upstream);
        return false;
    }

    std::string responseBody;
    char buffer[4096];
    for (;;) {
        const int count = mg_read(upstream, buffer, sizeof(buffer));
        if (count < 0) {
            error = "could not read Node app install bridge response";
            mg_close_connection(upstream);
            return false;
        }
        if (count == 0)
            break;
        if (responseBody.size() + static_cast<size_t>(count) > 1024 * 1024) {
            error = "Node app install bridge response exceeded the size limit";
            mg_close_connection(upstream);
            return false;
        }
        responseBody.append(buffer, static_cast<size_t>(count));
    }
    mg_close_connection(upstream);

    response = Json::parse(responseBody);
    if (!response.isObject()) {
        error = "Node app install bridge returned invalid JSON";
        return false;
    }
    return true;
}

bool HttpProxy::requestNodeRestart(bool checkOnly, int &status, std::string &body,
                                   std::string &error) const {
    if (local_) {
        status = 0;
        body.clear();
        error.clear();
        if (local_->restart)
            return local_->restart(checkOnly, status, body, error);
        error = "there is no Node to ask";
        return false;
    }
    status = 0;
    body.clear();
    error.clear();
    if (!valid(error))
        return false;

    char connectError[256] = {};
    mg_connection *upstream = mg_connect_client(options_.host.c_str(), options_.port, 0,
                                                connectError, sizeof(connectError));
    if (!upstream) {
        error = connectError[0] ? connectError : "could not connect to Node";
        return false;
    }

    const std::string target = checkOnly ? "/_internal/restart?check=1" : "/_internal/restart";
    const std::string request = "POST " + target + " HTTP/1.1\r\nHost: localhost\r\n"
                                "X-NativeTv-Proxy-Secret: " +
                                options_.sharedSecret +
                                "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    if (!writeAll(upstream, request.data(), request.size())) {
        error = "could not send Node restart request";
        mg_close_connection(upstream);
        return false;
    }

    char responseError[256] = {};
    if (mg_get_response(upstream, responseError, sizeof(responseError), 5000) < 0) {
        error = responseError[0] ? responseError : "Node restart request timed out";
        mg_close_connection(upstream);
        return false;
    }

    const mg_response_info *info = mg_get_response_info(upstream);
    if (!info) {
        error = "Node restart request returned no HTTP response";
        mg_close_connection(upstream);
        return false;
    }
    status = info->status_code;
    if (info->content_length > 4096) {
        error = "Node restart response exceeded the size limit";
        mg_close_connection(upstream);
        return false;
    }

    char buffer[1024];
    for (;;) {
        const int count = mg_read(upstream, buffer, sizeof(buffer));
        if (count < 0) {
            error = "could not read Node restart response";
            mg_close_connection(upstream);
            body.clear();
            return false;
        }
        if (count == 0)
            break;
        if (body.size() + static_cast<size_t>(count) > 4096) {
            error = "Node restart response exceeded the size limit";
            mg_close_connection(upstream);
            body.clear();
            return false;
        }
        body.append(buffer, static_cast<size_t>(count));
    }
    mg_close_connection(upstream);
    if (status < 200 || status > 599) {
        error = "Node restart request returned an invalid status";
        body.clear();
        return false;
    }
    if (!Json::parse(body).isObject()) {
        error = "Node restart response was not a JSON object";
        body.clear();
        return false;
    }
    return true;
}

bool HttpProxy::requestNodeSetupContext(bool demo, Json &context, std::string &error) const {
    if (local_) {
        error.clear();
        if (local_->setupContext)
            return local_->setupContext(demo, context, error);
        error = "there is no Node to ask";
        return false;
    }
    context = Json();
    error.clear();
    if (!valid(error))
        return false;

    char connectError[256] = {};
    mg_connection *upstream = mg_connect_client(options_.host.c_str(), options_.port, 0,
                                                connectError, sizeof(connectError));
    if (!upstream) {
        error = connectError[0] ? connectError : "could not connect to Node";
        return false;
    }

    const std::string target = demo ? "/_internal/setup-context?demo=1"
                                    : "/_internal/setup-context";
    const std::string request = "POST " + target + " HTTP/1.1\r\nHost: localhost\r\n"
                                "X-NativeTv-Proxy-Secret: " + options_.sharedSecret +
                                "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    if (!writeAll(upstream, request.data(), request.size())) {
        error = "could not send setup context request to Node";
        mg_close_connection(upstream);
        return false;
    }

    char responseError[256] = {};
    if (mg_get_response(upstream, responseError, sizeof(responseError), 5000) < 0) {
        error = responseError[0] ? responseError : "Node setup context request timed out";
        mg_close_connection(upstream);
        return false;
    }

    const mg_response_info *info = mg_get_response_info(upstream);
    if (!info || info->status_code != 200) {
        error = info ? "Node setup context returned HTTP " + std::to_string(info->status_code)
                     : "Node setup context returned no HTTP response";
        mg_close_connection(upstream);
        return false;
    }
    if (info->content_length > 4096) {
        error = "Node setup context exceeded the size limit";
        mg_close_connection(upstream);
        return false;
    }

    std::string body;
    char buffer[1024];
    for (;;) {
        const int count = mg_read(upstream, buffer, sizeof(buffer));
        if (count < 0) {
            error = "could not read Node setup context";
            mg_close_connection(upstream);
            return false;
        }
        if (count == 0)
            break;
        if (body.size() + static_cast<size_t>(count) > 4096) {
            error = "Node setup context exceeded the size limit";
            mg_close_connection(upstream);
            return false;
        }
        body.append(buffer, static_cast<size_t>(count));
    }
    mg_close_connection(upstream);

    context = Json::parse(body);
    if (!context.isObject() || !context["homeAssistant"].isObject()) {
        error = "Node setup context was not a valid JSON object";
        context = Json();
        return false;
    }
    return true;
}

bool HttpProxy::requestNodeSettingsContext(Json &context, std::string &error) const {
    if (local_) {
        error = "there is no Node to ask";
        return false;
    }
    context = Json();
    error.clear();
    if (!valid(error))
        return false;

    char connectError[256] = {};
    mg_connection *upstream = mg_connect_client(options_.host.c_str(), options_.port, 0,
                                                connectError, sizeof(connectError));
    if (!upstream) {
        error = connectError[0] ? connectError : "could not connect to Node";
        return false;
    }

    const std::string request = "POST /_internal/settings-context HTTP/1.1\r\n"
                                "Host: localhost\r\nX-NativeTv-Proxy-Secret: " +
                                options_.sharedSecret +
                                "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    if (!writeAll(upstream, request.data(), request.size())) {
        error = "could not send settings context request to Node";
        mg_close_connection(upstream);
        return false;
    }

    constexpr size_t kMaximumBytes = 65536;
    char responseError[256] = {};
    if (mg_get_response(upstream, responseError, sizeof(responseError), 5000) < 0) {
        error = responseError[0] ? responseError : "Node settings context request timed out";
        mg_close_connection(upstream);
        return false;
    }

    const mg_response_info *info = mg_get_response_info(upstream);
    if (!info || info->status_code != 200) {
        error = info ? "Node settings context returned HTTP " + std::to_string(info->status_code)
                     : "Node settings context returned no HTTP response";
        mg_close_connection(upstream);
        return false;
    }
    if (info->content_length > static_cast<long long>(kMaximumBytes)) {
        error = "Node settings context exceeded the size limit";
        mg_close_connection(upstream);
        return false;
    }

    std::string body;
    char buffer[1024];
    for (;;) {
        const int count = mg_read(upstream, buffer, sizeof(buffer));
        if (count < 0) {
            error = "could not read Node settings context";
            mg_close_connection(upstream);
            return false;
        }
        if (count == 0)
            break;
        if (body.size() + static_cast<size_t>(count) > kMaximumBytes) {
            error = "Node settings context exceeded the size limit";
            mg_close_connection(upstream);
            return false;
        }
        body.append(buffer, static_cast<size_t>(count));
    }
    mg_close_connection(upstream);

    context = Json::parse(body);
    if (!context.isObject() || !context["status"].isObject()) {
        error = "Node settings context was not a valid JSON object";
        context = Json();
        return false;
    }
    return true;
}

bool HttpProxy::requestNodeAppsContext(Json &context, std::string &error) const {
    if (local_) {
        error = "there is no Node to ask";
        return false;
    }
    context = Json();
    error.clear();
    if (!valid(error)) return false;

    char connectError[256] = {};
    mg_connection *upstream = mg_connect_client(options_.host.c_str(), options_.port, 0,
                                                connectError, sizeof(connectError));
    if (!upstream) {
        error = connectError[0] ? connectError : "could not connect to Node";
        return false;
    }
    const std::string request = "POST /_internal/apps-context HTTP/1.1\r\n"
                                "Host: localhost\r\nX-NativeTv-Proxy-Secret: " +
                                options_.sharedSecret +
                                "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    if (!writeAll(upstream, request.data(), request.size())) {
        error = "could not send apps context request to Node";
        mg_close_connection(upstream);
        return false;
    }

    constexpr size_t kMaximumBytes = 262144;
    char responseError[256] = {};
    const unsigned int timeout = std::min(options_.responseTimeoutMs, 30000U);
    if (mg_get_response(upstream, responseError, sizeof(responseError),
                        static_cast<int>(timeout)) < 0) {
        error = responseError[0] ? responseError : "Node apps context request timed out";
        mg_close_connection(upstream);
        return false;
    }
    const mg_response_info *info = mg_get_response_info(upstream);
    if (!info || info->status_code != 200) {
        error = info ? "Node apps context returned HTTP " + std::to_string(info->status_code)
                     : "Node apps context returned no HTTP response";
        mg_close_connection(upstream);
        return false;
    }
    if (info->content_length > static_cast<long long>(kMaximumBytes)) {
        error = "Node apps context exceeded the size limit";
        mg_close_connection(upstream);
        return false;
    }
    std::string body;
    char buffer[4096];
    for (;;) {
        const int count = mg_read(upstream, buffer, sizeof(buffer));
        if (count < 0) {
            error = "could not read Node apps context";
            mg_close_connection(upstream);
            return false;
        }
        if (count == 0) break;
        if (body.size() + static_cast<size_t>(count) > kMaximumBytes) {
            error = "Node apps context exceeded the size limit";
            mg_close_connection(upstream);
            return false;
        }
        body.append(buffer, static_cast<size_t>(count));
    }
    mg_close_connection(upstream);
    context = Json::parse(body);
    if (!context.isObject()) {
        error = "Node apps context was not a valid JSON object";
        context = Json();
        return false;
    }
    return true;
}

bool HttpProxy::requestNodeStats(const Json &system, bool withSettings, std::string &body,
                                 std::string &error, bool withPrometheusData) const {
    if (local_) {
        error = "there is no Node to ask";
        return false;
    }
    body.clear();
    error.clear();
    if (!valid(error))
        return false;
    if (!system.isObject() ||
        (!withPrometheusData &&
         (!system["ok"].isBoolean() || !system["ok"].boolean()))) {
        error = "native system snapshot is invalid";
        return false;
    }

    Json payload = Json::object();
    payload.set("system", system);
    payload.set("withSettings", Json::flag(withSettings));
    if (withPrometheusData) payload.set("withPrometheus", Json::flag(true));
    const std::string serialized = payload.dump();
    if (serialized.size() > 524288) {
        error = "native system snapshot exceeded the request limit";
        return false;
    }

    char connectError[256] = {};
    mg_connection *upstream = mg_connect_client(options_.host.c_str(), options_.port, 0,
                                                connectError, sizeof(connectError));
    if (!upstream) {
        error = connectError[0] ? connectError : "could not connect to Node";
        return false;
    }

    const std::string request =
        "POST /_internal/native-stats HTTP/1.1\r\nHost: localhost\r\n"
        "Content-Type: application/json\r\nX-NativeTv-Proxy-Secret: " +
        options_.sharedSecret + "\r\nContent-Length: " + std::to_string(serialized.size()) +
        "\r\nConnection: close\r\n\r\n";
    if (!writeAll(upstream, request.data(), request.size()) ||
        !writeAll(upstream, serialized.data(), serialized.size())) {
        error = "could not send native stats request to Node";
        mg_close_connection(upstream);
        return false;
    }

    char responseError[256] = {};
    if (mg_get_response(upstream, responseError, sizeof(responseError),
                        static_cast<int>(options_.responseTimeoutMs)) < 0) {
        error = responseError[0] ? responseError : "Node native stats request timed out";
        mg_close_connection(upstream);
        return false;
    }

    const mg_response_info *info = mg_get_response_info(upstream);
    if (!info || info->status_code != 200) {
        error = info ? "Node native stats request returned HTTP " +
                           std::to_string(info->status_code)
                     : "Node native stats request returned no HTTP response";
        mg_close_connection(upstream);
        return false;
    }
    if (info->content_length > 4 * 1024 * 1024) {
        error = "Node native stats response exceeded the size limit";
        mg_close_connection(upstream);
        return false;
    }

    char buffer[8192];
    for (;;) {
        const int count = mg_read(upstream, buffer, sizeof(buffer));
        if (count < 0) {
            error = "could not read Node native stats response";
            mg_close_connection(upstream);
            body.clear();
            return false;
        }
        if (count == 0)
            break;
        if (body.size() + static_cast<size_t>(count) > 4 * 1024 * 1024) {
            error = "Node native stats response exceeded the size limit";
            mg_close_connection(upstream);
            body.clear();
            return false;
        }
        body.append(buffer, static_cast<size_t>(count));
    }
    mg_close_connection(upstream);
    if (!Json::parse(body).isObject()) {
        error = "Node native stats response was not a JSON object";
        body.clear();
        return false;
    }
    return true;
}

bool HttpProxy::requestNodeHistoryOverlay(int64_t fromMs, int64_t toMs, const Json &ids,
                                          Json &overlay,
                                          std::string &error) const {
    if (local_) {
        error.clear();
        if (local_->historyOverlay)
            return local_->historyOverlay(fromMs, toMs, ids, overlay, error);
        error = "there is no Node to ask";
        return false;
    }
    overlay = Json();
    error.clear();
    if (!valid(error))
        return false;
    Json payload = Json::object();
    payload.set("fromMs", Json::numeric(static_cast<double>(fromMs)));
    payload.set("toMs", Json::numeric(static_cast<double>(toMs)));
    if (ids.isObject()) payload.set("ids", ids);
    const std::string serialized = payload.dump();
    if (serialized.size() > 16384) {
        error = "native history ID request exceeded the limit";
        return false;
    }

    char connectError[256] = {};
    mg_connection *upstream = mg_connect_client(options_.host.c_str(), options_.port, 0,
                                                connectError, sizeof(connectError));
    if (!upstream) {
        error = connectError[0] ? connectError : "could not connect to Node";
        return false;
    }
    const std::string request =
        "POST /_internal/native-history HTTP/1.1\r\nHost: localhost\r\n"
        "Content-Type: application/json\r\nX-NativeTv-Proxy-Secret: " +
        options_.sharedSecret + "\r\nContent-Length: " + std::to_string(serialized.size()) +
        "\r\nConnection: close\r\n\r\n";
    if (!writeAll(upstream, request.data(), request.size()) ||
        !writeAll(upstream, serialized.data(), serialized.size())) {
        error = "could not send native history request to Node";
        mg_close_connection(upstream);
        return false;
    }
    char responseError[256] = {};
    if (mg_get_response(upstream, responseError, sizeof(responseError),
                        static_cast<int>(options_.responseTimeoutMs)) < 0) {
        error = responseError[0] ? responseError : "Node native history request timed out";
        mg_close_connection(upstream);
        return false;
    }
    const mg_response_info *info = mg_get_response_info(upstream);
    if (!info || info->status_code != 200) {
        error = info ? "Node native history request returned HTTP " +
                           std::to_string(info->status_code)
                     : "Node native history request returned no HTTP response";
        mg_close_connection(upstream);
        return false;
    }
    if (info->content_length > 524288) {
        error = "Node native history overlay exceeded the response limit";
        mg_close_connection(upstream);
        return false;
    }
    std::string body;
    char buffer[8192];
    for (;;) {
        const int count = mg_read(upstream, buffer, sizeof(buffer));
        if (count < 0) {
            error = "could not read Node native history response";
            mg_close_connection(upstream);
            return false;
        }
        if (count == 0)
            break;
        if (body.size() + static_cast<size_t>(count) > 524288) {
            error = "Node native history overlay exceeded the response limit";
            mg_close_connection(upstream);
            return false;
        }
        body.append(buffer, static_cast<size_t>(count));
    }
    mg_close_connection(upstream);
    overlay = Json::parse(body);
    if (!overlay.isObject() || !overlay["hours"].isObject() ||
        !overlay["titles"].isObject() || !overlay["enabled"].isBoolean() ||
        !overlay["clockSet"].isBoolean() || !overlay["readPersisted"].isBoolean()) {
        error = "Node native history response was invalid";
        overlay = Json();
        return false;
    }
    return true;
}

bool HttpProxy::requestNodeState(const Json &event, std::string &error,
                                 unsigned int timeoutMs) const {
    if (local_) return true;   // nothing to tell
    error.clear();
    if (!valid(error))
        return false;
    if (!event.isObject()) {
        error = "native state event is invalid";
        return false;
    }
    const std::string serialized = event.dump();
    if (serialized.size() > 16384) {
        error = "native state event exceeded the request limit";
        return false;
    }

    char connectError[256] = {};
    mg_connection *upstream = mg_connect_client(options_.host.c_str(), options_.port, 0,
                                                connectError, sizeof(connectError));
    if (!upstream) {
        error = connectError[0] ? connectError : "could not connect to Node";
        return false;
    }
    const std::string request =
        "POST /_internal/native-state HTTP/1.1\r\nHost: localhost\r\n"
        "Content-Type: application/json\r\nX-NativeTv-Proxy-Secret: " +
        options_.sharedSecret + "\r\nContent-Length: " + std::to_string(serialized.size()) +
        "\r\nConnection: close\r\n\r\n";
    if (!writeAll(upstream, request.data(), request.size()) ||
        !writeAll(upstream, serialized.data(), serialized.size())) {
        error = "could not send native state event to Node";
        mg_close_connection(upstream);
        return false;
    }
    char responseError[256] = {};
    if (mg_get_response(upstream, responseError, sizeof(responseError),
                        static_cast<int>(timeoutMs ? timeoutMs : options_.responseTimeoutMs)) < 0) {
        error = responseError[0] ? responseError : "Node native state request timed out";
        mg_close_connection(upstream);
        return false;
    }
    const mg_response_info *info = mg_get_response_info(upstream);
    const bool accepted = info && info->status_code == 204;
    if (!accepted)
        error = info ? "Node native state request returned HTTP " +
                           std::to_string(info->status_code)
                     : "Node native state request returned no HTTP response";
    mg_close_connection(upstream);
    return accepted;
}


} // namespace daemon
} // namespace nativetv
