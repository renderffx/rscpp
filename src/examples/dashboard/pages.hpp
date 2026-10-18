#pragma once

#include "components.hpp"
#include "store.hpp"

#include "rscpp/rsc.hpp"

#include <regex>
#include <string>
#include <vector>

namespace demo {

namespace {

inline std::string match_param(const std::smatch& m, size_t index) {
    return (m.size() > index && m[index].matched) ? m[index].str() : std::string();
}

inline rscpp::NodePtr item_list(int count, int modulo, const std::string& unit) {
    rscpp::Children items;
    items.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; i++) {
        items.push_back(rscpp::Node::element(
            "div", {},
            {rscpp::Node::element("span", {}, {rscpp::Node::text("Item " + std::to_string(i))}),
             rscpp::Node::element(
                 "span", {}, {rscpp::Node::text(std::to_string(i % modulo) + unit)})}));
    }
    return rscpp::Node::element("ul", {}, std::move(items));
}

}  // namespace

inline rscpp::NodePtr StressPage(const std::smatch&, rscpp::Context&) {
    rscpp::Props props;
    props["percent"] = rscpp::Value(85);

    return rscpp::Node::element("div", props, {
        rscpp::Node::element("h1", {}, {rscpp::Node::text("Stress Test")}),
        rscpp::Node::element("p", {}, {rscpp::Node::text("Load: 85%")}),
    });
}

inline rscpp::NodePtr ProductPage(const std::smatch& m, rscpp::Context&) {
    const std::string id = match_param(m, 1).empty() ? "1" : match_param(m, 1);

    rscpp::Props props;
    props["productId"] = rscpp::Value(id);
    props["name"] = rscpp::Value("Product " + id);

    return rscpp::Node::element("div", props, {
        rscpp::Node::element("h1", {}, {rscpp::Node::text("Product " + id)}),
        rscpp::Node::element("p", {}, {rscpp::Node::text("Details for product " + id)}),
        rscpp::Node::element("div", {{"class", "likes"}}, {like_button(id)}),
    });
}

inline rscpp::NodePtr ProductsIndexPage(const std::smatch&, rscpp::Context&) {
    rscpp::Children children;
    children.push_back(rscpp::Node::element("h1", {}, {rscpp::Node::text("Products")}));
    for (int i = 1; i <= 3; i++) {
        const std::string id = std::to_string(i);
        children.push_back(rscpp::Node::element("div", {{"class", "product"}}, {
            rscpp::Node::element("h3", {}, {rscpp::Node::text("Product " + id)}),
            rscpp::Node::element("p", {}, {rscpp::Node::text("Details for product " + id)}),
        }));
    }

    rscpp::Props props;
    props["productCount"] = rscpp::Value(3);
    return rscpp::Node::element("div", props, std::move(children));
}

inline rscpp::NodePtr SearchPage(const std::smatch&, rscpp::Context&) {
    rscpp::Props props;
    props["query"] = rscpp::Value("");

    return rscpp::Node::element("div", props, {
        rscpp::Node::element("h1", {}, {rscpp::Node::text("Search")}),
        rscpp::Node::element("p", {}, {rscpp::Node::text("Search results")}),
    });
}

inline rscpp::NodePtr ErrorPage(const std::smatch&, rscpp::Context&) {
    return rscpp::Node::element("div", {}, {
        rscpp::Node::element("h1", {}, {rscpp::Node::text("Error Boundary Demo")}),
        rscpp::Node::element(
            "p", {},
            {rscpp::Node::text(
                "This page renders a component that throws, exercising the error boundary.")}),
        rscpp::Node::error_boundary(
            {rscpp::Node::component("ExplodingComponent")},
            rscpp::Node::element("div", {{"class", "rsc-error-fallback"}},
                                 {rscpp::Node::text("Something went wrong")})),
    });
}

inline rscpp::NodePtr AboutPage(const std::smatch&, rscpp::Context&) {
    rscpp::Props props;
    props["description"] = rscpp::Value("RSC++ demo application");

    return rscpp::Node::element("div", props, {
        rscpp::Node::element("h1", {}, {rscpp::Node::text("About")}),
        rscpp::Node::element("p", {}, {rscpp::Node::text("A demo RSC++ application")}),
        rscpp::Node::element("p", {}, {rscpp::Node::text("Server-side rendering with C++")}),
    });
}

inline rscpp::NodePtr PostsPage(const std::smatch&, rscpp::Context&) {
    rscpp::Props props;
    props["postCount"] = rscpp::Value(12);

    return rscpp::Node::element("div", props, {
        rscpp::Node::element("h1", {}, {rscpp::Node::text("Posts")}),
        rscpp::Node::element("p", {}, {rscpp::Node::text("Total posts: 12")}),
    });
}

inline rscpp::NodePtr HardcorePage(const std::smatch&, rscpp::Context& ctx) {
    auto fallback = [](const char* label) {
        return rscpp::Node::element("div", {{"class", "rsc-fallback"}},
                                    {rscpp::Node::text(label)});
    };

    rscpp::Children children;
    children.push_back(rscpp::Node::element("h1", {}, {rscpp::Node::text("Hardcore Streaming")}));
    children.push_back(rscpp::Node::element(
        "p", {},
        {rscpp::Node::text(
            "Each stage sleeps on a worker thread. Boundaries stream as they "
            "resolve; the response never blocks on any of them.")}));
    children.push_back(rscpp::Node::element(
        "p", {},
        {rscpp::Node::text(
            "Try ?ppr=1: the shell with every fallback is served immediately and "
            "the server resumes the parked stages on a second request.")}));

    children.push_back(rscpp::Node::suspense({rscpp::Node::component("StageQuick")},
                                             fallback("quick fallback (150ms)")));
    children.push_back(rscpp::Node::suspense({rscpp::Node::component("StageMedium")},
                                             fallback("medium fallback (450ms)")));
    children.push_back(rscpp::Node::suspense({rscpp::Node::component("StageSlow")},
                                             fallback("slow fallback (900ms)")));
    children.push_back(rscpp::Node::suspense({rscpp::Node::component("StageOuter")},
                                             fallback("outer fallback (500ms + 250ms)")));

    if (ctx.request().query.count("root") > 0) {
        // A suspension with no enclosing boundary: one slot, three children.
        children.push_back(rscpp::Node::element(
            "div", {{"class", "rsc-probe"}},
            {rscpp::Node::text("before the bare slot | "),
             rscpp::Node::component("StageProbe"),
             rscpp::Node::text(" | after the bare slot")}));
    }

    if (ctx.request().query.count("boom") > 0) {
        children.push_back(rscpp::Node::suspense(
            {rscpp::Node::component("StageBoom")}, fallback("boom fallback (fails at 300ms)")));
    }

    if (ctx.request().query.count("boombare") > 0) {
        children.push_back(rscpp::Node::element(
            "div", {{"class", "rsc-probe"}},
            {rscpp::Node::text("before boom | "), rscpp::Node::component("StageBoomBare"),
             rscpp::Node::text(" | after boom")}));
    }

    if (ctx.request().query.count("rootboom") > 0) {
        // The root component itself throws: no boundary can catch it, so the
        // failure is a root-level `E` row (never a blank shell).
        return rscpp::Node::component("StageRootBoom");
    }

    rscpp::Props props;
    props["title"] = rscpp::Value("Hardcore");
    return rscpp::Node::element("div", props, std::move(children));
}

inline rscpp::NodePtr SuspensePage(const std::smatch&, rscpp::Context&) {
    rscpp::Props props;
    props["loaded"] = rscpp::Value(false);

    return rscpp::Node::element("div", props, {
        rscpp::Node::element("h1", {}, {rscpp::Node::text("Suspense Demo")}),
        rscpp::Node::element("p", {}, {rscpp::Node::text("Loading heavy component...")}),
        rscpp::Node::suspense(
            {rscpp::Node::component("AsyncHeavyContent")},
            rscpp::Node::element("div", {{"class", "rsc-fallback"}},
                                 {rscpp::Node::text("Loading...")})),
    });
}

inline rscpp::NodePtr UserListPage(const std::smatch&, rscpp::Context&) {
    rscpp::Props props;
    props["userCount"] = rscpp::Value(0);

    return rscpp::Node::element("div", props, {
        rscpp::Node::element("h1", {}, {rscpp::Node::text("User List")}),
        rscpp::Node::element("p", {}, {rscpp::Node::text("No users found")}),
    });
}

inline rscpp::NodePtr UserListPageWithData(const std::smatch& m, rscpp::Context&) {
    const std::string data_id = match_param(m, 1).empty() ? "all" : match_param(m, 1);

    rscpp::Children users;
    for (int i = 0; i < 5; i++) {
        users.push_back(rscpp::Node::element(
            "div", {},
            {rscpp::Node::element("span", {}, {rscpp::Node::text("User " + std::to_string(i))})}));
    }

    rscpp::Props props;
    props["dataId"] = rscpp::Value(data_id);
    props["userCount"] = rscpp::Value(5);

    return rscpp::Node::element("div", props, {
        rscpp::Node::element("h1", {}, {rscpp::Node::text("User List")}),
        rscpp::Node::element("p", {}, {rscpp::Node::text("Load: " + data_id)}),
        rscpp::Node::element("ul", {}, std::move(users)),
    });
}

inline rscpp::NodePtr PostWithAuthor(const std::smatch& m, rscpp::Context&) {
    const std::string post_id = match_param(m, 1).empty() ? "1" : match_param(m, 1);
    const std::string author = "Author " + post_id;

    rscpp::Props props;
    props["postId"] = rscpp::Value(post_id);
    props["author"] = rscpp::Value(author);

    return rscpp::Node::element("article", props, {
        rscpp::Node::element("h2", {}, {rscpp::Node::text("Post " + post_id)}),
        rscpp::Node::element("footer", {},
                             {rscpp::Node::element("span", {}, {rscpp::Node::text("By " + author)})}),
    });
}

inline rscpp::NodePtr HeavyDataPage(const std::smatch&, rscpp::Context&) {
    rscpp::Props props;
    props["itemCount"] = rscpp::Value(50);

    return rscpp::Node::element("div", props, {
        rscpp::Node::element("h1", {}, {rscpp::Node::text("Heavy Data")}),
        rscpp::Node::element("p", {}, {rscpp::Node::text("Rendering 50 items")}),
        item_list(50, 10, "pt"),
    });
}

inline rscpp::NodePtr MassiveDataPage(const std::smatch&, rscpp::Context&) {
    rscpp::Props props;
    props["itemCount"] = rscpp::Value(1000);

    return rscpp::Node::element("div", props, {
        rscpp::Node::element("h1", {}, {rscpp::Node::text("Massive Data")}),
        rscpp::Node::element("p", {}, {rscpp::Node::text("Rendering 1000 items - stress test")}),
        item_list(1000, 100, "pt"),
    });
}

inline rscpp::NodePtr CounterPage(const std::smatch&, rscpp::Context&) {
    const std::string value = std::to_string(refresh_count());

    rscpp::Props props;
    props["count"] = rscpp::Value(static_cast<int64_t>(refresh_count()));

    auto counter_form = [&value](const char* action, const char* label) {
        rscpp::Props attrs;
        attrs["action"] = rscpp::Value(action);
        attrs["method"] = rscpp::Value("POST");
        return rscpp::Node::element("form", attrs, {
            rscpp::Node::element("input",
                                 {{"type", rscpp::Value("hidden")},
                                  {"name", rscpp::Value("counter_value")},
                                  {"value", rscpp::Value(value)}},
                                 {}),
            rscpp::Node::element("button", {{"type", rscpp::Value("submit")}},
                                 {rscpp::Node::text(label)}),
        });
    };

    return rscpp::Node::element("div", props, {
        rscpp::Node::element("h1", {}, {rscpp::Node::text("Counter")}),
        rscpp::Node::element("p", {}, {rscpp::Node::text("Count: " + value)}),
        counter_form("increment", "Increment"),
        counter_form("decrement", "Decrement"),
    });
}

// Declared last because it embeds the two stress pages above.
inline rscpp::NodePtr DashboardPage(const std::smatch&, rscpp::Context& ctx) {
    rscpp::Props props;
    props["title"] = rscpp::Value("RSC++ Dashboard");

    return rscpp::Node::element("div", props, {
        rscpp::Node::element("h1", {}, {rscpp::Node::text("Dashboard")}),
        rscpp::Node::element("p", {}, {rscpp::Node::text("Welcome to the RSC++ demo!")}),
        rscpp::Node::element("h2", {}, {rscpp::Node::text("Stress Testing Components")}),
        rscpp::Node::element("p", {}, {rscpp::Node::text("Heavy Data (50 items)")}),
        HeavyDataPage({}, ctx),
        MassiveDataPage({}, ctx),
    });
}

}  // namespace demo