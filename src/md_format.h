// Krastor — Markdown -> ANSI/SGR formatter (output_format_plan.md Step 1).
// Converts a raw Markdown reply (LLM output) into a string carrying the ANSI
// escapes ansi.h already understands (bold, dim, truecolor fg/bg), so the
// existing chat renderer draws headings, lists, quotes, inline code, links,
// tables and fenced code blocks without any renderer changes.
// Pure string transform: safe for GUI and CLI, unit-testable, no state.

#pragma once

#include <functional>
#include <string>

namespace krastor {

// Optional code-block highlighter: (code, language tag) -> ANSI-styled code.
// The language tag comes from the fence info string (```python -> "python").
// Returning an empty string keeps the block unhighlighted.
using CodeHighlightFn =
    std::function<std::string(const std::string& code, const std::string& lang)>;

// Markdown -> ANSI-styled text. Input that already contains ESC bytes is
// returned unchanged (pre-styled tool output must not be re-processed).
std::string md_to_ansi(const std::string& md,
                       const CodeHighlightFn& hl = nullptr);

} // namespace krastor
