#include "service/kernel_files.hpp"
#include <fstream>

namespace nativetv {
namespace service {
bool readKernelFile(const std::string &path, std::string &text) {
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return false;
    text.clear();
    char buffer[4096];
    // procfs reports size zero. Bound reads independently of st_size.
    while (file.read(buffer, sizeof(buffer)) || file.gcount()) {
        text.append(buffer, static_cast<size_t>(file.gcount()));
        if (text.size() > 65536) {
            text.resize(65536);
            return false;
        }
    }
    return !file.bad();
}
bool parseUnsigned(const std::string &text, uint64_t &value, unsigned base) {
    if (text.empty() || (base != 10 && base != 16))
        return false;
    size_t start =
        base == 16 && text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X') ? 2
                                                                                              : 0;
    value = 0;
    for (size_t i = start; i < text.size(); ++i) {
        char c = text[i];
        unsigned digit = c >= '0' && c <= '9'   ? c - '0'
                         : c >= 'a' && c <= 'f' ? c - 'a' + 10
                         : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                                : 16;
        if (digit >= base || value > (UINT64_MAX - digit) / base)
            return false;
        value = value * base + digit;
    }
    return true;
}
} // namespace service
} // namespace nativetv
