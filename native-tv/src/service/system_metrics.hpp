#pragma once
#include "core/json.hpp"
#include <string>

namespace nativetv {
namespace service {
Json collectSystem(const std::string &root = "/",
                   const std::string &dataRoot = "/var/lib/tvweb");
Json hdmiPortFactFromStatus(const std::string &raw);
} // namespace service
} // namespace nativetv
