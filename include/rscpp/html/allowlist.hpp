#pragma once

#include <algorithm>
#include <cctype>
#include <iterator>
#include <string>
#include <string_view>

namespace rscpp::html {

inline bool is_void_element(std::string_view tag) {
  static constexpr std::string_view voids[]{
      "area",  "base", "br",   "col",   "embed",  "hr",    "img",
      "input", "link", "meta", "param", "source", "track", "wbr"};
  return std::find(std::begin(voids), std::end(voids), tag) != std::end(voids);
}

inline bool is_tag_grammar(std::string_view tag) {
  if (tag.empty())
    return false;
  if (!std::isalpha(static_cast<unsigned char>(tag[0])))
    return false;
  return std::all_of(tag.begin(), tag.end(), [](unsigned char c) {
    return std::isalnum(c) || c == '-' || c == '_' || c == ':';
  });
}

inline bool is_safe_tag(std::string_view tag) {
  static constexpr std::string_view allowed[] = {
      "a",        "abbr",       "address",  "area",   "article",    "aside",
      "audio",    "b",          "bdi",      "bdo",    "blockquote", "br",
      "button",   "canvas",     "caption",  "cite",   "code",       "col",
      "colgroup", "data",       "datalist", "dd",     "del",        "details",
      "dfn",      "dialog",     "div",      "dl",     "dt",         "em",
      "fieldset", "figcaption", "figure",   "footer", "h1",         "h2",
      "h3",       "h4",         "h5",       "h6",     "header",     "hgroup",
      "hr",       "i",          "img",      "input",  "ins",        "kbd",
      "label",    "legend",     "li",       "main",   "map",        "mark",
      "menu",     "meter",      "nav",      "ol",     "optgroup",   "option",
      "output",   "p",          "picture",  "pre",    "progress",   "q",
      "rp",       "rt",         "ruby",     "s",      "samp",       "search",
      "section",  "select",     "small",    "source", "span",       "strong",
      "sub",      "summary",    "sup",      "table",  "tbody",      "td",
      "textarea", "tfoot",      "th",       "thead",  "time",       "title",
      "tr",       "track",      "u",        "ul",     "var",        "video",
      "wbr"};

  if (std::find(std::begin(allowed), std::end(allowed), tag) !=
      std::end(allowed))
    return true;

  return tag.find('-') != std::string_view::npos && is_tag_grammar(tag);
}

inline bool is_safe_attr_name(std::string_view name) {
  if (name.empty())
    return false;
  if (!std::isalpha(static_cast<unsigned char>(name[0])) && name[0] != '_')
    return false;
  return std::all_of(name.begin(), name.end(), [](unsigned char c) {
    return std::isalnum(c) || c == '-' || c == '_' || c == ':' || c == '.';
  });
}

inline bool is_url_attr(std::string_view name) {
  static constexpr std::string_view urls[]{"href",   "src",        "srcset",
                                           "action", "formaction", "poster",
                                           "cite",   "ping"};
  return std::find(std::begin(urls), std::end(urls), name) != std::end(urls);
}

inline bool is_safe_url(std::string_view url) {
  size_t i = 0;
  while (i < url.size() &&
         (url[i] == ' ' || url[i] == '\t' || url[i] == '\n' || url[i] == '\r' ||
          url[i] == '\f' || static_cast<unsigned char>(url[i]) < 0x20))
    ++i;
  const std::string_view v = url.substr(i);
  if (v.empty())
    return true;

  std::string lower;
  lower.reserve(v.size());
  for (char c : v)
    lower.push_back(
        static_cast<char>(std::tolower(static_cast<unsigned char>(c))));

  if (lower[0] == '#' || lower[0] == '/' || lower[0] == '?')
    return true;

  const size_t colon = lower.find(':');
  if (colon == std::string::npos)
    return true;
  const size_t delim = lower.find_first_of("/?#");
  if (delim != std::string::npos && delim < colon)
    return true;

  const std::string_view scheme(lower.data(), colon);
  static constexpr std::string_view schemes[]{"http", "https", "mailto", "tel",
                                              "ftp"};
  return std::find(std::begin(schemes), std::end(schemes), scheme) !=
         std::end(schemes);
}

inline bool is_known_html_attr(std::string_view name) {
  if (name.rfind("data-", 0) == 0)
    return true;
  if (name.rfind("aria-", 0) == 0)
    return true;

  static constexpr std::string_view kKnown[] = {
      // global
      "accesskey",
      "autocapitalize",
      "autocorrect",
      "autofocus",
      "class",
      "contenteditable",
      "dir",
      "draggable",
      "enterkeyhint",
      "hidden",
      "id",
      "inputmode",
      "is",
      "itemid",
      "itemprop",
      "itemref",
      "itemscope",
      "itemtype",
      "lang",
      "nonce",
      "part",
      "popover",
      "role",
      "slot",
      "spellcheck",
      "tabindex",
      "title",
      "translate",
      // links and embeds
      "href",
      "hreflang",
      "media",
      "ping",
      "referrerpolicy",
      "rel",
      "target",
      "type",
      "src",
      "srcdoc",
      "sizes",
      "width",
      "height",
      "loading",
      "decoding",
      "referrerPolicy",
      "crossorigin",
      "useMap",
      "ismap",
      "allow",
      "allowfullscreen",
      "allowpaymentrequest",
      "sandbox",
      "scrolling",
      "name",
      "download",
      "value",
      "open",
      "integrity",
      // forms
      "accept",
      "accept-charset",
      "action",
      "autocomplete",
      "checked",
      "cols",
      "disabled",
      "enctype",
      "for",
      "form",
      "formaction",
      "formenctype",
      "formmethod",
      "formnovalidate",
      "formtarget",
      "list",
      "max",
      "maxlength",
      "method",
      "min",
      "minlength",
      "multiple",
      "muted",
      "pattern",
      "placeholder",
      "readonly",
      "required",
      "rows",
      "selected",
      "step",
      "capture",
      // tables
      "colspan",
      "headers",
      "rowspan",
      "span",
      "summary",
      "scope",
      "abbr",
      "align",
      "valign",
      "bgcolor",
      "border",
      "cellpadding",
      "cellspacing",
      // document head
      "charset",
      "content",
      "http-equiv",
      "async",
      "defer",
      "crossOrigin",
      // Chrome spellings that the renderer maps 1:1 onto an HTML attribute
      "autoComplete",
      "autoFocus",
      "contentEditable",
      "spellCheck",
  };
  for (std::string_view k : kKnown) {
    if (name == k)
      return true;
  }
  return false;
}

} // namespace rscpp::html
