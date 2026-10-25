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
    bool block = false;  // inside an unterminated /* ... */
};

std::string colorize_line(const std::string& line,
                          const Highlighter::LangDef& L, State& st)
{
    std::string out;
    const size_t n = line.size();
    auto emit = [&](const char* col, const std::string& txt) {
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
                emit(cString, line.substr(i));
                return out;  // still open
            }
            emit(cString, line.substr(i, j + st.triple.size() - i));
            i = j + st.triple.size();
            st.triple.clear();
            continue;
        }
        if (st.block) {
            const size_t j = line.find(L.bc_close, i);
            if (j == std::string::npos) {
                emit(cComment, line.substr(i));
                return out;
            }
            emit(cComment, line.substr(i, j + L.bc_close.size() - i));
            i = j + L.bc_close.size();
            st.block = false;
            continue;
        }

        // --- line comment ---
        if (!L.line_comment.empty() && starts_with(line, i, L.line_comment)) {
            emit(cComment, line.substr(i));
            return out;
        }
        // --- block comment opener ---
        if (!L.bc_open.empty() && starts_with(line, i, L.bc_open)) {
            const size_t j = line.find(L.bc_close, i + L.bc_open.size());
            if (j == std::string::npos) {
                emit(cComment, line.substr(i));
                st.block = true;
                return out;
            }
            emit(cComment, line.substr(i, j + L.bc_close.size() - i));
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
                emit(cString, line.substr(i));
                st.triple = t;
                return out;
            }
            emit(cString, line.substr(i, j + t.size() - i));
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
            emit(cString, line.substr(i, (closed ? j : n) - i));
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
            emit(cDecor, line.substr(i, j - i));
            i = j;
            continue;
        }
        // --- number (hex/dec/float; stops at operators) ---
        if (std::isdigit(static_cast<unsigned char>(c))) {
            size_t j = i;
            while (j < n && (std::isalnum(static_cast<unsigned char>(line[j])) ||
                             line[j] == '.' || line[j] == '_'))
                ++j;
            emit(cNumber, line.substr(i, j - i));
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
                emit(cKeyword, w);
            else if (L.types.count(w))
                emit(cType, w);
            else if (L.builtins.count(w))
                emit(cBuiltin, w);
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
        if (j.contains("strings") && j["strings"].is_array())
            for (const auto& s : j["strings"])
                if (s.is_string())
                    d.strings.push_back(s.get<std::string>());
        if (j.contains("triples") && j["triples"].is_array())
            for (const auto& s : j["triples"])
                if (s.is_string())
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
        out += colorize_line(line, L, st);
        if (nl == std::string::npos)
            break;
        start = nl + 1;
    }
    return out;
}

} // namespace krastor
