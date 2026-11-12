#include "highlight.h"

#include "config.h"

#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

namespace krastor {
namespace {

// Token colors (fg-only; reset with 39 so the block background survives).
const char* cKeyword = "\x1b[38;2;198;120;221m";  // purple
const char* cType    = "\x1b[38;2;86;199;217m";   // cyan
const char* cBuiltin = "\x1b[38;2;229;200;100m";  // yellow
const char* cString  = "\x1b[38;2;150;200;120m";  // green
const char* cComment = "\x1b[38;2;110;110;120m";  // gray
const char* cNumber  = "\x1b[38;2;230;150;60m";   // orange
const char* cDecor   = "\x1b[38;2;229;200;100m";  // yellow
const char* cFgReset = "\x1b[39m";

// Extra token classes for the markup / yaml / markdown scanners.
const char* cTag     = "\x1b[38;2;97;175;239m";   // blue
const char* cAttr    = "\x1b[38;2;229;200;100m";  // yellow
const char* cEntity  = "\x1b[38;2;230;150;60m";   // orange
const char* cMeta    = "\x1b[38;2;110;110;120m";  // gray
const char* cKey     = "\x1b[38;2;97;175;239m";   // blue
const char* cHeading = "\x1b[38;2;198;120;221m";  // purple
const char* cEmph    = "\x1b[38;2;150;200;120m";  // green
const char* cCode    = "\x1b[38;2;230;150;60m";   // orange
const char* cLink    = "\x1b[38;2;86;199;217m";   // cyan

// Per-block palette: the built-in colors, overridden by the pack's "colors"
// map (token class -> "#rrggbb"). Declared before the scanners so they can
// take it by const ref.
struct Palette {
    std::string keyword = cKeyword, type = cType, builtin = cBuiltin,
                string = cString, comment = cComment, number = cNumber,
                decor = cDecor, tag = cTag, attr = cAttr, entity = cEntity,
                meta = cMeta, key = cKey, heading = cHeading,
                emphasis = cEmph, code = cCode, link = cLink;
};

// "#rrggbb" -> ANSI truecolor fg; "" when the string is not a valid color.
std::string hex_ansi(const std::string& hex)
{
    if (hex.size() != 7 || hex[0] != '#')
        return {};
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        return -1;
    };
    int v[3] = {0, 0, 0};
    for (int k = 0; k < 3; ++k) {
        const int hi = nibble(hex[1 + 2 * k]), lo = nibble(hex[2 + 2 * k]);
        if (hi < 0 || lo < 0)
            return {};
        v[k] = hi * 16 + lo;
    }
    return "\x1b[38;2;" + std::to_string(v[0]) + ";" + std::to_string(v[1]) +
           ";" + std::to_string(v[2]) + "m";
}

Palette make_palette(const std::map<std::string, std::string>& over)
{
    Palette p;
    auto set = [&](const char* key, std::string& dst) {
        const auto it = over.find(key);
        if (it == over.end())
            return;
        const std::string a = hex_ansi(it->second);
        if (!a.empty())  // invalid hex: keep the built-in color
            dst = a;
    };
    set("keyword", p.keyword);
    set("type", p.type);
    set("builtin", p.builtin);
    set("string", p.string);
    set("comment", p.comment);
    set("number", p.number);
    set("decor", p.decor);
    set("tag", p.tag);
    set("attr", p.attr);
    set("entity", p.entity);
    set("meta", p.meta);
    set("key", p.key);
    set("heading", p.heading);
    set("emphasis", p.emphasis);
    set("code", p.code);
    set("link", p.link);
    return p;
}

bool starts_with(const std::string& s, size_t i, const std::string& p)
{
    return p.size() <= s.size() - i && s.compare(i, p.size(), p) == 0;
}

bool is_ident(char c)
{
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

// Multi-line state carried between lines of one code block.
struct State {
    std::string triple;  // open triple-quote delimiter ("" = not in one)
    bool block = false;  // inside an unterminated /* ... */ or <!-- ... -->
    bool tag = false;    // inside an unterminated markup tag (attrs follow)
};

std::string colorize_line(const std::string& line,
                          const Highlighter::LangDef& L, State& st,
                          const Palette& p)
{
    std::string out;
    const size_t n = line.size();
    auto emit = [&](const std::string& col, const std::string& txt) {
        out += col;
        out += txt;
        out += cFgReset;
    };
    size_t i = 0;
    while (i < n) {
        // --- carried-over multi-line states ---
        if (!st.triple.empty()) {
            const size_t j = line.find(st.triple, i);
            if (j == std::string::npos) {
                emit(p.string, line.substr(i));
                return out;  // still open
            }
            emit(p.string, line.substr(i, j + st.triple.size() - i));
            i = j + st.triple.size();
            st.triple.clear();
            continue;
        }
        if (st.block) {
            const size_t j = line.find(L.bc_close, i);
            if (j == std::string::npos) {
                emit(p.comment, line.substr(i));
                return out;
            }
            emit(p.comment, line.substr(i, j + L.bc_close.size() - i));
            i = j + L.bc_close.size();
            st.block = false;
            continue;
        }

        // --- line comment ---
        if (!L.line_comment.empty() && starts_with(line, i, L.line_comment)) {
            emit(p.comment, line.substr(i));
            return out;
        }
        // --- block comment opener ---
        if (!L.bc_open.empty() && starts_with(line, i, L.bc_open)) {
            const size_t j = line.find(L.bc_close, i + L.bc_open.size());
            if (j == std::string::npos) {
                emit(p.comment, line.substr(i));
                st.block = true;
                return out;
            }
            emit(p.comment, line.substr(i, j + L.bc_close.size() - i));
            i = j + L.bc_close.size();
            continue;
        }
        // --- triple quotes (checked before plain strings) ---
        bool matched = false;
        for (const auto& t : L.triples) {
            if (!starts_with(line, i, t))
                continue;
            const size_t j = line.find(t, i + t.size());
            if (j == std::string::npos) {
                emit(p.string, line.substr(i));
                st.triple = t;
                return out;
            }
            emit(p.string, line.substr(i, j + t.size() - i));
            i = j + t.size();
            matched = true;
            break;
        }
        if (matched)
            continue;
        // --- single-line strings ---
        for (const auto& q : L.strings) {
            if (!starts_with(line, i, q))
                continue;
            size_t j = i + q.size();
            bool closed = false;
            while (j < n) {
                if (L.escapes && line[j] == '\\') {
                    j += 2;
                    continue;
                }
                if (starts_with(line, j, q)) {
                    j += q.size();
                    closed = true;
                    break;
                }
                ++j;
            }
            emit(p.string, line.substr(i, (closed ? j : n) - i));
            i = closed ? j : n;
            matched = true;
            break;
        }
        if (matched)
            continue;

        const char c = line[i];
        // --- decorator ---
        if (L.decorators && c == '@') {
            size_t j = i + 1;
            while (j < n && (is_ident(line[j]) || line[j] == '.'))
                ++j;
            emit(p.decor, line.substr(i, j - i));
            i = j;
            continue;
        }
        // --- number (hex/dec/float; stops at operators) ---
        if (std::isdigit(static_cast<unsigned char>(c))) {
            size_t j = i;
            while (j < n && (std::isalnum(static_cast<unsigned char>(line[j])) ||
                             line[j] == '.' || line[j] == '_'))
                ++j;
            emit(p.number, line.substr(i, j - i));
            i = j;
            continue;
        }
        // --- identifier / keyword ---
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            size_t j = i;
            while (j < n && is_ident(line[j]))
                ++j;
            const std::string w = line.substr(i, j - i);
            if (L.keywords.count(w))
                emit(p.keyword, w);
            else if (L.types.count(w))
                emit(p.type, w);
            else if (L.builtins.count(w))
                emit(p.builtin, w);
            else
                out += w;
            i = j;
            continue;
        }
        out += c;
        ++i;
    }
    return out;
}

// --- markup scanner (html/xml): tags, attributes, strings, entities,
// declarations (<!DOCTYPE>, <?xml?>, <![CDATA[...]>), <!-- --> comments.
// A tag may span lines: State::tag stays set until the closing '>'.
std::string colorize_markup(const std::string& line,
                            const Highlighter::LangDef& L, State& st,
                            const Palette& p)
{
    std::string out;
    const size_t n = line.size();
    auto emit = [&](const std::string& col, const std::string& txt) {
        out += col;
        out += txt;
        out += cFgReset;
    };
    auto name_char = [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '-' ||
               c == '_' || c == ':' || c == '.';
    };
    // Consume attribute text starting at i; returns the new position.
    // Quoted values -> string, bare names -> attr, '=' and spaces plain.
    // Stops at '>' (closing the tag) or end of line.
    auto attrs = [&](size_t i) {
        while (i < n) {
            const char c = line[i];
            if (c == '>') {
                emit(p.tag, ">");
                st.tag = false;
                return i + 1;
            }
            if (c == '"' || c == '\'') {
                const size_t j = line.find(c, i + 1);
                const size_t end = j == std::string::npos ? n : j + 1;
                emit(p.string, line.substr(i, end - i));
                i = end;
                continue;
            }
            if (std::isspace(static_cast<unsigned char>(c)) || c == '=') {
                out += c;
                ++i;
                continue;
            }
            size_t j = i;
            while (j < n && name_char(line[j]))
                ++j;
            if (j > i)
                emit(p.attr, line.substr(i, j - i));
            else {
                out += c;
                ++j;
            }
            i = j;
        }
        return i;
    };
    size_t i = 0;
    if (st.block) {  // inside a multi-line <!-- --> comment
        const size_t j =
            L.bc_close.empty() ? std::string::npos : line.find(L.bc_close, i);
        if (j == std::string::npos) {
            emit(p.comment, line.substr(i));
            return out;
        }
        emit(p.comment, line.substr(i, j + L.bc_close.size() - i));
        i = j + L.bc_close.size();
        st.block = false;
    }
    if (st.tag)  // attribute lines of a tag opened earlier
        i = attrs(i);
    while (i < n) {
        if (!L.bc_open.empty() && starts_with(line, i, L.bc_open)) {
            const size_t j = line.find(L.bc_close, i + L.bc_open.size());
            if (j == std::string::npos) {
                emit(p.comment, line.substr(i));
                st.block = true;
                return out;
            }
            emit(p.comment, line.substr(i, j + L.bc_close.size() - i));
            i = j + L.bc_close.size();
            continue;
        }
        if (starts_with(line, i, "<!") || starts_with(line, i, "<?")) {
            const size_t j = line.find('>', i);
            const size_t end = j == std::string::npos ? n : j + 1;
            emit(p.meta, line.substr(i, end - i));
            i = end;
            continue;
        }
        if (line[i] == '<' && i + 1 < n &&
            (std::isalpha(static_cast<unsigned char>(line[i + 1])) ||
             line[i + 1] == '/')) {
            size_t j = line[i + 1] == '/' ? i + 2 : i + 1;
            while (j < n && name_char(line[j]))
                ++j;
            emit(p.tag, line.substr(i, j - i));
            st.tag = true;
            i = attrs(j);
            continue;
        }
        if (line[i] == '&') {
            const size_t j = line.find(';', i + 1);
            if (j != std::string::npos && j - i <= 10) {
                emit(p.entity, line.substr(i, j + 1 - i));
                i = j + 1;
                continue;
            }
        }
        out += line[i];
        ++i;
    }
    return out;
}

// --- yaml scanner: keys, bullets, document markers, comments, strings,
// anchors/aliases/tags, literals, numbers, block scalars (| and >).
std::string colorize_yaml(const std::string& line,
                          const Highlighter::LangDef& L, State& st,
                          const Palette& p)
{
    std::string out;
    const size_t n = line.size();
    auto emit = [&](const std::string& col, const std::string& txt) {
        out += col;
        out += txt;
        out += cFgReset;
    };
    const size_t first = line.find_first_not_of(" \t");
    if (first == std::string::npos) {
        out += line;
        return out;
    }
    if (st.block) {  // inside a | or > block scalar: indented text lines
        if (line[first] == ' ' || line[first] == '\t') {
            emit(p.string, line);
            return out;
        }
        st.block = false;  // dedented: the scalar ended
    }
    out += line.substr(0, first);  // indentation
    // document markers
    if (starts_with(line, first, "---") || starts_with(line, first, "...")) {
        emit(p.meta, line.substr(first));
        return out;
    }
    size_t i = first;
    // sequence bullet "- " (or a lone "-")
    if (line[i] == '-' && (i + 1 == n || line[i + 1] == ' ')) {
        emit(p.keyword, "-");
        ++i;
    }
    auto at = [&](size_t k) { return k < n ? line[k] : '\0'; };
    while (i < n) {
        const char c = line[i];
        if (c == '#' && (i == 0 || std::isspace(static_cast<unsigned char>(
                                             line[i - 1])))) {
            emit(p.comment, line.substr(i));
            return out;
        }
        if (c == '"' || c == '\'') {
            size_t j = i + 1;
            while (j < n && line[j] != c)
                ++j;
            const size_t end = j < n ? j + 1 : n;
            emit(p.string, line.substr(i, end - i));
            i = end;
            continue;
        }
        if (c == '&' || c == '*' || starts_with(line, i, "!!")) {
            const size_t s = i;
            size_t j = c == '!' ? i + 2 : i + 1;
            while (j < n && !std::isspace(static_cast<unsigned char>(line[j])))
                ++j;
            emit(p.decor, line.substr(s, j - s));
            i = j;
            continue;
        }
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' ||
            c == '.' || c == '-' || c == '/') {
            const size_t s = i;
            size_t j = i;
            while (j < n && (std::isalnum(static_cast<unsigned char>(line[j])) ||
                             line[j] == '_' || line[j] == '.' ||
                             line[j] == '-' || line[j] == '/'))
                ++j;
            const std::string w = line.substr(s, j - s);
            // "key:" -> key color (colon followed by space or end of line)
            if (at(j) == ':' && (at(j + 1) == ' ' || at(j + 1) == '\0')) {
                emit(p.key, w + ":");
                i = j + 1;
                continue;
            }
            if (L.types.count(w))
                emit(p.type, w);
            else if (!w.empty() && std::isdigit(static_cast<unsigned char>(w[0])))
                emit(p.number, w);
            else
                out += w;
            i = j;
            continue;
        }
        out += c;
        ++i;
    }
    // block scalar opener: the line ends with | or > (|- |+ |> | >-)
    const std::string tail = line.substr(line.find_last_not_of(" \t") + 1);
    if (!tail.empty() && (tail[0] == '|' || tail[0] == '>') &&
        tail.size() <= 2)
        st.block = true;
    return out;
}

// --- markdown scanner: headings, blockquotes, bullets, emphasis, inline
// code, links, fenced code blocks (``` carried across lines).
std::string colorize_markdown(const std::string& line,
                              const Highlighter::LangDef& L, State& st,
                              const Palette& p)
{
    std::string out;
    const size_t n = line.size();
    auto emit = [&](const std::string& col, const std::string& txt) {
        out += col;
        out += txt;
        out += cFgReset;
    };
    const size_t first = line.find_first_not_of(" \t");
    if (first == std::string::npos) {
        out += line;
        return out;
    }
    // fenced code block: everything until the closing fence
    if (!st.triple.empty()) {
        if (starts_with(line, first, "```"))
            st.triple.clear();
        emit(p.code, line);
        return out;
    }
    if (starts_with(line, first, "```")) {
        st.triple = "```";
        emit(p.code, line);
        return out;
    }
    // heading line
    if (line[first] == '#') {
        emit(p.heading, line);
        return out;
    }
    size_t i = first;
    // blockquote marker
    if (line[i] == '>') {
        emit(p.meta, ">");
        ++i;
    }
    // list bullet: - * + or "1." followed by a space
    {
        size_t j = i;
        if ((line[j] == '-' || line[j] == '*' || line[j] == '+') &&
            j + 1 < n && line[j + 1] == ' ')
            ++j;
        else if (std::isdigit(static_cast<unsigned char>(line[j]))) {
            while (j < n && std::isdigit(static_cast<unsigned char>(line[j])))
                ++j;
            if (j + 1 < n && line[j] == '.' && line[j + 1] == ' ')
                j += 2;
            else
                j = i;
        }
        if (j > i) {
            emit(p.keyword, line.substr(i, j - i));
            i = j;
        }
    }
    // inline spans
    auto at2 = [&](size_t k) { return k < n ? line[k] : '\0'; };
    while (i < n) {
        const char c = line[i];
        if (c == '`') {
            const size_t j = line.find('`', i + 1);
            const size_t end = j == std::string::npos ? n : j + 1;
            emit(p.code, line.substr(i, end - i));
            i = end;
            continue;
        }
        if (c == '*' || c == '_') {
            const std::string run(1, c);
            const bool dbl = at2(i + 1) == c;
            const std::string close = dbl ? run + run : run;
            const size_t j = line.find(close, i + close.size());
            if (j != std::string::npos) {
                emit(p.emphasis, line.substr(i, j + close.size() - i));
                i = j + close.size();
                continue;
            }
        }
        if (c == '[' || c == '!') {
            const size_t rb = line.find(']', i);
            if (rb != std::string::npos && rb + 1 < n && line[rb + 1] == '(') {
                const size_t rp = line.find(')', rb + 2);
                const size_t end = rp == std::string::npos ? n : rp + 1;
                emit(p.link, line.substr(i, end - i));
                i = end;
                continue;
            }
        }
        out += c;
        ++i;
    }
    (void)L;
    return out;
}

std::string lower(std::string s)
{
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

} // namespace

bool Highlighter::load_dir(const std::string& dir)
{
    std::error_code ec;
    std::filesystem::directory_iterator it(dir, ec);
    if (ec)
        return false;
    for (const auto& e : it) {
        if (e.path().extension() != ".json")
            continue;
        std::ifstream f(e.path());
        if (!f)
            continue;
        nlohmann::json j;
        try {
            j = nlohmann::json::parse(f, nullptr, true, true);  // jsonc ok
        } catch (const nlohmann::json::exception&) {
            continue;  // broken pack: skip, keep the others
        }
        if (!j.is_object())
            continue;
        LangDef d;
        d.name = j.value("name", e.path().stem().string());
        d.mode = j.value("mode", std::string("code"));
        if (d.mode != "code" && d.mode != "markup" && d.mode != "yaml" &&
            d.mode != "markdown")
            continue;  // unknown scanner: skip the pack, keep the others
        if (j.contains("colors") && j["colors"].is_object())
            for (const auto& kv : j["colors"].items())
                if (kv.value().is_string())
                    d.colors[kv.key()] = kv.value().get<std::string>();
        // Empty delimiters would make the scanner spin without advancing, so
        // only non-empty ones are accepted.
        if (j.contains("strings") && j["strings"].is_array())
            for (const auto& s : j["strings"])
                if (s.is_string() && !s.get<std::string>().empty())
                    d.strings.push_back(s.get<std::string>());
        if (j.contains("triples") && j["triples"].is_array())
            for (const auto& s : j["triples"])
                if (s.is_string() && !s.get<std::string>().empty())
                    d.triples.push_back(s.get<std::string>());
        d.line_comment = j.value("line_comment", std::string());
        if (j.contains("block_comment") && j["block_comment"].is_array() &&
            j["block_comment"].size() == 2) {
            d.bc_open = j["block_comment"][0].get<std::string>();
            d.bc_close = j["block_comment"][1].get<std::string>();
        }
        auto fill = [&](const char* key, std::set<std::string>& dst) {
            if (j.contains(key) && j[key].is_array())
                for (const auto& s : j[key])
                    if (s.is_string())
                        dst.insert(s.get<std::string>());
        };
        fill("keywords", d.keywords);
        fill("types", d.types);
        fill("builtins", d.builtins);
        d.decorators = j.value("decorators", false);
        d.escapes = j.value("escapes", true);

        langs_[lower(e.path().stem().string())] = d;
        if (j.contains("aliases") && j["aliases"].is_array())
            for (const auto& a : j["aliases"])
                if (a.is_string())
                    langs_[lower(a.get<std::string>())] = d;
    }
    return !langs_.empty();
}

bool Highlighter::load_default()
{
    const std::string exe = Config::exe_dir() + "/data/languages";
    if (load_dir(exe))
        return true;
    return load_dir("data/languages");
}

std::string Highlighter::highlight(const std::string& code,
                                   const std::string& lang) const
{
    const auto it = langs_.find(lower(lang));
    if (it == langs_.end() || lang.empty())
        return {};
    const LangDef& L = it->second;
    const Palette pal = make_palette(L.colors);

    std::string out;
    State st;
    size_t start = 0;
    while (start <= code.size()) {
        const size_t nl = code.find('\n', start);
        const std::string line =
            code.substr(start, nl == std::string::npos ? std::string::npos
                                                       : nl - start);
        if (start)
            out += '\n';
        if (L.mode == "markup")
            out += colorize_markup(line, L, st, pal);
        else if (L.mode == "yaml")
            out += colorize_yaml(line, L, st, pal);
        else if (L.mode == "markdown")
            out += colorize_markdown(line, L, st, pal);
        else
            out += colorize_line(line, L, st, pal);
        if (nl == std::string::npos)
            break;
        start = nl + 1;
    }
    return out;
}

} // namespace krastor
