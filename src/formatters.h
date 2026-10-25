// Krastor — output formatting pipeline: raw LLM reply -> ANSI-styled text.
// The chat panel and the CLI terminal already render ANSI/SGR escapes
// (src/ansi.h), so every output plugin is a pure text transform that emits
// those escapes. This class owns the plugin chain; only the *displayed* text
// is formatted — conversation history and TTS always use the raw reply.

#pragma once

#include <string>

#include "md_format.h"

namespace krastor {

struct Config;

class OutputFormatter {
public:
    explicit OutputFormatter(const Config& cfg);

    // Applies the enabled plugins (currently: Markdown). Pass-through when
    // gui.format_output is off or the input already carries ESC bytes.
    std::string format(const std::string& raw) const;

    // Step 2 plugs the syntax highlighter into the fenced-code pass.
    void set_highlighter(CodeHighlightFn hl) { hl_ = std::move(hl); }

private:
    bool enabled_;
    CodeHighlightFn hl_;
};

} // namespace krastor
