#pragma once

#include "rscpp/rsc.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

namespace demo {

// Process-wide demo state. The dashboard is a single-node example, so globals
// are simpler than plumbing a store through Context.
inline int refresh_count() {
    static std::atomic<int> count{0};
    return count.load();
}

inline int bump_refresh_count(int delta) {
    static std::atomic<int> count{0};
    return count.fetch_add(delta) + delta;
}

namespace {

inline std::mutex& like_mutex() {
    static std::mutex m;
    return m;
}

inline std::unordered_map<std::string, int>& like_table() {
    static std::unordered_map<std::string, int> t;
    return t;
}

}  // namespace

inline void set_like(const std::string& id, bool liked) {
    std::lock_guard lock(like_mutex());
    if (liked) {
        ++like_table()[id];
    } else if (auto it = like_table().find(id); it != like_table().end() && it->second > 0) {
        --it->second;
    }
}

inline int like_count(const std::string& id) {
    std::lock_guard lock(like_mutex());
    auto it = like_table().find(id);
    return it != like_table().end() ? it->second : 0;
}

// Server actions backing the counter page and the like button.
inline rscpp::Value increment_counter(const rscpp::Value&) {
    rscpp::Object result;
    result["count"] = rscpp::Value(static_cast<int64_t>(bump_refresh_count(1)));
    result["ok"] = rscpp::Value(true);
    return rscpp::Value(std::move(result));
}

inline rscpp::Value decrement_counter(const rscpp::Value&) {
    rscpp::Object result;
    result["count"] = rscpp::Value(static_cast<int64_t>(bump_refresh_count(-1)));
    result["ok"] = rscpp::Value(true);
    return rscpp::Value(std::move(result));
}

}  // namespace demo