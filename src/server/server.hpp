#pragma once

#include "httplib.h"

#include "rscpp/rsc.hpp"

#include "config.hpp"
#include "logging.hpp"
#include "metrics.hpp"
#include "stream.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <regex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace rscpp {

struct RequestContext {
    std::string request_id;
    std::chrono::steady_clock::time_point start_time;
};

using Middleware = std::function<bool(const httplib::Request&, httplib::Response&, RequestContext&)>;

class MiddlewarePipeline {
public:
    void add(Middleware m) { middlewares_.push_back(std::move(m)); }

    bool execute(const httplib::Request& req, httplib::Response& res, RequestContext& ctx) const {
        for (const auto& mw : middlewares_) {
            if (!mw(req, res, ctx)) return false;
        }
        return true;
    }

private:
    std::vector<Middleware> middlewares_;
};

class RSCServer {
public:
    explicit RSCServer(const ServerConfig& config = {});
    ~RSCServer();

    RSCServer(const RSCServer&) = delete;
    RSCServer& operator=(const RSCServer&) = delete;

    void add_route(const std::string& pattern,
                   std::function<NodePtr(const std::smatch&, Context&)> handler) {
        router_.add_route(pattern, std::move(handler));
    }

    void register_action(const std::string& id, std::function<Value(const Value&)> handler) {
        registry_.register_action(ServerAction{id, id, std::move(handler), false});
    }

    Registry& get_registry() { return registry_; }

    void run();
    void stop();
    bool is_shutting_down() const { return shutting_down_.load(); }

private:
    void setup_routes();
    void setup_middleware();
    void apply_security_headers(httplib::Response& res) const;

    void handle_health(httplib::Response& res) const;
    void handle_metrics(httplib::Response& res) const;
    void handle_manifest(httplib::Response& res) const;
    void handle_flight_response(StreamResponse& resp, const RequestInfo& req_info, bool found);
    void handle_html_response(StreamResponse& resp, const RequestInfo& req_info, bool found,
                              const std::string& nonce);
    void handle_prerender_response(StreamResponse& resp, const RequestInfo& req_info,
                                   bool found, const std::string& nonce);
    void handle_resume_response(StreamResponse& resp, const std::string& token);
    void handle_action_response(StreamResponse& resp, const RequestInfo& req_info,
                                const std::string& action_id, const Value& args,
                                const std::string& path, const std::string& error);
    std::string build_shell_head(const std::string& title, const std::string& flight_wire,
                                 const std::string& nonce,
                                 const std::string& postponed_token = std::string()) const;
    std::string build_shell_tail(const std::string& nonce) const;

    std::string store_postponed(PostponedState state);
    std::optional<PostponedState> take_postponed(const std::string& token);
    void sweep_postponed();

    void setup_signal_handlers();
    static void signal_handler(int sig);

    struct PostponedEntry {
        PostponedState state;
        std::chrono::steady_clock::time_point expires;
    };

    ServerConfig config_;
    Registry registry_;
    Router router_;
    httplib::Server server_;
    MiddlewarePipeline middleware_;
    ServerMetrics metrics_;
    RateLimiter rate_limiter_;
    std::atomic<bool> shutting_down_{false};
    std::thread janitor_;

    // In-process, exactly like React's `postponed` object: it references live
    // nodes and context, so it can only be resumed by this process.
    mutable std::mutex postponed_mutex_;
    std::unordered_map<std::string, PostponedEntry> postponed_;

    static std::atomic<RSCServer*> instance_;
};

}