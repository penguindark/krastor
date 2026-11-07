#include "md_format.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace krastor {
namespace {

// ---- SGR style helpers ------------------------------------------------------
const char* kReset = "\x1b[0m";
const char* kBold  = "\x1b[1m";
const char* kDim   = "\x1b[2m";

std::string fg(int r, int g, int b)
{
    char buf[40];
    std::snprintf(buf, sizeof(buf), "\x1b[38;2;%d;%d;%dm", r, g, b);
    return buf;
}
std::string bg(int r, int g, int b)
{
    char buf[40];
    std::snprintf(buf, sizeof(buf), "\x1b[48;2;%d;%d;%dm", r, g, b);
    return buf;
}

// Palette (dark theme, opencode-ish).
const char* kH1      = "\x1b[38;2;96;155;255m";   // bright blue
const char* kH2      = "\x1b[38;2;86;199;217m";   // cyan
const char* kBullet  = "\x1b[38;2;230;150;60m";   // orange
const char* kItalic  = "\x1b[38;2;205;160;255m";  // soft purple
const char* kLink    = "\x1b[38;2;96;155;255m";   // blue
const char* kSep     = "\x1b[38;2;110;110;120m";  // dim gray (table/rule bars)
const char* kCodeFg  = "\x1b[38;2;210;200;120m";  // khaki inline code
const char* kCodeBg  = "\x1b[48;2;38;40;48m";     // inline code background
const char* kBlockBg = "\x1b[48;2;24;26;33m";     // fenced block background
const char* kTableHead = "\x1b[38;2;198;120;221m"; // violet table header text

const char* kRule = "────────────────────────────────────────────────────────────";

// ---- small helpers -----------------------------------------------------------
size_t utf8_len(char c)
{
    const unsigned char u = static_cast<unsigned char>(c);
    if (u >= 0xF0) return 4;
    if (u >= 0xE0) return 3;
    if (u >= 0xC0) return 2;
    return 1;
}

size_t cp_count(const std::string& s)
{
    size_t n = 0;
    for (size_t i = 0; i < s.size(); i += utf8_len(s[i]))
        ++n;
    return n;
}

// Visible codepoint count of an ANSI-styled string (escape sequences skipped):
// markdown markers like ` don't occupy screen space once styled.
size_t vis_cp(const std::string& s)
{
    size_t n = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\x1b') {
            while (i < s.size() && s[i] != 'm')
                ++i;
            continue;
        }
        i += utf8_len(s[i]) - 1;
        ++n;
    }
    return n;
}

std::string pad_right(const std::string& s, size_t w)
{
    std::string out = s;
    for (size_t n = cp_count(s); n < w; ++n)
        out += ' ';
    return out;
}

std::string ltrim(const std::string& s)
{
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t'))
        ++i;
    return s.substr(i);
}

std::string trim(const std::string& s)
{
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t'))
        ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t'))
        --b;
    return s.substr(a, b - a);
}

// ---- inline pass -------------------------------------------------------------
bool is_space(char c) { return c == ' ' || c == '\t' || c == '\r'; }

std::string inline_md(const std::string& s)
{
    std::string out;
    const size_t n = s.size();
    size_t i = 0;
    while (i < n) {
        const char c = s[i];
        // `code` span: content is copied raw.
        if (c == '`') {
            const size_t j = s.find('`', i + 1);
            if (j != std::string::npos && j > i + 1) {
                out += kCodeFg;
                out += kCodeBg;
                out += s.substr(i + 1, j - i - 1);
                out += kReset;
                i = j + 1;
                continue;
            }
        }
        // **bold**
        if (c == '*' && i + 1 < n && s[i + 1] == '*') {
            const size_t j = s.find("**", i + 2);
            if (j != std::string::npos && j > i + 2) {
                out += kBold;
                out += inline_md(s.substr(i + 2, j - i - 2));
                out += kReset;
                i = j + 2;
                continue;
            }
        }
        // *italic* (bold + purple; mono fonts have no italic). Require non-space
        // borders so "3 * 4 * 5" is left alone. `_` emphasis is not handled
        // (snake_case safety).
        if (c == '*' && (i + 1 >= n || s[i + 1] != '*')) {
            size_t j = i + 1;
            while (j < n && s[j] != '*')
                ++j;
            if (j < n && j > i + 1 && !is_space(s[i + 1]) && !is_space(s[j - 1])) {
                out += kBold;
                out += kItalic;
                out += inline_md(s.substr(i + 1, j - i - 1));
                out += kReset;
                i = j + 1;
                continue;
            }
        }
        // [text](url)
        if (c == '[') {
            const size_t close = s.find("](", i + 1);
            if (close != std::string::npos) {
                const size_t end = s.find(')', close + 2);
                if (end != std::string::npos) {
                    out += kLink;
                    out += inline_md(s.substr(i + 1, close - i - 1));
                    out += kReset;
                    out += kDim;
                    out += " (" + s.substr(close + 2, end - close - 2) + ")";
                    out += kReset;
                    i = end + 1;
                    continue;
                }
            }
        }
        const size_t step = utf8_len(c);
        out.append(s, i, step);
        i += step;
    }
    return out;
}

// ---- block scanners ----------------------------------------------------------
// Fenced code block opener: up to 3 spaces, then 3+ ` or ~. On success returns
// true and fills marker (the fence char run) + lang (fence info string).
bool parse_fence(const std::string& ln, std::string& marker, std::string& lang)
{
    size_t i = 0;
    while (i < ln.size() && ln[i] == ' ' && i < 3)
        ++i;
    if (i >= ln.size() || (ln[i] != '`' && ln[i] != '~'))
        return false;
    size_t j = i;
    while (j < ln.size() && ln[j] == ln[i])
        ++j;
    if (j - i < 3)
        return false;
    marker = std::string(j - i, ln[i]);
    lang = trim(ln.substr(j));
    // Info strings can carry extra params (```cpp title=x): keep the first word.
    const size_t sp = lang.find_first_of(" \t");
    if (sp != std::string::npos)
        lang = lang.substr(0, sp);
    return true;
}

bool is_fence_close(const std::string& ln, const std::string& marker)
{
    size_t i = 0;
    while (i < ln.size() && ln[i] == ' ' && i < 3)
        ++i;
    size_t j = i;
    while (j < ln.size() && ln[j] == marker[0])
        ++j;
    return j - i >= marker.size() && trim(ln.substr(j)).empty();
}

// Heading level (1..6) or 0; `rest` receives the text after the hashes.
int heading_level(const std::string& ln, std::string& rest)
{
    size_t i = 0;
    while (i < ln.size() && ln[i] == ' ' && i < 3)
        ++i;
    int lvl = 0;
    while (i < ln.size() && ln[i] == '#' && lvl < 7) {
        ++lvl;
        ++i;
    }
    if (lvl < 1 || lvl > 6 || i >= ln.size() || (ln[i] != ' ' && ln[i] != '\t'))
        return 0;
    rest = trim(ln.substr(i));
    return lvl;
}

bool is_hr(const std::string& ln)
{
    size_t i = 0;
    while (i < ln.size() && ln[i] == ' ' && i < 3)
        ++i;
    if (i >= ln.size())
        return false;
    const char c = ln[i];
    if (c != '-' && c != '*' && c != '_')
        return false;
    int count = 0;
    for (; i < ln.size(); ++i) {
        if (ln[i] == c)
            ++count;
        else if (ln[i] != ' ' && ln[i] != '\t')
            return false;
    }
    return count >= 3;
}

// "  - item" / "  * item" / "  + item" -> indent + marker + content.
bool list_item(const std::string& ln, std::string& indent, std::string& rest)
{
    size_t i = 0;
    while (i < ln.size() && (ln[i] == ' ' || ln[i] == '\t'))
        ++i;
    if (i >= ln.size() || (ln[i] != '-' && ln[i] != '*' && ln[i] != '+'))
        return false;
    if (i + 1 >= ln.size() || (ln[i + 1] != ' ' && ln[i + 1] != '\t'))
        return false;
    indent = ln.substr(0, i);
    rest = ln.substr(i + 1);
    return true;
}

// "  12. item" -> indent + "12." + content.
bool olist_item(const std::string& ln, std::string& indent,
                std::string& num, std::string& rest)
{
    size_t i = 0;
    while (i < ln.size() && (ln[i] == ' ' || ln[i] == '\t'))
        ++i;
    size_t j = i;
    while (j < ln.size() && ln[j] >= '0' && ln[j] <= '9')
        ++j;
    if (j == i || j > i + 3 || j + 1 >= ln.size())
        return false;
    if (ln[j] != '.' && ln[j] != ')')
        return false;
    if (ln[j + 1] != ' ' && ln[j + 1] != '\t')
        return false;
    indent = ln.substr(0, i);
    num = ln.substr(i, j - i + 1);
    rest = ln.substr(j + 1);
    return true;
}

bool table_row(const std::string& ln) { return ltrim(ln).size() > 1 && ltrim(ln)[0] == '|'; }

bool table_sep(const std::string& ln)
{
    const std::string t = trim(ln);
    if (t.size() < 3 || t[0] != '|')
        return false;
    bool dash = false;
    for (char c : t) {
        if (c == '-')
            dash = true;
        else if (c != '|' && c != ':' && c != '-' && c != ' ')
            return false;
    }
    return dash;
}

std::vector<std::string> table_cells(const std::string& ln)
{
    std::string t = trim(ln);
    if (!t.empty() && t.front() == '|')
        t = t.substr(1);
    if (!t.empty() && t.back() == '|')
        t.pop_back();
    std::vector<std::string> cells;
    size_t start = 0;
    while (true) {
        const size_t bar = t.find('|', start);
        if (bar == std::string::npos) {
            cells.push_back(trim(t.substr(start)));
            break;
        }
        cells.push_back(trim(t.substr(start, bar - start)));
        start = bar + 1;
    }
    return cells;
}

void append_block_bg(std::vector<std::string>& out, const std::string& code)
{
    size_t start = 0;
    while (start <= code.size()) {
        const size_t nl = code.find('\n', start);
        std::string line =
            code.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        // An empty line would carry no colored token, so the renderer paints no
        // background and the block shows a transparent gap: keep one space so
        // the full-line background stays continuous.
        if (line.empty())
            line = " ";
        out.push_back(std::string(kBlockBg) + line + kReset);
        if (nl == std::string::npos)
            break;
        start = nl + 1;
    }
}

} // namespace

std::string md_to_ansi(const std::string& md, const CodeHighlightFn& hl)
{
    std::vector<std::string> lines;
    {
        size_t start = 0;
        while (true) {
            const size_t nl = md.find('\n', start);
            if (nl == std::string::npos) {
                lines.push_back(md.substr(start));
                break;
            }
            lines.push_back(md.substr(start, nl - start));
            start = nl + 1;
        }
    }

    std::vector<std::string> out;
    const size_t n = lines.size();
    size_t i = 0;
    while (i < n) {
        const std::string& ln = lines[i];

        // Terminal passthrough: a line that already carries ESC bytes is
        // emitted verbatim (pre-styled tool output, ANSI art) while the
        // other lines keep their markdown formatting. Lines consumed inside
        // a fenced block or table below never reach this check.
        if (ln.find('\x1b') != std::string::npos) {
            out.push_back(ln);
            ++i;
            continue;
        }

        // fenced code block
        std::string marker, lang;
        if (parse_fence(ln, marker, lang)) {
            std::string code;
            ++i;
            while (i < n && !is_fence_close(lines[i], marker)) {
                if (!code.empty())
                    code += '\n';
                code += lines[i];
                ++i;
            }
            if (i < n)
                ++i;  // closing fence
            if (!lang.empty())
                // Header bar: same background as the block so the renderer's
                // full-line fill makes one continuous rectangle.
                out.push_back(std::string(kBlockBg) + kDim + "  " + lang + kReset);
            const std::string styled = hl ? hl(code, lang) : std::string();
            append_block_bg(out, styled.empty() ? code : styled);
            continue;
        }

        // table: full grid like the OpenCode TUI - box-drawing border on
        // every side and between all rows, violet header, 1-cell padding.
        if (table_row(ln) && i + 1 < n && table_sep(lines[i + 1])) {
            std::vector<std::vector<std::string>> rows;
            size_t j = i;
            while (j < n && table_row(lines[j])) {
                if (!table_sep(lines[j]))
                    rows.push_back(table_cells(lines[j]));
                ++j;
            }
            size_t cols = 0;
            for (const auto& r : rows)
                cols = std::max(cols, r.size());
            // Style every cell first: column widths must be measured on the
            // rendered text (markdown markers vanish once styled).
            std::vector<std::vector<std::string>> cell(rows.size());
            std::vector<size_t> w(cols, 3);
            for (size_t r = 0; r < rows.size(); ++r)
                for (size_t c = 0; c < cols; ++c) {
                    cell[r].push_back(c < rows[r].size()
                                          ? inline_md(rows[r][c])
                                          : std::string());
                    w[c] = std::max(w[c], vis_cp(cell[r][c]));
                }
            // Column alignment from the separator row (GFM): `:---` left,
            // `---:` right, `:---:` center; plain `---` defaults to left.
            std::vector<char> align(cols, 'l');
            {
                const std::vector<std::string> seps = table_cells(lines[i + 1]);
                for (size_t c = 0; c < cols && c < seps.size(); ++c) {
                    const std::string t = trim(seps[c]);
                    const bool lc = !t.empty() && t.front() == ':';
                    const bool rc = t.size() > 1 && t.back() == ':';
                    align[c] = (lc && rc) ? 'c' : (rc ? 'r' : 'l');
                }
            }

            // Horizontal rule: left/middle/right corners + dashes per column.
            auto bar = [&](const char* l, const char* m, const char* rc) {
                std::string s = std::string(kSep) + l;
                for (size_t c = 0; c < cols; ++c) {
                    for (size_t k = 0; k < w[c] + 2; ++k)
                        s += "─";
                    s += (c + 1 < cols) ? m : rc;
                }
                return s + kReset;
            };

            out.push_back(bar("┌", "┬", "┐"));
            for (size_t r = 0; r < rows.size(); ++r) {
                std::string line = std::string(kSep) + "│" + kReset + " ";
                for (size_t c = 0; c < cols; ++c) {
                    const size_t raw = vis_cp(cell[r][c]);
                    const size_t pad = w[c] - raw;
                    const size_t left = align[c] == 'r'   ? pad
                                        : align[c] == 'c' ? pad / 2
                                                          : 0;
                    line += std::string(left, ' ');
                    if (r == 0)
                        line += std::string(kTableHead) + cell[r][c] + kReset;
                    else
                        line += cell[r][c];
                    line += std::string(pad - left, ' ');
                    line += " ";
                    line += kSep;
                    line += "│";
                    if (c + 1 < cols)
                        line += " ";
                }
                line += kReset;
                out.push_back(line);
                out.push_back(r + 1 < rows.size() ? bar("├", "┼", "┤")
                                                  : bar("└", "┴", "┘"));
            }
            i = j;
            continue;
        }

        // heading
        std::string rest;
        const int lvl = heading_level(ln, rest);
        if (lvl) {
            out.push_back(std::string(lvl == 1 ? kH1 : kH2) + kBold +
                          inline_md(rest) + kReset);
            ++i;
            continue;
        }

        // horizontal rule
        if (is_hr(ln)) {
            out.push_back(std::string(kSep) + kRule + kReset);
            ++i;
            continue;
        }

        // blockquote
        {
            const std::string t = ltrim(ln);
            if (t.size() >= 2 && t[0] == '>' && (t[1] == ' ' || t[1] == '\t')) {
                out.push_back(std::string(kDim) + "│ " +
                              inline_md(trim(t.substr(1))) + kReset);
                ++i;
                continue;
            }
            if (t.size() == 1 && t[0] == '>') {
                out.push_back(std::string(kDim) + "│" + kReset);
                ++i;
                continue;
            }
        }

        // unordered list
        {
            std::string indent, body;
            if (list_item(ln, indent, body)) {
                out.push_back(indent + kBullet + "- " + kReset + inline_md(ltrim(body)));
                ++i;
                continue;
            }
        }
        // ordered list
        {
            std::string indent, num, body;
            if (olist_item(ln, indent, num, body)) {
                out.push_back(indent + kBullet + num + " " + kReset +
                              inline_md(ltrim(body)));
                ++i;
                continue;
            }
        }

        // plain line
        out.push_back(inline_md(ln));
        ++i;
    }

    std::string res;
    for (size_t k = 0; k < out.size(); ++k) {
        if (k)
            res += '\n';
        res += out[k];
    }
    if (!md.empty() && md.back() == '\n' && !res.empty() && res.back() != '\n')
        res += '\n';
    return res;
}

} // namespace krastor
