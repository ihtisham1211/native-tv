#pragma once

#include "daemon/http_server.hpp"
#include <memory>
#include <mutex>
#include <string>

namespace nativetv {
namespace daemon {
class HttpStaticAssets {
  public:
    explicit HttpStaticAssets(std::string root, std::string version = "1");
    bool assemblePage(const std::string &relative, std::string &page,
                      std::string &missing) const;
    // Returns true when the request was answered by the native static handler.
    // A false result leaves the route for the transitional Node proxy.
    bool handle(HttpExchange &exchange) const;

  private:
    bool loadUi(std::shared_ptr<const std::string> &page,
                std::shared_ptr<const std::string> &gzipPage,
                std::string &missing) const;
    std::string root_;
    std::string version_;
    mutable std::mutex uiMutex_;
    mutable bool uiLoaded_ = false;
    mutable bool uiAvailable_ = false;
    mutable std::shared_ptr<const std::string> uiPage_;
    mutable std::shared_ptr<const std::string> uiGzipPage_;
    mutable std::string uiMissing_;
};
} // namespace daemon
} // namespace nativetv
