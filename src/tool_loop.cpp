// Krastor — agentic tool loop implementation (see tool_loop.h).

#include "tool_loop.h"

#include <nlohmann/json.hpp>

#include <cstring>

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

std::string strip_textual_tool_calls(const std::string& text)
{
    static const char* kOpen = "<tool_call>";
    static const char* kClose = "</tool_call>";
    if (text.find(kOpen) == std::string::npos)
        return text;  // the overwhelmingly common case
    const std::string marker =
        "[tool call attempted after the round limit - raise "
        "agent.max_tool_rounds in config.jsonc]";
    std::string out;
    size_t i = 0;
    while (true) {
        const size_t s = text.find(kOpen, i);
        if (s == std::string::npos) {
            out.append(text, i, std::string::npos);
            break;
        }
        out.append(text, i, s - i);
        const size_t e = text.find(kClose, s);
        out += marker;
        if (e == std::string::npos)
            break;  // unterminated block: the rest of the message is the call
        i = e + std::strlen(kClose);
    }
    return out;
}

namespace {

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
                        const ToolLoopHooks& hooks, int max_rounds)
{
    const std::string tools = registry.empty() ? std::string() : registry.tools_json();
    LlmReply reply = client.complete(conv, cancel, tools);

    // The turn's cost is the sum of all rounds (each complete() reports its
    // own usage.cost); prompt_tokens stay the final round's = the current
    // context occupancy.
    double cost_prev = reply.cost_usd;
    auto next_round = [&](const std::string& t) {
        LlmReply r = client.complete(conv, cancel, t);
        r.cost_usd += cost_prev;
        cost_prev = r.cost_usd;
        return r;
    };

    int rounds = 0;
    while (reply.ok && !reply.tool_calls.empty() && !tools.empty()) {
        if (cancel && cancel->load()) {
            reply.error = "cancelled";
            break;
        }
        if (++rounds > max_rounds) {
            // Keep the history consistent, tell the model to wrap up and take
            // one final tool-less answer. The explicit instruction matters:
            // without it tool-trained models often answer this round with a
            // textual <tool_call> block that would leak raw XML into the chat.
            conv.add_assistant(reply.content, reply.tool_calls);
            for (const auto& tc : reply.tool_calls)
                conv.add_tool(tc.id,
                              "ERROR: tool loop limit reached. Do not call "
                              "any more tools; summarize what you did and "
                              "what remains to do.");
            reply = next_round(std::string());
            if (reply.ok)
                reply.content = strip_textual_tool_calls(reply.content);
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
        reply = next_round(tools);
    }
    return reply;
}

} // namespace krastor
