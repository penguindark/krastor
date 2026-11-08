// Krastor — LLM-driven conversation compaction (see sessions_plan.md §3).
// Replaces the conversation history with a dense summary produced by the
// *currently active* LLM plus the most recent tail kept verbatim. The
// summarization budget is tuned to the model's context window: oversized
// histories are summarized in rolling map-reduce chunks.

#pragma once

#include <functional>
#include <string>

#include "conversation.h"
#include "llm_client.h"

namespace krastor {

class CancelToken;

struct CompactResult {
    bool ok = false;
    std::string error;
    std::string summary;          // the summary message text (with marker)
    int messages_before = 0;
    int messages_after = 0;
    int est_tokens_before = 0;    // rough estimate (~4 chars/token)
    int est_tokens_after = 0;
    int llm_calls = 0;
    double cost_usd = 0.0;        // summarization calls' cost (0 = no API cost)
};

// Compacts `conv` in place: history becomes
//   [summary message] + [recent tail kept verbatim].
// `context_size`/`reserve_out` come from the active model config; the usable
// budget is context_size - reserve_out - headroom. `focus` (may be empty)
// adds extra summarization instructions from the user. Runs blocking; call
// it from a worker thread. Never throws: failures come back in `error`.
CompactResult compact_conversation(const LlmClient& client, Conversation& conv,
                                   int context_size, int reserve_out,
                                   const std::string& focus,
                                   CancelToken* cancel = nullptr,
                                   const std::function<void(const std::string&)>&
                                       progress = {});

// Free tokens that must stay available before auto-compaction triggers:
// `buf` is "NN%" of the window or a raw token count (default "15%"),
// floored at min(16k, window/2) - opencode v2's reserve rule.
int compact_buffer_tokens(const std::string& buf, int context_size);

// Rough token estimate for a whole conversation (same ~4 chars/token
// heuristic as est_tokens): the session-restore fallback when meta.json
// predates the prompt_tokens field.
int estimate_conv_tokens(const std::vector<Message>& msgs);

// Exposed for --session-test (not for production use):
std::string compactor_test_prompt(bool update);
std::string compactor_test_transcript(const std::vector<Message>& msgs,
                                      int level);  // 0..4 clip levels
int compactor_test_est(const Message& m);

} // namespace krastor
