#pragma once

#include "httplib.h"

#include "rscpp/flight/row.hpp"
#include "rscpp/html/escape.hpp"
#include "rscpp/runtime/renderer.hpp"

#include "logging.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

namespace rscpp {

// Bounded hand-off between the producer thread and httplib's chunked writer.
class ChunkedPipe {
public:
    ChunkedPipe(size_t max_queue, std::chrono::milliseconds stall_timeout)
        : max_queue_size_(max_queue), stall_timeout_(stall_timeout) {}

    bool push(std::string data) {
        {
            std::unique_lock lock(mutex_);
            if (!space_cv_.wait_for(lock, stall_timeout_,
                                    [this] { return closed_ || queue_.size() < max_queue_size_; }))
                return false;
            if (closed_) return false;
            queue_.push_back(std::move(data));
        }
        data_cv_.notify_all();
        return true;
    }

    bool next(std::string& out) {
        {
            std::unique_lock lock(mutex_);
            data_cv_.wait(lock, [this] { return closed_ || !queue_.empty(); });
            if (queue_.empty()) return false;
            out = std::move(queue_.front());
            queue_.pop_front();
        }
        space_cv_.notify_all();
        return true;
    }

    void close() {
        {
            std::lock_guard lock(mutex_);
            closed_ = true;
        }
        data_cv_.notify_all();
        space_cv_.notify_all();
    }

private:
    std::deque<std::string> queue_;
    mutable std::mutex mutex_;
    std::condition_variable data_cv_;
    std::condition_variable space_cv_;
    bool closed_ = false;
    size_t max_queue_size_;
    std::chrono::milliseconds stall_timeout_;
};

// Adapts a chunked response to the renderer's Flight row sink.
class ChunkSink final : public flight::RowSink {
public:
    explicit ChunkSink(std::function<bool(std::string_view)> fn) : fn_(std::move(fn)) {}
    bool write(std::string_view row) override { return fn_ ? fn_(row) : false; }

private:
    std::function<bool(std::string_view)> fn_;
};

// One chunked HTTP response body, used for both `text/x-component` Flight
// streams and `text/html` SSR streams. The only difference is the content type,
// the log label, and whether a producer crash becomes an error row.
class StreamResponse {
public:
    enum class Kind { FLIGHT, HTML };

    StreamResponse(httplib::Response& res, std::shared_ptr<ChunkedPipe> pipe, Kind kind)
        : pipe_(std::move(pipe)),
          response_(res),
          kind_(kind),
          start_time_(std::chrono::steady_clock::now()),
          first_write_time_(start_time_) {
        response_.set_header("Cache-Control", "no-cache, no-store, must-revalidate");
        response_.set_header("X-Request-ID", g_request_id);
    }

    void start(std::function<void(StreamResponse&)> producer) {
        response_.set_chunked_content_provider(
            content_type(), [pipe = pipe_](size_t, httplib::DataSink& sink) -> bool {
                std::string data;
                if (!pipe->next(data)) {
                    sink.done();
                    return true;
                }
                sink.write(data.data(), data.size());
                return true;
            });

        std::thread([this, producer = std::move(producer)]() {
            try {
                producer(*this);
            } catch (const std::exception& e) {
                RSC_ERROR(label(), " producer failed: ", e.what());
                write_error_row("Internal render error");
            } catch (...) {
                RSC_ERROR(label(), " producer failed: unknown exception");
                write_error_row("Internal render error");
            }
            end();
            const auto now = std::chrono::steady_clock::now();
            const auto total =
                std::chrono::duration_cast<std::chrono::milliseconds>(now - start_time_).count();
            const auto ttfb = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  first_write_time_ - start_time_)
                                  .count();
            RSC_DEBUG(label(), " stream completed: total=", total, "ms ttfb=", ttfb,
                      "ms bytes=", bytes_written_);
        }).detach();
    }

    bool write(std::string_view chunk) {
        const auto now = std::chrono::steady_clock::now();
        if (first_write_) {
            first_write_time_ = now;
            first_write_ = false;
        }
        bytes_written_ += chunk.size();
        if (!pipe_->push(std::string(chunk))) {
            RSC_WARN(label(), " consumer is gone; dropping ", chunk.size(), " bytes");
            return false;
        }
        return true;
    }

    bool write_row(const flight::RowRecord& row) { return write(row.to_wire()); }

    void write_error_row(const std::string& message) {
        if (kind_ != Kind::FLIGHT) return;
        write_row(flight::RowRecord{0, 'E', nlohmann::json{{"message", message}}});
    }

    void end() { pipe_->close(); }

private:
    const char* content_type() const {
        return kind_ == Kind::FLIGHT ? "text/x-component; charset=utf-8"
                                      : "text/html; charset=utf-8";
    }

    const char* label() const { return kind_ == Kind::FLIGHT ? "flight" : "html"; }

    std::shared_ptr<ChunkedPipe> pipe_;
    httplib::Response& response_;
    Kind kind_;
    std::chrono::steady_clock::time_point start_time_;
    std::chrono::steady_clock::time_point first_write_time_;
    size_t bytes_written_ = 0;
    bool first_write_ = true;
};

// Buffers the first paint so the SSR shell can carry the Flight rows that were
// produced before any markup was written.
class SsrStreamSink final : public flight::RowSink, public HtmlSink {
public:
    explicit SsrStreamSink(std::string nonce) : nonce_(std::move(nonce)) {}

    bool write(std::string_view row) override {
        wire_.append(row.data(), row.size());
        return true;
    }

    bool flush_rows() {
        const std::string row = unsent_wire();
        if (row.empty()) return true;
        return resp_ &&
               resp_->write("<script nonce=\"" + nonce_ + "\">$R(" + html::js_string_literal(row) +
                            ")</script>");
    }

    bool write_markup(std::string_view html) override {
        if (body_started_) return resp_ && resp_->write(html);
        markup_.append(html.data(), html.size());
        return true;
    }

    bool begin_body() {
        body_started_ = true;
        sent_ = wire_.size();
        return markup_.empty() || (resp_ && resp_->write(markup_));
    }

    bool write_boundary_content(uint64_t id, std::string_view html,
                                std::string_view row) override {
        if (!resp_) return false;
        const std::string sid = "S:" + flight::hex(id);
        const std::string bid = "B:" + flight::hex(id);

        std::string out;
        out.reserve(html.size() + row.size() + 256);
        out += "<div hidden id=\"" + sid + "\">";
        out.append(html.data(), html.size());
        out += "</div><script nonce=\"" + nonce_ + "\">$RC(\"" + bid + "\",\"" + sid + "\",";
        out += html::js_string_literal(row);
        out += ")</script>";
        return resp_->write(out);
    }

    const std::string& wire() const { return wire_; }

    std::string unsent_wire() {
        std::string out = wire_.substr(sent_);
        sent_ = wire_.size();
        return out;
    }

    void set_response(StreamResponse* resp) { resp_ = resp; }

private:
    std::string nonce_;
    std::string wire_;
    std::string markup_;
    size_t sent_ = 0;
    bool body_started_ = false;
    StreamResponse* resp_ = nullptr;
};

// A resume stream. Carries only what the pre-rendered shell is missing: the
// rows for parked slots and the markup segments that replace their
// placeholders. Nothing already sent is repeated, which is what makes resume
// a continuation instead of a re-render.
class ResumeSink final : public flight::RowSink, public HtmlSink {
public:
    explicit ResumeSink() = default;

    bool write(std::string_view row) override {
        if (!resp_) return false;
        return resp_->write("<script type=\"application/json\" data-rsc-row>" +
                            json_script_body(row) + "</script>");
    }

    // A resume stream has no shell; markup only arrives as boundary segments.
    bool write_markup(std::string_view) override { return true; }

    bool write_boundary_content(uint64_t id, std::string_view html,
                                std::string_view /*row*/) override {
        if (!resp_) return false;
        std::string out = "<template data-rsc-segment id=\"S:";
        out += flight::hex(id);
        out += "\" data-rsc-boundary-id=\"";
        out += flight::hex(id);
        out += "\">";
        out.append(html.data(), html.size());
        out += "</template>";
        return resp_->write(out);
    }

    void set_response(StreamResponse* resp) { resp_ = resp; }

private:
    static std::string json_script_body(std::string_view row) {
        std::string encoded = nlohmann::json(std::string(row)).dump();
        std::string out;
        out.reserve(encoded.size());
        for (char c : encoded) {
            if (c == '<') out += "\\u003c";
            else out += c;
        }
        return out;
    }

    StreamResponse* resp_ = nullptr;
};

}