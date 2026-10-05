#pragma once

#include "httplib.h"

#include "rscpp/runtime/context.hpp"

#include <optional>
#include <string>

namespace rscpp::delivery {

// Turns an httplib request into the trimmed RequestInfo the render layer needs.
RequestInfo request_info_from(const httplib::Request& req, std::string path, std::string method);

NodePtr not_found_node(const std::string& path);

std::string error_page(int status, const std::string& heading, const std::string& detail);

std::string page_title(const NodePtr& node);

// The tiny $R/$RC runtime that the SSR shell installs for streamed rows.
std::string boundary_runtime(const std::string& nonce);

std::string mime_type(const std::string& path);

// Resolves relative_path under root, rejecting anything that escapes it.
std::optional<std::string> resolve_static(const std::string& root,
                                          const std::string& relative_path);

void serve_static_file(httplib::Response& res, const httplib::Request& req,
                       const std::string& root, const std::string& relative_path);

}