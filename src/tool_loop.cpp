// Krastor — agentic tool loop implementation (see tool_loop.h).

#include "tool_loop.h"

#include <nlohmann/json.hpp>

#include "http_client.h"  // CancelToken

namespace krastor {

namespace {

constexpr int kMaxRounds = 8;      // hard cap on tool-loop iterations
constexpr size_t kToolResultCap = 8000;  // chars of tool output fed back

std::string action_of(const ToolCall& tc)
{
    try {
        const auto j = nlohmann::json::parse(tc.arguments);
        if (j.is_object() && j.contains("action") && j["action"].is_string())
            return j["action"].get<std::string>();
    } catch (const nlohmann::json::exception&) {
    }
    return tc.name;
}

std::string cap_text(std::string s)
{
    if (s.size() > kToolResultCap) {
        s.resize(kToolResultCap);
        s += "\n[... output truncated]";
    }
    return s;
}

} // namespace

LlmReply agent_complete(const LlmClient& client, Conversation& conv,
                        const ToolRegistry& registry, ToolRunner& runner,
                        AskHandler* ask, CancelToken* cancel,
                        const ToolLoopHooks& hooks)
{
    const std::string tools = registry.empty() ? std::string() : registry.tools_json();
    LlmReply reply = client.complete(conv, cancel, tools);

    int rounds = 0;
    while (reply.ok && !reply.tool_calls.empty() && !tools.empty()) {
        if (cancel && cancel->load()) {
            reply.error = "cancelled";
            break;
        }
        if (++rounds > kMaxRounds) {
            // Keep the history consistent, tell the model to wrap up and take
            // one final tool-less answer.
            conv.add_assistant(reply.content, reply.tool_calls);
            for (const auto& tc : reply.tool_calls)
                conv.add_tool(tc.id, "ERROR: tool loop limit reached");
            reply = client.complete(conv, cancel, std::string());
            break;
        }

        conv.add_assistant(reply.content, reply.tool_calls);
        for (const auto& tc : reply.tool_calls) {
            ToolCallInfo info;
            info.tool = tc.name;
            info.action = action_of(tc);
            info.args_json = tc.arguments;
            if (hooks.on_call)
                hooks.on_call(info);

            ToolResult tr;
            const ToolManifest* m = registry.find(tc.name);
            if (!m)
                tr.error = "unknown tool: " + tc.name;
            else
                tr = runner.run(*m, tc.arguments, ask, cancel);
            if (hooks.on_done)
                hooks.on_done(info, tr);

            conv.add_tool(tc.id,
                           tr.ok ? cap_text(tr.result)
                                 : "ERROR: " + cap_text(tr.error));
        }
        reply = client.complete(conv, cancel, tools);
    }
    return reply;
}

} // namespace krastor
