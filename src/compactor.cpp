// Krastor — LLM-driven conversation compaction (see compactor.h).
// Summary format and transcript-clipping strategy follow opencode v2's
// compaction engine: a fixed structured template (Objective/Requirements/
// Decisions/Work State/Next Move/Relevant Files/Important Context), terse
// bullets that preserve exact paths/commands/identifiers, an UPDATE mode
// that folds new history into an existing checkpoint instead of restarting,
// and progressive transcript clipping (tool outputs clipped -> removed ->
// user-only -> omitted) so the summarization request always fits its budget.

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

// ---- transcript building (opencode v2 clipping levels) ----
// 0 full; 1 tool outputs/args clipped; 2 tool outputs dropped, args
// clipped; 3 user text only; 4 everything omitted (last resort).
enum ClipLevel { kFull = 0, kOutputsClipped = 1, kOutputsRemoved = 2,
                 kUserOnly = 3, kOmitted = 4 };

// Keep the first and last kClipKeep chars of a long payload.
constexpr size_t kClipKeep = 2000;

std::string clip_ends(const std::string& s)
{
    if (s.size() <= kClipKeep * 2)
        return s;
    return s.substr(0, kClipKeep) + "\n...[clipped]...\n" +
           s.substr(s.size() - kClipKeep);
}

std::string transcript_of(const std::vector<Message>& msgs, int level)
{
    std::string out;
    for (const auto& m : msgs) {
        if (m.role == "assistant" && !m.tool_calls.empty()) {
            if (level >= kUserOnly)
                continue;
            for (const auto& tc : m.tool_calls) {
                if (level >= kOutputsRemoved)
                    out += "[assistant calls " + tc.name + "]\n";
                else if (level == kOutputsClipped)
                    out += "[assistant calls " + tc.name + " " +
                           clip_ends(tc.arguments) + "]\n";
                else
                    out += "[assistant calls " + tc.name + " " +
                           tc.arguments.substr(0, 4000) + "]\n";
            }
            if (!m.content.empty())
                out += "[assistant] " + clip_ends(m.content) + "\n";
            continue;
        }
        if (m.role == "tool") {
            if (level >= kUserOnly)
                continue;
            if (level >= kOutputsRemoved) {
                out += "[tool result omitted]\n";
                continue;
            }
            out += "[tool result] " +
                   (level == kOutputsClipped ? clip_ends(m.content)
                                             : m.content.substr(0, 8000)) +
                   "\n";
            continue;
        }
        // user / assistant text
        if (level >= kOmitted)
            continue;
        if (m.content.empty() && m.images.empty())
            continue;
        out += "[" + m.role + "] ";
        if (!m.images.empty())
            out += "[+" + std::to_string(m.images.size()) + " image] ";
        out += clip_ends(m.content) + "\n";
    }
    return out;
}

// Transcript for a chunk, escalating the clipping level until the estimate
// fits the summarization budget.
std::string transcript_fit(const std::vector<Message>& msgs, int budget_tokens)
{
    for (int level = kFull; level <= kOmitted; ++level) {
        std::string t = transcript_of(msgs, level);
        if (level == kOmitted || static_cast<int>(t.size() / 4) <= budget_tokens)
            return t;
    }
    return {};
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

// ---- prompts (adapted from opencode v2 compaction.ts) ----

const char* kSummaryTemplate =
    "You MUST use this format for your response (you may omit sections that "
    "aren't applicable). Do not include the <template> tags in your response.\n"
    "<template>\n"
    "## Objective\n"
    "- [one or two brief sentences describing what the user is trying to "
    "accomplish]\n\n"
    "## Requirements\n"
    "- [constraints, preferences, requirements, and scope boundaries stated "
    "by the user, or \"(none)\"]\n\n"
    "## Decisions\n"
    "- [decisions already made and why, or \"(none)\"]\n\n"
    "## Work State\n"
    "Break the objective into smaller goals and report which are completed, "
    "which are being worked on, and which are blocked.\n"
    "### Completed\n"
    "- [goals that have been completed; otherwise \"(none)\"]\n\n"
    "### Active\n"
    "- [goals currently being worked on; otherwise \"(none)\"]\n\n"
    "### Blocked\n"
    "- [anything blocking progress, and why, or \"(none)\"]\n\n"
    "## Next Move\n"
    "1. [ordered list of next actions, or \"(none)\"]\n\n"
    "## Relevant Files\n"
    "List the files and directories that another agent would need to open to "
    "continue this work. Include at most 15, most important first.\n"
    "- `[file or directory path]`: [brief reason it matters]\n\n"
    "## Important Context\n"
    "- [facts the next agent cannot continue without and cannot easily find "
    "on its own; or \"(none)\"]\n"
    "</template>";

const char* kSummaryRules =
    "Rules:\n"
    "- Keep each section concise. Use terse, single-line bullets, not prose "
    "paragraphs or nested lists.\n"
    "- Prefer short references over detailed restatement. It is fine to "
    "leave out information the next agent can recover from the files listed "
    "above.\n"
    "- Preserve exact file paths, symbols, commands, error strings, URLs, "
    "and identifiers.\n"
    "- Carry forward only user questions or requests that remain unanswered "
    "or require further action. Preserve exact wording when carrying one "
    "forward.\n"
    "- Do not mention the summary process or that context was compacted.";

const char* kSummarizerPrompt =
    "You are a conversation compaction engine for a voice assistant agent. "
    "Write the summary in the primary language of the conversation.\n\n";

std::string build_prompt_head(bool update)
{
    std::string p = kSummarizerPrompt;
    if (update)
        p += "Update the existing checkpoint provided below into one "
             "consolidated summary. Newer history always takes precedence "
             "over the existing checkpoint.\n\n";
    else
        p += "Summarize the conversation transcript below into a structured "
             "summary that will be given to another agent to resume the "
             "work.\n\n";
    p += kSummaryTemplate;
    p += "\n\n";
    p += kSummaryRules;
    p += "\n\nDo not continue the task or call tools. Return only the "
         "structured summary in the requested format.";
    return p;
}

} // namespace

std::string compactor_test_prompt(bool update) { return build_prompt_head(update); }
std::string compactor_test_transcript(const std::vector<Message>& msgs,
                                      int level)
{
    return transcript_of(msgs, level);
}
int compactor_test_est(const Message& m) { return est_tokens(m); }

int compact_buffer_tokens(const std::string& buf, int context_size)
{
    // "NN%" of the window, or a raw token count; never below 16k free
    // (opencode v2 RESERVE_MIN), never above half the window.
    int v = 0;
    const bool pct = !buf.empty() && buf.back() == '%';
    std::string num = pct ? buf.substr(0, buf.size() - 1) : buf;
    bool bad = false;
    try {
        v = std::stoi(num);
    } catch (...) {
        bad = true;  // garbage: fall back to the default "15%"
    }
    if (bad)
        v = context_size * 15 / 100;
    else if (pct)
        v = context_size * v / 100;
    const int floor_v = (std::min)(16000, context_size / 2);
    return (std::max)(v, floor_v);
}

int estimate_conv_tokens(const std::vector<Message>& msgs)
{
    int t = 0;
    for (const auto& m : msgs)
        t += est_tokens(m);
    return t;
}

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

    // UPDATE mode: the history already starts with a checkpoint - fold the
    // newer messages into it instead of summarizing from scratch.
    std::string checkpoint;
    if (!history.empty() && is_summary_message(history.front())) {
        checkpoint = history.front().content;
        history.erase(history.begin());
        --res.messages_before;
    }

    // Budget tuned to the model (development.md).
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
    if (chunks.empty() && checkpoint.empty()) {
        res.error = "nothing to compact";
        return res;
    }

    // Rolling map-reduce: each pass folds the next chunk into the summary.
    std::string summary = checkpoint;
    const int total = static_cast<int>(chunks.size());
    for (size_t i = 0; i < chunks.size(); ++i) {
        if (cancel && cancel->load()) {
            res.error = "cancelled";
            return res;
        }
        if (progress)
            progress("summarizing part " + std::to_string(i + 1) + "/" +
                     std::to_string(total) + "...");
        const bool first = summary.empty();
        std::string user_prompt;
        if (!first && i > 0)
            user_prompt = "Summary of the earlier part:\n" + summary +
                          "\n\nContinue the summary with the next part of "
                          "the transcript:\n";
        else if (!checkpoint.empty() && i == 0)
            user_prompt = "Existing checkpoint:\n" + checkpoint +
                          "\n\nNewer history to fold in:\n";
        else
            user_prompt = "Summarize this transcript:\n";
        user_prompt += transcript_fit(chunks[i], chunk_budget);
        if (!focus.empty())
            user_prompt += "\nExtra instruction from the user: " + focus;

        Conversation ask(build_prompt_head(!checkpoint.empty()));
        ask.add_user(user_prompt);
        const LlmReply r = client.complete(ask, cancel, "");
        ++res.llm_calls;
        res.cost_usd += r.cost_usd;  // summarization bills like any call
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
