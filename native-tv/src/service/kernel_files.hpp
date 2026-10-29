#pragma once
#include <cstdint>
#include <string>

namespace nativetv {
namespace service {
bool readKernelFile(const std::string &path, std::string &text);
bool parseUnsigned(const std::string &text, uint64_t &value, unsigned base = 10);
} // namespace service
} // namespace nativetv
