#include "llm_client.h"

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

    const std::map<std::string, std::string> headers = {
        {"Authorization", "Bearer " + config_.api_key},
    };

    const std::string payload = body.dump();
    const auto res = https_post(url, headers, payload, "application/json",
                                cancel);
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
        reply.model = json.value("model", config_.model);
        // OpenAI usage block; some servers omit it or report zeros.
        if (json.contains("usage") && json["usage"].is_object()) {
            reply.prompt_tokens = json["usage"].value("prompt_tokens", 0);
            reply.completion_tokens = json["usage"].value("completion_tokens", 0);
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
