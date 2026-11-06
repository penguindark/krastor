// Krastor websearch tool — web search for the agent via DuckDuckGo's
// key-free HTML endpoints (html.duckduckgo.com, fallback lite.duckduckgo.com).
// JSON in on stdin, JSON out on stdout:
//   in:  {"query":"...","max_results":8,"_approved":true,"_policy":{...}}
//   out: {"ok":true,"result":{"query":"...","results":[{"title","url",
//         "snippet"}, ...]}}
// Self-enforcement mirrors webfetch: deny_always applies to the engine URL,
// without _approved the full policy applies and any ask becomes a denial.

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <map>

#include <nlohmann/json.hpp>

#include "../src/permissions.h"
#include "../html_util.h"
#include "http_client.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#endif

using nlohmann::json;
using namespace krastor;
using namespace krastor_html;

namespace {

json ok(const json& result) { return {{"ok", true}, {"result", result}}; }
json fail(const std::string& error) { return {{"ok", false}, {"error", error}}; }

std::string url_encode(const std::string& s)
{
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (const unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
            out += static_cast<char>(c);
        else if (c == ' ')
            out += '+';
        else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 0xF];
        }
    }
    return out;
}

// href="..." following `from` (the position right after the class match).
std::string attr_href(const std::string& html, size_t from)
{
    const size_t a = html.find("href=\"", from);
    if (a == std::string::npos)
        return {};
    const size_t b = html.find('"', a + 6);
    return b == std::string::npos ? std::string()
                                  : html.substr(a + 6, b - a - 6);
}

// Inner text of the anchor starting at `from` (entities decoded, tags cut).
std::string anchor_text(const std::string& html, size_t from)
{
    const size_t gt = html.find('>', from);
    if (gt == std::string::npos)
        return {};
    const size_t end = html.find("</a>", gt);
    return text_of(html.substr(gt + 1, end == std::string::npos
                                           ? std::string::npos
                                           : end - gt - 1));
}

// DuckDuckGo wraps result URLs: //duckduckgo.com/l/?uddg=<enc>&rut=...
std::string unwrap_url(const std::string& href)
{
    const size_t u = href.find("uddg=");
    if (u != std::string::npos) {
        size_t e = href.find('&', u + 5);
        return url_decode(
            href.substr(u + 5, e == std::string::npos
                                   ? std::string::npos
                                   : e - u - 5));
    }
    if (href.rfind("//", 0) == 0)
        return "https:" + href;
    return href;
}

struct Hit {
    std::string title, url, snippet;
};

// html.duckduckgo.com/html/: a.result__a anchors + a.result__snippet anchors.
std::vector<Hit> parse_ddg_html(const std::string& html, size_t max_results)
{
    std::vector<Hit> hits;
    size_t pos = 0;
    while (hits.size() < max_results) {
        const size_t a = html.find("class=\"result__a\"", pos);
        if (a == std::string::npos)
            break;
        Hit h;
        h.url = unwrap_url(decode_entities(attr_href(html, a)));
        h.title = anchor_text(html, a);
        const size_t s = html.find("class=\"result__snippet\"", a);
        const size_t next_a = html.find("class=\"result__a\"", a + 10);
        if (s != std::string::npos &&
            (next_a == std::string::npos || s < next_a))
            h.snippet = anchor_text(html, s);
        if (!h.title.empty() && !h.url.empty())
            hits.push_back(std::move(h));
        pos = a + 10;
    }
    return hits;
}

// lite.duckduckgo.com/lite/: a.result-link + td.result-snippet.
std::vector<Hit> parse_ddg_lite(const std::string& html, size_t max_results)
{
    std::vector<Hit> hits;
    size_t pos = 0;
    while (hits.size() < max_results) {
        const size_t a = html.find("class=\"result-link\"", pos);
        if (a == std::string::npos)
            break;
        Hit h;
        h.url = unwrap_url(decode_entities(attr_href(html, a)));
        h.title = anchor_text(html, a);
        const size_t s = html.find("class=\"result-snippet\"", a);
        const size_t next_a = html.find("class=\"result-link\"", a + 10);
        if (s != std::string::npos &&
            (next_a == std::string::npos || s < next_a)) {
            const size_t gt = html.find('>', s);
            const size_t end = html.find("</td>", s);
            if (gt != std::string::npos && end != std::string::npos)
                h.snippet = text_of(html.substr(gt + 1, end - gt - 1));
        }
        if (!h.title.empty() && !h.url.empty())
            hits.push_back(std::move(h));
        pos = a + 10;
    }
    return hits;
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
        const std::string query = args.value("query", std::string());
        if (query.empty())
            throw std::runtime_error("missing 'query'");
        size_t max_results = args.value("max_results", 8LL);
        if (max_results == 0 || max_results > 15)
            max_results = 8;

        const bool approved = args.value("_approved", false);
        const ToolPolicy policy = ToolPolicy::from_json(
            args.contains("_policy") ? args["_policy"] : json());

        const std::string q = url_encode(query);
        const std::map<std::string, std::string> headers = {
            {"User-Agent",
             "Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:128.0) Gecko/"
             "20100101 Firefox/128.0"},
            {"Accept", "text/html,application/xhtml+xml,*/*"},
            {"Accept-Language", "en-US,en;q=0.8"},
        };

        // Two engines, tried in order: full HTML page, then the lite page.
        struct Engine {
            std::string url;
            std::vector<Hit> (*parse)(const std::string&, size_t);
        } engines[] = {
            {"https://html.duckduckgo.com/html/?q=" + q, parse_ddg_html},
            {"https://lite.duckduckgo.com/lite/?q=" + q, parse_ddg_lite},
        };

        std::string last_error = "no results";
        for (const auto& eng : engines) {
            for (const auto& g : policy.deny_always)
                if (glob_match(g, eng.url))
                    throw std::runtime_error(
                        "search engine is on the never-allowed list: " +
                        eng.url);
            if (!approved &&
                policy.check(PermClass::Net, eng.url) != Decision::Allow)
                throw std::runtime_error(
                    "search engine not permitted by policy (run through "
                    "Krastor to request permission)");

            const HttpResponse resp = https_get(eng.url, headers, nullptr);
            if (resp.status == 0) {
                last_error = "fetch failed: " + resp.error;
                continue;
            }
            if (resp.status >= 400) {
                last_error = "HTTP " + std::to_string(resp.status);
                continue;
            }
            auto hits = eng.parse(resp.body, max_results);
            if (hits.empty()) {
                // DDG answers 200 with an anomaly/redirect page when it
                // dislikes the client: fall through to the next engine.
                last_error = "no results parsed (engine may have refused)";
                continue;
            }
            json results = json::array();
            for (const auto& h : hits)
                results.push_back({{"title", h.title},
                                   {"url", h.url},
                                   {"snippet",
                                    h.snippet.size() > 500
                                        ? h.snippet.substr(0, 500) + "..."
                                        : h.snippet}});
            out = ok({{"query", query},
                      {"engine", eng.url},
                      {"count", results.size()},
                      {"results", std::move(results)}});
            break;
        }
        if (out.is_null())
            out = fail("websearch: " + last_error);
    } catch (const std::exception& e) {
        out = fail(std::string("websearch: ") + e.what());
    }

#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    std::fputs(out.dump().c_str(), stdout);
    std::fflush(stdout);
    return 0;
}
