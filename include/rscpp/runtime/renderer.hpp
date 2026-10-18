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

#include <algorithm>
#include <chrono>
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

// A suspension parked for later. Real React keeps the same thing inside its
// request/task objects: the subtree (or slot) that suspended, the wakeup
// source, and where in the output it belongs. The state is *in-process only*,
// exactly like React's `postponed` object -- it holds live context and nodes,
// not bytes.
struct PostponedState {
    struct Pending {
        uint64_t id = 0;
        NodePtr node;
        std::shared_ptr<std::shared_future<Value>> future;
        uint8_t kind = 0;  // 0 = root row, 1 = html boundary, 2 = flight slot
        std::string fallback_html;
    };

    uint64_t next_id = 1;
    std::vector<Pending> pending;
    std::shared_ptr<Context> ctx;
    bool html = false;
};

class HtmlSink {
public:
    virtual ~HtmlSink() = default;

    virtual bool write_markup(std::string_view html) = 0;

    virtual bool flush_rows() { return true; }

    virtual bool write_boundary_content(uint64_t id, std::string_view html,
                                        std::string_view row) = 0;
};

class Renderer : public std::enable_shared_from_this<Renderer> {
public:
    static constexpr size_t kInlineTextLimit = 1024;

    static constexpr uint8_t KIND_ROOT = 0;
    static constexpr uint8_t KIND_BOUNDARY = 1;
    static constexpr uint8_t KIND_SLOT = 2;

    explicit Renderer(const Registry& reg, std::shared_ptr<Context> ctx = nullptr)
        : reg_(reg), ctx_(ctx ? std::move(ctx) : std::make_shared<Context>()) {}

    void init(const RequestInfo& req) { ctx_->init(req, reg_); }
    void enable_html(bool v = true) { html_enabled_ = v; }

    void set_sink(flight::RowSink* sink) { sink_ = sink; }
    void set_html_sink(HtmlSink* sink) { html_sink_ = sink; }

    const std::vector<flight::RowRecord>& rows() const { return rows_; }

    bool aborted() const {
        std::lock_guard lock(queue_mutex_);
        return aborted_;
    }

    // Streaming render. Never blocks on a pending future: whatever is ready is
    // serialized now, suspensions are parked, and `drain_pending()` reacts to
    // them as they resolve.
    void render(const NodePtr& root) {
        postpone_mode_ = false;
        render_impl(root);
    }

    // PPR pre-render. Serialize what is ready and park every suspension
    // instead of waiting for it. The shell (markup + rows) is complete for
    // everything that resolved; `resume` later produces only the missing
    // segments. This is what makes the first byte fast even when data takes
    // arbitrarily long.
    PostponedState prerender(const NodePtr& root) {
        postpone_mode_ = true;
        render_impl(root);
        PostponedState st;
        st.next_id = next_id_;
        st.pending = std::move(postponed_slots_);
        st.ctx = ctx_;
        st.html = html_sink_ != nullptr;
        postponed_slots_.clear();
        return st;
    }

    // Continue a postponement in the same id space with the same data
    // context. Only parked slots are serialized; nothing already sent is
    // re-emitted (React's resume behaves exactly this way: the second stream
    // carries only the missing segments and their rows).
    void resume(const PostponedState& state) {
        reset_state();
        next_id_ = state.next_id;
        if (state.ctx) ctx_ = state.ctx;
        postpone_mode_ = false;
        {
            std::lock_guard lock(queue_mutex_);
            for (const auto& p : state.pending) {
                if (p.future) waits_.push_back(p);
            }
        }
        drain_pending();
    }

    void drain_pending() {
        for (;;) {
            std::function<void()> task;
            {
                std::unique_lock lock(queue_mutex_);
                if (aborted_) return;
                promote_ready_locked();
                if (!tasks_.empty()) {
                    task = std::move(tasks_.front());
                    tasks_.pop_front();
                } else if (waits_.empty()) {
                    return;
                } else {
                    // Real React polls its work loop; we do the same instead
                    // of parking one thread per suspension.
                    queue_cv_.wait_for(lock, std::chrono::milliseconds(5));
                    continue;
                }
            }
            task();
        }
    }

private:
    static std::string error_digest(const std::string& msg) {
        return util::hex_digest(msg);
    }

    static std::string escape_dollar(std::string s) {
        // `$`-prefixed strings collide with the reference encoding, so React
        // prefixes one more `$` when the literal is inlined in the JSON model.
        if (!s.empty() && s[0] == '$') s.insert(s.begin(), '$');
        return s;
    }

    static std::string root_placeholder() {
        return "<div id=\"B:0\" class=\"rsc-boundary\" data-rsc-boundary=\"0\"></div>";
    }

    void abort_now() {
        {
            std::lock_guard lock(queue_mutex_);
            aborted_ = true;
        }
        queue_cv_.notify_all();
    }

    void reset_state() {
        rows_.clear();
        html_.clear();
        extra_rows_.clear();
        client_refs_.clear();
        action_refs_.clear();
        next_id_ = 1;
        postponed_slots_.clear();
        std::lock_guard lock(queue_mutex_);
        tasks_.clear();
        waits_.clear();
        aborted_ = false;
    }

    void render_impl(const NodePtr& root) {
        reset_state();

        std::string html_buf;
        Json root_model = nullptr;

        try {
            root_model = serialize_node(root, html_buf);
            flush_extra_rows();
            emit(flight::RowRecord{0, 0, std::move(root_model)});
        } catch (const Suspend& s) {
            // Nothing has been written for the root yet. In HTML mode the
            // shell keeps a placeholder that the retry/resume patches; in
            // Flight mode row 0 simply arrives later, which real React does
            // too (rows for other tasks still stream first).
            html_buf = html_sink_ ? root_placeholder() : std::string();
            park_or_wait(0, root, s.future, KIND_ROOT);
        } catch (const std::exception& ex) {
            // A root failure replaces row 0 with an `E` row -- the same shape
            // `run_root` produces on a retry. Emitting a null model row instead
            // would leave the client with a chunk that has no error to render.
            const std::string msg = ex.what();
            flush_extra_rows();
            emit(flight::RowRecord{
                0, 'E', Json{{"digest", error_digest(msg)}, {"message", msg}}});
            html_buf.clear();
        }

        if (html_sink_) {
            if (!html_sink_->write_markup(html_buf)) abort_now();
            html_.clear();
        } else {
            html_ = std::move(html_buf);
        }
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

    uint64_t reserve_chunk() { return next_id_++; }

    std::string fulfill_chunk(uint64_t id, flight::RowRecord&& rec) {
        std::string wire = rec.to_wire();
        flush_extra_rows();
        emit(std::move(rec));
        return wire;
    }

    static constexpr uint64_t k_no_parked_id = ~static_cast<uint64_t>(0);

    // ctx.async_once caches futures per Context, so when a failed attempt is
    // re-serialized (the root retry, or a boundary whose root re-rendered) the
    // very same future comes back for the same suspension point. React keys
    // Flight rows by promise identity for exactly this reason; minting a
    // second id would emit the row and segment twice. Node identity is not
    // enough: re-executed components rebuild their subtrees, so the Suspense
    // node object around an unchanged future is brand new.
    uint64_t lookup_parked(const std::shared_ptr<std::shared_future<Value>>& future,
                           uint8_t kind) const {
        if (!future) return k_no_parked_id;
        std::lock_guard lock(queue_mutex_);
        for (const auto& p : waits_) {
            if (p.kind == kind && p.future.get() == future.get()) return p.id;
        }
        return k_no_parked_id;
    }

    uint64_t park_or_wait(uint64_t id, const NodePtr& node,
                          std::shared_ptr<std::shared_future<Value>> future, uint8_t kind,
                          std::string fallback_html = {}) {
        std::lock_guard lock(queue_mutex_);
        if (future) {
            for (const auto& p : waits_) {
                if (p.kind == kind && p.future.get() == future.get()) return p.id;
            }
        }
        PostponedState::Pending p{id, node, std::move(future), kind, std::move(fallback_html)};
        if (postpone_mode_) {
            postponed_slots_.push_back(std::move(p));
            return id;
        }
        if (!p.future) return id;  // nothing to wake on; leave parked in the output
        if (aborted_) return id;
        waits_.push_back(std::move(p));
        queue_cv_.notify_all();
        return id;
    }

    std::function<void()> make_task(PostponedState::Pending p) {
        std::weak_ptr<Renderer> self = shared_from_this();
        return [self, p]() {
            if (auto r = self.lock()) r->run_pending(p);
        };
    }

    void promote_ready_locked() {
        for (auto it = waits_.begin(); it != waits_.end();) {
            const bool ready =
                it->future &&
                it->future->wait_for(std::chrono::seconds(0)) == std::future_status::ready;
            if (!ready) {
                ++it;
                continue;
            }
            tasks_.push_back(make_task(*it));
            it = waits_.erase(it);
        }
    }

    void run_pending(PostponedState::Pending p) {
        if (aborted()) return;
        if (p.kind == KIND_ROOT) {
            run_root(std::move(p));
        } else if (p.kind == KIND_BOUNDARY) {
            run_boundary(p.node, p.id);
        } else {
            run_slot(std::move(p));
        }
    }

    void run_root(const PostponedState::Pending& p) {
        std::string html;
        try {
            Json model = serialize_node(p.node, html);
            if (html_sink_) {
                // Same shape as run_boundary: the row goes through the row
                // sink (so resume sinks, which have no shell, actually deliver
                // it), then flush, then the segment. write_boundary_content
                // only receives the row for sinks that embed it in $RC.
                const std::string wire = fulfill_chunk(0, flight::RowRecord{0, 0, std::move(model)});
                if (!html_sink_->flush_rows()) abort_now();
                if (!html_sink_->write_boundary_content(0, html, wire)) abort_now();
            } else {
                flush_extra_rows();
                emit(flight::RowRecord{0, 0, std::move(model)});
            }
        } catch (const Suspend& s) {
            park_or_wait(0, p.node, s.future, KIND_ROOT);
        } catch (const std::exception& ex) {
            const std::string msg = ex.what();
            const Json payload{{"digest", error_digest(msg)}, {"message", msg}};
            if (html_sink_) {
                // No segment: an empty S:0 would patch the root placeholder to
                // nothing (blank page) and consume its data-rsc-boundary before
                // the client could render the error. The E row alone lets the
                // client turn the placeholder into an error fallback.
                fulfill_chunk(0, flight::RowRecord{0, 'E', payload});
                if (!html_sink_->flush_rows()) abort_now();
            } else {
                emit(flight::RowRecord{0, 'E', payload});
            }
        }
    }

    void run_slot(const PostponedState::Pending& p) {
        std::string ignored;
        try {
            Json model = serialize_node(p.node, ignored);
            flush_extra_rows();
            fulfill_chunk(p.id, flight::RowRecord{p.id, 0, std::move(model)});
        } catch (const Suspend& s) {
            park_or_wait(p.id, p.node, s.future, KIND_SLOT);
        } catch (const std::exception& ex) {
            const std::string msg = ex.what();
            fulfill_chunk(p.id,
                          flight::RowRecord{p.id, 'E', Json{{"digest", error_digest(msg)}}});
        }
    }

    void run_boundary(const NodePtr& node, uint64_t id) {
        if (aborted()) return;
        try {
            std::string html;
            Json model = serialize_children(node->children, html);
            const std::string wire = fulfill_chunk(id, flight::RowRecord{id, 0, std::move(model)});
            if (html_sink_) {
                if (!html_sink_->flush_rows()) abort_now();
                if (!html_sink_->write_boundary_content(id, html, wire)) abort_now();
            }
        } catch (const Suspend& s) {
            park_or_wait(id, node, std::move(s.future), KIND_BOUNDARY);
        } catch (const std::exception& ex) {
            const std::string msg = ex.what();
            fulfill_chunk(id, flight::RowRecord{id, 'E', Json{{"digest", error_digest(msg)}}});
        } catch (...) {
            const std::string msg = "suspended boundary failed to resolve";
            fulfill_chunk(id, flight::RowRecord{id, 'E', Json{{"digest", error_digest(msg)}}});
        }
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

    // Large strings travel out of line in T rows, raw and unescaped -- a T
    // row is not re-parsed as a model, so `$$` escaping would corrupt it.
    Json outline_text(const std::string& raw) {
        const uint64_t id = next_id_++;
        extra_rows_.push_back(flight::RowRecord{id, 'T', Json(raw)});
        return Json("$" + flight::hex(id));
    }

    Json serialize_text(const std::string& text) {
        if (text.size() >= kInlineTextLimit) return outline_text(text);
        return Json(escape_dollar(text));
    }

    // Resolves one DYNAMIC hop. Returns the replacement node, which may itself be
    // DYNAMIC -- `serialize_node` re-dispatches on it, so a component that
    // returns another component reference resolves transitively.
    NodePtr resolve_node(const NodePtr& n) {
        if (!n) return n;
        if (n->type != NodeType::DYNAMIC) return n;
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
            if (html_sink_) {
                std::string ignored;
                return serialize_node(v.as_node(), ignored);
            }
            try {
                std::string ignored;
                return serialize_node(v.as_node(), ignored);
            } catch (const Suspend& s) {
                // A suspended *value* is a value reference; a suspended child
                // slot is `$L`. The client resolves both when the row lands.
                const uint64_t id = park_or_wait(reserve_chunk(), v.as_node(), s.future, KIND_SLOT);
                return Json("$" + flight::hex(id));
            }
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
            Json j = serialize_child(children[0], child_html);
            html_out += child_html;
            return j;
        }
        Json arr = Json::array();
        for (const auto& c : children) {
            std::string child_html;
            Json j = serialize_child(c, child_html);
            html_out += child_html;
            arr.push_back(std::move(j));
        }
        return arr;
    }

    // Flight only: a suspension inside one slot becomes a targeted `$L`
    // reference and the siblings keep their serialized form. React does the
    // same -- only the suspended property is replaced and retried.
    Json serialize_child(const NodePtr& child, std::string& html_out) {
        if (html_sink_) return serialize_node(child, html_out);
        try {
            return serialize_node(child, html_out);
        } catch (const Suspend& s) {
            const uint64_t id = park_or_wait(reserve_chunk(), child, s.future, KIND_SLOT);
            return Json("$L" + flight::hex(id));
        }
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
                // A component reference with no registry entry. Rendering
                // nothing is correct; rendering the tag name would not be.
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
                // `open` is a real HTML attribute (details/dialog); only
                // event-handler props start with `on`.
                if (name != "open" && name.rfind("on", 0) == 0) continue;
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
        if (!html_sink_) {
            // Flight: Suspense has no special server behavior. Slots below it
            // suspend individually; the client's nearest boundary shows the
            // fallback until their rows land.
            std::string fallback_html;
            Json fallback =
                node->fallback ? serialize_node(node->fallback, fallback_html) : Json(nullptr);
            Json props = serialize_props(node->props);
            props["fallback"] = std::move(fallback);
            std::string child_html;
            Json children = serialize_children(node->children, child_html);
            props["children"] = std::move(children);
            return flight::make_element(Json(flight::REACT_SUSPENSE_TYPE), node->key,
                                        std::move(props));
        }

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
        } catch (const std::exception&) {
            // The boundary failed synchronously: the fallback itself is the
            // rendered result. No slot was reserved, so no `E` row is emitted.
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
        uint64_t id = lookup_parked(suspend.future, KIND_BOUNDARY);
        if (id == k_no_parked_id) id = reserve_chunk();

        html_out += "<div id=\"B:";
        html_out += flight::hex(id);
        html_out += "\" class=\"rsc-boundary\" data-rsc-boundary=\"";
        html_out += flight::hex(id);
        html_out += "\">";
        html_out += fallback_html;
        html_out += "</div>";

        park_or_wait(id, node, std::move(suspend.future), KIND_BOUNDARY, std::move(fallback_html));

        return flight::make_element(Json(flight::REACT_SUSPENSE_TYPE), node->key,
                                    Json{{"fallback", std::move(fallback)},
                                         {"children", Json("$L" + flight::hex(id))}});
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
        } catch (const std::exception&) {
            // Same as the sync-boundary case: the fallback is the result.
            html_out += fallback_html;
            return fallback;
        }
    }

    const Registry& reg_;
    std::shared_ptr<Context> ctx_;
    uint64_t next_id_ = 1;
    bool html_enabled_ = false;
    bool postpone_mode_ = false;

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
    std::vector<PostponedState::Pending> waits_;
    std::vector<PostponedState::Pending> postponed_slots_;
    bool aborted_ = false;
};

}
