#pragma once

#include "store.hpp"

#include "rscpp/rsc.hpp"

#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <thread>

namespace demo {

inline constexpr const char* kLikeButtonModule = "/rsc/modules/LikeButton.js";
inline constexpr const char* kLikeButtonExport = "LikeButton";

// Registered as a client component so the module is preloaded and shipped in
// the manifest; `Node::client` below is what actually emits the `I` row.
inline const rscpp::ClientComponent& like_button_component() {
    static const rscpp::ClientComponent component{"LikeButton", kLikeButtonModule,
                                                  kLikeButtonExport};
    return component;
}

inline rscpp::NodePtr like_button(const std::string& id) {
    rscpp::Props props;
    props["id"] = rscpp::Value(id);
    props["count"] = rscpp::Value(static_cast<int64_t>(like_count(id)));
    return rscpp::Node::client(kLikeButtonModule, kLikeButtonExport, std::move(props), {});
}

// Renders inside an ErrorBoundary to show how a thrown component is handled.
inline rscpp::NodePtr ExplodingComponent(rscpp::Context&) {
    throw std::runtime_error("Something went wrong");
}

// Suspends on first render; the renderer postpones the boundary and streams the
// content once the future resolves.
inline rscpp::NodePtr AsyncHeavyContent(rscpp::Context& ctx) {
    auto future = ctx.async_once("heavy-content", [] {
        return std::make_shared<std::shared_future<rscpp::Value>>(
            std::async(std::launch::async, []() -> rscpp::Value {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                rscpp::Object o;
                o["ready"] = rscpp::Value(true);
                o["message"] = rscpp::Value("Streamed content arrived");
                return rscpp::Value(std::move(o));
            }));
    });
    const rscpp::Value resolved = ctx.await(future, rscpp::Value());
    std::string message = "content";
    if (resolved.is_object()) {
        if (auto m = resolved.find("message"); m && m->is_string()) message = m->as_string();
    }
    return rscpp::Node::element("div", {{"class", "rsc-async"}},
                                {rscpp::Node::element("p", {}, {rscpp::Node::text(message)})});
}

}  // namespace demo