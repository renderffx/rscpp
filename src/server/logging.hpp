#pragma once

#include <chrono>
#include <cstdint>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <random>
#include <string>

namespace rscpp {

enum class LogLevel { DEBUG, INFO, WARN, SEVERE };

bool log_level_from_string(const std::string& s, LogLevel& out);

class Logger {
public:
    static Logger& instance() {
        static Logger logger;
        return logger;
    }

    void set_level(LogLevel level) { level_ = level; }

    template <typename... Args>
    void log(LogLevel lvl, const char* file, int line, Args&&... args) {
        if (lvl < level_) return;
        const auto now = std::chrono::system_clock::now();
        const auto ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
        const std::time_t time = std::chrono::system_clock::to_time_t(now);
        std::tm tm{};
#ifdef _WIN32
        localtime_s(&tm, &time);
#else
        localtime_r(&time, &tm);
#endif
        std::lock_guard lock(mutex_);
        std::cerr << std::put_time(&tm, "%Y-%m-%d %H:%M:%S") << '.' << std::setfill('0')
                  << std::setw(3) << ms.count() << " [" << level_str(lvl) << "] " << file << ':'
                  << line << ' ';
        ((std::cerr << args), ...);
        std::cerr << '\n';
    }

private:
    static const char* level_str(LogLevel lvl) {
        switch (lvl) {
            case LogLevel::DEBUG: return "DEBUG";
            case LogLevel::INFO: return "INFO ";
            case LogLevel::WARN: return "WARN ";
            case LogLevel::SEVERE: return "ERROR";
        }
        return "UNKWN";
    }

    LogLevel level_ = LogLevel::INFO;
    std::mutex mutex_;
};

}  // namespace rscpp

#define RSC_LOG(lvl, ...) \
    ::rscpp::Logger::instance().log(::rscpp::LogLevel::lvl, __FILE__, __LINE__, __VA_ARGS__)
#define RSC_DEBUG(...) RSC_LOG(DEBUG, __VA_ARGS__)
#define RSC_INFO(...) RSC_LOG(INFO, __VA_ARGS__)
#define RSC_WARN(...) RSC_LOG(WARN, __VA_ARGS__)
#define RSC_ERROR(...) RSC_LOG(SEVERE, __VA_ARGS__)

namespace rscpp {

extern thread_local std::string g_request_id;
extern thread_local std::string g_csp_nonce;

inline std::string generate_request_id() {
    thread_local std::mt19937_64 gen(std::random_device{}());
    static constexpr char kHex[] = "0123456789abcdef";
    std::string id(32, '0');
    for (char& c : id) c = kHex[gen() & 0xf];
    return id;
}

inline std::string make_nonce() {
    thread_local std::mt19937_64 gen(std::random_device{}());
    static constexpr char kHex[] = "0123456789abcdef";
    std::string nonce(32, '0');
    for (char& c : nonce) c = kHex[gen() & 0xf];
    return nonce;
}

inline std::string current_nonce() { return g_csp_nonce; }

}  // namespace rscpp