#pragma once

// Dashboard example: the routes and server actions wired into rscpp_server.
//
// components.hpp  client and boundary components
// pages.hpp       route handlers
// store.hpp       demo state and action helpers

#include "components.hpp"
#include "pages.hpp"
#include "store.hpp"

#include "rscpp/rsc.hpp"

namespace demo {

// Only the names actually referenced through Node::component() need a registry
// entry; pages are resolved directly by the router.
inline void register_components(rscpp::Registry& registry) {
    registry.register_server_component(
        rscpp::ServerComponent{"ExplodingComponent",
                               [](rscpp::Context& ctx) { return ExplodingComponent(ctx); }});

    registry.register_server_component(
        rscpp::ServerComponent{"AsyncHeavyContent",
                               [](rscpp::Context& ctx) { return AsyncHeavyContent(ctx); }});

    registry.register_client_component(like_button_component());
}

}  // namespace demo