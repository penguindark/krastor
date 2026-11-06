// Krastor files tool — file/folder management for the agent (Phase B of
// tools_plan.md). One executable, JSON in on stdin, JSON out on stdout:
//   in:  {"action":"read","path":"...","start":0,"end":4096,"_approved":true,
//         "_policy":{...},"_workspace":"..."}
//   out: {"ok":true,"result":{...}} | {"ok":false,"error":"..."}
// Actions: list, stat, read (byte range), write, edit (find/replace),
// append, rename, mkdir. No delete by design (first increment).
//
// Self-enforcement (defense in depth): the deny_always floor is ALWAYS
// applied, even when Krastor already approved the call (_approved). Without
// _approved (manual run from a terminal) the full policy applies and any
// "ask" outcome becomes a denial.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "../src/permissions.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#endif

namespace fs = std::filesystem;
using nlohmann::json;
using namespace krastor;  // ToolPolicy, PermClass, Decision, glob_match...

namespace {

// UTF-8 string -> filesystem path (Windows APIs are wide; the plain string
// ctor would use the ANSI codepage and mangle accented names).
fs::path P(const std::string& s)
{
#ifdef _WIN32
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    if (!w.empty())
        w.pop_back();  // trailing NUL from the -1 length convention
    return fs::path(w);
#else
    return fs::path(s);
#endif
}

std::string S(const fs::path& p)
{
#ifdef _WIN32
    const std::wstring& w = p.native();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0,
                                      nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr,
                        nullptr);
    while (!s.empty() && s.back() == '\0')
        s.pop_back();
    return s;
#else
    return p.string();
#endif
}

// Replace invalid UTF-8 bytes with U+FFFD so the JSON dump never throws.
std::string utf8_sanitize(const std::string& in)
{
    std::string out;
    out.reserve(in.size());
    size_t i = 0;
    while (i < in.size()) {
        const unsigned char c = static_cast<unsigned char>(in[i]);
        size_t len = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2
                     : (c & 0xF0) == 0xE0            ? 3
                     : (c & 0xF8) == 0xF0            ? 4
                                                     : 0;
        bool ok = len > 0 && i + len <= in.size();
        for (size_t k = 1; ok && k < len; ++k)
            ok = (static_cast<unsigned char>(in[i + k]) & 0xC0) == 0x80;
        if (ok)
            out.append(in, i, len);
        else
            out += "\xEF\xBF\xBD";  // U+FFFD
        i += ok ? len : 1;
    }
    return out;
}

bool looks_binary(const std::string& s)
{
    const size_t n = std::min<size_t>(s.size(), 4096);
    return s.substr(0, n).find('\0') != std::string::npos;
}

// File mtime as ISO-8601 UTC. std::filesystem::clock is not available in
// every libstdc++ build, so on Windows read the FILETIME directly (1601-based
// 100ns ticks); POSIX file_time_type is already the Unix epoch.
std::string mtime_iso(const fs::path& p)
{
    std::time_t tt = 0;
#ifdef _WIN32
    HANDLE h = CreateFileW(p.c_str(), 0,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        FILETIME ft{};
        if (GetFileTime(h, nullptr, nullptr, &ft)) {
            ULARGE_INTEGER ul{};
            ul.LowPart = ft.dwLowDateTime;
            ul.HighPart = ft.dwHighDateTime;
            tt = static_cast<std::time_t>(ul.QuadPart / 10000000ULL) -
                 11644473600LL;  // 1601 -> 1970 epoch shift
        }
        CloseHandle(h);
    }
#else
    std::error_code ec;
    const auto t = fs::last_write_time(p, ec);
    if (!ec)
        tt = static_cast<std::time_t>(
            std::chrono::duration_cast<std::chrono::seconds>(
                t.time_since_epoch())
                .count());
#endif
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &tt);
#else
    gmtime_r(&tt, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

struct Ctx {
    json args;
    ToolPolicy policy;
    bool approved = false;  // Krastor's supervisor already checked
    std::string workspace;

    // Resolve + enforce. Returns false and fills `err` when denied.
    bool path_ok(const std::string& raw, PermClass cls, const char* what,
                 std::string& resolved_out, std::string& err)
    {
        const std::string abs = canonical_path(raw, workspace);
        for (const auto& g : policy.deny_always)
            if (glob_match(g, abs)) {
                err = std::string("path is on the never-allowed list: ") + abs;
                return false;
            }
        if (!approved) {
            const Decision d = policy.check(cls, abs);
            if (d != Decision::Allow) {
                err = std::string(what) +
                      " not permitted by policy for " + abs +
                      " (run through Krastor to request permission)";
                return false;
            }
        }
        resolved_out = abs;
        return true;
    }
};

json ok(const json& result)
{
    return {{"ok", true}, {"result", result}};
}
json fail(const std::string& error)
{
    return {{"ok", false}, {"error", error}};
}

// ------------------------------------------------------------------ actions

json do_list(Ctx& c, const fs::path& p, std::string& err)
{
    std::error_code ec;
    if (!fs::is_directory(p, ec)) {
        err = "not a directory: " + S(p);
        return json();
    }
    const bool rec = c.args.value("recursive", false);
    json entries = json::array();
    auto add = [&](const fs::path& e) {
        std::error_code eec;
        const bool dir = fs::is_directory(e, eec);
        entries.push_back({{"name", S(e.filename())},
                           {"path", S(e)},
                           {"dir", dir},
                           {"size", dir ? 0 : static_cast<long long>(
                                                  fs::file_size(e, eec))},
                           {"mtime", mtime_iso(e)}});
    };
    if (rec) {
        std::error_code itec;
        auto it = fs::recursive_directory_iterator(
            p, fs::directory_options::skip_permission_denied, itec);
        if (!itec) {
            const auto end = fs::recursive_directory_iterator();
            while (it != end) {
                add(it->path());
                it.increment(itec);
                if (itec)
                    break;
            }
        }
    } else {
        for (const auto& e : fs::directory_iterator(
                 p, fs::directory_options::skip_permission_denied))
            add(e.path());
    }
    return ok({{"path", S(p)}, {"count", entries.size()},
               {"entries", std::move(entries)}});
}

json do_stat(Ctx& c, const fs::path& p, std::string& err)
{
    (void)c;
    std::error_code ec;
    if (!fs::exists(p, ec))
        return ok({{"path", S(p)}, {"exists", false}});
    const bool dir = fs::is_directory(p, ec);
    return ok({{"path", S(p)},
               {"exists", true},
               {"dir", dir},
               {"size", dir ? 0 : static_cast<long long>(fs::file_size(p, ec))},
               {"mtime", mtime_iso(p)}});
}

json do_read(Ctx& c, const fs::path& p, std::string& err)
{
    (void)c;
    std::error_code ec;
    if (fs::is_directory(p, ec)) {
        err = "is a directory - use list";
        return json();
    }
    std::ifstream f(p, std::ios::binary);
    if (!f) {
        err = "cannot open " + S(p);
        return json();
    }
    const long long size = static_cast<long long>(
        fs::file_size(p, ec));
    long long start = c.args.value("start", 0LL);
    long long end = c.args.value("end", size);  // exclusive; default = EOF
    if (start < 0)
        start = 0;
    if (end > size)
        end = size;
    if (start > end) {
        err = "start > end";
        return json();
    }
    // Hard cap per read so huge files cannot flood the LLM context.
    constexpr long long kMax = 200000;
    const bool capped = end - start > kMax;
    if (capped)
        end = start + kMax;
    std::string data(static_cast<size_t>(end - start), '\0');
    f.seekg(start);
    f.read(data.data(), static_cast<std::streamsize>(data.size()));
    data.resize(static_cast<size_t>(f.gcount()));
    if (looks_binary(data)) {
        err = "binary file (NUL byte found) - not readable as text";
        return json();
    }
    return ok({{"path", S(p)},
               {"size", size},
               {"start", start},
               {"end", end},
               {"truncated", capped},
               {"text", utf8_sanitize(data)}});
}

std::string arg_content(const Ctx& c)
{
    return c.args.value("content", std::string());
}

json do_write(Ctx& c, const fs::path& p, std::string& err)
{
    std::error_code ec;
    if (c.args.value("append", false)) {
        fs::create_directories(p.parent_path(), ec);
        std::ofstream f(p, std::ios::binary | std::ios::app);
        if (!f) {
            err = "cannot append to " + S(p);
            return json();
        }
        const std::string data = arg_content(c);
        f.write(data.data(), static_cast<std::streamsize>(data.size()));
        if (!f) {
            err = "write failed on " + S(p);
            return json();
        }
        return ok({{"path", S(p)}, {"appended", data.size()}});
    }
    const bool exists = fs::exists(p, ec);
    if (exists && !c.args.value("overwrite", false)) {
        err = "file exists - pass overwrite:true to replace it";
        return json();
    }
    fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) {
        err = "cannot write " + S(p);
        return json();
    }
    const std::string data = arg_content(c);
    f.write(data.data(), static_cast<std::streamsize>(data.size()));
    if (!f) {
        err = "write failed on " + S(p);
        return json();
    }
    return ok({{"path", S(p)}, {"written", data.size()}, {"created", !exists}});
}

json do_edit(Ctx& c, const fs::path& p, std::string& err)
{
    const std::string find = c.args.value("find", std::string());
    const std::string repl = c.args.value("replace", std::string());
    if (find.empty()) {
        err = "edit needs a non-empty 'find'";
        return json();
    }
    std::error_code ec;
    if (!fs::is_regular_file(p, ec)) {
        err = "not a file: " + S(p);
        return json();
    }
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    std::string data = ss.str();
    int count = 0;
    size_t pos = 0;
    const bool all = c.args.value("all", false);
    while ((pos = data.find(find, pos)) != std::string::npos) {
        data.replace(pos, find.size(), repl);
        pos += repl.size();
        ++count;
        if (!all)
            break;
    }
    if (count == 0) {
        err = "text not found in " + S(p);
        return json();
    }
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    if (!out) {
        err = "cannot save " + S(p);
        return json();
    }
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    return ok({{"path", S(p)}, {"replacements", count}});
}

json do_rename(Ctx& c, const fs::path& from, std::string& err)
{
    const std::string to_raw = c.args.value("to", std::string());
    if (to_raw.empty()) {
        err = "rename needs 'to'";
        return json();
    }
    std::string to_abs;
    if (!c.path_ok(to_raw, PermClass::Write, "rename target", to_abs, err))
        return json();
    std::error_code ec;
    fs::rename(from, P(to_abs), ec);
    if (ec) {
        err = "rename failed: " + ec.message();
        return json();
    }
    return ok({{"from", S(from)}, {"path", to_abs}});
}

json do_mkdir(Ctx&, const fs::path& p, std::string& err)
{
    std::error_code ec;
    fs::create_directories(p, ec);
    if (!fs::is_directory(p, ec)) {
        err = "cannot create directory: " + S(p);
        return json();
    }
    return ok({{"path", S(p)}});
}

} // namespace

int main()
{
    // Read the whole stdin (the JSON request from Krastor or a human).
    std::stringstream ss;
    ss << std::cin.rdbuf();
    const std::string input = ss.str();

    if (const char* dbg = std::getenv("FILETOOL_DEBUG")) {
        std::ofstream log(dbg, std::ios::app);
        log << "run: stdin=" << input.size() << " bytes ["
            << input.substr(0, 200) << "]\n";
    }

    json out;
    try {
        Ctx c;
        c.args = json::parse(input);
        if (!c.args.is_object())
            throw std::runtime_error("input must be a JSON object");
        c.approved = c.args.value("_approved", false);
        c.workspace = c.args.value("_workspace", std::string());
        c.policy = ToolPolicy::from_json(
            c.args.contains("_policy") ? c.args["_policy"] : json());

        const std::string action = c.args.value("action", std::string());
        const std::string path_raw = c.args.value("path", std::string());
        if (path_raw.empty())
            throw std::runtime_error("missing 'path'");

        const bool read_action =
            action == "list" || action == "stat" || action == "read";
        const PermClass cls = read_action ? PermClass::Read : PermClass::Write;
        std::string abs;
        std::string err;
        if (!c.path_ok(path_raw, cls, read_action ? "read" : "write", abs, err))
            out = fail(err);
        else {
            const fs::path p = P(abs);
            if (action == "list")
                out = do_list(c, p, err);
            else if (action == "stat")
                out = do_stat(c, p, err);
            else if (action == "read")
                out = do_read(c, p, err);
            else if (action == "write" || action == "append")
                out = do_write(c, p, err);
            else if (action == "edit")
                out = do_edit(c, p, err);
            else if (action == "rename")
                out = do_rename(c, p, err);
            else if (action == "mkdir")
                out = do_mkdir(c, p, err);
            else
                err = "unknown action: " + action;
        }
        if (out.is_null() && !err.empty())
            out = fail(err);
    } catch (const std::exception& e) {
        out = fail(std::string("filetool: ") + e.what());
    }

#ifdef _WIN32
    // stdout must stay binary: no \n -> \r\n translation inside the JSON.
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    std::fputs(out.dump().c_str(), stdout);
    std::fflush(stdout);
    return 0;
}
