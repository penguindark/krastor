// Krastor webfetch tool — wget-like text fetcher for the agent (Phase C of
// tools_plan.md). One executable, JSON in on stdin, JSON out on stdout:
//   in:  {"url":"https://...","max_chars":40000,"_approved":true,
//         "_policy":{...}}
//   out: {"ok":true,"result":{"status":200,"text":"..."}}
// HTML responses are reduced to readable text (scripts/styles dropped,
// block tags become line breaks, entities decoded). Binary content is
// refused. Only http(s) URLs are accepted.
//
// Self-enforcement (defense in depth): the deny_always floor is ALWAYS
// applied to the URL, even when Krastor already approved the call. Without
// _approved the full policy applies and any "ask" outcome becomes a denial.

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

#include "../src/permissions.h"
#include "http_client.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#endif

using nlohmann::json;
using namespace krastor;

namespace {

json ok(const json& result) { return {{"ok", true}, {"result", result}}; }
json fail(const std::string& error) { return {{"ok", false}, {"error", error}}; }

// ---------------------------------------------------------------- URL checks

bool http_url(const std::string& url)
{
    return url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0;
}

// Case-insensitive substring search (for content-type sniffing).
bool contains_ci(const std::string& hay, const std::string& needle)
{
    std::string h = hay, n = needle;
    for (auto& c : h)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (auto& c : n)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return h.find(n) != std::string::npos;
}

// ------------------------------------------------------- HTML -> plain text

// Decode the HTML5 named entities that actually matter for readability plus
// numeric &#NN;/&#xNN; references. Unknown entities are left as-is.
std::string decode_entities(const std::string& s)
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
std::string html_to_text(const std::string& html)
{
    std::string out;
    out.reserve(html.size() / 3 + 16);
    size_t i = 0;
    auto tag_at = [&](size_t p, const char* name) {
        size_t n = 0;
        while (name[n]) {
            char a = static_cast<char>(std::tolower(
                static_cast<unsigned char>(p + n < html.size() ? html[p + n] : 0)));
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
            const size_t end = html.find("</", i);  // crude: first closing tag
            // find the matching close of the same tag name
            size_t j = i + 1, name_len = 0;
            while (j + name_len < html.size() &&
                   std::isalnum(static_cast<unsigned char>(html[j + name_len])))
                ++name_len;
            const std::string close =
                "</" + html.substr(i + 1, name_len);
            size_t k = i + 1;
            size_t found = std::string::npos;
            while ((k = html.find(close, k)) != std::string::npos) {
                found = k;
                break;
            }
            (void)end;
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

// Extract <title> for the result metadata (before stripping).
std::string html_title(const std::string& html)
{
    size_t a = std::string::npos;
    {
        std::string lower;
        lower.reserve(html.size());
        for (size_t i = 0; i < html.size() && lower.find("<title") == std::string::npos; ++i)
            lower += static_cast<char>(std::tolower(
                static_cast<unsigned char>(html[i])));
        a = lower.find("<title");
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
}

} // namespace

int main()
{
    std::stringstream ss;
    ss << std::cin.rdbuf();
    const std::string input = ss.str();

    json out;
    try {
        const json args = json::parse(input);
        if (!args.is_object())
            throw std::runtime_error("input must be a JSON object");
        const std::string url = args.value("url", std::string());
        if (url.empty())
            throw std::runtime_error("missing 'url'");
        if (!http_url(url))
            throw std::runtime_error("only http(s) URLs are supported");

        // --- self-enforced policy on the URL (net class) ---------------------
        const bool approved = args.value("_approved", false);
        const ToolPolicy policy = ToolPolicy::from_json(
            args.contains("_policy") ? args["_policy"] : json());
        for (const auto& g : policy.deny_always)
            if (glob_match(g, url))
                throw std::runtime_error("url is on the never-allowed list: " +
                                         url);
        if (!approved && policy.check(PermClass::Net, url) != Decision::Allow)
            throw std::runtime_error(
                "url not permitted by policy (run through Krastor to request "
                "permission)");

        // --- fetch ------------------------------------------------------------
        const std::map<std::string, std::string> headers = {
            {"User-Agent", "Mozilla/5.0 (compatible; Krastor/0.3)"},
            {"Accept", "text/html,application/xhtml+xml,text/plain,*/*"},
            {"Accept-Language", "en-US,en;q=0.8"},
        };
        const HttpResponse resp = https_get(url, headers, nullptr);
        if (resp.status == 0)
            throw std::runtime_error("fetch failed: " + resp.error);
        if (resp.status >= 400) {
            out = fail("HTTP " + std::to_string(resp.status) + " from " + url);
#ifdef _WIN32
            _setmode(_fileno(stdout), _O_BINARY);
#endif
            std::fputs(out.dump().c_str(), stdout);
            std::fflush(stdout);
            return 0;
        }

        // --- convert to text ----------------------------------------------------
        const std::string& ctype = resp.content_type;
        std::string text;
        std::string title;
        if (contains_ci(ctype, "html")) {
            title = html_title(resp.body);
            text = html_to_text(resp.body);
        } else if (contains_ci(ctype, "text") || contains_ci(ctype, "json") ||
                   contains_ci(ctype, "xml") || ctype.empty()) {
            text = resp.body;  // also accept unknown types as text, sniff NULs
            if (text.find('\0') != std::string::npos)
                throw std::runtime_error(
                    "binary content (" + (ctype.empty() ? std::string("?") : ctype) +
                    ") - not text");
        } else {
            throw std::runtime_error("unsupported content type: " + ctype);
        }

        // Cap the text (default 40k chars) so the LLM context stays sane.
        size_t max_chars = args.value("max_chars", 40000LL);
        if (max_chars == 0 || max_chars > 200000)
            max_chars = 40000;
        const bool truncated = text.size() > max_chars;
        if (truncated)
            text.resize(max_chars);

        out = ok({{"url", url},
                  {"status", resp.status},
                  {"content_type", ctype},
                  {"title", title},
                  {"bytes", resp.body.size()},
                  {"truncated", truncated},
                  {"text", text}});
    } catch (const std::exception& e) {
        out = fail(std::string("webfetch: ") + e.what());
    }

#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    std::fputs(out.dump().c_str(), stdout);
    std::fflush(stdout);
    return 0;
}
