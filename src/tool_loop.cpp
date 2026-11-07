// Krastor — agentic tool loop implementation (see tool_loop.h).

#include "tool_loop.h"

#include <nlohmann/json.hpp>

#include "http_client.h"  // CancelToken

namespace krastor {

// Replace invalid UTF-8 bytes with '?' (Windows console tools print
// cp1252/cp850; nlohmann refuses to serialize invalid UTF-8 and the session
// autosave would throw on the main thread, aborting the app). Declared in
// tool_loop.h so the self-test can exercise it.
std::string utf8_clean(const std::string& in)
{
    std::string out;
    out.reserve(in.size());
    size_t i = 0;
    while (i < in.size()) {
        const unsigned char c = static_cast<unsigned char>(in[i]);
        size_t len = 0;
        if (c < 0x80) len = 1;
        else if ((c & 0xE0) == 0xC0) len = 2;
        else if ((c & 0xF0) == 0xE0) len = 3;
        else if ((c & 0xF8) == 0xF0) len = 4;
        else { out += '?'; ++i; continue; }  // stray continuation/lead byte
        if (i + len > in.size()) { out += '?'; ++i; continue; }  // truncated
        bool ok = true;
        for (size_t k = 1; k < len; ++k)
            if ((static_cast<unsigned char>(in[i + k]) & 0xC0) != 0x80)
                ok = false;
        if (!ok) { out += '?'; ++i; continue; }
        out.append(in, i, len);
        i += len;
    }
    return out;
}

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
        size_t cut = kToolResultCap;
        // Never cut in the middle of a UTF-8 sequence (s[cut] is the first
        // dropped byte: back off while it is a continuation byte 10xxxxxx).
        while (cut > 0 &&
               (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80)
            --cut;
        s.resize(cut);
        s += "\n[... output truncated]";
    }
    return utf8_clean(std::move(s));
}

// filetool edit/write results carry a structured diff for the GUI renderer;
// the model already knows what it changed, so the bulky op list must not
// bloat its context. The GUI grabs the diff from the on_done hook instead.
std::string strip_diff_for_model(const std::string& result)
{
    auto j = nlohmann::json::parse(result, nullptr, false);
    if (j.is_discarded() || !j.is_object() || !j.contains("diff"))
        return result;
    j.erase("diff");
    return j.dump();
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
                           tr.ok ? cap_text(strip_diff_for_model(tr.result))
                                 : "ERROR: " + cap_text(tr.error));
        }
        reply = client.complete(conv, cancel, tools);
    }
    return reply;
}

} // namespace krastor
