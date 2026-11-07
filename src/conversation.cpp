#include "conversation.h"

namespace krastor {

Conversation::Conversation(std::string system_prompt)
    : system_prompt_(std::move(system_prompt))
{
    messages_.push_back({"system", system_prompt_});
}

void Conversation::add_user(const std::string& text)
{
    messages_.push_back({"user", text, {}});
}

void Conversation::add_user(const std::string& text, std::vector<std::string> images)
{
    messages_.push_back({"user", text, std::move(images)});
}

void Conversation::add_assistant(const std::string& text)
{
    messages_.push_back({"assistant", text});
}

void Conversation::add_assistant(const std::string& text,
                                 std::vector<ToolCall> calls)
{
    Message m;
    m.role = "assistant";
    m.content = text;
    m.tool_calls = std::move(calls);
    messages_.push_back(std::move(m));
}

void Conversation::add_tool(const std::string& tool_call_id,
                            const std::string& content)
{
    Message m;
    m.role = "tool";
    m.content = content;
    m.tool_call_id = tool_call_id;
    messages_.push_back(std::move(m));
}

void Conversation::reset()
{
    messages_.clear();
    messages_.push_back({"system", system_prompt_});
}

void Conversation::set_system_prompt(const std::string& prompt)
{
    system_prompt_ = prompt;
    if (!messages_.empty() && messages_.front().role == "system")
        messages_.front().content = prompt;
    else
        messages_.insert(messages_.begin(), {"system", prompt});
}

void Conversation::replace_history(std::vector<Message> history)
{
    std::vector<Message> msgs;
    msgs.push_back({"system", system_prompt_});
    for (auto& m : history)
        if (m.role != "system")
            msgs.push_back(std::move(m));
    messages_ = std::move(msgs);
}

std::vector<Message> Conversation::messages_since(size_t idx) const
{
    if (idx >= messages_.size())
        return {};
    return std::vector<Message>(messages_.begin() + long(idx), messages_.end());
}

size_t Conversation::turn_count() const
{
    return (messages_.size() - 1) / 2;
}

} // namespace krastor
