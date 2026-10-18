#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX  // windows.h min/max macros break std::min/std::max
#endif
#include <windows.h>
#endif

#include "examples/dashboard/demo.hpp"
#include "server.hpp"

namespace {

using rscpp::Context;
using rscpp::NodePtr;
using rscpp::Value;

// The server must work no matter where it is launched from: a terminal at the
// repository root, or a double click on build/Debug/rscpp_server.exe whose
// working directory is the build folder. Assets and the ini are searched in
// the working directory first, then next to the executable, then the two
// parent levels that cover the standard build/Debug and build/<cfg> layouts.
std::filesystem::path executable_dir(const char* argv0) {
#ifdef _WIN32
    char buf[MAX_PATH];
    const DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        return std::filesystem::path(std::string(buf, n)).parent_path();
    }
#endif
    std::error_code ec;
    const auto p = std::filesystem::absolute(argv0, ec);
    if (ec) return std::filesystem::current_path();
    return p.parent_path();
}

std::filesystem::path find_upwards(const std::filesystem::path& exe_dir,
                                   const std::string& relative) {
    std::error_code ec;
    if (std::filesystem::exists(relative, ec)) return relative;
    const std::vector<std::filesystem::path> bases = {
        exe_dir, exe_dir / "..", exe_dir / ".." / ".."};
    for (const auto& base : bases) {
        const auto candidate = base / relative;
        if (std::filesystem::exists(candidate, ec)) return candidate;
    }
    return relative;
}

using PageHandler = NodePtr (*)(const std::smatch&, Context&);

struct RouteEntry {
    const char* pattern;
    PageHandler handler;
};

constexpr RouteEntry kRoutes[] = {
    {"/", demo::DashboardPage},
    {"/about", demo::AboutPage},
    {"/stress", demo::StressPage},
    {"/posts", demo::PostsPage},
    {"/post/:post_id", demo::PostWithAuthor},
    {"/products", demo::ProductsIndexPage},
    {"/products/:id", demo::ProductPage},
    {"/product", demo::ProductsIndexPage},
    {"/product/:id", demo::ProductPage},
    {"/search", demo::SearchPage},
    {"/error", demo::ErrorPage},
    {"/hardcore", demo::HardcorePage},
    {"/suspense", demo::SuspensePage},
    {"/userlist", demo::UserListPage},
    {"/userlist/:data", demo::UserListPageWithData},
    {"/heavy", demo::HeavyDataPage},
    {"/counter", demo::CounterPage},
};

void register_routes(rscpp::RSCServer& server) {
    for (const auto& [pattern, handler] : kRoutes) {
        server.add_route(pattern, [handler](const std::smatch& m, Context& c) {
            return handler(m, c);
        });
    }
}

void register_actions(rscpp::RSCServer& server) {
    server.register_action("increment", [](const Value&) { return demo::increment_counter({}); });
    server.register_action("decrement", [](const Value&) { return demo::decrement_counter({}); });

    server.register_action("like:toggle", [](const Value& p) -> Value {
        std::string id;
        bool liked = false;
        if (auto v = p.find("id"); v && v->is_string()) id = v->as_string();
        if (auto v = p.find("liked"); v && v->is_bool()) liked = v->as_bool();
        if (!id.empty()) demo::set_like(id, liked);
        rscpp::Object o;
        o["ok"] = rscpp::Value(true);
        o["count"] = rscpp::Value(static_cast<int64_t>(demo::like_count(id)));
        return rscpp::Value(std::move(o));
    });

    server.register_action("dash:refresh", [](const Value&) -> Value {
        demo::bump_refresh_count(1);
        rscpp::Object o;
        o["ok"] = rscpp::Value(true);
        o["count"] = rscpp::Value(static_cast<int64_t>(demo::refresh_count()));
        return rscpp::Value(std::move(o));
    });
}

void print_usage(const char* argv0) {
    std::cout << "usage: " << argv0 << " [port]\n"
              << "  Reads rscpp.ini from the working directory; a port argument wins.\n";
}

}

int main(int argc, char* argv[]) {
    if (argc > 2) {
        print_usage(argv[0]);
        return 2;
    }

    const std::filesystem::path exe_dir = executable_dir(argv[0]);

    const std::filesystem::path ini_path = find_upwards(exe_dir, "rscpp.ini");
    std::string ini_error;
    rscpp::ServerConfig cfg = rscpp::ServerConfig::from_ini(ini_path.string(), ini_error);
    if (!ini_error.empty()) std::cerr << "config: " << ini_error << "\n";

    if (std::filesystem::path(cfg.static_dir).is_relative()) {
        cfg.static_dir = find_upwards(exe_dir, cfg.static_dir).string();
    }
    std::error_code web_ec;
    if (!std::filesystem::exists(cfg.static_dir, web_ec)) {
        std::cerr << "warning: web asset directory '" << cfg.static_dir
                  << "' does not exist; client scripts will 404\n";
    }

    if (argc == 2) {
        try {
            size_t used = 0;
            const long port = std::stol(argv[1], &used);
            if (used != std::string(argv[1]).size() || port < 1 || port > 65535)
                throw std::invalid_argument("not a port");
            cfg.port = static_cast<uint16_t>(port);
        } catch (const std::exception& e) {
            std::cerr << "invalid port '" << argv[1] << "': " << e.what() << "\n";
            return 2;
        }
    }

    try {
        rscpp::RSCServer server(cfg);
        demo::register_components(server.get_registry());
        register_routes(server);
        register_actions(server);

        std::cout << "RSC++ dashboard: http://" << cfg.host << ":" << cfg.port
                  << "/  (Flight: /rsc/...)\n";
        std::cout << "config: " << ini_path.string() << "\n";
        std::cout << "web root: " << cfg.static_dir << "\n";
        server.run();
    } catch (const std::exception& e) {
        std::cerr << "fatal: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
