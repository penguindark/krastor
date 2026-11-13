// Krastor webfetch tool — wget-like text fetcher for the agent (Phase C of
// development.md). One executable, JSON in on stdin, JSON out on stdout:
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

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
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

bool http_url(const std::string& url)
{
    return url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0;
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
