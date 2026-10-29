// Krastor — TTS text filter: Markdown -> plain speech text.
// The mirror of md_format.h for the voice: the chat shows the full Markdown,
// the speakers only get the prose. Pure string transform (no state, no
// dependencies), applied once inside Tts::speak() so no markup can ever
// reach the synthesizer.

#pragma once

#include <string>

namespace krastor {

// Strip everything that reads badly aloud: fenced code blocks (dropped
// silently), inline-code backticks, heading/bold/italic/quote/list markers,
// links (text kept, URL dropped), bare URLs, tables (cells flattened with
// commas), horizontal rules, HTML tags, emoji and ANSI escapes. Plain prose
// passes through unchanged. Empty input or a code-only reply yields "".
std::string tts_filter(const std::string& md);

} // namespace krastor
