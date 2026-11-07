// Krastor — conversation state: the message list sent to the LLM.

#pragma once

#include <string>
#include <vector>

namespace krastor {

// One function call requested by the assistant (OpenAI tool_calls).
struct ToolCall {
    std::string id;          // server-assigned call id
    std::string name;        // tool/function name
    std::string arguments;   // raw JSON text of the arguments object
};

struct Message {
    std::string role;     // "system" | "user" | "assistant" | "tool"
    std::string content;
    // Base64-encoded PNG images attached to this message (vision input).
    // When non-empty, content is sent as an OpenAI-style multimodal parts array.
    std::vector<std::string> images;
    // Assistant messages may request tool calls; "tool" messages answer one
    // (tool_call_id = the call being answered, content = the tool output).
    std::vector<ToolCall> tool_calls;
    std::string tool_call_id;
};

class Conversation {
public:
    explicit Conversation(std::string system_prompt);

    void add_user(const std::string& text);
    void add_user(const std::string& text, std::vector<std::string> images);
    void add_assistant(const std::string& text);
    // Assistant turn that requests tool calls (content may be empty).
    void add_assistant(const std::string& text, std::vector<ToolCall> calls);
    // Result of one tool call, fed back to the model.
    void add_tool(const std::string& tool_call_id, const std::string& content);

    // Clears the history but keeps the system prompt.
    void reset();

    // Replaces the system prompt (first message) without touching history;
    // used when runtime text is appended after construction (tools/media
    // usage instructions).
    void set_system_prompt(const std::string& prompt);

    // Replaces the whole history (system prompt kept as first message);
    // `history` must not contain a system message. Used to restore persisted
    // sessions and compacted histories (see sessions_plan.md).
    void replace_history(std::vector<Message> history);

    // Messages added since index `idx` (used to persist new turns).
    std::vector<Message> messages_since(size_t idx) const;

    const std::string& system_prompt() const { return system_prompt_; }

    // System prompt + full history, ready to serialize for the API.
    const std::vector<Message>& messages() const { return messages_; }

    size_t turn_count() const;

private:
    std::string system_prompt_;
    std::vector<Message> messages_;
};

} // namespace krastor
