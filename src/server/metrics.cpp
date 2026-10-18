#include "metrics.hpp"

namespace rscpp {

nlohmann::json ServerMetrics::to_json() const {
    const auto uptime =
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() -
                                                         start_time)
            .count();
    return {{"uptime_seconds", uptime},
            {"requests_total", requests_total.load()},
            {"requests_active", requests_active.load()},
            {"requests_failed", requests_failed.load()},
            {"flight_streams", flight_streams.load()},
            {"html_responses", html_responses.load()},
            {"actions_executed", actions_executed.load()},
            {"actions_failed", actions_failed.load()},
            {"rate_limited", rate_limited.load()}};
}

nlohmann::json ServerMetrics::health_json(bool draining) const {
    const auto uptime =
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() -
                                                         start_time)
            .count();
    return {{"status", draining ? "draining" : "ok"},
            {"uptime_seconds", uptime},
            {"requests_total", requests_total.load()},
            {"requests_failed", requests_failed.load()},
            {"flight_streams", flight_streams.load()},
            {"html_responses", html_responses.load()},
            {"actions_executed", actions_executed.load()},
            {"actions_failed", actions_failed.load()},
            {"rate_limited", rate_limited.load()}};
}

}