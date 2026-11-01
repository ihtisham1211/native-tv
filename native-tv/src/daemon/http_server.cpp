#include "daemon/http_server.hpp"
#include "civetweb.h"
#include "domain/say_translation.hpp"
#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <string>

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

bool validHeaderName(const std::string &name) {
    if (name.empty())
        return false;
    for (unsigned char c : name) {
        const bool alphaNumeric = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                                  (c >= '0' && c <= '9');
        if (!alphaNumeric && c != '!' && c != '#' && c != '$' && c != '%' && c != '&' &&
            c != '\'' && c != '*' && c != '+' && c != '-' && c != '.' && c != '^' &&
            c != '_' && c != '`' && c != '|' && c != '~')
            return false;
    }
    return true;
}

bool validHeaderValue(const std::string &value) {
    return value.find('\r') == std::string::npos && value.find('\n') == std::string::npos &&
           value.find('\0') == std::string::npos;
}

std::string reasonPhrase(int status) {
    const char *phrase = mg_get_response_code_text(nullptr, status);
    return phrase ? phrase : "Unknown";
}

std::string portBinding(const HttpServerOptions &options) {
    const std::string port = std::to_string(options.port);
    if (options.host.empty() || options.host == "::")
        return "+" + port;
    if (options.host == "0.0.0.0")
        return options.host + ":" + port;
    if (options.host.front() == '[')
        return options.host + ":" + port;
    if (options.host.find(':') != std::string::npos)
        return "[" + options.host + "]:" + port;
    return options.host + ":" + port;
}

bool validOptions(const HttpServerOptions &options, std::string &error) {
    if (options.host.find_first_of(",\r\n") != std::string::npos) {
        error = "HTTP server host contains an invalid character";
        return false;
    }
    if (options.workerCount < 1 || options.workerCount > 64) {
        error = "HTTP server worker count must be between 1 and 64";
        return false;
    }
    if (options.connectionQueue < 1 || options.connectionQueue > 512 ||
        options.listenBacklog < 1 || options.listenBacklog > 512) {
        error = "HTTP server queue and backlog must be between 1 and 512";
        return false;
    }
    if (options.maximumHeaderBytes < 1024 || options.maximumHeaderBytes > 1024 * 1024 ||
        options.maximumBodyBytes == 0 || options.maximumBodyBytes > 536870912 ||
        options.requestTimeoutMs == 0 || options.keepAliveTimeoutMs == 0) {
        error = "HTTP server header and timeout limits are invalid";
        return false;
    }
    return true;
}

HttpRequest copyRequest(mg_connection *connection) {
    HttpRequest request;
    const mg_request_info *info = mg_get_request_info(connection);
    if (!info)
        return request;
    if (info->request_method)
        request.method = info->request_method;
    if (info->request_uri)
        request.uri = info->request_uri;
    if (info->query_string)
        request.query = info->query_string;
    if (info->http_version)
        request.version = info->http_version;
    request.remoteAddress = info->remote_addr;
    request.contentLength = info->content_length;
    for (int index = 0; index < info->num_headers; ++index) {
        HttpHeader header;
        if (info->http_headers[index].name)
            header.name = info->http_headers[index].name;
        if (info->http_headers[index].value)
            header.value = info->http_headers[index].value;
        request.headers.push_back(std::move(header));
    }
    return request;
}
} // namespace

std::string HttpRequest::header(const std::string &name) const {
    const std::string wanted = lower(name);
    for (const HttpHeader &item : headers) {
        if (lower(item.name) == wanted)
            return item.value;
    }
    return std::string();
}

bool HttpRequest::keepAlive() const {
    const std::string connection = lower(header("connection"));
    if (hasToken(connection, "close"))
        return false;
    if (version == "1.0")
        return hasToken(connection, "keep-alive");
    return version == "1.1";
}

HttpExchange::HttpExchange(mg_connection *connection, HttpRequest request,
                           size_t maximumBodyBytes)
    : connection_(connection), request_(std::move(request)), bodyLimit_(maximumBodyBytes),
      headRequest_(request_.method == "HEAD") {}

HttpExchange::HttpExchange(HttpRequest request, std::string requestBody)
    : connection_(nullptr), request_(std::move(request)), bodyLimit_(536870912),
      bufferedRequestBody_(std::move(requestBody)), buffered_(true),
      headRequest_(request_.method == "HEAD") {}

const HttpRequest &HttpExchange::request() const { return request_; }

int HttpExchange::readBody(void *buffer, size_t capacity) {
    if (!buffer || capacity == 0)
        return -1;
    if (bodyLimitExceeded_)
        return -2;
    if (request_.contentLength >= 0 &&
        static_cast<unsigned long long>(bodyBytesRead_) >=
            static_cast<unsigned long long>(request_.contentLength))
        return 0;
    if (buffered_) {
        if (bufferedRequestOffset_ >= bufferedRequestBody_.size())
            return 0;
        if (bodyBytesRead_ >= bodyLimit_) {
            bodyLimitExceeded_ = true;
            return -2;
        }
        const size_t count = std::min(
            std::min(capacity, bodyLimit_ - bodyBytesRead_),
            bufferedRequestBody_.size() - bufferedRequestOffset_);
        std::memcpy(buffer, bufferedRequestBody_.data() + bufferedRequestOffset_, count);
        bufferedRequestOffset_ += count;
        bodyBytesRead_ += count;
        return static_cast<int>(count);
    }
    if (bodyBytesRead_ >= bodyLimit_) {
        char extra;
        const int count = mg_read(connection_, &extra, 1);
        if (count > 0) {
            bodyLimitExceeded_ = true;
            return -2;
        }
        return count;
    }
    const size_t remaining = bodyLimit_ - bodyBytesRead_;
    const size_t requested = std::min(std::min(capacity, remaining),
                                      static_cast<size_t>(0x7fffffff));
    const int count = mg_read(connection_, buffer, requested);
    if (count > 0)
        bodyBytesRead_ += static_cast<size_t>(count);
    return count;
}

bool HttpExchange::setBodyLimit(size_t maximumBytes) {
    if (bodyBytesRead_ || maximumBytes > bodyLimit_)
        return false;
    bodyLimit_ = maximumBytes;
    if (request_.contentLength >= 0 &&
        static_cast<unsigned long long>(request_.contentLength) > maximumBytes)
        bodyLimitExceeded_ = true;
    return !bodyLimitExceeded_;
}

bool HttpExchange::bodyLimitExceeded() const { return bodyLimitExceeded_; }

bool HttpExchange::writeAll(const void *data, size_t size) {
    if (buffered_)
        return true;
    const char *bytes = static_cast<const char *>(data);
    size_t offset = 0;
    while (offset < size) {
        const int written = mg_write(connection_, bytes + offset, size - offset);
        if (written <= 0)
            return false;
        offset += static_cast<size_t>(written);
    }
    return true;
}

bool HttpExchange::beginResponse(int status, const std::vector<HttpHeader> &headers,
                                 long long contentLength) {
    if (responseStarted_ || status < 100 || status > 599 || contentLength < -1)
        return false;
    bool hasLength = false;
    bool hasTransferEncoding = false;
    bool hasConnection = false;
    const bool statusHasNoBody = (status >= 100 && status < 200) || status == 204 || status == 304;
    bodySuppressed_ = headRequest_ || statusHasNoBody;
    const std::string protocol = request_.version == "1.0" ? "HTTP/1.0 " : "HTTP/1.1 ";
    std::string response = protocol + std::to_string(status) + " " +
                           reasonPhrase(status) + "\r\n";
    for (const HttpHeader &header : headers) {
        if (!validHeaderName(header.name) || !validHeaderValue(header.value))
            return false;
        const std::string name = lower(header.name);
        hasLength = hasLength || name == "content-length";
        hasTransferEncoding = hasTransferEncoding || name == "transfer-encoding";
        hasConnection = hasConnection || name == "connection";
        response += header.name + ": " + header.value + "\r\n";
    }
    if (contentLength >= 0) {
        if (hasLength || hasTransferEncoding)
            return false;
        if (status != 204 && status != 304)
            response += "Content-Length: " + std::to_string(contentLength) + "\r\n";
    } else {
        if (hasTransferEncoding || (hasLength && !bodySuppressed_))
            return false;
        if (!bodySuppressed_) {
            response += "Transfer-Encoding: chunked\r\n";
            chunked_ = true;
        }
    }
    if (!hasConnection)
        response += request_.keepAlive() ? "Connection: keep-alive\r\n"
                                         : "Connection: close\r\n";
    response += "\r\n";
    if (!writeAll(response.data(), response.size()))
        return false;
    responseStarted_ = true;
    responseStatus_ = status;
    responseLength_ = bodySuppressed_ ? -1 : contentLength;
    return true;
}

bool HttpExchange::writeBody(const void *data, size_t size) {
    if (!responseStarted_ || !data || size == 0)
        return responseStarted_ && size == 0;
    if (bodySuppressed_)
        return true;
    if (responseLength_ >= 0 && bytesWritten_ + size > static_cast<unsigned long long>(responseLength_))
        return false;
    bytesWritten_ += size;
    if (headRequest_)
        return true;
    if (buffered_) {
        bufferedResponseBody_.append(static_cast<const char *>(data), size);
        return true;
    }
    if (chunked_) {
        char length[32];
        const int digits = std::snprintf(length, sizeof(length), "%zx\r\n", size);
        if (digits <= 0 || static_cast<size_t>(digits) >= sizeof(length) ||
            !writeAll(length, static_cast<size_t>(digits)) || !writeAll(data, size))
            return false;
        return writeAll("\r\n", 2);
    }
    return writeAll(data, size);
}

bool HttpExchange::finishResponse(const std::vector<HttpHeader> &trailers) {
    if (!responseStarted_)
        return false;
    if (!bodySuppressed_ && responseLength_ >= 0 &&
        bytesWritten_ != static_cast<unsigned long long>(responseLength_))
        return false;
    if (!trailers.empty() && (!chunked_ || headRequest_))
        return false;
    if (chunked_ && !headRequest_) {
        if (!writeAll("0\r\n", 3)) return false;
        for (const HttpHeader &header : trailers) {
            if (!validHeaderName(header.name) || !validHeaderValue(header.value))
                return false;
            const std::string line = header.name + ": " + header.value + "\r\n";
            if (!writeAll(line.data(), line.size())) return false;
        }
        if (!writeAll("\r\n", 2)) return false;
    }
    return true;
}

bool HttpExchange::respond(int status, const std::vector<HttpHeader> &headers,
                           const std::string &body) {
    // Every route answers through here, so the language of a dashboard in
    // another one is applied once, rather than at each of the routes.
    std::string translated;
    if (!responseLanguage_.empty()) {
        for (const HttpHeader &header : headers)
            if (lower(header.name) == "content-type" &&
                lower(header.value).compare(0, 16, "application/json") == 0) {
                translated = say::translateJson(body, responseLanguage_);
                break;
            }
    }
    const std::string &sent = translated.empty() ? body : translated;
    if (!beginResponse(status, headers, static_cast<long long>(sent.size())))
        return false;
    return writeBody(sent.data(), sent.size()) && finishResponse();
}

bool HttpExchange::responseStarted() const { return responseStarted_; }
int HttpExchange::responseStatus() const { return responseStatus_; }
const std::string &HttpExchange::responseBody() const { return bufferedResponseBody_; }

HttpServer::HttpServer() = default;
HttpServer::~HttpServer() { stop(); }

bool HttpServer::start(const HttpServerOptions &options, Handler handler, std::string &error) {
    if (context_) {
        error = "HTTP server is already running";
        return false;
    }
    if (!validOptions(options, error) || !handler)
        return false;

    handler_ = std::move(handler);
    maximumBodyBytes_ = options.maximumBodyBytes;
    bool fatalStartFailure = false;
    const auto startListener = [this, &error, &fatalStartFailure](const HttpServerOptions &binding) {
        std::vector<std::string> values;
        values.push_back("listening_ports");
        values.push_back(portBinding(binding));
        values.push_back("num_threads");
        values.push_back(std::to_string(binding.workerCount));
        values.push_back("connection_queue");
        values.push_back(std::to_string(binding.connectionQueue));
        values.push_back("listen_backlog");
        values.push_back(std::to_string(binding.listenBacklog));
        values.push_back("max_request_size");
        values.push_back(std::to_string(binding.maximumHeaderBytes));
        values.push_back("request_timeout_ms");
        values.push_back(std::to_string(binding.requestTimeoutMs));
        values.push_back("keep_alive_timeout_ms");
        values.push_back(std::to_string(binding.keepAliveTimeoutMs));
        values.push_back("enable_keep_alive");
        values.push_back("yes");
        values.push_back("enable_directory_listing");
        values.push_back("no");
        values.push_back("document_root");
        values.push_back(".");
        std::vector<const char *> cOptions;
        cOptions.reserve(values.size() + 1);
        for (const std::string &value : values)
            cOptions.push_back(value.c_str());
        cOptions.push_back(nullptr);

        struct sigaction previousChild;
        const bool hadPreviousChild = sigaction(SIGCHLD, nullptr, &previousChild) == 0;
        struct mg_callbacks callbacks;
        std::memset(&callbacks, 0, sizeof(callbacks));
        callbacks.begin_request = beginRequest;
        char startupError[256] = {};
        struct mg_error_data startupErrorData = {0, 0, startupError, sizeof(startupError)};
        struct mg_init_data initialization = {&callbacks, this, cOptions.data()};
        context_ = mg_start2(&initialization, &startupErrorData);
        if (hadPreviousChild && sigaction(SIGCHLD, &previousChild, nullptr) != 0) {
            fatalStartFailure = true;
            error = std::string("could not restore SIGCHLD after CivetWeb startup: ") +
                    std::strerror(errno);
            stop();
            return false;
        }
        if (!context_) {
            error = "CivetWeb could not start on " + values[1];
            if (startupError[0])
                error += ": " + std::string(startupError);
            return false;
        }
        struct mg_server_port ports[4];
        const int count = mg_get_server_ports(context_, 4, ports);
        if (count <= 0) {
            error = "CivetWeb started without a listening port on " + values[1];
            stop();
            return false;
        }
        for (int index = 0; index < count; ++index) {
            if (!ports[index].is_ssl) {
                boundPort_ = static_cast<unsigned short>(ports[index].port);
                break;
            }
        }
        if (!boundPort_) {
            error = "CivetWeb returned no HTTP listener on " + values[1];
            stop();
            return false;
        }
        return true;
    };

    if (startListener(options))
        return true;

    const std::string dualStackError = error;
    if (fatalStartFailure) {
        handler_ = Handler();
        return false;
    }
    if (options.host.empty() || options.host == "::") {
        HttpServerOptions fallback = options;
        fallback.host = "0.0.0.0";
        error.clear();
        if (startListener(fallback)) {
            ipv4Fallback_ = true;
            return true;
        }
        error = dualStackError + "; IPv4 fallback failed: " + error;
    }
    handler_ = Handler();
    return false;
}

void HttpServer::stop() {
    if (context_) {
        mg_context *context = context_;
        context_ = nullptr;
        mg_stop(context);
    }
    handler_ = Handler();
    boundPort_ = 0;
    ipv4Fallback_ = false;
}

bool HttpServer::running() const { return context_ != nullptr; }
unsigned short HttpServer::boundPort() const { return boundPort_; }
bool HttpServer::usingIpv4Fallback() const { return ipv4Fallback_; }
std::string HttpServer::libraryVersion() {
    const char *version = mg_version();
    return version ? version : "unknown";
}

int HttpServer::beginRequest(mg_connection *connection) {
    const mg_request_info *info = mg_get_request_info(connection);
    HttpServer *server = info ? static_cast<HttpServer *>(info->user_data) : nullptr;
    return server ? server->handleRequest(connection) : 500;
}

int HttpServer::handleRequest(mg_connection *connection) {
    HttpExchange exchange(connection, copyRequest(connection), maximumBodyBytes_);
    try {
        handler_(exchange);
    } catch (const std::exception &) {
        if (!exchange.responseStarted())
            (void)exchange.respond(500, {{"Content-Type", "text/plain; charset=utf-8"}},
                                   "Internal Server Error\n");
    } catch (...) {
        if (!exchange.responseStarted())
            (void)exchange.respond(500, {{"Content-Type", "text/plain; charset=utf-8"}},
                                   "Internal Server Error\n");
    }
    if (!exchange.responseStarted())
        (void)exchange.respond(500, {{"Content-Type", "text/plain; charset=utf-8"}},
                               "Internal Server Error\n");
    return exchange.responseStatus() ? exchange.responseStatus() : 500;
}
} // namespace daemon
} // namespace nativetv
