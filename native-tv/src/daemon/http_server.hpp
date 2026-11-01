#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

struct mg_connection;
struct mg_context;

namespace nativetv {
namespace daemon {
struct HttpHeader {
    std::string name;
    std::string value;
};

struct HttpRequest {
    std::string method;
    std::string uri;
    std::string query;
    std::string version;
    std::string remoteAddress;
    long long contentLength = -1;
    std::vector<HttpHeader> headers;

    std::string header(const std::string &name) const;
    bool keepAlive() const;
};

class HttpExchange {
  public:
    // Buffered exchanges let trusted in-process callers reuse route validation
    // and command policy without opening a second HTTP connection.
    HttpExchange(HttpRequest request, std::string requestBody);
    const HttpRequest &request() const;
    int readBody(void *buffer, size_t capacity);
    bool setBodyLimit(size_t maximumBytes);
    bool bodyLimitExceeded() const;
    bool beginResponse(int status, const std::vector<HttpHeader> &headers,
                       long long contentLength);
    bool writeBody(const void *data, size_t size);
    bool finishResponse(const std::vector<HttpHeader> &trailers = {});
    bool respond(int status, const std::vector<HttpHeader> &headers,
                 const std::string &body);
    bool responseStarted() const;
    int responseStatus() const;
    const std::string &responseBody() const;
    // Made by this process for itself, never read from a socket.
    bool inProcess() const { return buffered_; }
    // Answer JSON bodies sent with respond() in this language, where there is a
    // dictionary for it (say::translateJson); "" is English.
    void setResponseLanguage(std::string language) { responseLanguage_ = std::move(language); }

  private:
    friend class HttpServer;
    HttpExchange(mg_connection *connection, HttpRequest request, size_t maximumBodyBytes);
    bool writeAll(const void *data, size_t size);

    mg_connection *connection_;
    HttpRequest request_;
    long long responseLength_ = -1;
    unsigned long long bytesWritten_ = 0;
    size_t bodyLimit_;
    size_t bodyBytesRead_ = 0;
    std::string bufferedRequestBody_;
    size_t bufferedRequestOffset_ = 0;
    std::string bufferedResponseBody_;
    std::string responseLanguage_;
    int responseStatus_ = 0;
    bool responseStarted_ = false;
    bool buffered_ = false;
    bool chunked_ = false;
    bool headRequest_ = false;
    bool bodySuppressed_ = false;
    bool bodyLimitExceeded_ = false;
};

struct HttpServerOptions {
    std::string host = "::";
    unsigned short port = 8080;
    size_t workerCount = 32;
    size_t connectionQueue = 32;
    size_t listenBacklog = 64;
    size_t maximumHeaderBytes = 32768;
    size_t maximumBodyBytes = 536870912;
    unsigned int requestTimeoutMs = 15000;
    unsigned int keepAliveTimeoutMs = 2000;
};

class HttpServer {
  public:
    using Handler = std::function<void(HttpExchange &)>;

    HttpServer();
    ~HttpServer();
    HttpServer(const HttpServer &) = delete;
    HttpServer &operator=(const HttpServer &) = delete;

    bool start(const HttpServerOptions &options, Handler handler, std::string &error);
    void stop();
    bool running() const;
    unsigned short boundPort() const;
    bool usingIpv4Fallback() const;
    static std::string libraryVersion();

  private:
    static int beginRequest(mg_connection *connection);
    int handleRequest(mg_connection *connection);

    mg_context *context_ = nullptr;
    Handler handler_;
    unsigned short boundPort_ = 0;
    bool ipv4Fallback_ = false;
    size_t maximumBodyBytes_ = 536870912;
};
} // namespace daemon
} // namespace nativetv
