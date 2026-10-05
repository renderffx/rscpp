#pragma once

#include "json.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace rscpp {

struct ServerMetrics {
    std::atomic<uint64_t> requests_total{0};
    std::atomic<uint64_t> requests_active{0};
    std::atomic<uint64_t> requests_failed{0};
    std::atomic<uint64_t> flight_streams{0};
    std::atomic<uint64_t> html_responses{0};
    std::atomic<uint64_t> actions_executed{0};
    std::atomic<uint64_t> actions_failed{0};
    std::atomic<uint64_t> rate_limited{0};
    std::chrono::steady_clock::time_point start_time = std::chrono::steady_clock::now();

    void record_request(bool success) {
        requests_total++;
        if (!success) requests_failed++;
    }
    void record_action(bool success) {
        actions_executed++;
        if (!success) actions_failed++;
    }
    void record_flight() { flight_streams++; }
    void record_html() { html_responses++; }

    nlohmann::json to_json() const;

    // /_health reports the same counters without the scrape-oriented shape.
    nlohmann::json health_json(bool draining) const;
};

class RateLimiter {
public:
    RateLimiter(int capacity = 100, int refill_rate = 20)
        : capacity_(capacity > 0 ? capacity : 1), refill_rate_(refill_rate > 0 ? refill_rate : 1) {}

    bool allow(const std::string& key) {
        const auto now = std::chrono::steady_clock::now();

        std::shared_ptr<Bucket> bucket;
        {
            std::lock_guard lock(mutex_);
            auto& slot = buckets_[key];
            if (!slot) {
                slot = std::make_shared<Bucket>();
                slot->tokens = capacity_;
                slot->last_refill = now;
            }
            bucket = slot;
        }

        std::lock_guard lock(bucket->mutex);
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 now - bucket->last_refill)
                                 .count();
        if (elapsed > 0) {
            const long long refilled = static_cast<long long>(capacity_);
            const long long earned = elapsed * refill_rate_ / 1000;
            bucket->tokens = static_cast<int>(std::min<long long>(refilled, bucket->tokens + earned));
            bucket->last_refill = now;
        }
        if (bucket->tokens < 1) return false;
        --bucket->tokens;
        return true;
    }

    void cleanup_idle(std::chrono::minutes idle = std::chrono::minutes(10)) {
        const auto now = std::chrono::steady_clock::now();
        std::lock_guard lock(mutex_);
        for (auto it = buckets_.begin(); it != buckets_.end();) {
            const auto idle_for =
                std::chrono::duration_cast<std::chrono::minutes>(now - it->second->last_refill)
                    .count();
            it = idle_for > idle.count() ? buckets_.erase(it) : std::next(it);
        }
    }

    size_t size() const {
        std::lock_guard lock(mutex_);
        return buckets_.size();
    }

private:
    struct Bucket {
        int tokens = 0;
        std::chrono::steady_clock::time_point last_refill;
        std::mutex mutex;
    };

    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<Bucket>> buckets_;
    int capacity_;
    int refill_rate_;
};

}