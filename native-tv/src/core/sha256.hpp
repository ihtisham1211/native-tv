#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace nativetv {
namespace crypto {

class Sha256 {
  public:
    Sha256();
    void update(const void *data, size_t size);
    std::array<uint8_t, 32> finish();

  private:
    void transform(const uint8_t *block);

    std::array<uint32_t, 8> state_;
    std::array<uint8_t, 64> block_;
    uint64_t totalBytes_;
    size_t buffered_;
    bool finished_;
};

std::string hexDigest(const std::array<uint8_t, 32> &digest);

} // namespace crypto
} // namespace nativetv
