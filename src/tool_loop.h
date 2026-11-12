// Krastor — agentic tool loop (see tools_plan.md section 5).
// Wraps LlmClient::complete(): when the model requests tool calls, each call
// goes through the ToolRunner (permission gate inside), the results are fed
// back as "tool" messages and the model is asked again — up to a hard round
// limit so a confused model can never spin the loop.

#pragma once

#include <functional>
#include <string>

#include "llm_client.h"
#include "permissions.h"
#include "tool_registry.h"
#include "tool_runner.h"

namespace krastor {

class CancelToken;

// Replace invalid UTF-8 bytes with '?' (Windows console tools print
// cp1252/cp850; nlohmann refuses to serialize invalid UTF-8 and the session
// autosave would throw on the main thread). Applied to every tool result
// before it enters the conversation.
std::string utf8_clean(const std::string& in);

// Info about one requested call, for UI/status rendering.
struct ToolCallInfo {
    std::string tool;    // manifest name
    std::string action;  // args.action (or the tool name)
    std::string args_json;
};

struct ToolLoopHooks {
    std::function<void(const ToolCallInfo&)> on_call;  // about to run
    std::function<void(const ToolCallInfo&, const ToolResult&)> on_done;
};

// Runs complete() + the tool loop. `ask` may be null: then tools that would
// ask are denied (and no tools are sent at all when the registry is empty).
// The final reply (no tool_calls, or the loop limit hit) is returned; all
// intermediate assistant/tool messages are already appended to `conv`.
// `max_rounds` caps the tool-loop iterations per user message
// (agent.max_tool_rounds in config.jsonc).
LlmReply agent_complete(const LlmClient& client, Conversation& conv,
                        const ToolRegistry& registry, ToolRunner& runner,
                        AskHandler* ask, CancelToken* cancel,
                        const ToolLoopHooks& hooks = {},
                        int max_rounds = 16);

} // namespace krastor
