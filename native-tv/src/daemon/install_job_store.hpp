#pragma once

#include "core/json.hpp"
#include <string>

namespace nativetv {
namespace daemon {

bool readInstallerJob(const std::string &path, Json &job, bool &found,
                      std::string *error = nullptr);
bool recoverInstallerJob(const std::string &path, Json &job, bool &found,
                         std::string *error = nullptr);
bool recordInstallerJob(const std::string &path, const Json &job,
                        std::string *error = nullptr);
bool transitionInstallerJob(const std::string &path, const Json &job,
                            std::string *error = nullptr);
bool clearInstallerJob(const std::string &path, std::string *error = nullptr);

} // namespace daemon
} // namespace nativetv
