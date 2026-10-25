#include "llm_client.h"

#include <nlohmann/json.hpp>

#include "http_client.h"

namespace krastor {
namespace {

// Moves inline ... /<thought>...</thought> blocks from
// content into `reasoning` (some reasoning models inline their thinking).
void extract_inline_thinking(std::string& content, std::string& reasoning)
{
    static const char* opens[] = {"", "<thought>"};
    static const char* closes[] = {"", "</thought>"};
    for (int t = 0; t < 2; ++t) {
        size_t pos = 0;
        while (true) {
            const size_t a = content.find(opens[t], pos);
            if (a == std::string::npos)
                break;
            const size_t b = content.find(closes[t], a + std::string(opens[t]).size());
            if (b == std::string::npos)
                break;  // unterminated: leave as-is
            std::string inner = content.substr(a + std::string(opens[t]).size(),
                                               b - a - std::string(opens[t]).size());
            if (!reasoning.empty())
                reasoning += "\n";
            reasoning += inner;
            content.erase(a, b + std::string(closes[t]).size() - a);
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

LlmReply LlmClient::complete(const Conversation& conversation) const
{
    LlmReply reply;

    // base_url is like https://api.openai.com/v1 -> append /chat/completions
    std::string url = config_.base_url;
    while (!url.empty() && url.back() == '/')
        url.pop_back();
    url += "/chat/completions";

    nlohmann::json messages = nlohmann::json::array();
    for (const auto& m : conversation.messages()) {
        if (m.images.empty()) {
            messages.push_back({{"role", m.role}, {"content", m.content}});
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
        messages.push_back({{"role", m.role}, {"content", parts}});
    }

    nlohmann::json body = {
        {"model", config_.model},
        {"messages", messages},
        {"temperature", config_.temperature},
        {"max_tokens", config_.max_tokens},
    };

    const std::map<std::string, std::string> headers = {
        {"Authorization", "Bearer " + config_.api_key},
    };

    const std::string payload = body.dump();
    const auto res = https_post(url, headers, payload);
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
        reply.content = msg.value("content", std::string());
        // Reasoning models expose their thinking through the DeepSeek-style
        // reasoning_content field (LM Studio, vLLM, ...) or a reasoning field;
        // some others inline it into content between  /<thought> tags.
        if (msg.contains("reasoning_content") &&
            msg["reasoning_content"].is_string())
            reply.reasoning = msg["reasoning_content"].get<std::string>();
        else if (msg.contains("reasoning") && msg["reasoning"].is_string())
            reply.reasoning = msg["reasoning"].get<std::string>();
        extract_inline_thinking(reply.content, reply.reasoning);
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
