#pragma once

#include <string>
#include <string_view>

namespace rscpp::html {

inline void escape_text(std::string& out, std::string_view s) {
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&#39;"; break;
            default: out += c;
        }
    }
}

inline std::string escape_html(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 16);
    escape_text(out, s);
    return out;
}

namespace detail {

// Escapes what would otherwise let a string terminate the <script> element it
// sits in, or break a JavaScript line: `<`, `>`, `&`, U+2028, U+2029 and the
// C0 controls. With `js_quoting`, `"` and `\` are escaped too, which makes the
// result a JavaScript string body rather than JSON text.
inline void escape_inline(std::string& out, std::string_view s, bool js_quoting) {
    static constexpr char kHex[] = "0123456789abcdef";
    for (size_t i = 0; i < s.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        switch (c) {
            case '<': out += "\\u003c"; continue;
            case '>': out += "\\u003e"; continue;
            case '&': out += "\\u0026"; continue;
            case '"':
                if (js_quoting) out += "\\\"";
                else out.push_back(char(c));
                continue;
            case '\\':
                if (js_quoting) out += "\\\\";
                else out.push_back(char(c));
                continue;
            case '\n':
                out += js_quoting ? "\\n" : "\n";
                continue;
            case '\r':
                out += js_quoting ? "\\r" : "\r";
                continue;
            case 0xE2:
                if (i + 2 < s.size() && static_cast<unsigned char>(s[i + 1]) == 0x80 &&
                    (static_cast<unsigned char>(s[i + 2]) == 0xA8 ||
                     static_cast<unsigned char>(s[i + 2]) == 0xA9)) {
                    out += (static_cast<unsigned char>(s[i + 2]) == 0xA8) ? "\\u2028"
                                                                          : "\\u2029";
                    i += 2;
                    continue;
                }
                out.push_back(char(c));
                continue;
            default:
                if (c < 0x20) {
                    out += "\\u00";
                    out.push_back(kHex[(c >> 4) & 0xf]);
                    out.push_back(kHex[c & 0xf]);
                } else {
                    out.push_back(char(c));
                }
        }
    }
}

}  // namespace detail

// Makes already-serialized JSON safe to embed in an inline <script> element.
// Only valid on JSON text: `"` and `\` must survive untouched.
inline std::string script_safe(std::string_view s) {
    std::string out;
    out.reserve(s.size() + s.size() / 16);
    detail::escape_inline(out, s, false);
    return out;
}

// Renders s as a double-quoted JavaScript string literal.
inline std::string js_string_literal(std::string_view s) {
    std::string out;
    out.reserve(s.size() + s.size() / 16 + 2);
    out.push_back('"');
    detail::escape_inline(out, s, true);
    out.push_back('"');
    return out;
}

}