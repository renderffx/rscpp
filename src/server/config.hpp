#pragma once

#include "logging.hpp"

#include <cstdint>
#include <string>

namespace rscpp {

struct ServerConfig {
    std::string host = "0.0.0.0";
    uint16_t port = 3000;
    std::string static_dir = "web";

    int rate_limit_capacity = 200;
    int rate_limit_refill = 100;
    size_t max_request_size = 1 << 20;
    LogLevel log_level = LogLevel::INFO;

    bool enable_csp = true;
    bool enable_hsts = true;

    // Backpressure bounds for the chunked producer/consumer hand-off. These are
    // constants rather than ini keys: they are load-bearing for stream lifetime
    // (a stalled producer parks forever), so they are deliberately not tunable.
    size_t stream_queue_limit = 256;
    int stream_stall_timeout_ms = 10000;

    // Injected into `script-src` per response alongside the nonce.
    std::string csp_policy =
        "default-src 'self'; script-src 'self'; style-src 'self'; img-src 'self' data:; "
        "font-src 'self'; connect-src 'self'; object-src 'none'; base-uri 'self'; "
        "form-action 'self'; frame-ancestors 'none'";

    static ServerConfig from_ini(const std::string& path, std::string& error);
};

}