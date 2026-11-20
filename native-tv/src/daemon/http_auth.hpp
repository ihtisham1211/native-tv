#pragma once

#include "daemon/http_server.hpp"
#include <string>

namespace nativetv {
namespace daemon {
bool isLoopbackPeer(const std::string &address);
// The Host header's name, lower case, without port, brackets or a trailing dot; empty when it
// is malformed.
std::string normalizedHostName(const std::string &header);
// localhost, 127.0.0.0/8 or ::1, with or without a port.
bool loopbackHostName(const std::string &hostHeader);
// A request from the TV itself: a loopback peer that also named a loopback host. A page in the
// TV's browser that reached the server through a DNS-rebinding name connects from loopback too,
// but its Host still carries that name, so it is trusted no more than any network client.
bool loopbackRequest(const std::string &remoteAddress, const std::string &hostHeader);
bool loopbackRequest(const HttpRequest &request);
bool tokenMatchesConstantTime(const std::string &given, const std::string &expected);
bool requestHasValidToken(const HttpRequest &request, const std::string &expected);
bool requestIsAuthorized(const HttpRequest &request, const std::string &token);
} // namespace daemon
} // namespace nativetv
