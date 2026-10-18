#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace rscpp::flight {

inline constexpr const char* REACT_SUSPENSE_TYPE = "$Sreact.suspense";

inline constexpr std::string_view LENGTH_PREFIXED_TAGS = "TOobUSsLlGgMmV";

inline bool is_length_prefixed_tag(char c) {
    return LENGTH_PREFIXED_TAGS.find(c) != std::string_view::npos;
}

inline bool is_newline_tag(char c) {
    if (c == 'A') return true;
    return (c >= 'B' && c <= 'Z') || c == '#' || c == 'r' || c == 'x';
}

inline char classify_tag(char c) {
    return (is_length_prefixed_tag(c) || is_newline_tag(c)) ? c : 0;
}

inline constexpr uint64_t ACTION_RESULT_ROW_ID = 0x9c40000000000000ULL;

inline std::string hex(uint64_t id) {
    static constexpr char digits[] = "0123456789abcdef";
    if (id == 0) return "0";
    std::string out;
    while (id) {
        out.push_back(digits[id & 0xf]);
        id >>= 4;
    }
    return std::string(out.rbegin(), out.rend());
}

inline bool parse_hex(std::string_view s, uint64_t& out) {
    // A uint64 is exactly 16 hex digits. `v = (v << 4) | d` wraps silently, so
    // a wider input would decode to a *different* string's value -- e.g.
    // "ffffffffffffffff" and "1ffffffffffffffff" both yield 0xffffffffffffffff.
    // Row ids index the reference table, so aliasing two distinct ids into one
    // is reference confusion. Restrict the domain to keep this injective.
    if (s.empty() || s.size() > 16) return false;
    uint64_t v = 0;
    for (char c : s) {
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else return false;
        v = (v << 4) | static_cast<uint64_t>(d);
    }
    out = v;
    return true;
}

}