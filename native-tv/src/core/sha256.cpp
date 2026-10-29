#include "core/sha256.hpp"

#include <algorithm>
#include <cstring>

namespace nativetv {
namespace crypto {
namespace {

const uint32_t kConstants[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
    0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
    0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

uint32_t rotateRight(uint32_t value, unsigned count) {
    return (value >> count) | (value << (32 - count));
}

} // namespace

Sha256::Sha256()
    : state_{{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
              0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19}},
      block_{{}}, totalBytes_(0), buffered_(0), finished_(false) {}

void Sha256::update(const void *data, size_t size) {
    if (finished_ || size == 0 || !data)
        return;
    const uint8_t *bytes = static_cast<const uint8_t *>(data);
    totalBytes_ += size;
    while (size > 0) {
        const size_t count = std::min(size, block_.size() - buffered_);
        std::memcpy(block_.data() + buffered_, bytes, count);
        buffered_ += count;
        bytes += count;
        size -= count;
        if (buffered_ == block_.size()) {
            transform(block_.data());
            buffered_ = 0;
        }
    }
}

void Sha256::transform(const uint8_t *block) {
    uint32_t schedule[64] = {};
    for (size_t index = 0; index < 16; ++index) {
        const size_t offset = index * 4;
        schedule[index] = (static_cast<uint32_t>(block[offset]) << 24) |
            (static_cast<uint32_t>(block[offset + 1]) << 16) |
            (static_cast<uint32_t>(block[offset + 2]) << 8) |
            static_cast<uint32_t>(block[offset + 3]);
    }
    for (size_t index = 16; index < 64; ++index) {
        const uint32_t s0 = rotateRight(schedule[index - 15], 7) ^
            rotateRight(schedule[index - 15], 18) ^ (schedule[index - 15] >> 3);
        const uint32_t s1 = rotateRight(schedule[index - 2], 17) ^
            rotateRight(schedule[index - 2], 19) ^ (schedule[index - 2] >> 10);
        schedule[index] = schedule[index - 16] + s0 + schedule[index - 7] + s1;
    }
    uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
    uint32_t e = state_[4], f = state_[5], g = state_[6], h = state_[7];
    for (size_t index = 0; index < 64; ++index) {
        const uint32_t sum1 = rotateRight(e, 6) ^ rotateRight(e, 11) ^ rotateRight(e, 25);
        const uint32_t choose = (e & f) ^ (~e & g);
        const uint32_t first = h + sum1 + choose + kConstants[index] + schedule[index];
        const uint32_t sum0 = rotateRight(a, 2) ^ rotateRight(a, 13) ^ rotateRight(a, 22);
        const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t second = sum0 + majority;
        h = g;
        g = f;
        f = e;
        e = d + first;
        d = c;
        c = b;
        b = a;
        a = first + second;
    }
    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
}

std::array<uint8_t, 32> Sha256::finish() {
    if (!finished_) {
        const uint64_t bitLength = totalBytes_ * 8;
        block_[buffered_++] = 0x80;
        if (buffered_ > 56) {
            std::fill(block_.begin() + buffered_, block_.end(), 0);
            transform(block_.data());
            buffered_ = 0;
        }
        std::fill(block_.begin() + buffered_, block_.begin() + 56, 0);
        for (size_t index = 0; index < 8; ++index)
            block_[56 + index] = static_cast<uint8_t>(bitLength >> (56 - index * 8));
        transform(block_.data());
        buffered_ = 0;
        finished_ = true;
    }
    std::array<uint8_t, 32> digest = {};
    for (size_t index = 0; index < state_.size(); ++index) {
        digest[index * 4] = static_cast<uint8_t>(state_[index] >> 24);
        digest[index * 4 + 1] = static_cast<uint8_t>(state_[index] >> 16);
        digest[index * 4 + 2] = static_cast<uint8_t>(state_[index] >> 8);
        digest[index * 4 + 3] = static_cast<uint8_t>(state_[index]);
    }
    return digest;
}

std::string hexDigest(const std::array<uint8_t, 32> &digest) {
    static const char digits[] = "0123456789abcdef";
    std::string text;
    text.reserve(digest.size() * 2);
    for (uint8_t value : digest) {
        text.push_back(digits[value >> 4]);
        text.push_back(digits[value & 0x0f]);
    }
    return text;
}

} // namespace crypto
} // namespace nativetv
