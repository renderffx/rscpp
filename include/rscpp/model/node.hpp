#pragma once

#include "rscpp/model/value.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace rscpp {

class Context;

enum class NodeType : uint8_t {
    ELEMENT,
    TEXT,
    FRAGMENT,
    SUSPENSE,
    ERROR_BOUNDARY,
    CLIENT,
    DYNAMIC,
};

using Props = std::map<std::string, Value>;
using Children = std::vector<NodePtr>;

struct Node {
    NodeType type = NodeType::ELEMENT;
    std::string tag;
    Props props;
    Children children;
    NodePtr fallback;
    std::string export_name = "default";
    std::string key;

    static NodePtr element(std::string tag, Props props, Children children) {
        auto n = std::make_shared<Node>();
        n->type = NodeType::ELEMENT;
        n->tag = std::move(tag);
        n->props = std::move(props);
        n->children = std::move(children);
        return n;
    }

    static NodePtr text(std::string content) {
        auto n = std::make_shared<Node>();
        n->type = NodeType::TEXT;
        n->tag = "#text";
        n->props["text"] = Value(std::move(content));
        return n;
    }

    static NodePtr fragment(Children children) {
        auto n = std::make_shared<Node>();
        n->type = NodeType::FRAGMENT;
        n->tag = "#fragment";
        n->children = std::move(children);
        return n;
    }

    static NodePtr component(std::string name) {
        auto n = std::make_shared<Node>();
        n->type = NodeType::DYNAMIC;
        n->tag = std::move(name);
        return n;
    }

    static NodePtr client(std::string module, std::string export_name, Props props,
                          Children children) {
        auto n = std::make_shared<Node>();
        n->type = NodeType::CLIENT;
        n->tag = std::move(module);
        n->export_name = std::move(export_name);
        n->props = std::move(props);
        n->children = std::move(children);
        return n;
    }

    static NodePtr suspense(Children children, NodePtr fallback) {
        auto n = std::make_shared<Node>();
        n->type = NodeType::SUSPENSE;
        n->tag = "Suspense";
        n->children = std::move(children);
        n->fallback = std::move(fallback);
        return n;
    }

    static NodePtr error_boundary(Children children, NodePtr fallback) {
        auto n = std::make_shared<Node>();
        n->type = NodeType::ERROR_BOUNDARY;
        n->tag = "ErrorBoundary";
        n->children = std::move(children);
        n->fallback = std::move(fallback);
        return n;
    }
};

}