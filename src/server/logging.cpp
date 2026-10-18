#include "logging.hpp"

#include <cctype>

namespace rscpp {

thread_local std::string g_request_id;
thread_local std::string g_csp_nonce;

bool log_level_from_string(const std::string& s, LogLevel& out) {
    std::string v;
    for (char c : s) v.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    if (v == "debug") { out = LogLevel::DEBUG; return true; }
    if (v == "info") { out = LogLevel::INFO; return true; }
    if (v == "warn" || v == "warning") { out = LogLevel::WARN; return true; }
    if (v == "error") { out = LogLevel::SEVERE; return true; }
    return false;
}

}