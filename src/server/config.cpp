#include "config.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <stdexcept>

namespace rscpp {
namespace {

std::string trim(std::string s) {
    const auto not_space = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
    return s;
}

}  // namespace

ServerConfig ServerConfig::from_ini(const std::string& path, std::string& error) {
    ServerConfig cfg;
    std::ifstream in(path);
    if (!in) {
        error = "no config file at '" + path + "'; using defaults";
        return cfg;
    }
    std::string line;
    int line_no = 0;
    while (std::getline(in, line)) {
        ++line_no;
        const size_t hash = line.find_first_of("#;");
        if (hash != std::string::npos) line.erase(hash);
        line = trim(line);
        if (line.empty()) continue;

        const size_t eq = line.find('=');
        if (eq == std::string::npos) {
            error = path + ":" + std::to_string(line_no) + ": expected 'key = value'";
            continue;
        }
        const std::string key = trim(line.substr(0, eq));
        const std::string val = trim(line.substr(eq + 1));

        auto as_int = [&](int& target) {
            try {
                size_t used = 0;
                const long v = std::stol(val, &used);
                if (used != val.size()) throw std::invalid_argument("trailing");
                target = static_cast<int>(v);
            } catch (const std::exception&) {
                error = path + ":" + std::to_string(line_no) + ": '" + key + "' is not an integer";
            }
        };

        if (key == "host") {
            cfg.host = val;
        } else if (key == "port") {
            int p = cfg.port;
            as_int(p);
            if (p < 1 || p > 65535) {
                error = path + ":" + std::to_string(line_no) + ": port out of range 1-65535";
            } else {
                cfg.port = static_cast<uint16_t>(p);
            }
        } else if (key == "static_dir") {
            cfg.static_dir = val;
        } else if (key == "rate_limit_capacity") {
            as_int(cfg.rate_limit_capacity);
        } else if (key == "rate_limit_refill") {
            as_int(cfg.rate_limit_refill);
        } else if (key == "max_request_size") {
            int v = 0;
            as_int(v);
            if (v > 0) cfg.max_request_size = static_cast<size_t>(v);
        } else if (key == "log_level") {
            if (!log_level_from_string(val, cfg.log_level))
                error = path + ":" + std::to_string(line_no) + ": unknown log_level '" + val + "'";
        } else if (key == "enable_csp") {
            cfg.enable_csp = (val == "1" || val == "true" || val == "yes");
        } else if (key == "enable_hsts") {
            cfg.enable_hsts = (val == "1" || val == "true" || val == "yes");
        }
    }
    return cfg;
}

}