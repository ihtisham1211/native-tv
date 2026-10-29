#pragma once

#include "daemon/http_server.hpp"
#include <string>

namespace nativetv {
namespace daemon {
bool isLoopbackPeer(const std::string &address);
bool tokenMatchesConstantTime(const std::string &given, const std::string &expected);
bool requestHasValidToken(const HttpRequest &request, const std::string &expected);
bool requestIsAuthorized(const HttpRequest &request, const std::string &token);
} // namespace daemon
} // namespace nativetv
