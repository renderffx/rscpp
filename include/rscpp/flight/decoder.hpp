#pragma once

#include "rscpp/flight/row.hpp"
#include "rscpp/flight/tags.hpp"

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace rscpp::flight {

class StreamDecoder {
public:
    std::vector<RawRow> feed(std::string_view data) {
        std::vector<RawRow> out;
        if (failed_) return {};
        // Bound total buffered bytes absolutely. Checked before the append so
        // memory never exceeds this, regardless of what the peer declares.
        if (data.size() > kMaxBufferBytes - std::min(buf_.size(), kMaxBufferBytes))
            return fail_out(out, "stream buffer limit exceeded");
        buf_.append(data.data(), data.size());

        while (true) {
            const size_t colon = buf_.find(':', head_);
            if (colon == std::string::npos) {
                compact();
                break;
            }
            if (colon == head_) return fail_out(out, "row id is empty");

            // kMaxIdBytes must bound the id on *every* path. It used to be
            // checked only when no colon was found, so a colon present but a
            // megabyte-long id was parsed with no width limit at all.
            if (colon - head_ > kMaxIdBytes) return fail_out(out, "row id too long");

            RawRow row;
            if (!parse_hex(std::string_view(buf_).substr(head_, colon - head_), row.id))
                return fail_out(out, "invalid hexadecimal row id");

            if (colon + 1 >= buf_.size()) break;

            const char tag = classify_tag(buf_[colon + 1]);

            if (tag != 0 && is_length_prefixed_tag(tag)) {
                const size_t comma = buf_.find(',', colon + 2);
                // The digit guard belongs here, NOT inside the `comma == npos`
                // branch: once the comma has landed the digit count was never
                // validated, so a megabyte of digits reached parse_hex.
                if (buf_.size() - (colon + 2) > kMaxPayloadLenDigits &&
                    (comma == std::string::npos || comma - (colon + 2) > kMaxPayloadLenDigits))
                    return fail_out(out, "payload length prefix too long");
                if (comma == std::string::npos) break;
                uint64_t len = 0;
                if (!parse_hex(std::string_view(buf_).substr(colon + 2, comma - colon - 2), len))
                    return fail_out(out, "invalid hexadecimal payload length");
                // The digit count bounds the *encoding* of the length, not the
                // length itself. 16 hex digits reach 2^64-1; cap the value.
                if (len > kMaxPayloadBytes) return fail_out(out, "payload length exceeds limit");

                const size_t body = comma + 1;
                if (buf_.size() - body < len) break;

                row.tag = tag;
                row.is_text = (tag == 'T');
                row.payload.assign(buf_, body, static_cast<size_t>(len));
                head_ = body + static_cast<size_t>(len);
                out.push_back(std::move(row));
                continue;
            }

            const size_t nl = buf_.find('\n', colon);
            if (nl == std::string::npos) break;

            const std::string line = buf_.substr(head_, nl - head_);
            head_ = nl + 1;

            std::string err;
            auto parsed = parse_row(line, err);
            if (!parsed) return fail_out(out, err);
            out.push_back(std::move(*parsed));
        }
        compact();
        return out;
    }

    bool ok() const { return !failed_; }
    const std::string& error() const { return error_; }

private:
    // A uint64 is 16 hex digits; that is the width of both ids and lengths.
    static constexpr size_t kMaxIdBytes = 16;
    static constexpr size_t kMaxPayloadLenDigits = 16;
    // The digit count bounds the *encoding* of the length, never the value.
    // 16 digits reach 2^64-1, so the value needs its own bound. The largest
    // real row in this project is ~5.6 KB (/heavy); these leave ~175x headroom.
    static constexpr uint64_t kMaxPayloadBytes = 1u << 20;   // 1 MiB per row
    static constexpr size_t kMaxBufferBytes = 4u << 20;      // 4 MiB buffered
    static constexpr size_t kCompactThreshold = 64 * 1024;

    // Latches the failure but RETURNS the rows already decoded. Returning {}
    // used to discard them, so a single malformed row silently destroyed every
    // good row that arrived in the same chunk -- unrecoverable in a stream.
    std::vector<RawRow> fail_out(std::vector<RawRow>& out, const std::string& msg) {
        failed_ = true;
        error_ = msg;
        buf_.clear();
        head_ = 0;
        return out;
    }

    void compact() {
        if (head_ == 0) return;
        if (head_ >= buf_.size()) { buf_.clear(); head_ = 0; return; }
        if (head_ >= kCompactThreshold) { buf_.erase(0, head_); head_ = 0; }
    }

    std::string buf_;
    size_t head_ = 0;
    std::string error_;
    bool failed_ = false;
};

}