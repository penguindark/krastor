#include "llm_client.h"

#include <algorithm>
#include <cstring>

#include <nlohmann/json.hpp>

#include "http_client.h"

namespace krastor {
namespace {

// Moves inline  / <thinking>..</thinking> / <thought>..</thought>
// blocks from content into `reasoning` (some reasoning models inline their
// thinking instead of exposing a reasoning_content field). Empty delimiters
// are skipped: find("") always succeeds without advancing, which would spin
// forever (this exact bug hung complete() after the reply arrived).
void extract_inline_thinking(std::string& content, std::string& reasoning)
{
    static const char* tags[] = {"think", "thinking", "thought"};
    for (const char* tag : tags) {
        const std::string open = std::string("<") + tag + ">";
        const std::string close = std::string("</") + tag + ">";
        size_t pos = 0;
        while (true) {
            const size_t a = content.find(open, pos);
            if (a == std::string::npos)
                break;
            const size_t b = content.find(close, a + open.size());
            if (b == std::string::npos)
                break;  // unterminated: leave as-is
            std::string inner =
                content.substr(a + open.size(), b - a - open.size());
            if (!reasoning.empty())
                reasoning += "\n";
            reasoning += inner;
            content.erase(a, b + close.size() - a);
            pos = a;
        }
    }
    // Trim whitespace-only leftovers.
    auto trim = [](std::string& s) {
        size_t a = s.find_first_not_of(" \t\r\n");
        size_t b = s.find_last_not_of(" \t\r\n");
        s = a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
    };
    trim(content);
}


} // namespace

// Some models (Qwen/Hermes XML dialects, servers without tool-call parsing)
// emit the tool call as plain text instead of the structured tool_calls
// field. Two shapes are salvaged:
//   Hermes/Qwen:  <function=NAME> <parameter=KEY>value</parameter> </function>
//   Claude:       <function_calls><invoke name="NAME">
//                   <parameter=KEY>value</parameter> </invoke></function_calls>
// Each <parameter> value is raw text: parsed as JSON when it looks like a
// number/bool/object/array, kept as a string otherwise (the OpenAI arguments
// field is a JSON object). Calls whose name is not a registered tool are
// left in the text (prose about tools, not a call). Declared in llm_client.h
// so the self-test can exercise it.
void salvage_text_tool_calls(const std::string& known_tools,
                             std::string& content,
                             std::vector<ToolCall>& out)
{
    if (content.find("<function") == std::string::npos &&
        content.find("<invoke") == std::string::npos)
        return;  // the common case: no XML tool markup at all

    auto is_known = [&](const std::string& name) {
        if (name.empty())
            return false;
        if (known_tools.empty())
            return true;  // no registry: accept anything that parses
        try {
            const auto arr = nlohmann::json::parse(known_tools);
            for (const auto& t : arr)
                if (t.is_object() && t.contains("function") &&
                    t["function"].value("name", std::string()) == name)
                    return true;
        } catch (const nlohmann::json::exception&) {
        }
        return false;
    };
    // <parameter> children inside [body, limit) -> arguments object.
    auto parse_params = [&](size_t body, size_t limit) {
        nlohmann::json args = nlohmann::json::object();
        size_t pos = body;
        while (true) {
            const size_t p = content.find("<parameter", pos);
            if (p == std::string::npos || p >= limit)
                break;
            // key after "<parameter": skips =, quotes and spaces
            size_t k = p + 10;  // strlen("<parameter")
            while (k < limit &&
                   (content[k] == ' ' || content[k] == '=' || content[k] == '"'))
                ++k;
            const size_t key_end = content.find_first_of(" \t\r\n\">", k);
            if (key_end == std::string::npos || key_end >= limit)
                break;
            const std::string key = content.substr(k, key_end - k);
            const size_t v0 = content.find('>', key_end);
            const size_t v1 = v0 == std::string::npos
                                  ? std::string::npos
                                  : content.find("</parameter>", v0);
            if (v1 == std::string::npos || v1 >= limit)
                break;
            std::string val = content.substr(v0 + 1, v1 - v0 - 1);
            const size_t a = val.find_first_not_of(" \t\r\n");
            const size_t b = val.find_last_not_of(" \t\r\n");
            val = a == std::string::npos ? std::string()
                                         : val.substr(a, b - a + 1);
            // JSON-looking values keep their type, the rest stays a string
            const auto j = nlohmann::json::parse(val, nullptr, false);
            if (!key.empty())
                args[key] =
                    (!j.is_discarded() && !j.is_string()) ? j
                                                          : nlohmann::json(val);
            pos = v1 + 11;  // strlen("</parameter>")
        }
        return args;
    };

    struct Span {
        size_t a, b;
    };
    std::vector<Span> spans;
    // Shape 1: <function=NAME> ... </function>
    size_t pos = 0;
    while (true) {
        const size_t a = content.find("<function=", pos);
        if (a == std::string::npos)
            break;
        const size_t name_end = content.find_first_of(" \t\r\n>", a + 10);
        const size_t close = name_end == std::string::npos
                                 ? std::string::npos
                                 : content.find("</function>", name_end);
        if (close == std::string::npos) {
            pos = a + 10;
            continue;
        }
        std::string name = content.substr(a + 10, name_end - a - 10);
        if (name.size() > 1 && name.front() == '"' && name.back() == '"')
            name = name.substr(1, name.size() - 2);
        if (!is_known(name)) {
            pos = a + 10;  // prose about an unknown tool: leave it alone
            continue;
        }
        ToolCall tc;
        tc.id = "text_" + std::to_string(out.size() + 1);
        tc.name = name;
        tc.arguments = parse_params(name_end, close).dump();
        out.push_back(std::move(tc));
        spans.push_back({a, close + 11});  // strlen("</function>")
        pos = close + 11;
    }
    // Shape 2: <invoke name="NAME"> ... </invoke>
    pos = 0;
    while (true) {
        const size_t a = content.find("<invoke", pos);
        if (a == std::string::npos)
            break;
        const size_t q1 = content.find('"', a);
        const size_t q2 =
            q1 == std::string::npos ? std::string::npos : content.find('"', q1 + 1);
        const size_t close =
            q2 == std::string::npos ? std::string::npos
                                    : content.find("</invoke>", q2);
        if (close == std::string::npos) {
            pos = a + 7;
            continue;
        }
        const std::string name = content.substr(q1 + 1, q2 - q1 - 1);
        if (!is_known(name)) {
            pos = a + 7;
            continue;
        }
        ToolCall tc;
        tc.id = "text_" + std::to_string(out.size() + 1);
        tc.name = name;
        tc.arguments = parse_params(q2, close).dump();
        out.push_back(std::move(tc));
        spans.push_back({a, close + 9});  // strlen("</invoke>")
        pos = close + 9;
    }
    if (spans.empty())
        return;  // nothing salvaged: the markup was prose, keep it verbatim
    std::sort(spans.begin(), spans.end(),
              [](const Span& x, const Span& y) { return x.a < y.a; });
    for (size_t i = spans.size(); i-- > 0;)
        content.erase(spans[i].a, spans[i].b - spans[i].a);
    // The wrapper tags carry no meaning in prose: drop the leftovers.
    for (const char* tag : {"<tool_call>", "<|im_end|>", "<function_calls>",
                            "</function_calls>"}) {
        size_t p = 0;
        while ((p = content.find(tag, p)) != std::string::npos)
            content.erase(p, std::strlen(tag));
    }
    const size_t a = content.find_first_not_of(" \t\r\n");
    const size_t b = content.find_last_not_of(" \t\r\n");
    content = a == std::string::npos ? std::string()
                                     : content.substr(a, b - a + 1);
}

LlmClient::LlmClient(LlmConfig config) : config_(std::move(config)) {}

LlmReply LlmClient::complete(const Conversation& conversation,
                             CancelToken* cancel,
                             const std::string& tools_json) const
{
    LlmReply reply;
    if (cancel && cancel->load()) {
        reply.error = "cancelled";
        return reply;
    }

    // base_url is like https://api.openai.com/v1 -> append /chat/completions
    std::string url = config_.base_url;
    while (!url.empty() && url.back() == '/')
        url.pop_back();
    url += "/chat/completions";

    nlohmann::json messages = nlohmann::json::array();
    for (const auto& m : conversation.messages()) {
        if (m.role == "tool") {
            messages.push_back({{"role", "tool"},
                                {"tool_call_id", m.tool_call_id},
                                {"content", m.content}});
            continue;
        }
        nlohmann::json jm{{"role", m.role}, {"content", m.content}};
        if (!m.tool_calls.empty()) {
            nlohmann::json calls = nlohmann::json::array();
            for (const auto& tc : m.tool_calls)
                calls.push_back({{"id", tc.id},
                                 {"type", "function"},
                                 {"function",
                                  {{"name", tc.name},
                                   {"arguments", tc.arguments}}}});
            jm["tool_calls"] = std::move(calls);
        }
        if (m.images.empty()) {
            messages.push_back(std::move(jm));
            continue;
        }
        // OpenAI-style multimodal content: text part + one image_url part per
        // attachment (base64 data URL). Required by vision-capable models.
        nlohmann::json parts = nlohmann::json::array();
        if (!m.content.empty())
            parts.push_back({{"type", "text"}, {"text", m.content}});
        for (const auto& img : m.images)
            parts.push_back({{"type", "image_url"},
                             {"image_url",
                              {{"url", "data:image/png;base64," + img}}}});
        jm["content"] = std::move(parts);
        messages.push_back(std::move(jm));
    }

    nlohmann::json body = {
        {"model", config_.model},
        {"messages", messages},
        {"temperature", config_.temperature},
        {"max_tokens", config_.max_tokens},
    };
    if (!tools_json.empty() && tools_json != "[]") {
        try {
            body["tools"] = nlohmann::json::parse(tools_json);
        } catch (const nlohmann::json::exception&) {
            // malformed registry JSON: send the request without tools
        }
    }
    // Thinking effort (OpenAI-compat reasoning knob accepted by llama.cpp /
    // vLLM for reasoning models): omitted when unset so plain models and
    // strict servers never see an unknown field.
    if (!config_.reasoning_effort.empty())
        body["reasoning_effort"] = config_.reasoning_effort;

    const std::map<std::string, std::string> headers = {
        {"Authorization", "Bearer " + config_.api_key},
    };

    const std::string payload = body.dump();
    const auto res = https_post(url, headers, payload, "application/json",
                                cancel, config_.timeout_s);
    if (res.status == 0) {
        reply.error = "HTTP request failed: " + res.error;
        return reply;
    }
    if (res.status != 200) {
        reply.error = "HTTP " + std::to_string(res.status) + ": " + res.body;
        return reply;
    }

    try {
        const auto json = nlohmann::json::parse(res.body);
        const auto& msg = json.at("choices").at(0).at("message");
        // content can be null on pure tool_calls replies: treat as empty.
        reply.content = msg.contains("content") && msg["content"].is_string()
                            ? msg["content"].get<std::string>()
                            : std::string();
        // Reasoning models expose their thinking through the DeepSeek-style
        // reasoning_content field (LM Studio, vLLM, ...) or a reasoning field;
        // some others inline it into content between think/thought tags.
        if (msg.contains("reasoning_content") &&
            msg["reasoning_content"].is_string())
            reply.reasoning = msg["reasoning_content"].get<std::string>();
        else if (msg.contains("reasoning") && msg["reasoning"].is_string())
            reply.reasoning = msg["reasoning"].get<std::string>();
        extract_inline_thinking(reply.content, reply.reasoning);
        // Function calls requested by the model (finish_reason "tool_calls").
        if (msg.contains("tool_calls") && msg["tool_calls"].is_array()) {
            for (const auto& c : msg["tool_calls"]) {
                if (!c.is_object() || !c.contains("function"))
                    continue;
                ToolCall tc;
                tc.id = c.value("id", std::string());
                const auto& fn = c["function"];
                tc.name = fn.value("name", std::string());
                // arguments arrive as a JSON string (some servers send an object)
                if (fn.contains("arguments"))
                    tc.arguments = fn["arguments"].is_string()
                                       ? fn["arguments"].get<std::string>()
                                       : fn["arguments"].dump();
                if (!tc.id.empty())  // servers always assign ids; skip junk
                    reply.tool_calls.push_back(std::move(tc));
            }
        }
        // Tool calls leaked as plain text (models whose server does not parse
        // the XML tool dialect): salvage them into tool_calls and strip the
        // markup from content. Only when tools are configured and the server
        // gave no structured calls, so prose quoting XML stays untouched.
        if (reply.tool_calls.empty() && !tools_json.empty() &&
            tools_json != "[]")
            salvage_text_tool_calls(tools_json, reply.content,
                                    reply.tool_calls);
        reply.model = json.value("model", config_.model);
        // OpenAI usage block; some servers omit it or report zeros.
        if (json.contains("usage") && json["usage"].is_object()) {
            reply.prompt_tokens = json["usage"].value("prompt_tokens", 0);
            reply.completion_tokens = json["usage"].value("completion_tokens", 0);
            reply.cost_usd = parse_usage_cost(json["usage"]);
        }
        if (reply.prompt_tokens <= 0 || reply.completion_tokens <= 0) {
            // Rough fallback (~4 chars per token) so the stats line still works
            // against servers without usage support.
            reply.prompt_tokens = static_cast<int>(payload.size() / 4);
            reply.completion_tokens = static_cast<int>(reply.content.size() / 4);
            reply.tokens_estimated = true;
        }
        reply.ok = true;
    } catch (const nlohmann::json::exception& e) {
        reply.error = std::string("Unexpected response JSON: ") + e.what();
    }
    return reply;
}

} // namespace krastor
