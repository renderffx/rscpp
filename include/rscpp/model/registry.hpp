#pragma once

#include "rscpp/model/node.hpp"
#include "rscpp/model/value.hpp"

#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace rscpp {

struct ServerComponent {
    std::string name;
    std::function<NodePtr(Context&)> fn;

    ServerComponent() = default;
    ServerComponent(std::string n, std::function<NodePtr(Context&)> f)
        : name(std::move(n)), fn(std::move(f)) {}
};

struct ClientComponent {
    std::string name;
    std::string module;
    std::string export_name = "default";
    bool is_async = false;
    std::vector<std::string> dependencies;

    ClientComponent() = default;
    ClientComponent(std::string n, std::string mod, std::string exprt = "default",
                    std::vector<std::string> deps = {}, bool async = false)
        : name(std::move(n)),
          module(std::move(mod)),
          export_name(std::move(exprt)),
          is_async(async),
          dependencies(std::move(deps)) {}
};

struct ServerAction {
    std::string id;
    std::string name;
    std::function<Value(const Value&)> handler;
    bool is_form_action = false;

    ServerAction() = default;
    ServerAction(std::string i, std::string n, std::function<Value(const Value&)> h,
                 bool form = false)
        : id(std::move(i)),
          name(std::move(n)),
          handler(std::move(h)),
          is_form_action(form) {}
    ServerAction(std::string i, std::function<Value(const Value&)> h, bool form = false)
        : id(std::move(i)), name(id), handler(std::move(h)), is_form_action(form) {}
};

class Registry {
    std::unordered_map<std::string, ServerComponent> server_components_;
    std::unordered_map<std::string, ClientComponent> client_components_;
    std::unordered_map<std::string, ServerAction> actions_;
    mutable std::mutex mutex_;

public:
    void register_server_component(ServerComponent c) {
        std::lock_guard lock(mutex_);
        server_components_[c.name] = std::move(c);
    }

    void register_client_component(ClientComponent c) {
        std::lock_guard lock(mutex_);
        client_components_[c.name] = std::move(c);
    }

    void register_action(ServerAction a) {
        std::lock_guard lock(mutex_);
        actions_[a.id] = std::move(a);
    }

    std::optional<ServerComponent> get_server(const std::string& name) const {
        std::lock_guard lock(mutex_);
        auto it = server_components_.find(name);
        if (it == server_components_.end()) return std::nullopt;
        return it->second;
    }

    std::optional<ServerAction> get_action(const std::string& id) const {
        std::lock_guard lock(mutex_);
        auto it = actions_.find(id);
        if (it == actions_.end()) return std::nullopt;
        return it->second;
    }

    std::unordered_map<std::string, ClientComponent> client_components() const {
        std::lock_guard lock(mutex_);
        return client_components_;
    }

    std::unordered_map<std::string, ServerAction> actions() const {
        std::lock_guard lock(mutex_);
        return actions_;
    }
};

}