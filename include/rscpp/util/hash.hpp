#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace rscpp::util {

inline constexpr uint64_t FNV_OFFSET_BASIS = 1469598103934665603ULL;
inline constexpr uint64_t FNV_PRIME = 1099511628211ULL;

inline uint64_t fnv1a(std::string_view s) {
    uint64_t h = FNV_OFFSET_BASIS;
    for (unsigned char c : s) {
        h ^= c;
        h *= FNV_PRIME;
    }
    return h;
}

inline std::string hex_digest(std::string_view s) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out(16, '0');
    uint64_t h = fnv1a(s);
    for (size_t i = out.size(); i-- > 0;) {
        out[i] = kHex[h & 0xf];
        h >>= 4;
    }
    return out;
}

}