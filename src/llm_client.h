// Krastor — OpenAI-compatible chat completion client.
// Classic HTTP POST to {base_url}/chat/completions (see project.md section 4).
// Built on cpp-httplib: TLS via Schannel on Windows (both MinGW and MSVC),
// OpenSSL on Linux.

#pragma once

#include <string>

#include <nlohmann/json.hpp>

#include "config.h"
#include "conversation.h"
#include "http_client.h"

namespace krastor {

// Vendor extension of the OpenAI usage block: OpenRouter (and a few
// gateways) add "cost" (USD credits) to the response's usage object;
// plain OpenAI and local servers (llama.cpp, Ollama, LM Studio) never
// send it. Tolerant read: number or numeric string; absent, invalid or
// negative -> 0 (= the API does not report costs, UI stays hidden).
inline double parse_usage_cost(const nlohmann::json& usage)
{
    if (!usage.is_object() || !usage.contains("cost"))
        return 0.0;
    const auto& c = usage.at("cost");
    double v = 0.0;
    if (c.is_number())
        v = c.get<double>();
    else if (c.is_string()) {
        try {
            v = std::stod(c.get<std::string>());
        } catch (const std::exception&) {
            v = 0.0;
        }
    }
    return v > 0.0 ? v : 0.0;
}

struct LlmReply {
    bool ok = false;
    std::string content;      // assistant message text when ok
    std::string reasoning;    // model thinking text (reasoning_content /
                              //  tags), "" when the model does not reason
    std::string error;        // human-readable failure reason when not ok
    // Function calls requested by the model (empty for plain replies).
    std::vector<ToolCall> tool_calls;
    // Usage stats. Servers that follow the OpenAI
    // schema report them in the `usage` block; otherwise they are estimated
    // from character counts and tokens_estimated is set.
    std::string model;        // model reported by the server (fallback: config)
    double elapsed_ms = 0.0;  // measured by the caller around complete()
    int prompt_tokens = 0;
    int completion_tokens = 0;
    bool tokens_estimated = false;
    // Cost of this call in USD as reported by the server (usage.cost,
    // see parse_usage_cost). 0 = the API does not report costs.
    double cost_usd = 0.0;
};

class LlmClient {
public:
    explicit LlmClient(LlmConfig config);

    // Blocking chat completion for the given conversation history.
    // Never throws: transport and protocol errors come back as LlmReply::error.
    // `cancel` (optional) aborts the in-flight HTTP request immediately when
    // cancel() is called from another thread (the GUI's Stop button).
    // `tools_json` (optional): serialized OpenAI `tools` array; when non-empty
    // it is attached to the request and tool_calls in the reply are surfaced.
    LlmReply complete(const Conversation& conversation,
                      CancelToken* cancel = nullptr,
                      const std::string& tools_json = {}) const;

private:
    LlmConfig config_;
};

} // namespace krastor
