#include "delivery.hpp"

#include "rscpp/html/escape.hpp"
#include "rscpp/util/hash.hpp"

#include "logging.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>

namespace rscpp::delivery {
namespace {

std::string find_heading(const NodePtr& node) {
    if (!node) return {};
    if (node->type == NodeType::TEXT) {
        auto it = node->props.find("text");
        if (it != node->props.end() && it->second.is_string()) return it->second.as_string();
        return {};
    }
    if (node->type == NodeType::ELEMENT && (node->tag == "h1" || node->tag == "title") &&
        !node->children.empty()) {
        std::string out;
        for (const auto& c : node->children) {
            const std::string part = find_heading(c);
            if (part.empty()) continue;
            if (!out.empty()) out += ' ';
            out += part;
        }
        if (!out.empty()) return out;
    }
    for (const auto& c : node->children) {
        const std::string found = find_heading(c);
        if (!found.empty()) return found;
    }
    return {};
}

}  // namespace

RequestInfo request_info_from(const httplib::Request& req, std::string path, std::string method) {
    RequestInfo info;
    info.method = std::move(method);
    info.path = std::move(path);
    for (const auto& [k, v] : req.params) info.query[k] = v;
    return info;
}

NodePtr not_found_node(const std::string& path) {
    return Node::element("div", {{"class", "rsc-error-fallback"}}, {
                            Node::element("h1", {}, {Node::text("404 Not Found")}),
                            Node::element("p", {}, {Node::text("No route matches: " + path)}),
                        });
}

std::string error_page(int status, const std::string& heading, const std::string& detail) {
    return std::string("<!DOCTYPE html><html lang=\"en\"><head><meta charset=\"utf-8\">"
                       "<title>") +
           std::to_string(status) + " " + html::escape_html(heading) +
           "</title></head><body><h1>" + html::escape_html(heading) + "</h1><p>" +
           html::escape_html(detail) + "</p></body></html>";
}

std::string page_title(const NodePtr& node) {
    const std::string heading = find_heading(node);
    return heading.empty() ? std::string("RSC++") : heading;
}

std::string boundary_runtime(const std::string& nonce) {
    return "<script nonce=\"" + nonce +
           "\">window.$RC=function(b,s,r){var x=document.getElementById(b),y=document.getElementById(s);"
           "if(x&&y){while(x.firstChild)x.removeChild(x.firstChild);"
           "while(y.firstChild)x.appendChild(y.firstChild);"
           "y.remove();x.removeAttribute('data-rsc-boundary')}"
           "(window.__RSC_ROWS__=window.__RSC_ROWS__||[]).push(r);"
           "if(window.__RSC_ROWS_PUSH__)window.__RSC_ROWS_PUSH__()};"
           "window.$R=function(r){(window.__RSC_ROWS__=window.__RSC_ROWS__||[]).push(r);"
           "if(window.__RSC_ROWS_PUSH__)window.__RSC_ROWS_PUSH__()};</script>";
}

std::string mime_type(const std::string& path) {
    namespace fs = std::filesystem;
    const std::string ext = fs::path(path).extension().string();
    if (ext == ".js" || ext == ".mjs") return "text/javascript; charset=utf-8";
    if (ext == ".html") return "text/html; charset=utf-8";
    if (ext == ".css") return "text/css; charset=utf-8";
    if (ext == ".json" || ext == ".map") return "application/json";
    if (ext == ".wasm") return "application/wasm";
    if (ext == ".svg") return "image/svg+xml";
    if (ext == ".ico") return "image/x-icon";
    if (ext == ".png") return "image/png";
    if (ext == ".txt") return "text/plain; charset=utf-8";
    return "application/octet-stream";
}

std::optional<std::string> resolve_static(const std::string& root,
                                          const std::string& relative_path) {
    if (relative_path.empty()) return std::nullopt;

    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path base = fs::weakly_canonical(fs::path(root), ec);
    if (ec) return std::nullopt;

    const fs::path candidate = fs::weakly_canonical(base / relative_path, ec);
    if (ec) return std::nullopt;

    const std::string root_str = base.string();
    const std::string full = candidate.string();
    if (full.compare(0, root_str.size(), root_str) != 0 ||
        (full.size() > root_str.size() &&
         full[root_str.size()] != fs::path::preferred_separator)) {
        RSC_WARN("rejected static path outside root: ", relative_path);
        return std::nullopt;
    }
    if (!fs::is_regular_file(candidate, ec)) return std::nullopt;
    return full;
}

void serve_static_file(httplib::Response& res, const httplib::Request& req,
                       const std::string& root, const std::string& relative_path) {
    const auto resolved = resolve_static(root, relative_path);
    if (!resolved) {
        res.status = 404;
        res.set_content("Not Found", "text/plain");
        return;
    }

    std::ifstream file(*resolved, std::ios::binary);
    if (!file) {
        res.status = 404;
        res.set_content("Not Found", "text/plain");
        return;
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    const std::string content = buffer.str();

    const std::string etag = "\"" + util::hex_digest(content) + "\"";

    res.set_header("ETag", etag);
    res.set_header("Cache-Control", "public, max-age=0, must-revalidate");
    if (req.get_header_value("If-None-Match") == etag) {
        res.status = 304;
        return;
    }
    res.set_content(content, mime_type(*resolved));
}

}