#include "server.hpp"

#include "delivery.hpp"

#include "rscpp/html/escape.hpp"

#include <csignal>
#include <optional>

namespace rscpp {

std::atomic<RSCServer*> RSCServer::instance_{nullptr};

thread_local RequestContext g_request_ctx;

RSCServer::RSCServer(const ServerConfig& config)
    : config_(config),
      rate_limiter_(config.rate_limit_capacity, config.rate_limit_refill) {
    Logger::instance().set_level(config_.log_level);
    instance_ = this;

    registry_.register_action(ServerAction{"rsc:ping", [](const Value&) {
        Object o;
        o["pong"] = true;
        return Value(std::move(o));
    }});

    server_.set_payload_max_length(config_.max_request_size);
    setup_middleware();
    setup_routes();
}

RSCServer::~RSCServer() {
    stop();
    if (janitor_.joinable()) janitor_.join();
    if (instance_ == this) instance_ = nullptr;
}

void RSCServer::setup_routes() {
    const std::chrono::milliseconds stall{config_.stream_stall_timeout_ms};
    const size_t queue_limit = config_.stream_queue_limit;
    const auto make_pipe = [queue_limit, stall] {
        return std::make_shared<ChunkedPipe>(queue_limit, stall);
    };
    const std::string& root = config_.static_dir;

    server_.Get("/rsc/client.js", [this, root](const httplib::Request& req,
                                               httplib::Response& res) {
        delivery::serve_static_file(res, req, root, "client.js");
    });
    server_.Get(R"(/rsc/modules/(.*))", [this, root](const httplib::Request& req,
                                                    httplib::Response& res) {
        delivery::serve_static_file(res, req, root, "modules/" + req.matches[1].str());
    });
    server_.Get("/rsc/rsc.css", [this, root](const httplib::Request& req,
                                             httplib::Response& res) {
        delivery::serve_static_file(res, req, root, "rsc.css");
    });
    server_.Get("/rsc/manifest.json", [this](const httplib::Request&, httplib::Response& res) {
        handle_manifest(res);
    });

    server_.Get(R"(/rsc(/.*)?)", [this, make_pipe](const httplib::Request& req,
                                                   httplib::Response& res) {
        std::string path = req.matches[1].str();
        if (path.empty()) path = "/";

        const bool found = router_.has_route(path);
        res.status = found ? 200 : 404;

        const RequestInfo info = delivery::request_info_from(req, path, "GET");
        auto resp = std::make_shared<StreamResponse>(res, make_pipe(),
                                                     StreamResponse::Kind::FLIGHT);
        resp->start([this, resp, info, found](StreamResponse& f) {
            handle_flight_response(f, info, found);
        });
    });

    server_.Post("/rsc/action", [this, make_pipe](const httplib::Request& req,
                                                  httplib::Response& res) {
        nlohmann::json payload = nlohmann::json::object();
        std::string error;
        if (req.body.empty()) {
            error = "empty action payload";
        } else {
            try {
                payload = nlohmann::json::parse(req.body);
            } catch (const std::exception& e) {
                error = std::string("invalid action payload: ") + e.what();
            }
        }

        const std::string action_id =
            (error.empty() && payload.is_object()) ? payload.value("action_id", "") : "";
        const bool known = error.empty() && !action_id.empty() &&
                           registry_.get_action(action_id).has_value();
        if (!known && error.empty()) error = "unknown action: " + action_id;

        res.status = error.empty() ? 200 : (payload.is_object() && !action_id.empty() ? 404 : 400);

        Value args;
        std::string path = "/";
        if (payload.is_object()) {
            if (payload.contains("payload")) {
                try {
                    args = from_json(payload["payload"]);
                } catch (const std::exception& e) {
                    error = std::string("invalid action arguments: ") + e.what();
                    args = Value();
                }
            }
            path = payload.value("path", "/");
        }
        if (path.empty() || path[0] != '/') {
            error = "action path must be absolute";
            path = "/";
        }

        const RequestInfo info = delivery::request_info_from(req, path, "POST");
        auto resp = std::make_shared<StreamResponse>(res, make_pipe(),
                                                     StreamResponse::Kind::FLIGHT);
        resp->start([this, resp, info, action_id, args, path, error](StreamResponse& f) {
            handle_action_response(f, info, action_id, args, path, error);
        });
    });

    server_.Get("/_health", [this](const httplib::Request&, httplib::Response& res) {
        handle_health(res);
    });
    server_.Get("/_metrics", [this](const httplib::Request&, httplib::Response& res) {
        handle_metrics(res);
    });

    server_.Get(R"(/(.*))", [this, make_pipe](const httplib::Request& req,
                                             httplib::Response& res) {
        const std::string path = req.path.empty() ? "/" : req.path;
        const bool found = router_.has_route(path);
        res.status = found ? 200 : 404;

        const RequestInfo info = delivery::request_info_from(req, path, "GET");
        const std::string nonce = current_nonce();
        auto resp =
            std::make_shared<StreamResponse>(res, make_pipe(), StreamResponse::Kind::HTML);
        resp->start([this, resp, info, found, nonce](StreamResponse& h) {
            handle_html_response(h, info, found, nonce);
        });
    });
}

void RSCServer::handle_health(httplib::Response& res) const {
    res.set_content(metrics_.health_json(is_shutting_down()).dump(), "application/json");
}

void RSCServer::handle_metrics(httplib::Response& res) const {
    res.set_content(metrics_.to_json().dump(), "application/json");
}

void RSCServer::handle_manifest(httplib::Response& res) const {
    nlohmann::json client_modules = nlohmann::json::array();
    for (const auto& [name, cc] : registry_.client_components()) {
        client_modules.push_back({{"name", name},
                                  {"module", cc.module},
                                  {"export", cc.export_name},
                                  {"async", cc.is_async},
                                  {"dependencies", cc.dependencies}});
    }

    nlohmann::json server_actions = nlohmann::json::array();
    for (const auto& [id, sa] : registry_.actions()) {
        server_actions.push_back(
            {{"id", sa.id}, {"name", sa.name}, {"is_form_action", sa.is_form_action}});
    }

    nlohmann::json routes = nlohmann::json::array();
    for (const auto& route : router_.list_routes()) routes.push_back(route);

    nlohmann::json manifest{{"version", "rscpp-1.0"},
                            {"client_modules", std::move(client_modules)},
                            {"server_actions", std::move(server_actions)},
                            {"routes", std::move(routes)}};
    res.set_content(manifest.dump(2), "application/json");
}

void RSCServer::handle_flight_response(StreamResponse& resp, const RequestInfo& req_info,
                                       bool found) {
    metrics_.record_flight();

    auto ctx = std::make_shared<Context>();
    ctx->init(req_info, registry_);

    NodePtr node = found ? router_.resolve(req_info.path, *ctx) : nullptr;
    if (!node) node = delivery::not_found_node(req_info.path);

    ChunkSink sink([&resp](std::string_view row) { return resp.write(row); });
    auto renderer = std::make_shared<Renderer>(registry_, ctx);
    renderer->set_sink(&sink);

    renderer->render(node);
    renderer->drain_pending();
}

void RSCServer::handle_html_response(StreamResponse& resp, const RequestInfo& req_info,
                                     bool found, const std::string& nonce) {
    metrics_.record_html();

    auto ctx = std::make_shared<Context>();
    ctx->init(req_info, registry_);

    NodePtr node = found ? router_.resolve(req_info.path, *ctx) : nullptr;
    if (!node) {
        if (resp.write(delivery::error_page(404, "Not Found", "No route matches: " +
                                                            req_info.path)))
            resp.end();
        return;
    }

    auto sink = std::make_shared<SsrStreamSink>(nonce);
    sink->set_response(&resp);

    auto renderer = std::make_shared<Renderer>(registry_, ctx);
    renderer->enable_html();
    renderer->set_sink(sink.get());
    renderer->set_html_sink(sink.get());

    const auto t_render0 = std::chrono::steady_clock::now();
    renderer->render(node);
    const auto t_render1 = std::chrono::steady_clock::now();

    const std::string title = delivery::page_title(node);
    if (!resp.write(build_shell_head(title, sink->wire(), nonce))) return;
    if (!sink->begin_body()) return;

    renderer->drain_pending();

    sink->flush_rows();

    if (resp.write(build_shell_tail(nonce))) resp.end();
    const auto t_end = std::chrono::steady_clock::now();
    RSC_DEBUG("html render: ",
              std::chrono::duration_cast<std::chrono::milliseconds>(t_render1 - t_render0).count(),
              "ms, total incl drain: ",
              std::chrono::duration_cast<std::chrono::milliseconds>(t_end - t_render0).count(),
              "ms, path=", req_info.path);
}

void RSCServer::handle_action_response(StreamResponse& resp, const RequestInfo& req_info,
                                       const std::string& action_id, const Value& args,
                                       const std::string& path, const std::string& error) {
    if (!error.empty()) {
        metrics_.record_action(false);
        resp.write_row(flight::RowRecord{0, 'E', nlohmann::json{{"message", error}}});
        resp.end();
        return;
    }

    auto action = registry_.get_action(action_id);
    if (!action) {
        metrics_.record_action(false);
        resp.write_row(
            flight::RowRecord{0, 'E', nlohmann::json{{"message", "Action not found: " + action_id}}});
        resp.end();
        return;
    }

    Value result;
    try {
        result = action->handler(args);
        metrics_.record_action(true);
    } catch (const std::exception& e) {
        metrics_.record_action(false);
        RSC_ERROR("action '", action_id, "' threw: ", e.what());
        resp.write_row(flight::RowRecord{0, 'E', nlohmann::json{{"message", e.what()}}});
        resp.end();
        return;
    }

    RequestInfo info = req_info;
    info.path = path;
    info.method = "POST";

    auto ctx = std::make_shared<Context>();
    ctx->init(info, registry_);
    ctx->provide("action_result", result);

    NodePtr node = router_.resolve(path, *ctx);
    if (!node) {
        resp.write_row(flight::RowRecord{
            0, 'E', nlohmann::json{{"message", "No route for path after action: " + path}}});
        resp.end();
        return;
    }

    // An action response is not a streaming endpoint, so the re-render must be
    // complete before any row is written. Shared ownership is load-bearing:
    // a suspended boundary spawns a detached thread that re-enters this
    // renderer through a weak_ptr, which requires the Renderer to be owned by
    // a shared_ptr. Stack-allocating it threw bad_weak_ptr. drain_pending()
    // then runs the retry tasks, so the boundary's chunk is resolved rather
    // than left as an $L reference the client could never fetch.
    auto renderer = std::make_shared<Renderer>(registry_, ctx);
    const auto t_render0 = std::chrono::steady_clock::now();
    renderer->render(node);
    renderer->drain_pending();
    const auto t_render1 = std::chrono::steady_clock::now();
    for (const auto& row : renderer->rows()) {
        if (!resp.write(row.to_wire())) return;
    }
    const auto t_written = std::chrono::steady_clock::now();
    RSC_DEBUG("action flight render: ",
              std::chrono::duration_cast<std::chrono::milliseconds>(t_render1 - t_render0).count(),
              "ms, serialize+write: ",
              std::chrono::duration_cast<std::chrono::milliseconds>(t_written - t_render1).count(),
              "ms, path=", path);

    resp.write_row(flight::RowRecord{flight::ACTION_RESULT_ROW_ID, 'A', to_json(result)});
}

std::string RSCServer::build_shell_head(const std::string& title, const std::string& flight_wire,
                                        const std::string& nonce) const {
    std::string shell;
    shell.reserve(flight_wire.size() + 4096);
    shell += "<!DOCTYPE html><html lang=\"en\"><head><meta charset=\"utf-8\">";
    shell += "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">";
    shell += "<title>" + html::escape_html(title) + "</title>";
    for (const auto& [name, cc] : registry_.client_components()) {
        (void)name;
        shell += "<link rel=\"modulepreload\" href=\"" + html::escape_html(cc.module) + "\">";
    }
    shell += "<link rel=\"stylesheet\" href=\"/rsc/rsc.css\">";
    shell += "<script id=\"__RSC_DATA__\" type=\"application/json\">";
    shell += html::script_safe(nlohmann::json(flight_wire).dump());
    shell += "</script>";
    shell += delivery::boundary_runtime(nonce);
    shell += "</head><body><div id=\"root\">";
    return shell;
}

std::string RSCServer::build_shell_tail(const std::string& nonce) const {
    return "</div><script nonce=\"" + nonce +
           "\" type=\"module\" src=\"/rsc/client.js\"></script></body></html>";
}

void RSCServer::setup_middleware() {
    middleware_.add([](const httplib::Request&, httplib::Response& res, RequestContext& ctx) {
        res.set_header("X-Request-ID", ctx.request_id);
        return true;
    });

    middleware_.add([this](const httplib::Request&, httplib::Response& res, RequestContext&) {
        apply_security_headers(res);
        return true;
    });

    middleware_.add([this](const httplib::Request& req, httplib::Response& res, RequestContext&) {
        const std::string key = req.remote_addr.empty() ? "unknown" : req.remote_addr;
        if (rate_limiter_.allow(key)) return true;
        metrics_.rate_limited++;
        res.status = 429;
        res.set_header("Retry-After", "1");
        res.set_content("Too Many Requests", "text/plain");
        return false;
    });
}

void RSCServer::apply_security_headers(httplib::Response& res) const {
    res.set_header("X-Content-Type-Options", "nosniff");
    res.set_header("X-Frame-Options", "DENY");
    res.set_header("Referrer-Policy", "strict-origin-when-cross-origin");
    res.set_header("Permissions-Policy", "camera=(), microphone=(), geolocation=()");
    if (config_.enable_csp) {
        std::string policy = config_.csp_policy;
        const std::string nonce = current_nonce();
        if (!nonce.empty()) {
            const std::string marker = "script-src 'self'";
            const size_t at = policy.find(marker);
            if (at != std::string::npos) {
                policy.insert(at + marker.size(), " 'nonce-" + nonce + "'");
            } else {
                policy += "; script-src 'self' 'nonce-" + nonce + "'";
            }
        }
        res.set_header("Content-Security-Policy", policy);
    }
    if (config_.enable_hsts)
        res.set_header("Strict-Transport-Security", "max-age=31536000; includeSubDomains");
}

void RSCServer::run() {
    setup_signal_handlers();
    RSC_INFO("routes registered: ", router_.list_routes().size());

    server_.set_pre_routing_handler([this](const httplib::Request& req, httplib::Response& res) {
        RequestContext& ctx = g_request_ctx;
        ctx.request_id = generate_request_id();
        ctx.start_time = std::chrono::steady_clock::now();
        g_request_id = ctx.request_id;

        g_csp_nonce = req.path.rfind("/rsc", 0) == 0 ? std::string() : make_nonce();

        metrics_.requests_active++;
        if (!middleware_.execute(req, res, ctx)) {
            metrics_.requests_active--;
            metrics_.record_request(false);
            return httplib::Server::HandlerResponse::Handled;
        }
        return httplib::Server::HandlerResponse::Unhandled;
    });

    server_.set_post_routing_handler([this](const httplib::Request& req, httplib::Response& res) {
        if (metrics_.requests_active.load() > 0) metrics_.requests_active--;
        metrics_.record_request(res.status < 500);
        RSC_DEBUG(req.method, " ", req.path, " -> ", res.status, " in ",
                  std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - g_request_ctx.start_time)
                      .count(),
                  "ms");
    });

    janitor_ = std::thread([this] {
        while (!shutting_down_.load()) {
            for (int i = 0; i < 60 && !shutting_down_.load(); ++i)
                std::this_thread::sleep_for(std::chrono::seconds(1));
            if (shutting_down_.load()) break;
            const size_t before = rate_limiter_.size();
            rate_limiter_.cleanup_idle();
            if (rate_limiter_.size() != before)
                RSC_DEBUG("rate limiter: ", before, " -> ", rate_limiter_.size(), " buckets");
        }
    });

    RSC_INFO("listening on http://", config_.host, ":", config_.port);
    if (!server_.listen(config_.host.c_str(), config_.port)) {
        RSC_ERROR("failed to bind ", config_.host, ":", config_.port);
    }
    RSC_INFO("shutdown complete");
}

void RSCServer::stop() {
    if (shutting_down_.exchange(true)) return;
    server_.stop();
}

void RSCServer::setup_signal_handlers() {
    instance_ = this;
    std::signal(SIGINT, &RSCServer::signal_handler);
    std::signal(SIGTERM, &RSCServer::signal_handler);
}

void RSCServer::signal_handler(int sig) {
    (void)sig;
    if (RSCServer* self = instance_.load()) {
        self->shutting_down_.store(true);
        self->server_.stop();
    }
}

}