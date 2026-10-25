// Krastor — OpenAI-compatible chat completion client.
// Classic HTTP POST to {base_url}/chat/completions (see project.md section 4).
// Built on cpp-httplib: TLS via Schannel on Windows (both MinGW and MSVC),
// OpenSSL on Linux.

#pragma once

#include <string>

#include "config.h"
#include "conversation.h"

namespace krastor {

struct LlmReply {
    bool ok = false;
    std::string content;      // assistant message text when ok
    std::string error;        // human-readable failure reason when not ok
};

class LlmClient {
public:
    explicit LlmClient(LlmConfig config);

    // Blocking chat completion for the given conversation history.
    // Never throws: transport and protocol errors come back as LlmReply::error.
    LlmReply complete(const Conversation& conversation) const;

private:
    LlmConfig config_;
};

} // namespace krastor
