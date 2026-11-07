// Krastor — Markdown -> ANSI/SGR formatter.
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

// Markdown palette: the SGR strings md_to_ansi styles with. Defaults are the
// opencode-ish dark palette; every entry can be overridden with a hex color
// (config.jsonc "colors" md_* keys, see ColorScheme in config.h).
struct MdPalette {
    std::string h1, h2, bullet, italic, link, sep, code_fg, code_bg,
                block_bg, table_head;
    MdPalette();  // opencode-ish defaults
    // Override one entry from a "#rrggbb" color (empty/invalid keeps the
    // current value). Keys: h1 h2 bullet italic link sep code code_bg
    // block_bg table_head.
    void set(const std::string& key, const std::string& hex);
};

// The built-in default palette (constructed once).
const MdPalette& md_palette_default();

// Markdown -> ANSI-styled text. Source lines that already contain ESC bytes
// are emitted verbatim (terminal passthrough: pre-styled tool output, ANSI
// art), while the remaining lines keep their markdown formatting.
std::string md_to_ansi(const std::string& md,
                       const CodeHighlightFn& hl = nullptr,
                       const MdPalette& pal = md_palette_default());

// Appended to the system prompt (GUI + CLI, always on): the chat renders
// ANSI SGR colors, so the model can emit colored output directly instead of
// describing or escaping it. Guarded against double-append like
// kMediaUsagePrompt (marker: "renders ANSI SGR colors").
inline constexpr const char* kAnsiUsagePrompt =
    "\nThe chat renders ANSI SGR colors (16/256/truecolor fg+bg, bold): to "
    "show colored output (ANSI art, colored listings, terminal demos) print "
    "the raw escape codes directly instead of describing or escaping them, "
    "and do not wrap them in code blocks. Lines containing ANSI codes are "
    "shown verbatim (markdown is skipped for those lines only), so keep "
    "colored blocks on their own lines.";

} // namespace krastor
