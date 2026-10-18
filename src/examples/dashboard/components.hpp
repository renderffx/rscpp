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

// ---------------------------------------------------------------------------
// Hardcore demo: real asynchronous rendering, real suspension granularity and
// real resumption.
//
// Every stage below takes real wall-clock time and suspends until its future
// resolves. A boundary shows its fallback until the data lands; a slot with no
// boundary around it becomes a `$L` reference at exactly that position on the
// Flight wire (siblings are serialized immediately and keep their values), and
// a pre-rendered shell parks every stage and serves the fallbacks instantly.
// ---------------------------------------------------------------------------

inline rscpp::NodePtr async_stage(rscpp::Context& ctx, const std::string& key, int ms,
                                  const std::string& label) {
    auto future = ctx.async_once(key, [ms, label] {
        return std::make_shared<std::shared_future<rscpp::Value>>(
            std::async(std::launch::async, [ms, label]() -> rscpp::Value {
                std::this_thread::sleep_for(std::chrono::milliseconds(ms));
                rscpp::Object o;
                o["ms"] = rscpp::Value(static_cast<int64_t>(ms));
                o["label"] = rscpp::Value(label);
                return rscpp::Value(std::move(o));
            }));
    });
    const rscpp::Value resolved = ctx.await(future, rscpp::Value());
    int elapsed = ms;
    if (resolved.is_object()) {
        if (auto m = resolved.find("ms"); m && m->is_int64()) elapsed = static_cast<int>(m->as_int64());
    }
    return rscpp::Node::element(
        "div", {{"class", "rsc-stage"}},
        {rscpp::Node::element("h3", {}, {rscpp::Node::text(label)}),
         rscpp::Node::element(
             "p", {},
             {rscpp::Node::text("resolved after " + std::to_string(elapsed) + "ms")})});
}

inline rscpp::NodePtr StageQuick(rscpp::Context& ctx) {
    rscpp::NodePtr stage = async_stage(ctx, "hardcore-quick", 150, "quick stage (150ms)");
    // A literal that looks exactly like a reference: must survive `$$`
    // escaping on the wire and render as text in the browser.
    stage->children.push_back(rscpp::Node::element(
        "code", {{"class", "rsc-dollar"}}, {rscpp::Node::text("$L5")}));
    return stage;
}

inline rscpp::NodePtr StageMedium(rscpp::Context& ctx) {
    rscpp::NodePtr stage = async_stage(ctx, "hardcore-medium", 450, "medium stage (450ms)");
    // Two instances of the same client component: one shared `I` row, two
    // templates, both must hydrate.
    stage->children.push_back(rscpp::Node::element(
        "div", {{"class", "rsc-client-zone"}}, {like_button("hardcore-a"), like_button("hardcore-b")}));
    return stage;
}

inline rscpp::NodePtr StageSlow(rscpp::Context& ctx) {
    rscpp::NodePtr stage = async_stage(ctx, "hardcore-slow", 900, "slow stage (900ms)");
    // A client component inside a suspended (and, under ?ppr=1, resumed)
    // stage: its template + row travel with the segment, so hydration has to
    // happen when the segment lands, not when the shell is built.
    stage->children.push_back(
        rscpp::Node::element("div", {{"class", "rsc-client-zone"}}, {like_button("hardcore")}));
    return stage;
}

// Suspends for 300ms and then fails: exercises the error paths for a parked
// slot/boundary (the row for the slot id is replaced by an `E` row, matching
// what React emits).
inline rscpp::NodePtr StageBoom(rscpp::Context& ctx) {
    auto future = ctx.async_once("hardcore-boom", [] {
        return std::make_shared<std::shared_future<rscpp::Value>>(
            std::async(std::launch::async, []() -> rscpp::Value {
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                throw std::runtime_error("stage boom: worker rejected the request");
            }));
    });
    const rscpp::Value resolved = ctx.await(future, rscpp::Value());
    (void)resolved;
    return rscpp::Node::element("div", {}, {rscpp::Node::text("unreachable")});
}

inline rscpp::NodePtr StageBoomBare(rscpp::Context& ctx) {
    auto future = ctx.async_once("hardcore-boom-bare", [] {
        return std::make_shared<std::shared_future<rscpp::Value>>(
            std::async(std::launch::async, []() -> rscpp::Value {
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                throw std::runtime_error("bare slot boom: worker rejected the request");
            }));
    });
    const rscpp::Value resolved = ctx.await(future, rscpp::Value());
    (void)resolved;
    return rscpp::Node::element("div", {}, {rscpp::Node::text("unreachable")});
}

inline rscpp::NodePtr StageInner(rscpp::Context& ctx) {
    return async_stage(ctx, "hardcore-inner", 250, "inner stage (250ms)");
}

// Suspends itself, and once resolved its content suspends again: the nested
// Suspense underneath gets its own boundary and its own streamed segment.
inline rscpp::NodePtr StageOuter(rscpp::Context& ctx) {
    const rscpp::Value resolved =
        ctx.await(ctx.async_once("hardcore-outer", [] {
                      return std::make_shared<std::shared_future<rscpp::Value>>(
                          std::async(std::launch::async, []() -> rscpp::Value {
                              std::this_thread::sleep_for(std::chrono::milliseconds(500));
                              return rscpp::Value(true);
                          }));
                  }),
                  rscpp::Value());
    (void)resolved;
    return rscpp::Node::element(
        "div", {{"class", "rsc-stage"}},
        {rscpp::Node::element("h3", {}, {rscpp::Node::text("outer stage (500ms)")}),
         rscpp::Node::element(
             "p", {}, {rscpp::Node::text("outer resolved; inner boundary below")}),
         rscpp::Node::suspense(
             {rscpp::Node::component("StageInner")},
             rscpp::Node::element("div", {{"class", "rsc-fallback"}},
                                  {rscpp::Node::text("inner fallback (250ms)")}))});
}

// Deliberately *not* wrapped in a Suspense by the page. On the Flight wire the
// only suspended slot becomes `$L`; "before" and "after" are serialized in the
// first payload. In HTML mode it bubbles to the root, which the renderer parks
// the same way instead of blocking.
inline rscpp::NodePtr StageProbe(rscpp::Context& ctx) {
    return async_stage(ctx, "hardcore-probe", 400, "bare slot probe (400ms)");
}

}  // namespace demo