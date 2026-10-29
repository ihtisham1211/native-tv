#include "core/sha256.hpp"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

using nativetv::crypto::Sha256;
using nativetv::crypto::hexDigest;

namespace {
std::string digest(const std::string &value) {
    Sha256 hash;
    hash.update(value.data(), value.size());
    return hexDigest(hash.finish());
}
} // namespace

int main() {
    assert(digest("") == "e3b0c44298fc1c149afbf4c8996fb924"
                       "27ae41e4649b934ca495991b7852b855");
    assert(digest("abc") == "ba7816bf8f01cfea414140de5dae2223"
                           "b00361a396177a9cb410ff61f20015ad");

    Sha256 split;
    split.update("a", 1);
    split.update(nullptr, 0);
    split.update("b", 1);
    split.update("c", 1);
    assert(hexDigest(split.finish()) == digest("abc"));
    assert(hexDigest(split.finish()) == digest("abc"));

    const std::string thousandA(1000, 'a');
    Sha256 million;
    for (size_t index = 0; index < 1000; ++index)
        million.update(thousandA.data(), thousandA.size());
    assert(hexDigest(million.finish()) ==
           "cdc76e5c9914fb9281a1c7e284d73e67"
           "f1809a48a497200e046d39ccc7112cd0");

    std::vector<uint8_t> boundary(65, 'x');
    Sha256 chunked;
    chunked.update(boundary.data(), 7);
    chunked.update(boundary.data() + 7, 57);
    chunked.update(boundary.data() + 64, 1);
    std::string whole(boundary.begin(), boundary.end());
    assert(hexDigest(chunked.finish()) == digest(whole));

    std::cout << "native SHA-256 assertions passed\n";
    return 0;
}
