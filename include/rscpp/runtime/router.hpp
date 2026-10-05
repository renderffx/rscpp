#pragma once

#include "rscpp/runtime/context.hpp"

#include "rscpp/model/node.hpp"

#include <algorithm>
#include <functional>
#include <map>
#include <optional>
#include <regex>
#include <string>
#include <vector>

namespace rscpp {

struct Route {
    std::string pattern;
    std::regex regex;
    std::function<NodePtr(const std::smatch&, Context&)> handler;
    std::vector<std::string> param_names;

    Route(std::string p, std::function<NodePtr(const std::smatch&, Context&)> h)
        : pattern(std::move(p)), handler(std::move(h)) {
        static const std::regex param_re(R"(:(\w+))");

        std::string regex_str = "^" + pattern + "$";
        regex_str = std::regex_replace(regex_str, param_re, R"(([^/]+))");
        regex = std::regex(regex_str);

        for (std::sregex_iterator it(pattern.begin(), pattern.end(), param_re), last;
             it != last; ++it) {
            param_names.push_back((*it)[1].str());
        }
    }

    std::optional<std::smatch> match(const std::string& path) const {
        std::smatch m;
        if (std::regex_match(path, m, regex)) return m;
        return std::nullopt;
    }

    std::map<std::string, std::string> extract_params(const std::smatch& m) const {
        std::map<std::string, std::string> params;
        for (size_t i = 0; i < param_names.size() && i + 1 < m.size(); ++i) {
            params[param_names[i]] = m[i + 1].str();
        }
        return params;
    }
};

class Router {
public:
    void set_base_path(std::string path) {
        while (!path.empty() && path.front() == '/') path.erase(0, 1);
        while (!path.empty() && path.back() == '/') path.pop_back();
        base_path_ = path.empty() ? std::string() : "/" + path;
    }

    void add_route(std::string pattern,
                   std::function<NodePtr(const std::smatch&, Context&)> handler) {
        routes_.emplace_back(base_path_ + pattern, std::move(handler));
    }

    NodePtr resolve(const std::string& path, Context& ctx) {
        const std::string normalized = normalize(path);
        for (const auto& route : routes_) {
            auto m = route.match(normalized);
            if (!m) continue;
            RequestInfo info = ctx.request();
            info.path = normalized;
            info.params = route.extract_params(*m);
            ctx.set_request(info);
            ctx.provide("params",
                        Value(Object(info.params.begin(), info.params.end())));
            return route.handler(*m, ctx);
        }
        return nullptr;
    }

    bool has_route(const std::string& path) const {
        const std::string normalized = normalize(path);
        return std::any_of(routes_.begin(), routes_.end(), [&](const Route& r) {
            return r.match(normalized).has_value();
        });
    }

    std::vector<std::string> list_routes() const {
        std::vector<std::string> out;
        out.reserve(routes_.size());
        for (const auto& r : routes_) out.push_back(r.pattern);
        return out;
    }

    static std::string normalize(std::string path) {
        if (path.find("//") == std::string::npos &&
            (path.size() <= 1 || path.back() != '/'))
            return path;
        std::string out;
        out.reserve(path.size());
        bool prev_slash = false;
        for (char c : path) {
            if (c == '/') {
                if (prev_slash) continue;
                prev_slash = true;
            } else {
                prev_slash = false;
            }
            out.push_back(c);
        }
        if (out.size() > 1 && out.back() == '/') out.pop_back();
        return out;
    }

private:
    std::vector<Route> routes_;
    std::string base_path_;
};

}