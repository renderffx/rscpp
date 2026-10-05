#pragma once

#include "rscpp/runtime/context.hpp"

#include "rscpp/flight/decoder.hpp"
#include "rscpp/flight/row.hpp"
#include "rscpp/flight/tags.hpp"
#include "rscpp/html/allowlist.hpp"
#include "rscpp/html/escape.hpp"
#include "rscpp/model/node.hpp"
#include "rscpp/model/registry.hpp"
#include "rscpp/model/suspend.hpp"
#include "rscpp/util/hash.hpp"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace rscpp {

using Json = nlohmann::json;

class HtmlSink {
public:
    virtual ~HtmlSink() = default;

    virtual bool write_markup(std::string_view html) = 0;

    virtual bool write_boundary_content(uint64_t id, std::string_view html,
                                        std::string_view row) = 0;
};

class Renderer : public std::enable_shared_from_this<Renderer> {
public:
    static constexpr size_t kInlineTextLimit = 1024;

    explicit Renderer(const Registry& reg, std::shared_ptr<Context> ctx = nullptr)
        : reg_(reg), ctx_(ctx ? std::move(ctx) : std::make_shared<Context>()) {}

    void init(const RequestInfo& req) { ctx_->init(req, reg_); }
    void enable_html(bool v = true) { html_enabled_ = v; }

    void set_sink(flight::RowSink* sink) { sink_ = sink; }
    void set_html_sink(HtmlSink* sink) { html_sink_ = sink; }

    const std::string& html() const { return html_; }
    const std::vector<flight::RowRecord>& rows() const { return rows_; }

    bool aborted() const {
        std::lock_guard lock(queue_mutex_);
        return aborted_;
    }

    size_t pending_count() const {
        std::lock_guard lock(queue_mutex_);
        return pending_;
    }

    void drain_pending() {
        for (;;) {
            std::function<void()> task;
            {
                std::unique_lock lock(queue_mutex_);
                if (aborted_) return;
                if (tasks_.empty()) {
                    if (pending_ == 0) return;
                    queue_cv_.wait(lock);
                    continue;
                }
                task = std::move(tasks_.front());
                tasks_.pop_front();
            }
            task();
        }
    }

    void render(const NodePtr& root) {
        rows_.clear();
        html_.clear();
        extra_rows_.clear();
        client_refs_.clear();
        action_refs_.clear();
        next_id_ = 1;
        {
            std::lock_guard lock(queue_mutex_);
            tasks_.clear();
            pending_ = 0;
            aborted_ = false;
        }

        std::string html_buf;
        Json root_model = nullptr;

        try {
            root_model = serialize_node(root, html_buf);
        } catch (const Suspend& s) {
            if (s.future) s.future->wait();
            html_buf.clear();
            try {
                root_model = serialize_node(root, html_buf);
            } catch (const std::exception& ex) {
                emit_error_row(ex.what());
                root_model = Json(nullptr);
                html_buf.clear();
            }
        } catch (const std::exception& ex) {
            emit_error_row(ex.what());
            root_model = Json(nullptr);
            html_buf.clear();
        }

        flush_extra_rows();
        emit(flight::RowRecord{0, 0, std::move(root_model)});

        if (html_sink_) {
            if (!html_sink_->write_markup(html_buf)) abort_now();
            html_.clear();
        } else {
            html_ = std::move(html_buf);
        }
    }

private:
    static std::string error_digest(const std::string& msg) {
        return util::hex_digest(msg);
    }

    void abort_now() {
        {
            std::lock_guard lock(queue_mutex_);
            aborted_ = true;
        }
        queue_cv_.notify_all();
    }

    void emit(flight::RowRecord&& rec) {
        if (aborted()) return;
        if (sink_) {
            if (!sink_->write(rec.to_wire())) abort_now();
            return;
        }
        rows_.push_back(std::move(rec));
    }

    void flush_extra_rows() {
        for (auto& r : extra_rows_) emit(std::move(r));
        extra_rows_.clear();
    }

    void emit_error_row(const std::string& msg) {
        extra_rows_.push_back(flight::RowRecord{
            next_id_++, 'E', Json{{"digest", error_digest(msg)}, {"message", msg}}});
    }

    uint64_t reserve_chunk() {
        const uint64_t id = next_id_++;
        std::lock_guard lock(queue_mutex_);
        ++pending_;
        return id;
    }

    std::string fulfill_chunk(uint64_t id, flight::RowRecord&& rec) {
        std::string wire = rec.to_wire();
        flush_extra_rows();
        emit(std::move(rec));
        {
            std::lock_guard lock(queue_mutex_);
            if (pending_ > 0) --pending_;
        }
        queue_cv_.notify_all();
        return wire;
    }

    void enqueue(std::function<void()> task) {
        {
            std::lock_guard lock(queue_mutex_);
            if (aborted_) return;
            tasks_.push_back(std::move(task));
        }
        queue_cv_.notify_all();
    }

    uint64_t ensure_client_ref(const std::string& module, const std::string& export_name) {
        const std::string key = module + "#" + export_name;
        auto it = client_refs_.find(key);
        if (it != client_refs_.end()) return it->second;

        const uint64_t id = next_id_++;
        client_refs_[key] = id;
        extra_rows_.push_back(
            flight::RowRecord{id, 'I',
                              Json{{"id", module}, {"chunks", Json::array({module})},
                                   {"name", export_name}}});
        return id;
    }

    uint64_t ensure_action_ref(const std::string& action_id, const std::string& name) {
        auto it = action_refs_.find(action_id);
        if (it != action_refs_.end()) return it->second;

        const uint64_t id = next_id_++;
        action_refs_[action_id] = id;
        Json meta{{"$$typeof", "REACT_SERVER_REFERENCE"}, {"id", action_id}};
        if (!name.empty() && name != action_id) meta["name"] = name;
        meta["bound"] = nullptr;
        extra_rows_.push_back(flight::RowRecord{id, 0, std::move(meta)});
        return id;
    }

    Json serialize_text(const std::string& text) {
        if (text.size() < kInlineTextLimit) return Json(text);

        const uint64_t id = next_id_++;
        extra_rows_.push_back(flight::RowRecord{id, 'T', Json(text)});
        return Json("$" + flight::hex(id));
    }

    // Resolves one DYNAMIC hop. Returns the replacement node, which may itself be
    // DYNAMIC — `serialize_node` re-dispatches on it, so a component that
    // returns another component reference resolves transitively.
    NodePtr resolve_node(const NodePtr& n) {
        if (!n) return n;
        if (n->type != NodeType::DYNAMIC) return n;
        if (n->component_fn) return n->component_fn(*ctx_);
        if (auto sc = ctx_->registry().get_server(n->tag)) return sc->fn(*ctx_);
        throw std::runtime_error("Unknown server component: " + n->tag);
    }

    Json serialize_value(const Value& v) {
        if (v.is_bool()) return Json(v.as_bool());
        if (v.is_int64()) return Json(v.as_int64());
        if (v.is_double()) return Json(v.as_double());
        if (v.is_string()) return serialize_text(v.as_string());
        if (v.is_null()) return Json(nullptr);

        if (v.is_array()) {
            Json arr = Json::array();
            for (const auto& e : v.as_array()) arr.push_back(serialize_value(e));
            return arr;
        }
        if (v.is_object()) {
            Json obj = Json::object();
            for (const auto& [k, val] : v.as_object()) obj[k] = serialize_value(val);
            return obj;
        }
        if (v.is_node()) {
            std::string ignored;
            return serialize_node(v.as_node(), ignored);
        }
        if (v.is_ref()) {
            const Ref& r = *v.as_ref();
            return Json("$F" + flight::hex(ensure_action_ref(r.id, r.name)));
        }
        return Json(nullptr);
    }

    Json serialize_props(const Props& props) {
        Json obj = Json::object();
        for (const auto& [k, v] : props) obj[k] = serialize_value(v);
        return obj;
    }

    Json serialize_children(const Children& children, std::string& html_out) {
        if (children.empty()) return Json(nullptr);
        if (children.size() == 1) {
            std::string child_html;
            Json j = serialize_node(children[0], child_html);
            html_out += child_html;
            return j;
        }
        Json arr = Json::array();
        for (const auto& c : children) {
            std::string child_html;
            Json j = serialize_node(c, child_html);
            html_out += child_html;
            arr.push_back(std::move(j));
        }
        return arr;
    }

    Json serialize_node(const NodePtr& orig, std::string& html_out) {
        NodePtr node = resolve_node(orig);
        if (!node) return Json(nullptr);

        switch (node->type) {
            case NodeType::ELEMENT:
                return serialize_element_node(node, html_out);
            case NodeType::TEXT: {
                auto it = node->props.find("text");
                const std::string txt =
                    (it != node->props.end() && it->second.is_string()) ? it->second.as_string() : "";
                if (html_enabled_) html::escape_text(html_out, txt);
                return serialize_text(txt);
            }
            case NodeType::FRAGMENT: {
                std::string frag_html;
                Json j = serialize_children(node->children, frag_html);
                html_out += frag_html;
                return j;
            }
            case NodeType::CLIENT:
                return serialize_client_node(node, html_out);
            case NodeType::SUSPENSE:
                return serialize_suspense_node(node, html_out);
            case NodeType::ERROR_BOUNDARY:
                return serialize_error_boundary_node(node, html_out);
            case NodeType::DYNAMIC:
                // A component that resolved to a bare reference with no
                // component_fn and no registry entry. Rendering nothing is
                // correct; silently rendering the tag name would not be.
                return Json(nullptr);
        }
        return Json(nullptr);
    }

    Json serialize_element_node(const NodePtr& node, std::string& html_out) {
        Json props = serialize_props(node->props);

        std::string child_html;
        Json children = serialize_children(node->children, child_html);
        if (!children.is_null()) props["children"] = std::move(children);

        if (html_enabled_) {
            if (!html::is_safe_tag(node->tag)) {
                html_out += child_html;
                return flight::make_element(Json(node->tag), node->key, std::move(props));
            }
            html_out += "<";
            html_out += node->tag;
            for (const auto& [name, value] : props.items()) {
                if (name == "children") continue;
                if (name.rfind("on", 0) == 0) continue;
                if (name == "style") continue;
                if (!html::is_safe_attr_name(name)) continue;
                if (value.is_null()) continue;

                std::string attr = name;
                if (name == "className") attr = "class";
                else if (name == "htmlFor") attr = "for";

                if (!html::is_known_html_attr(attr)) continue;

                std::string sval;
                if (value.is_string()) sval = value.get<std::string>();
                else if (value.is_number() || value.is_boolean()) sval = value.dump();
                else continue;

                if (html::is_url_attr(attr) && !html::is_safe_url(sval)) continue;

                html_out += " ";
                html_out += attr;
                html_out += "=\"";
                html::escape_text(html_out, sval);
                html_out += "\"";
            }
            html_out += ">";
            if (!html::is_void_element(node->tag)) {
                html_out += child_html;
                html_out += "</";
                html_out += node->tag;
                html_out += ">";
            }
        }
        return flight::make_element(Json(node->tag), node->key, std::move(props));
    }

    Json serialize_client_node(const NodePtr& node, std::string& html_out) {
        const uint64_t id = ensure_client_ref(node->tag, node->export_name);

        Json props = serialize_props(node->props);
        std::string child_html;
        Json children = serialize_children(node->children, child_html);
        if (!node->children.empty()) props["children"] = std::move(children);

        if (html_enabled_) {
            html_out += "<template data-rsc-client=\"";
            html_out += flight::hex(id);
            html_out += "\"></template>";
            html_out += child_html;
        }
        return flight::make_element(Json("$L" + flight::hex(id)), node->key, std::move(props));
    }

    Json serialize_suspense_node(const NodePtr& node, std::string& html_out) {
        std::string fallback_html;
        Json fallback =
            node->fallback ? serialize_node(node->fallback, fallback_html) : Json(nullptr);

        std::string children_html;
        Json children = Json(nullptr);
        try {
            children = serialize_children(node->children, children_html);
        } catch (const Suspend& s) {
            return postpone_boundary(node, std::move(s), std::move(fallback),
                                     std::move(fallback_html), html_out);
        } catch (const std::exception& ex) {
            emit_error_row(ex.what());
            html_out += fallback_html;
            Json fallback_children = fallback;
            return flight::make_element(Json(flight::REACT_SUSPENSE_TYPE), node->key,
                                        Json{{"fallback", std::move(fallback)},
                                             {"children", std::move(fallback_children)}});
        }

        html_out += children_html;
        return flight::make_element(Json(flight::REACT_SUSPENSE_TYPE), node->key,
                                    Json{{"fallback", std::move(fallback)},
                                         {"children", std::move(children)}});
    }

    Json postpone_boundary(const NodePtr& node, Suspend suspend, Json fallback,
                           std::string fallback_html, std::string& html_out) {
        const uint64_t id = reserve_chunk();

        html_out += "<div id=\"B:";
        html_out += flight::hex(id);
        html_out += "\" class=\"rsc-boundary\" data-rsc-boundary=\"";
        html_out += flight::hex(id);
        html_out += "\">";
        html_out += fallback_html;
        html_out += "</div>";

        schedule_retry(node, id, std::move(suspend.future));

        return flight::make_element(Json(flight::REACT_SUSPENSE_TYPE), node->key,
                                    Json{{"fallback", std::move(fallback)},
                                         {"children", Json("$L" + flight::hex(id))}});
    }

    void schedule_retry(const NodePtr& node, uint64_t id,
                        std::shared_ptr<std::shared_future<Value>> future) {
        if (!future) {
            enqueue([this, node, id]() { run_boundary(node, id); });
            return;
        }

        std::weak_ptr<Renderer> self = shared_from_this();
        std::thread([future, self, node, id]() {
            future->wait();
            if (auto r = self.lock())
                r->enqueue([r, node, id]() { r->run_boundary(node, id); });
        }).detach();
    }

    void run_boundary(const NodePtr& node, uint64_t id) {
        if (aborted()) return;
        try {
            std::string html;
            Json model = serialize_children(node->children, html);
            const std::string wire =
                fulfill_chunk(id, flight::RowRecord{id, 0, std::move(model)});
            if (html_sink_ && !html_sink_->write_boundary_content(id, html, wire)) {
                abort_now();
            }
        } catch (const Suspend& s) {
            schedule_retry(node, id, std::move(s.future));
        } catch (const std::exception& ex) {
            const std::string msg = ex.what();
            fulfill_chunk(id, flight::RowRecord{id, 'E',
                                               Json{{"digest", error_digest(msg)},
                                                    {"message", msg}}});
        } catch (...) {
            const std::string msg = "suspended boundary failed to resolve";
            fulfill_chunk(id, flight::RowRecord{id, 'E',
                                               Json{{"digest", error_digest(msg)},
                                                    {"message", msg}}});
        }
    }

    Json serialize_error_boundary_node(const NodePtr& node, std::string& html_out) {
        std::string fallback_html;
        Json fallback =
            node->fallback ? serialize_node(node->fallback, fallback_html) : Json(nullptr);
        try {
            std::string child_html;
            Json j = serialize_children(node->children, child_html);
            html_out += child_html;
            return j;
        } catch (const std::exception& ex) {
            emit_error_row(ex.what());
            html_out += fallback_html;
            return fallback;
        }
    }

    const Registry& reg_;
    std::shared_ptr<Context> ctx_;
    uint64_t next_id_ = 1;
    bool html_enabled_ = false;

    std::vector<flight::RowRecord> rows_;
    std::vector<flight::RowRecord> extra_rows_;
    std::string html_;

    std::unordered_map<std::string, uint64_t> client_refs_;
    std::unordered_map<std::string, uint64_t> action_refs_;

    flight::RowSink* sink_ = nullptr;
    HtmlSink* html_sink_ = nullptr;
    std::deque<std::function<void()>> tasks_;
    mutable std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    size_t pending_ = 0;
    bool aborted_ = false;
};

}