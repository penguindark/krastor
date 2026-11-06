// Krastor — external tool runner implementation (see tool_runner.h).
// Protocol: the tool receives one JSON object on stdin (the LLM args plus an
// injected "_policy" object), answers one JSON object on stdout
// ({"ok":true,"result":...} / {"ok":false,"error":"..."}) and may log to
// stderr. stdout/stderr are capped; a hard timeout kills the process.

#include "tool_runner.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "http_client.h"  // CancelToken

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <csignal>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace krastor {

namespace {

constexpr size_t kOutputCap = 200000;  // bytes kept per stream

struct RawRun {
    bool started = false;
    bool timed_out = false;
    int exit_code = -1;
    std::string out;
    std::string err;
};

#ifdef _WIN32

// Reads a pipe fully into `dst` (capped); runs on its own thread.
void read_pipe_win(HANDLE h, std::string& dst)
{
    char buf[8192];
    DWORD got = 0;
    while (ReadFile(h, buf, sizeof(buf), &got, nullptr) && got > 0) {
        if (dst.size() < kOutputCap)
            dst.append(buf, got);
        else if (dst.size() == kOutputCap)
            dst += "\n[truncated]";
    }
}

void write_pipe_win(HANDLE h, const std::string& data)
{
    size_t off = 0;
    while (off < data.size()) {
        DWORD got = 0;
        const auto chunk = static_cast<DWORD>(data.size() - off);
        if (!WriteFile(h, data.data() + off, chunk, &got, nullptr) || got == 0)
            break;  // tool closed stdin early: ignore, it never reads
        off += got;
    }
}

RawRun spawn(const std::string& exe, const std::vector<std::string>& argv,
             const std::string& cwd,
             const std::string& stdin_data, int timeout_ms)
{
    RawRun r;
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE in_r = nullptr, in_w = nullptr, out_r = nullptr, out_w = nullptr,
           err_r = nullptr, err_w = nullptr;
    if (!CreatePipe(&in_r, &in_w, &sa, 0) ||
        !CreatePipe(&out_r, &out_w, &sa, 0) ||
        !CreatePipe(&err_r, &err_w, &sa, 0))
        return r;
    // The child's ends (in_r/out_w/err_w) must stay inheritable so
    // STARTF_USESTDHANDLES can duplicate them; the parent's ends must NOT be
    // inherited (else the child would hold them open too: stdin would never
    // see EOF and the readers would never terminate).
    SetHandleInformation(in_w, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(err_r, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput = in_r;
    si.hStdOutput = out_w;
    si.hStdError = err_w;

    auto wutf8 = [](const std::string& s) {
        std::wstring w(s.size(), L'\0');
        const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(),
                                          static_cast<int>(s.size()),
                                          w.data(), static_cast<int>(s.size()));
        w.resize(n > 0 ? static_cast<size_t>(n) : 0);
        return w;
    };
    std::wstring cmd = L"\"" + wutf8(exe) + L"\"";
    for (const auto& a : argv)
        cmd += L" \"" + wutf8(a) + L"\"";  // paths never contain quotes
    std::wstring wdir = wutf8(cwd);

    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE,
                                   CREATE_NO_WINDOW, nullptr,
                                   wdir.empty() ? nullptr : wdir.c_str(), &si,
                                   &pi);
    CloseHandle(in_r);
    CloseHandle(out_w);
    CloseHandle(err_w);
    if (!ok) {
        CloseHandle(in_w);
        CloseHandle(out_r);
        CloseHandle(err_r);
        return r;
    }
    r.started = true;

    std::thread writer([in_w, &stdin_data] {
        write_pipe_win(in_w, stdin_data);
        CloseHandle(in_w);
    });
    std::thread ro([out_r, &r] {
        read_pipe_win(out_r, r.out);
        CloseHandle(out_r);
    });
    std::thread re([err_r, &r] {
        read_pipe_win(err_r, r.err);
        CloseHandle(err_r);
    });

    const DWORD wait = WaitForSingleObject(pi.hProcess,
                                           static_cast<DWORD>(timeout_ms));
    if (wait == WAIT_TIMEOUT) {
        r.timed_out = true;
        TerminateProcess(pi.hProcess, 1);
    }
    WaitForSingleObject(pi.hProcess, 5000);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    r.exit_code = static_cast<int>(code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    writer.join();
    ro.join();
    re.join();
    return r;
}

#else  // POSIX ---------------------------------------------------------------

void read_pipe_posix(int fd, std::string& dst)
{
    char buf[8192];
    ssize_t got;
    while ((got = read(fd, buf, sizeof(buf))) > 0) {
        if (dst.size() < kOutputCap)
            dst.append(buf, static_cast<size_t>(got));
        else if (dst.size() == kOutputCap)
            dst += "\n[truncated]";
    }
    close(fd);
}

RawRun spawn(const std::string& exe, const std::vector<std::string>& argv,
             const std::string& cwd,
             const std::string& stdin_data, int timeout_ms)
{
    RawRun r;
    int in_pipe[2], out_pipe[2], err_pipe[2];
    if (pipe(in_pipe) || pipe(out_pipe) || pipe(err_pipe))
        return r;
    const pid_t pid = fork();
    if (pid < 0)
        return r;
    if (pid == 0) {  // child
        dup2(in_pipe[0], STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(err_pipe[1], STDERR_FILENO);
        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        close(err_pipe[0]); close(err_pipe[1]);
        if (!cwd.empty())
            if (chdir(cwd.c_str()) != 0) {
            }
        std::vector<std::string> cmdv;
        cmdv.push_back(exe);
        for (const auto& a : argv)
            cmdv.push_back(a);
        std::vector<char*> cargv;
        for (auto& s : cmdv)
            cargv.push_back(const_cast<char*>(s.c_str()));
        cargv.push_back(nullptr);
        execv(exe.c_str(), cargv.data());
        _exit(127);
    }
    r.started = true;
    close(in_pipe[0]);
    close(out_pipe[1]);
    close(err_pipe[1]);

    std::thread writer([fd = in_pipe[1], &stdin_data] {
        size_t off = 0;
        while (off < stdin_data.size()) {
            const ssize_t w = write(fd, stdin_data.data() + off,
                                    stdin_data.size() - off);
            if (w <= 0)
                break;
            off += static_cast<size_t>(w);
        }
        close(fd);
    });
    std::thread ro([fd = out_pipe[0], &r] { read_pipe_posix(fd, r.out); });
    std::thread re([fd = err_pipe[0], &r] { read_pipe_posix(fd, r.err); });

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    int status = 0;
    bool done = false;
    while (std::chrono::steady_clock::now() < deadline) {
        const pid_t w = waitpid(pid, &status, WNOHANG);
        if (w == pid) {
            done = true;
            break;
        }
        if (w < 0)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (!done) {
        r.timed_out = true;
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
    }
    r.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    writer.join();
    ro.join();
    re.join();
    return r;
}

#endif

// Action -> permission class. Manifest mapping wins; fallback heuristic.
PermClass classify(const ToolManifest& tool, const std::string& action)
{
    auto it = tool.action_class.find(action);
    if (it != tool.action_class.end()) {
        if (it->second == "read")  return PermClass::Read;
        if (it->second == "write") return PermClass::Write;
        if (it->second == "net")   return PermClass::Net;
        return PermClass::Run;
    }
    static const std::vector<std::string> read_actions = {"list",  "stat",
                                                          "read",  "search",
                                                          "fetch", "get"};
    static const std::vector<std::string> write_actions = {
        "write", "edit", "append", "rename", "mkdir", "move", "copy", "delete"};
    for (const auto& a : read_actions)
        if (action == a)
            return PermClass::Read;
    for (const auto& a : write_actions)
        if (action == a)
            return PermClass::Write;
    return PermClass::Run;  // unknown actions are treated as the risky class
}

std::string truncate(const std::string& s, size_t cap)
{
    if (s.size() <= cap)
        return s;
    return s.substr(0, cap) + "\n[...]";
}

} // namespace

const ToolPolicy& ToolRunner::policy_for(const ToolManifest& tool)
{
    auto it = cache_.find(tool.name);
    if (it != cache_.end() && it->second.second == tool.permissions)
        return it->second.first;
    ToolPolicy pol = ToolPolicy::from_json(tool.permissions);
    pol.merge_override(global_permissions_);
    auto res = cache_.insert_or_assign(tool.name,
                                       std::make_pair(pol, tool.permissions));
    return res.first->second.first;
}

ToolResult ToolRunner::run(const ToolManifest& tool,
                           const std::string& args_json, AskHandler* ask,
                           CancelToken* cancel)
{
    ToolResult tr;
    const auto t0 = std::chrono::steady_clock::now();
    auto finish = [&]() {
        tr.ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - t0)
                    .count();
        return tr;
    };

    if (cancel && cancel->load()) {
        tr.error = "cancelled";
        return finish();
    }

    nlohmann::json args;
    try {
        args = args_json.empty() ? nlohmann::json::object()
                                 : nlohmann::json::parse(args_json);
    } catch (const nlohmann::json::exception& e) {
        tr.error = std::string("bad arguments JSON: ") + e.what();
        return finish();
    }
    if (!args.is_object()) {
        tr.error = "arguments must be a JSON object";
        return finish();
    }

    const std::string action = args.value("action", tool.name);
    const PermClass cls = classify(tool, action);
    const ToolPolicy& policy = policy_for(tool);

    // Working directory: "workspace" sandbox or the tools folder.
    const std::string cwd = tool.cwd == "workspace" ? workspace_dir()
                                                    : tool.dir;

    // --- permission gate ----------------------------------------------------
    // 1) filesystem path arguments (canonicalized, then checked)
    std::vector<std::string> paths;
    for (const auto& key : tool.path_args)
        if (args.contains(key) && args[key].is_string())
            paths.push_back(
                canonical_path(args[key].get<std::string>(), cwd));
    Decision decision = Decision::Allow;
    for (const auto& p : paths) {
        if (grants_.granted(cls, p))
            continue;
        const Decision d = policy.check(cls, p);
        if (d == Decision::Deny)
            decision = Decision::Deny;
        else if (d == Decision::Ask && decision != Decision::Deny)
            decision = Decision::Ask;
    }
    // 1b) net targets (URLs): matched as strings against the "net" rules
    std::vector<std::string> urls;
    for (const auto& key : tool.net_args)
        if (args.contains(key) && args[key].is_string())
            urls.push_back(args[key].get<std::string>());
    for (const auto& u : urls) {
        const Decision d = policy.check(PermClass::Net, u);
        if (d == Decision::Deny)
            decision = Decision::Deny;
        else if (d == Decision::Ask && decision != Decision::Deny)
            decision = Decision::Ask;
    }
    // 2) class-level check without a target: only meaningful for "run"
    //    (run.ask gates execution). Net rules match URLs (net_args); a net
    //    tool without URL args is governed by its own self-enforcement.
    if (decision == Decision::Allow && paths.empty() && urls.empty() &&
        cls == PermClass::Run)
        decision = policy.check(cls, std::string());
    // 3) ask_actions: these actions ALWAYS confirm - they override the policy
    //    AND session grants (e.g. file deletion). Deny floors still win.
    const bool forced_ask =
        std::find(tool.ask_actions.begin(), tool.ask_actions.end(), action) !=
        tool.ask_actions.end();
    if (forced_ask && decision != Decision::Deny)
        decision = Decision::Ask;

    if (decision == Decision::Ask) {
        if (!ask) {
            tr.error = "permission required but no interactive handler";
            return finish();
        }
        std::string detail = "action: " + action;
        for (const auto& p : paths)
            detail += "\n  path: " + p;
        for (const auto& u : urls)
            detail += "\n  url: " + u;
        detail += "\nargs: " + truncate(args.dump(2), 2000);
        const Decision answer = ask->ask(tool.name, action, detail);
        if (answer == Decision::Deny) {
            tr.error = "denied by user";
            return finish();
        }
        // "Always allow" never silences a forced-ask action.
        if (answer == Decision::Always && !forced_ask)
            for (const auto& p : paths)
                grants_.allow(cls, p);
    } else if (decision == Decision::Deny) {
        std::string what = paths.empty() ? std::string(action) : paths.front();
        tr.error = std::string("blocked by permission policy: ") +
                   perm_class_name(cls) + " not allowed for " + what;
        return finish();
    }

    // --- spawn --------------------------------------------------------------
    nlohmann::json payload = args;
    payload["_policy"] = tool.permissions.is_null()
                             ? nlohmann::json::object()
                             : tool.permissions;
    payload["_workspace"] = workspace_dir();
    payload["_approved"] = true;  // the supervisor gate above has passed

    const RawRun raw =
        spawn(tool.exe, tool.args, cwd, payload.dump(), tool.timeout_ms);
    tr.stderr_text = truncate(raw.err, 4000);
    if (!raw.started) {
        tr.error = "cannot start tool: " + tool.exe;
        return finish();
    }
    if (raw.timed_out) {
        tr.error = "tool timed out after " + std::to_string(tool.timeout_ms) +
                   " ms";
        return finish();
    }

    // --- envelope -----------------------------------------------------------
    if (raw.out.empty()) {
        tr.error = "tool produced no output (exit " +
                   std::to_string(raw.exit_code) + ")";
        if (!tr.stderr_text.empty())
            tr.error += " - " + truncate(tr.stderr_text, 300);
        return finish();
    }
    try {
        const auto j = nlohmann::json::parse(raw.out);
        if (j.is_object() && j.contains("ok")) {
            tr.ok = j.value("ok", false);
            if (tr.ok) {
                if (j.contains("result"))
                    tr.result = j["result"].is_string()
                                    ? j["result"].get<std::string>()
                                    : j["result"].dump();
            } else {
                tr.error = j.value("error", std::string("tool reported failure"));
            }
        } else {
            // No envelope: raw text output, success follows the exit code.
            tr.ok = raw.exit_code == 0;
            tr.result = raw.out;
            if (!tr.ok)
                tr.error = "exit " + std::to_string(raw.exit_code);
        }
    } catch (const nlohmann::json::exception&) {
        tr.ok = raw.exit_code == 0;
        tr.result = raw.out;
    }
    return finish();
}

} // namespace krastor
