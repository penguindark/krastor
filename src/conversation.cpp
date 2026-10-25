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

void Conversation::reset()
{
    messages_.clear();
    messages_.push_back({"system", system_prompt_});
}

size_t Conversation::turn_count() const
{
    return (messages_.size() - 1) / 2;
}

} // namespace krastor
