#include "llm_client.h"

#include <nlohmann/json.hpp>

#include "http_client.h"

namespace krastor {

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
    for (const auto& m : conversation.messages())
        messages.push_back({{"role", m.role}, {"content", m.content}});

    nlohmann::json body = {
        {"model", config_.model},
        {"messages", messages},
        {"temperature", config_.temperature},
        {"max_tokens", config_.max_tokens},
    };

    const std::map<std::string, std::string> headers = {
        {"Authorization", "Bearer " + config_.api_key},
    };

    const auto res = https_post(url, headers, body.dump());
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
        reply.content = json.at("choices").at(0).at("message").at("content")
                            .get<std::string>();
        reply.ok = true;
    } catch (const nlohmann::json::exception& e) {
        reply.error = std::string("Unexpected response JSON: ") + e.what();
    }
    return reply;
}

} // namespace krastor
