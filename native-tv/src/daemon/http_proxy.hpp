#pragma once

#include "core/json.hpp"
#include "daemon/http_server.hpp"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace nativetv {
namespace daemon {
// With no Node (TVWEB_NODE_FREE=1): what its private routes would have answered,
// from the daemon itself. The daemon fills these in once it has its front door.
struct NodeFreeHooks {
    std::function<bool(bool checkOnly, int &status, std::string &body, std::string &error)> restart;
    std::function<bool(bool demo, Json &context, std::string &error)> setupContext;
    std::function<bool(int64_t fromMs, int64_t toMs, const Json &ids, Json &overlay,
                       std::string &error)> historyOverlay;
};

struct HttpProxyOptions {
    std::string host = "127.0.0.1";
    unsigned short port = 8081;
    std::string sharedSecret;
    unsigned int responseTimeoutMs = 30000;
    size_t maximumBodyBytes = 536870912;
};

// Streams one request to the loopback Node server and its response back to the
// caller. The secret and original peer headers are consumed by the temporary
// Node trust bridge; callers must authenticate requests before invoking this.
class HttpProxy {
  public:
    // `local` set: there is no Node. Forwarded requests are 404, the notifications it
    // took (state, config reload, Home escape) are accepted, and what it answered
    // comes from these hooks.
    explicit HttpProxy(HttpProxyOptions options, std::shared_ptr<NodeFreeHooks> local = nullptr);
    bool valid(std::string &error) const;
    bool noNode() const { return local_ != nullptr; }
    bool forward(HttpExchange &exchange) const;
    bool forwardBufferedBody(HttpExchange &exchange, const std::string &body) const;
    bool requestNodeConfigReload(std::string &error) const;
    bool requestNodeRestart(bool checkOnly, int &status, std::string &body,
                            std::string &error) const;
    bool requestNodeSetupContext(bool demo, Json &context, std::string &error) const;
    bool requestNodeSettingsContext(Json &context, std::string &error) const;
    bool requestNodeAppsContext(Json &context, std::string &error) const;
    bool requestNodeHomeEscape(bool escaped, std::string &error) const;
    bool requestNodeHomeForegroundFallback(const std::string &appId,
                                           std::string &error) const;
    bool requestNodeAppInstallStart(const Json &payload,
                                    int &status, Json &response,
                                    std::string &error) const;
    bool requestNodeAppInstallAction(const std::string &action, const Json &payload,
                                     bool nativeValidated,
                                     int &status, Json &response,
                                     std::string &error) const;
    bool requestNodeAppUploadStart(const std::string &file, int &status,
                                   Json &response, std::string &error) const;
    bool requestNodeStats(const Json &system, bool withSettings, std::string &body,
                          std::string &error, bool withPrometheusData = false) const;
    bool requestNodeHistoryOverlay(int64_t fromMs, int64_t toMs, const Json &ids,
                                   Json &overlay,
                                   std::string &error) const;
    bool requestNodeState(const Json &event, std::string &error,
                          unsigned int timeoutMs = 0) const;

  private:
    bool requestNodeAppInstallBridge(const std::string &path, const Json &payload,
                                     int &status, Json &response,
                                     std::string &error) const;
    bool forwardImpl(HttpExchange &exchange, const std::string *bufferedBody) const;
    HttpProxyOptions options_;
    std::shared_ptr<NodeFreeHooks> local_;
};
} // namespace daemon
} // namespace nativetv
