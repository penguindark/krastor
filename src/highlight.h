// Krastor — data-driven syntax highlighting for fenced code blocks
// Language definitions are JSON "plugins"
// in data/languages/*.json (same philosophy as the GLSL shader plugins):
//
//   { "name": "Python", "aliases": ["py"],
//     "line_comment": "#", "block_comment": ["/*", "*/"],
//     "strings": ["\"", "'"], "triples": ["\"\"\"", "'''"],
//     "keywords": ["def", "class", ...], "types": ["int", ...],
//     "builtins": ["print", ...], "decorators": true }
//
// The highlighter is a per-line scanner (comments/strings -> numbers ->
// identifiers) with state carried across lines only for triple strings and
// block comments. Output uses truecolor fg codes + fg-only resets (39) so the
// code-block background applied by the markdown pass survives.

#pragma once

#include <map>
#include <set>
#include <string>
#include <vector>

namespace krastor {

class Highlighter {
public:
    struct LangDef {
        std::string name;
        std::vector<std::string> strings;      // quote delimiters (" ' `)
        std::vector<std::string> triples;      // multi-line quotes (\"\"\" ''')
        std::string line_comment;              // "" = none
        std::string bc_open, bc_close;         // block comment, "" = none
        std::set<std::string> keywords, types, builtins;
        bool decorators = false;               // highlight @name tokens
        bool escapes = true;                   // backslash escapes in strings
    };

    // Loads every *.json in `dir` (unknown fields ignored, broken files
    // skipped). Returns false when the directory is missing or empty.
    bool load_dir(const std::string& dir);
    // exe_dir()/data/languages when present, else ./data/languages.
    bool load_default();

    // ANSI-styled copy of `code` for language tag `lang` (case-insensitive,
    // aliases resolved). Returns "" when the language is unknown (the caller
    // then keeps the block unhighlighted).
    std::string highlight(const std::string& code, const std::string& lang) const;

    size_t size() const { return langs_.size(); }

private:
    // key: lowercased language id / alias
    std::map<std::string, LangDef> langs_;
};

} // namespace krastor
