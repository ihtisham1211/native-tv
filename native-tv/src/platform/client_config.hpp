#pragma once

#include <string>

namespace nativetv {

std::string clientServerConfigPath();
std::string clientServerConfigPathForDataRoot(const std::string &dataRoot);
std::string clientServerOrigin(const std::string &configPath);
// The theme the client last drew in, kept in a file beside its config so the next start opens in it,
// before the server has said (caps answer after the first frame).
std::string clientThemePath(const std::string &configPath);
// The saved theme id, or "" when there is none or it is not a theme this build has.
std::string readClientTheme(const std::string &path);
// Saves a theme id; an id that is not a theme, or a folder that cannot be written, changes nothing.
void writeClientTheme(const std::string &path, const std::string &id);
bool writeClientServerPort(const std::string &configPath, unsigned port,
                           std::string *error = nullptr);

} // namespace nativetv
