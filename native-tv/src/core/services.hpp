#pragma once
#include "core/json.hpp"
#include "core/operations.hpp"
#include <string>

namespace nativetv {
struct Response {
    std::string channel, path, bytes, error;
    unsigned generation = 0;
    long status = 0;
    bool ok() const {
        return error.empty() && status >= 200 && status < 300;
    }
    Json json() const {
        return Json::parse(bytes);
    }
};
class Services {
  public:
    virtual ~Services() = default;
    virtual unsigned read(const std::string &channel, Operation operation,
                          const Json &arguments = Json()) = 0;
    virtual unsigned write(const std::string &channel, Operation operation, const Json &body) = 0;
    virtual void cancel(const std::string &channel) = 0;
    // The admin PIN's session (the reply of POST /api/admin/unlock or of setup's adminPin step):
    // sent as X-Admin-Session on every API request from now on. Empty forgets it. A stand-in
    // that does not speak HTTP has nothing to do.
    virtual void adminSession(const std::string &) {}
};
} // namespace nativetv
