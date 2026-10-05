#pragma once

#include "rscpp/model/registry.hpp"
#include "rscpp/model/suspend.hpp"
#include "rscpp/model/value.hpp"

#include <chrono>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <type_traits>
#include <unordered_map>

namespace rscpp {

struct RequestInfo {
    std::string method = "GET";
    std::string path = "/";
    std::map<std::string, std::string> query;
    std::map<std::string, std::string> params;
};

class Context {
public:
    Context() = default;

    void init(const RequestInfo& r, const Registry& reg) {
        std::unique_lock lock(mutex_);
        request_ = r;
        registry_ = &reg;
        data_.clear();
        cache_.clear();
        async_.clear();
}

    // Used by Router::resolve to publish the matched path and params.
    void set_request(RequestInfo r) {
        std::unique_lock lock(mutex_);
        request_ = std::move(r);
    }

    RequestInfo request() const {
        std::shared_lock lock(mutex_);
        return request_;
    }

    std::string query_value(const std::string& name) const {
        std::shared_lock lock(mutex_);
        auto it = request_.query.find(name);
        return it != request_.query.end() ? it->second : std::string();
    }

    std::string param(const std::string& name) const {
        std::shared_lock lock(mutex_);
        auto it = request_.params.find(name);
        return it != request_.params.end() ? it->second : std::string();
    }

    const Registry& registry() const {
        static Registry empty;
        std::shared_lock lock(mutex_);
        return registry_ ? *registry_ : empty;
    }

    void provide(const std::string& key, Value v) {
        std::unique_lock lock(mutex_);
        data_[key] = std::move(v);
    }

    std::optional<Value> consume(const std::string& key) const {
        std::shared_lock lock(mutex_);
        auto it = data_.find(key);
        if (it == data_.end()) return std::nullopt;
        return it->second;
    }

    // Typed accessor for a provided value. Returns nullopt unless the stored Value
    // already has type T, so it never coerces.
    template <typename T>
    std::optional<T> get(const std::string& key) const {
        auto v = consume(key);
        if (!v) return std::nullopt;
        if constexpr (std::is_same_v<T, std::string>) {
            if (v->is_string()) return v->as_string();
        } else if constexpr (std::is_same_v<T, int64_t>) {
            if (v->is_int64()) return v->as_int64();
        } else if constexpr (std::is_same_v<T, bool>) {
            if (v->is_bool()) return v->as_bool();
        }
        return std::nullopt;
    }

    Value cached(const std::string& key, const std::function<Value()>& compute) {
        {
            std::shared_lock lock(mutex_);
            auto it = cache_.find(key);
            if (it != cache_.end()) return it->second;
        }
        Value v = compute();
        std::unique_lock lock(mutex_);
        cache_[key] = v;
        return v;
    }

    std::shared_ptr<std::shared_future<Value>> async_once(
        const std::string& key,
        const std::function<std::shared_ptr<std::shared_future<Value>>()>& factory) {
        {
            std::shared_lock lock(mutex_);
            auto it = async_.find(key);
            if (it != async_.end()) return it->second;
        }
        auto f = factory();
        std::unique_lock lock(mutex_);
        return async_.emplace(key, std::move(f)).first->second;
    }

    Value await(std::shared_ptr<std::shared_future<Value>> future, Value fallback = {}) {
        if (!future) return fallback;
        if (future->wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            return future->get();
        }
        throw Suspend{std::move(future), std::move(fallback)};
    }

private:
    mutable std::shared_mutex mutex_;
    const Registry* registry_ = nullptr;
    RequestInfo request_;
    std::unordered_map<std::string, Value> data_;
    std::unordered_map<std::string, Value> cache_;
    std::unordered_map<std::string, std::shared_ptr<std::shared_future<Value>>> async_;
};

}