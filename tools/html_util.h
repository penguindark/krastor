// Krastor tools — shared HTML helpers (used by webfetch and websearch).
// Header-only, no dependencies beyond the standard library.

#pragma once

#include <cctype>
#include <cstdlib>
#include <string>
#include <utility>

namespace krastor_html {

// Case-insensitive substring search (content-type sniffing, class matching).
inline bool contains_ci(const std::string& hay, const std::string& needle)
{
    std::string h = hay, n = needle;
    for (auto& c : h)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (auto& c : n)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return h.find(n) != std::string::npos;
}

// Decode the HTML5 named entities that actually matter for readability plus
// numeric &#NN;/&#xNN; references. Unknown entities are left as-is.
inline std::string decode_entities(const std::string& s)
{
    static const std::pair<const char*, const char*> named[] = {
        {"amp", "&"},    {"lt", "<"},      {"gt", ">"},
        {"quot", "\""},  {"apos", "'"},    {"nbsp", " "},
        {"ndash", "\xE2\x80\x93"}, {"mdash", "\xE2\x80\x94"},
        {"hellip", "\xE2\x80\xA6"}, {"bull", "\xE2\x80\xA2"},
        {"middot", "\xC2\xB7"}, {"copy", "\xC2\xA9"}, {"reg", "\xC2\xAE"},
        {"trade", "\xE2\x84\xA2"}, {"deg", "\xC2\xB0"},
        {"laquo", "\xC2\xAB"}, {"raquo", "\xC2\xBB"},
        {"lsquo", "\xE2\x80\x98"}, {"rsquo", "\xE2\x80\x99"},
        {"ldquo", "\xE2\x80\x9C"}, {"rdquo", "\xE2\x80\x9D"},
    };
    std::string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        if (s[i] != '&') {
            out += s[i++];
            continue;
        }
        const size_t semi = s.find(';', i + 1);
        if (semi == std::string::npos || semi - i > 12) {
            out += s[i++];
            continue;
        }
        const std::string ent = s.substr(i + 1, semi - i - 1);
        bool done = false;
        if (!ent.empty() && ent[0] == '#') {
            unsigned long cp = 0;
            bool hex = ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X');
            try {
                cp = std::stoul(ent.substr(hex ? 2 : 1), nullptr, hex ? 16 : 10);
            } catch (...) {
                cp = 0;
            }
            if (cp > 0 && cp <= 0x10FFFF) {  // encode codepoint as UTF-8
                if (cp < 0x80)
                    out += static_cast<char>(cp);
                else if (cp < 0x800) {
                    out += static_cast<char>(0xC0 | (cp >> 6));
                    out += static_cast<char>(0x80 | (cp & 0x3F));
                } else if (cp < 0x10000) {
                    out += static_cast<char>(0xE0 | (cp >> 12));
                    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                    out += static_cast<char>(0x80 | (cp & 0x3F));
                } else {
                    out += static_cast<char>(0xF0 | (cp >> 18));
                    out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
                    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                    out += static_cast<char>(0x80 | (cp & 0x3F));
                }
                done = true;
            }
        } else {
            for (const auto& [name, repl] : named)
                if (ent == name) {
                    out += repl;
                    done = true;
                    break;
                }
        }
        if (done)
            i = semi + 1;
        else
            out += s[i++];
    }
    return out;
}

// Strip tags and scripts/styles from an HTML document, keeping a readable
// text flow: block-level closing tags emit newlines, everything else is
// whitespace-collapsed.
inline std::string html_to_text(const std::string& html)
{
    std::string out;
    out.reserve(html.size() / 3 + 16);
    size_t i = 0;
    auto tag_at = [&](size_t p, const char* name) {
        size_t n = 0;
        while (name[n]) {
            char a = static_cast<char>(std::tolower(static_cast<unsigned char>(
                p + n < html.size() ? html[p + n] : 0)));
            if (a != name[n])
                return false;
            ++n;
        }
        const char next = p + n < html.size()
                              ? static_cast<char>(std::tolower(
                                    static_cast<unsigned char>(html[p + n])))
                              : '>';
        return next == '>' || next == ' ' || next == '/';
    };
    while (i < html.size()) {
        if (html[i] != '<') {
            out += html[i++];
            continue;
        }
        if (html.compare(i, 4, "<!--") == 0) {  // comment
            const size_t end = html.find("-->", i + 4);
            i = end == std::string::npos ? html.size() : end + 3;
            continue;
        }
        if (tag_at(i + 1, "script") || tag_at(i + 1, "style") ||
            tag_at(i + 1, "noscript") || tag_at(i + 1, "svg") ||
            tag_at(i + 1, "template")) {
            size_t j = i + 1, name_len = 0;
            while (j + name_len < html.size() &&
                   std::isalnum(static_cast<unsigned char>(html[j + name_len])))
                ++name_len;
            const std::string close = "</" + html.substr(i + 1, name_len);
            const size_t found = html.find(close, i + 1);
            i = found == std::string::npos ? html.size() : found;
            continue;
        }
        const size_t end = html.find('>', i);
        if (end == std::string::npos)
            break;
        // block-level tags become line breaks
        std::string tag = html.substr(i + 1, end - i - 1);
        for (auto& c : tag)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        const bool block =
            tag.rfind("p", 0) == 0 || tag.rfind("/p", 0) == 0 ||
            tag.rfind("div", 0) == 0 || tag.rfind("br", 0) == 0 ||
            tag.rfind("li", 0) == 0 || tag.rfind("tr", 0) == 0 ||
            tag.rfind("h1", 0) == 0 || tag.rfind("h2", 0) == 0 ||
            tag.rfind("h3", 0) == 0 || tag.rfind("h4", 0) == 0 ||
            tag.rfind("h5", 0) == 0 || tag.rfind("h6", 0) == 0 ||
            tag.rfind("table", 0) == 0 || tag.rfind("pre", 0) == 0 ||
            tag.rfind("blockquote", 0) == 0 || tag.rfind("section", 0) == 0 ||
            tag.rfind("article", 0) == 0 || tag.rfind("header", 0) == 0 ||
            tag.rfind("footer", 0) == 0 || tag.rfind("nav", 0) == 0 ||
            tag.rfind("main", 0) == 0;
        if (block)
            out += '\n';
        i = end + 1;
    }
    out = decode_entities(out);
    // collapse runs of spaces/tabs, trim lines, drop blank-line runs
    std::string clean;
    clean.reserve(out.size());
    bool in_space = false;
    for (const char c : out) {
        if (c == ' ' || c == '\t' || c == '\r') {
            in_space = clean.empty() ? false : in_space;
            if (!clean.empty() && clean.back() != '\n')
                in_space = true;
            continue;
        }
        if (c == '\n') {
            if (!clean.empty() && clean.back() != '\n')
                clean += '\n';
            in_space = false;
            continue;
        }
        if (in_space && !clean.empty() && clean.back() != '\n')
            clean += ' ';
        in_space = false;
        clean += c;
    }
    while (!clean.empty() && clean.back() == '\n')
        clean.pop_back();
    return clean;
}

// Extract <title> content (entities decoded), "" when absent.
inline std::string html_title(const std::string& html)
{
    std::string lower;
    lower.reserve(html.size());
    for (size_t i = 0;
         i < html.size() && lower.find("<title") == std::string::npos; ++i)
        lower +=
            static_cast<char>(std::tolower(static_cast<unsigned char>(html[i])));
    size_t a = lower.find("<title");
    if (a == std::string::npos)
        return {};
    a = html.find('>', a);
    if (a == std::string::npos)
        return {};
    const size_t b = html.find("</title", a + 1);
    if (b == std::string::npos)
        return {};
    return decode_entities(html.substr(a + 1, b - a - 1));
}

// Strip tags from a small HTML fragment (one element's inner text) and
// collapse whitespace. For search-result titles/snippets.
inline std::string text_of(const std::string& html)
{
    std::string out;
    size_t i = 0;
    while (i < html.size()) {
        if (html[i] == '<') {
            const size_t end = html.find('>', i);
            i = end == std::string::npos ? html.size() : end + 1;
            continue;
        }
        out += html[i++];
    }
    out = decode_entities(out);
    std::string clean;
    bool space = false;
    for (const char c : out) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            space = !clean.empty();
            continue;
        }
        if (space)
            clean += ' ';
        space = false;
        clean += c;
    }
    return clean;
}

// Percent-decode a URL query value (+ = space).
inline std::string url_decode(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            const auto hex = s.substr(i + 1, 2);
            char* end = nullptr;
            const long v = std::strtol(hex.c_str(), &end, 16);
            if (end && *end == '\0') {
                out += static_cast<char>(v);
                i += 2;
                continue;
            }
        }
        out += s[i] == '+' ? ' ' : s[i];
    }
    return out;
}

} // namespace krastor_html
