#pragma once

#include <string>

namespace nativetv {

std::string clientServerConfigPath();
std::string clientServerConfigPathForDataRoot(const std::string &dataRoot);
std::string clientServerOrigin(const std::string &configPath);
bool writeClientServerPort(const std::string &configPath, unsigned port,
                           std::string *error = nullptr);

} // namespace nativetv
