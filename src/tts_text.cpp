// Krastor — TTS text filter implementation (see tts_text.h).
// Line-oriented pass (fences, tables, line-start markers) + inline pass
// (links, URLs, emphasis, tags, emoji) + whitespace cleanup.

#include "tts_text.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace krastor {

namespace {

// ------------------------------------------------------------- UTF-8 bits
size_t utf8_len(unsigned char c)
{
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

uint32_t utf8_decode(const std::string& s, size_t i, size_t& len)
{
    const unsigned char c = static_cast<unsigned char>(s[i]);
    len = utf8_len(c);
    if (i + len > s.size()) { len = 1; return c; }
    uint32_t cp = c & (0xFF >> len);
    for (size_t k = 1; k < len; ++k)
        cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
    return cp;
}

void utf8_append(std::string& out, uint32_t cp)
{
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

// Emoji / symbol pictographs espeak-ng would spell out or skip oddly.
// Latin-1/Latin Extended accents (Italian) are NOT touched.
bool is_emoji(uint32_t cp)
{
    return (cp >= 0x1F000 && cp <= 0x1FAFF) ||  // pictographs, symbols, flags
           (cp >= 0x2600 && cp <= 0x27BF) ||    // misc symbols + dingbats
           (cp >= 0x2B00 && cp <= 0x2BFF) ||    // arrows
           cp == 0xFE0F || cp == 0x200D ||      // variation selector, ZWJ
           cp == 0x2764 || cp == 0x00A9 || cp == 0x00AE || cp == 0x2122;
}

std::string strip_emoji(const std::string& s)
{
    std::string out;
    for (size_t i = 0; i < s.size();) {
        size_t len = 1;
        const uint32_t cp = utf8_decode(s, i, len);
        if (!is_emoji(cp))
            utf8_append(out, cp);
        i += len;
    }
    return out;
}

// ------------------------------------------------------------- line tests
std::string trimmed(const std::string& s)
{
    const auto a = s.find_first_not_of(" \t\r");
    if (a == std::string::npos)
        return "";
    const auto b = s.find_last_not_of(" \t\r");
    return s.substr(a, b - a + 1);
}

bool is_fence(const std::string& t)
{
    return t.rfind("```", 0) == 0 || t.rfind("~~~", 0) == 0;
}

// A line of 3+ repeated - * _ (optional spaces between): ---, * * *, ___
bool is_hr(const std::string& t)
{
    char mark = 0;
    int count = 0;
    for (const char c : t) {
        if (c == ' ')
            continue;
        if (c != '-' && c != '*' && c != '_')
            return false;
        if (!mark)
            mark = c;
        else if (c != mark)
            return false;
        ++count;
    }
    return count >= 3;
}

// Table alignment row: |---|:--:| (only | : - and spaces, has | and -).
bool is_table_sep(const std::string& t)
{
    if (t.find('|') == std::string::npos || t.find('-') == std::string::npos)
        return false;
    for (const char c : t)
        if (c != '|' && c != '-' && c != ':' && c != ' ')
            return false;
    return true;
}

// ------------------------------------------------------------- inline pass
// [text](url) -> text, ![alt](url) -> "", [text][ref] -> text.
std::string strip_links(const std::string& s)
{
    std::string out;
    size_t i = 0;
    while (i < s.size()) {
        const bool image = s[i] == '!' && i + 1 < s.size() && s[i + 1] == '[';
        if (s[i] == '[' || image) {
            const size_t open = image ? i + 1 : i;
            const size_t close = s.find(']', open);
            if (close != std::string::npos && close + 1 < s.size() &&
                (s[close + 1] == '(' || s[close + 1] == '[')) {
                const char end = s[close + 1] == '(' ? ')' : ']';
                const size_t url_end = s.find(end, close + 2);
                if (url_end != std::string::npos) {
                    if (!image)
                        out += s.substr(open + 1, close - open - 1);
                    i = url_end + 1;
                    continue;
                }
            }
        }
        out += s[i++];
    }
    return out;
}

// Bare http(s):// and www. URLs: dropped (espeak spells them out horribly).
std::string strip_urls(const std::string& s)
{
    std::string out;
    size_t i = 0;
    while (i < s.size()) {
        const bool http = s.compare(i, 7, "http://") == 0 ||
                          s.compare(i, 8, "https://") == 0;
        const bool www = s.compare(i, 4, "www.") == 0;
        if (http || www) {
            size_t j = i;
            while (j < s.size() && !isspace(static_cast<unsigned char>(s[j])) &&
                   s[j] != '>' && s[j] != ')' && s[j] != ']' && s[j] != '"' &&
                   s[j] != '\'')
                ++j;
            i = j;  // drop the URL
            continue;
        }
        out += s[i++];
    }
    return out;
}

// HTML/XML-ish tags: < needs a letter, / or ! right after, so "a < b" is safe.
std::string strip_tags(const std::string& s)
{
    std::string out;
    size_t i = 0;
    while (i < s.size()) {
        if (s[i] == '<' && i + 1 < s.size() &&
            (isalpha(static_cast<unsigned char>(s[i + 1])) || s[i + 1] == '/' ||
             s[i + 1] == '!')) {
            const size_t close = s.find('>', i);
            if (close != std::string::npos) {
                i = close + 1;
                continue;
            }
        }
        out += s[i++];
    }
    return out;
}

bool is_word_char(char c)
{
    return isalnum(static_cast<unsigned char>(c)) != 0;
}

// Emphasis markers: ** __ ~~ * and _ (only outside words, so snake_case
// identifiers survive). Backticks are simply removed.
std::string strip_emphasis(const std::string& s)
{
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '`')
            continue;
        if ((c == '*' || c == '~') && i + 1 < s.size() && s[i + 1] == c) {
            ++i;  // ** and ~~
            continue;
        }
        if (c == '*' || c == '~')
            continue;
        if (c == '_' && i + 1 < s.size() && s[i + 1] == '_') {
            ++i;  // __
            continue;
        }
        if (c == '_') {
            const bool inside = i > 0 && i + 1 < s.size() &&
                                is_word_char(s[i - 1]) && is_word_char(s[i + 1]);
            if (!inside)
                continue;
        }
        out += c;
    }
    return out;
}

// Collapse space runs; drop a space before common punctuation (emoji/URL
// removal leaves "Ciao !" -> "Ciao!").
std::string tidy_spaces(const std::string& s)
{
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (c == ' ' || c == '\t') {
            if (!out.empty() && out.back() == ' ')
                continue;
            out += ' ';
            continue;
        }
        if ((c == ',' || c == '.' || c == ';' || c == ':' || c == '!' ||
             c == '?' || c == ')' || c == '}' || c == '%') &&
            !out.empty() && out.back() == ' ')
            out.pop_back();
        out += c;
    }
    return out;
}

// ------------------------------------------------------------- line pass
// Table row: cells flattened to "a, b, c" (values read fine aloud).
std::string flatten_table_row(const std::string& t)
{
    std::string out;
    size_t start = 0;
    while (start <= t.size()) {
        const size_t bar = t.find('|', start);
        const std::string cell = trimmed(
            t.substr(start, bar == std::string::npos ? std::string::npos
                                                    : bar - start));
        if (!cell.empty()) {
            if (!out.empty())
                out += ", ";
            out += cell;
        }
        if (bar == std::string::npos)
            break;
        start = bar + 1;
    }
    return out;
}

// Line-start Markdown: heading, blockquote, list bullet.
std::string strip_line_markers(std::string t)
{
    // heading: up to 6 '#' + space
    size_t h = 0;
    while (h < t.size() && t[h] == '#' && h < 7)
        ++h;
    if (h > 0 && h <= 6 && h < t.size() && (t[h] == ' ' || t[h] == '\t'))
        t = t.substr(h + 1);
    else if (h > 0 && h == t.size())
        t.clear();  // a bare "###" line
    // blockquote(s): > or >> ...
    while (!t.empty() && t[0] == '>') {
        t = t.substr(1);
        if (!t.empty() && t[0] == ' ')
            t = t.substr(1);
    }
    // list bullet: - * + or "1." / "1)" after optional indent
    size_t i = t.find_first_not_of(" \t");
    if (i == std::string::npos)
        return "";
    size_t after = i;
    if (t[i] == '-' || t[i] == '*' || t[i] == '+') {
        after = i + 1;
    } else if (isdigit(static_cast<unsigned char>(t[i]))) {
        size_t d = i;
        while (d < t.size() && isdigit(static_cast<unsigned char>(t[d])))
            ++d;
        if (d < t.size() && (t[d] == '.' || t[d] == ')'))
            after = d + 1;
    }
    if (after != i) {
        if (after < t.size() && (t[after] == ' ' || t[after] == '\t'))
            t = t.substr(after + 1);
        else if (after == t.size())
            t.clear();
    }
    return t;
}

std::string strip_ansi(const std::string& s)
{
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\x1B') {
            size_t j = i + 1;
            if (j < s.size() && s[j] == '[') {
                ++j;
                while (j < s.size() && s[j] != 'm')
                    ++j;
            }
            i = j;  // skip ESC[...m
            continue;
        }
        out += s[i];
    }
    return out;
}

} // namespace

std::string tts_filter(const std::string& md)
{
    const std::string src = md.find('\x1B') != std::string::npos
                                ? strip_ansi(md)
                                : md;

    std::vector<std::string> lines;
    std::string out;
    bool in_fence = false;
    size_t start = 0;
    while (start <= src.size()) {
        size_t e = src.find('\n', start);
        const bool last = e == std::string::npos;
        std::string line = src.substr(start, last ? std::string::npos : e - start);
        start = last ? src.size() + 1 : e + 1;

        const std::string t = trimmed(line);
        if (is_fence(t)) {
            in_fence = !in_fence;
            continue;
        }
        if (in_fence)
            continue;
        if (t.empty())
            continue;
        if (is_hr(t) || is_table_sep(t))
            continue;

        std::string s = t;
        const size_t bars = static_cast<size_t>(std::count(
            s.begin(), s.end(), '|'));
        if (bars >= 2) {
            s = flatten_table_row(s);
        } else {
            s = strip_line_markers(s);
        }
        s = strip_links(s);
        s = strip_urls(s);
        s = strip_emphasis(s);
        s = strip_tags(s);
        s = strip_emoji(s);
        s = tidy_spaces(trimmed(s));
        if (!s.empty())
            lines.push_back(s);
    }

    // Join the kept lines; blank lines are dropped (pauses come from the
    // line break and the punctuation itself).
    for (const auto& l : lines) {
        if (l.empty())
            continue;
        if (!out.empty())
            out += '\n';
        out += l;
    }
    return out;
}

} // namespace krastor
