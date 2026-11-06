// Krastor — tool permission policies implementation (see permissions.h).

#include "permissions.h"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>

namespace krastor {

namespace fs = std::filesystem;

const char* perm_class_name(PermClass c)
{
    switch (c) {
    case PermClass::Read:  return "read";
    case PermClass::Write: return "write";
    case PermClass::Net:   return "net";
    case PermClass::Run:   return "run";
    }
    return "run";
}

namespace {

// User home: std::filesystem::path::home_path() is missing in some MinGW
// libstdc++ builds, so read the environment directly (Windows: USERPROFILE).
std::string home_dir()
{
#ifdef _WIN32
    if (const char* p = std::getenv("USERPROFILE"))
        return p;
#else
    if (const char* p = std::getenv("HOME"))
        return p;
#endif
    return fs::current_path().string();
}

} // namespace

std::string expand_home(const std::string& p)
{
    if (p == "~")
        return home_dir();
    if (p.size() >= 2 && p[0] == '~' && (p[1] == '/' || p[1] == '\\'))
        return home_dir() + p.substr(1);
    return p;
}

namespace {

// Normalize for matching: home-expanded, separators to '/', lowercased on
// Windows (case-insensitive filesystem), trailing '/' dropped.
std::string norm(std::string s)
{
    s = expand_home(s);
    for (auto& c : s) {
#ifdef _WIN32
        if (c == '\\')
            c = '/';
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
#endif
    }
    while (s.size() > 1 && s.back() == '/')
        s.pop_back();
    return s;
}

// Recursive glob matcher over normalized strings. `**` crosses separators,
// `*` stays inside one path segment, `?` matches exactly one char.
bool glob_here(const char* p, const char* s)
{
    while (*p) {
        if (p[0] == '*') {
            if (p[1] == '*') {
                const char* q = p + 2;
                if (*q == '/')
                    ++q;  // "**/" also matches the empty segment
                if (!*q)
                    return true;  // trailing ** swallows everything
                do {
                    if (glob_here(q, s))
                        return true;
                } while (*s++);
                return false;
            }
            ++p;  // single '*': any run of chars inside the current segment
            if (!*p)
                return std::strchr(s, '/') == nullptr;
            while (true) {
                if (glob_here(p, s))
                    return true;
                if (!*s || *s == '/')
                    return false;
                ++s;
            }
        }
        if (!*s)
            return false;
        if (p[0] != '?' && p[0] != s[0])
            return false;
        ++p;
        ++s;
    }
    return !*s;
}

} // namespace

bool glob_match(const std::string& pattern, const std::string& path)
{
    return glob_here(norm(pattern).c_str(), norm(path).c_str());
}

std::string canonical_path(const std::string& p, const std::string& relative_base)
{
    fs::path path = fs::path(expand_home(p));
    if (path.is_relative() && !relative_base.empty())
        path = fs::path(relative_base) / path;
    std::error_code ec;
    fs::path canon = fs::weakly_canonical(path, ec);
    return (ec ? path : canon).string();
}

std::string workspace_dir()
{
    std::error_code ec;
    fs::path ws = fs::path(home_dir()) / "Documents" / "KrastorWorkspace";
    fs::create_directories(ws, ec);  // idempotent; ec ignored
    return ws.string();
}

ToolPolicy ToolPolicy::from_json(const nlohmann::json& j)
{
    ToolPolicy pol;
    // Safe defaults: writes outside the explicit allow list always ask;
    // arbitrary code execution always asks.
    pol.write.ask = true;
    pol.run.ask = true;
    if (!j.is_object())
        return pol;

    auto rule = [&](const char* key, PermRule& out) {
        if (!j.contains(key) || !j[key].is_object())
            return;
        const auto& r = j[key];
        out.allow.clear();
        out.deny.clear();
        out.ask = false;
        if (r.contains("allow") && r["allow"].is_array())
            for (const auto& g : r["allow"])
                if (g.is_string())
                    out.allow.push_back(g.get<std::string>());
        if (r.contains("deny") && r["deny"].is_array())
            for (const auto& g : r["deny"])
                if (g.is_string())
                    out.deny.push_back(g.get<std::string>());
        out.ask = r.value("ask", false);
    };
    rule("read", pol.read);
    rule("write", pol.write);
    rule("net", pol.net);
    rule("run", pol.run);
    if (j.contains("deny_always") && j["deny_always"].is_array())
        for (const auto& g : j["deny_always"])
            if (g.is_string())
                pol.deny_always.push_back(g.get<std::string>());
    return pol;
}

void ToolPolicy::merge_override(const nlohmann::json& j)
{
    // Same shape as from_json, but a key present in the override fully
    // replaces that rule; deny_always is additive (floor only grows).
    if (!j.is_object())
        return;
    auto has = [&](const char* key) {
        return j.contains(key) && j[key].is_object();
    };
    if (has("read"))
        read = from_json(j).read;
    if (has("write"))
        write = from_json(j).write;
    if (has("net"))
        net = from_json(j).net;
    if (has("run"))
        run = from_json(j).run;
    if (j.contains("deny_always") && j["deny_always"].is_array())
        for (const auto& g : j["deny_always"])
            if (g.is_string())
                deny_always.push_back(g.get<std::string>());
}

const PermRule& ToolPolicy::rule(PermClass c) const
{
    switch (c) {
    case PermClass::Read:  return read;
    case PermClass::Write: return write;
    case PermClass::Net:   return net;
    case PermClass::Run:   return run;
    }
    return run;
}

Decision ToolPolicy::check(PermClass cls, const std::string& abs_path) const
{
    for (const auto& g : deny_always)
        if (glob_match(g, abs_path))
            return Decision::Deny;
    const PermRule& r = rule(cls);
    for (const auto& g : r.deny)
        if (glob_match(g, abs_path))
            return Decision::Deny;
    if (r.allow.empty())
        // Unrestricted list, but ask=true still gates everything (run.ask
        // default: every execution asks).
        return r.ask ? Decision::Ask : Decision::Allow;
    {
        bool allowed = false;
        for (const auto& g : r.allow)
            if (glob_match(g, abs_path)) {
                allowed = true;
                break;
            }
        if (!allowed)
            return r.ask ? Decision::Ask : Decision::Deny;
    }
    return Decision::Allow;
}

void GrantStore::allow(PermClass cls, const std::string& canonical_path)
{
    std::string p = canonical_path;
    while (p.size() > 1 && (p.back() == '/' || p.back() == '\\'))
        p.pop_back();
    std::lock_guard<std::mutex> lock(mtx_);
    grants_.emplace_back(static_cast<int>(cls), std::move(p));
}

bool GrantStore::granted(PermClass cls, const std::string& canonical_path) const
{
    std::string p = canonical_path;
    while (p.size() > 1 && (p.back() == '/' || p.back() == '\\'))
        p.pop_back();
    std::string lower = p;
#ifdef _WIN32
    for (auto& c : lower)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
#endif
    std::lock_guard<std::mutex> lock(mtx_);
    for (const auto& g : grants_) {
        if (g.first != static_cast<int>(cls))
            continue;
        std::string gp = g.second;
#ifdef _WIN32
        for (auto& c : gp)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
#endif
        if (lower == gp)
            return true;
        // prefix + separator boundary: a granted folder covers its subtree
        if (lower.size() > gp.size() &&
            lower.compare(0, gp.size(), gp) == 0 &&
            (lower[gp.size()] == '/' || lower[gp.size()] == '\\'))
            return true;
    }
    return false;
}

} // namespace krastor
