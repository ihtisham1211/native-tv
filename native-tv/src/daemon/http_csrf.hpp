#pragma once

#include "daemon/http_server.hpp"

namespace nativetv {
namespace daemon {
// Returns true when a guarded request is rejected and answered. Requests that
// pass the CSRF contract, or routes Node still owns without this guard, return
// false for the caller to continue routing.
bool rejectCrossSitePost(HttpExchange &exchange);
} // namespace daemon
} // namespace nativetv
