#pragma once

#include "rscpp/flight/tags.hpp"

#include "json.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace rscpp::flight {

using Json = nlohmann::json;

struct RawRow {
    uint64_t id = 0;
    char tag = 0;
    std::string payload;
    bool is_text = false;
};

inline std::optional<RawRow> parse_row(std::string_view line, std::string& error) {
    error.clear();
    if (line.empty()) {
        error = "empty row";
        return std::nullopt;
    }
    const size_t colon = line.find(':');
    if (colon == std::string_view::npos) {
        error = "missing colon separator";
        return std::nullopt;
    }
    RawRow r;
    if (!parse_hex(line.substr(0, colon), r.id)) {
        error = "invalid hexadecimal row id";
        return std::nullopt;
    }

    const std::string_view rest = line.substr(colon + 1);
    if (rest.empty()) {
        error = "row has no payload";
        return std::nullopt;
    }

    const char tag = classify_tag(rest[0]);
    if (tag != 0 && is_length_prefixed_tag(tag)) {
        error = "length-prefixed row passed to a newline framer";
        return std::nullopt;
    }
    if (tag != 0) {
        r.tag = tag;
        r.payload = std::string(rest.substr(1));
    } else {
        r.tag = 0;
        r.payload = std::string(rest);
    }

    if (r.tag == 0 && !Json::accept(r.payload)) {
        error = "model row is not valid JSON";
        return std::nullopt;
    }
    return r;
}

struct RowRecord {
    uint64_t id = 0;
    char tag = 0;
    Json payload;

    std::string to_wire() const {
        if (tag != 0 && is_length_prefixed_tag(tag)) {
            const std::string data =
                payload.is_string() ? payload.get<std::string>() : std::string();
            return hex(id) + ":" + std::string(1, tag) + hex(data.size()) + "," + data;
        }
        if (tag == 0) return hex(id) + ":" + payload.dump() + "\n";
        return hex(id) + ":" + std::string(1, tag) + payload.dump() + "\n";
    }
};

inline Json make_element(Json type, const std::string& key, Json props) {
    return Json::array({"$",
                        std::move(type),
                        key.empty() ? Json(nullptr) : Json(key),
                        std::move(props)});
}

class RowSink {
public:
    virtual ~RowSink() = default;
    virtual bool write(std::string_view row) = 0;
};

}