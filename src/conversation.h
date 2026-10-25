// Krastor — conversation state: the message list sent to the LLM.

#pragma once

#include <string>
#include <vector>

namespace krastor {

struct Message {
    std::string role;     // "system" | "user" | "assistant"
    std::string content;
};

class Conversation {
public:
    explicit Conversation(std::string system_prompt);

    void add_user(const std::string& text);
    void add_assistant(const std::string& text);

    // Clears the history but keeps the system prompt.
    void reset();

    // System prompt + full history, ready to serialize for the API.
    const std::vector<Message>& messages() const { return messages_; }

    size_t turn_count() const;

private:
    std::string system_prompt_;
    std::vector<Message> messages_;
};

} // namespace krastor
