#pragma once

#include "json.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace rscpp {

struct Node;
using NodePtr = std::shared_ptr<Node>;
struct Ref;
using RefPtr = std::shared_ptr<Ref>;
class Value;

using Object = std::map<std::string, Value>;
using Array = std::vector<Value>;

class Value {
public:
    std::variant<std::monostate,
                 std::nullptr_t,
                 bool,
                 int64_t,
                 double,
                 std::string,
                 Array,
                 Object,
                 NodePtr,
                 RefPtr>
        data{std::monostate{}};

    Value() = default;
    Value(std::nullptr_t) : data(nullptr) {}
    Value(bool b) : data(b) {}
    Value(int i) : data(static_cast<int64_t>(i)) {}
    Value(int64_t i) : data(i) {}
    Value(double d) : data(d) {}
    Value(const char* s) : data(std::string(s)) {}
    Value(std::string s) : data(std::move(s)) {}
    Value(Array a) : data(std::move(a)) {}
    Value(Object o) : data(std::move(o)) {}
    Value(NodePtr n) : data(std::move(n)) {}
    Value(RefPtr r) : data(std::move(r)) {}

    bool is_null() const {
        return std::holds_alternative<std::monostate>(data) ||
               std::holds_alternative<std::nullptr_t>(data);
    }
    bool is_bool() const { return std::holds_alternative<bool>(data); }
    bool is_int64() const { return std::holds_alternative<int64_t>(data); }
    bool is_double() const { return std::holds_alternative<double>(data); }
    bool is_number() const { return is_int64() || is_double(); }
    bool is_string() const { return std::holds_alternative<std::string>(data); }
    bool is_array() const { return std::holds_alternative<Array>(data); }
    bool is_object() const { return std::holds_alternative<Object>(data); }
    bool is_node() const { return std::holds_alternative<NodePtr>(data); }
    bool is_ref() const { return std::holds_alternative<RefPtr>(data); }

    const std::string& as_string() const { return std::get<std::string>(data); }
    bool as_bool() const { return std::get<bool>(data); }
    int64_t as_int64() const { return std::get<int64_t>(data); }
    double as_double() const { return std::get<double>(data); }
    const Array& as_array() const { return std::get<Array>(data); }
    const Object& as_object() const { return std::get<Object>(data); }
    NodePtr as_node() const { return std::get<NodePtr>(data); }
    RefPtr as_ref() const { return std::get<RefPtr>(data); }

    std::optional<Value> find(const std::string& key) const {
        if (!is_object()) return std::nullopt;
        auto it = as_object().find(key);
        if (it == as_object().end()) return std::nullopt;
        return it->second;
    }

    friend bool operator==(const Value&, const Value&) = default;
};

struct Ref {
    std::string id;
    std::string name;

    static RefPtr server_action(std::string id, std::string name = {}) {
        auto r = std::make_shared<Ref>();
        r->id = std::move(id);
        r->name = std::move(name);
        return r;
    }
};

namespace detail {

inline nlohmann::json to_plain_json(const Value& v) {
    if (v.is_null()) return nlohmann::json();
    if (auto* b = std::get_if<bool>(&v.data)) return *b;
    if (auto* i = std::get_if<int64_t>(&v.data)) return *i;
    if (auto* d = std::get_if<double>(&v.data)) return *d;
    if (auto* s = std::get_if<std::string>(&v.data)) return *s;
    if (auto* a = std::get_if<Array>(&v.data)) {
        nlohmann::json out = nlohmann::json::array();
        for (const auto& e : *a) out.push_back(to_plain_json(e));
        return out;
    }
    if (auto* o = std::get_if<Object>(&v.data)) {
        nlohmann::json out = nlohmann::json::object();
        for (const auto& [k, val] : *o) out[k] = to_plain_json(val);
        return out;
    }
    return nlohmann::json();
}

inline Value from_json_impl(const nlohmann::json& j) {
    if (j.is_null()) return Value();
    if (j.is_boolean()) return Value(j.get<bool>());
    if (j.is_number_integer()) return Value(j.get<int64_t>());
    if (j.is_number_float()) return Value(j.get<double>());
    if (j.is_string()) return Value(j.get<std::string>());
    if (j.is_array()) {
        Array a;
        a.reserve(j.size());
        for (const auto& e : j) a.push_back(from_json_impl(e));
        return Value(std::move(a));
    }
    if (j.is_object()) {
        Object o;
        for (const auto& [k, v] : j.items()) o[k] = from_json_impl(v);
        return Value(std::move(o));
    }
    return Value();
}

}  // namespace detail

inline nlohmann::json to_json(const Value& v) { return detail::to_plain_json(v); }
inline Value from_json(const nlohmann::json& j) { return detail::from_json_impl(j); }

}