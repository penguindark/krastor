#include "compactor.h"

#include <algorithm>
#include <ctime>
#include <vector>

#include "session_store.h"  // kSummaryMarker

namespace krastor {

namespace {

// Rough token estimate (~4 chars/token, same heuristic as llm_client.cpp);
// base64 images are counted on their encoded size.
int est_tokens(const Message& m)
{
    size_t chars = m.content.size();
    for (const auto& tc : m.tool_calls)
        chars += tc.arguments.size() + tc.name.size() + 16;
    for (const auto& img : m.images)
        chars += img.size();
    return static_cast<int>(chars / 4) + 8;
}

// One transcript line per message; oversized payloads (huge tool outputs)
// are truncated - the summary only needs the gist.
std::string transcript_of(const std::vector<Message>& msgs)
{
    constexpr size_t kMaxPerMsg = 4000;
    auto clip = [](const std::string& s) -> std::string {
        if (s.size() <= kMaxPerMsg)
            return s;
        return s.substr(0, kMaxPerMsg) + " ...[truncated]";
    };
    std::string out;
    for (const auto& m : msgs) {
        if (m.role == "assistant" && !m.tool_calls.empty()) {
            for (const auto& tc : m.tool_calls)
                out += "[assistant calls " + tc.name + " " + clip(tc.arguments) + "]\n";
            if (!m.content.empty())
                out += "[assistant] " + clip(m.content) + "\n";
            continue;
        }
        const std::string role = m.role == "tool" ? "tool result" : m.role;
        out += "[" + role + "] ";
        if (!m.images.empty())
            out += "[+" + std::to_string(m.images.size()) + " image] ";
        out += clip(m.content) + "\n";
    }
    return out;
}

std::string today()
{
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
    return buf;
}

constexpr const char* kSummarizerPrompt =
    "You are a conversation compaction engine for a voice assistant agent. "
    "Summarize the given conversation transcript so the assistant can resume "
    "the work seamlessly after the original messages are discarded. Preserve: "
    "the user's goals and explicit requests, decisions and their reasons, "
    "file paths, commands and URLs with their outcomes, open questions, and "
    "anything the user asked to remember. Write a dense, structured summary "
    "in the primary language of the conversation. Output only the summary.";

} // namespace

CompactResult compact_conversation(
    const LlmClient& client, Conversation& conv, int context_size,
    int reserve_out, const std::string& focus, CancelToken* cancel,
    const std::function<void(const std::string&)>& progress)
{
    CompactResult res;
    const auto& msgs = conv.messages();
    if (msgs.size() <= 1) {
        res.error = "nothing to compact";
        return res;
    }
    std::vector<Message> history(msgs.begin() + 1, msgs.end());
    res.messages_before = static_cast<int>(history.size());
    for (const auto& m : history)
        res.est_tokens_before += est_tokens(m);

    // Budget tuned to the model (sessions_plan.md §3.1).
    const int ctx = context_size > 0 ? context_size : 8192;
    const int headroom = (std::max)(512, ctx / 16);
    const int budget = ctx - reserve_out - headroom;
    if (budget < 1024) {
        res.error = "model context too small to compact";
        return res;
    }
    const int chunk_budget = budget * 7 / 10;
    const int tail_budget = budget / 5;

    // Recent tail kept verbatim (recency beats fidelity); it must not start
    // with a tool result (that would orphan the assistant call before it).
    size_t tail_start = history.size();
    int acc = 0;
    while (tail_start > 0) {
        const int t = est_tokens(history[tail_start - 1]);
        if (acc + t > tail_budget)
            break;
        acc += t;
        --tail_start;
    }
    while (tail_start < history.size() && history[tail_start].role == "tool")
        ++tail_start;
    if (tail_start == 0)
        tail_start = history.size();  // everything fits: summarize it all

    // Head split into chunks that fit the summarization budget; a chunk never
    // ends between an assistant tool call and its results.
    std::vector<std::vector<Message>> chunks;
    {
        std::vector<Message> cur;
        int cur_tok = 0;
        for (size_t i = 0; i < tail_start; ++i) {
            const int t = est_tokens(history[i]);
            if (!cur.empty() && cur_tok + t > chunk_budget &&
                history[i].role != "tool") {
                chunks.push_back(std::move(cur));
                cur.clear();
                cur_tok = 0;
            }
            cur.push_back(history[i]);
            cur_tok += t;
        }
        if (!cur.empty())
            chunks.push_back(std::move(cur));
    }
    if (chunks.empty()) {
        res.error = "nothing to compact";
        return res;
    }

    // Rolling map-reduce: each pass folds the next chunk into the summary.
    std::string summary;
    for (size_t i = 0; i < chunks.size(); ++i) {
        if (cancel && cancel->load()) {
            res.error = "cancelled";
            return res;
        }
        if (progress)
            progress("summarizing part " + std::to_string(i + 1) + "/" +
                     std::to_string(chunks.size()) + "...");
        std::string user_prompt;
        if (!summary.empty())
            user_prompt = "Summary of the earlier part:\n" + summary +
                          "\n\nContinue the summary with the next part of the "
                          "transcript:\n";
        else
            user_prompt = "Summarize this transcript:\n";
        user_prompt += transcript_of(chunks[i]);
        if (!focus.empty())
            user_prompt += "\nExtra instruction from the user: " + focus;

        Conversation ask(kSummarizerPrompt);
        ask.add_user(user_prompt);
        const LlmReply r = client.complete(ask, cancel, "");
        ++res.llm_calls;
        if (!r.ok) {
            res.error = r.error;
            return res;
        }
        summary = r.content;
    }
    if (summary.empty()) {
        res.error = "empty summary from model";
        return res;
    }

    // New history: summary message + verbatim tail.
    Message sum_msg;
    sum_msg.role = "user";
    sum_msg.content =
        std::string(kSummaryMarker) + " — compacted " + today() + "]\n" +
        summary;
    if (tail_start < history.size())
        sum_msg.content +=
            "\n\n[The most recent messages below are kept verbatim.]";

    std::vector<Message> fresh;
    fresh.push_back(std::move(sum_msg));
    for (size_t i = tail_start; i < history.size(); ++i)
        fresh.push_back(history[i]);
    conv.replace_history(std::move(fresh));

    res.messages_after = static_cast<int>(tail_start < history.size() ?
                                              1 + (history.size() - tail_start)
                                              : 1);
    res.est_tokens_after = est_tokens(conv.messages()[1]);
    for (size_t i = tail_start; i < history.size(); ++i)
        res.est_tokens_after += est_tokens(history[i]);
    res.summary = conv.messages()[1].content;
    res.ok = true;
    return res;
}

} // namespace krastor
