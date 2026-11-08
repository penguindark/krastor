// Krastor — tool permission policies (see tools_plan.md section 4).
// A policy is a set of per-class rules (read/write/net/run), each with glob
// `allow`/`deny` lists and an `ask` flag. Precedence: deny_always > deny >
// allow > ask > default-deny. `~` expands to the user home; on Windows
// matching is case-insensitive and backslashes are normalized.

#pragma once

#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace krastor {

enum class PermClass { Read, Write, Net, Run };
// Policy check result: Allow / Deny / Ask. AskHandler answers an ask with
// Allow (once), Always (allow + remember for the session) or Deny.
enum class Decision { Allow, Deny, Ask, Always };

const char* perm_class_name(PermClass c);

// `~` / `~/...` -> user home (std::filesystem::path::home_path()).
std::string expand_home(const std::string& p);

// Canonical absolute path for policy checks: home-expanded, weakly canonical
// (works on not-yet-existing paths), separators normalized.
std::string canonical_path(const std::string& p,
                           const std::string& relative_base);

// Glob match: `**` crosses separators, `*` stays inside one segment, `?`
// matches one char. Patterns and paths are normalized (home-expanded,
// lowercased on Windows, '/' separators) before matching.
bool glob_match(const std::string& pattern, const std::string& path);

// The writable sandbox (tools_plan.md): ~/Documents/KrastorWorkspace.
// Created on demand; every "write"-class default policy allows it freely.
std::string workspace_dir();

// Platform temp folder (fs::temp_directory_path). The "%TEMP%" placeholder
// in permission globs expands to this (see norm() in permissions.cpp).
std::string temp_dir();

// Appended to the system prompt when tools are enabled: the sandbox location
// plus the tool-selection rule - dedicated tools first, python last resort.
// Callers must guard against double-appending a saved config (same pattern
// as kMediaUsagePrompt).
inline std::string tool_usage_prompt()
{
    return "\nYou can call the provided tools to inspect and change the "
           "machine. Writable sandbox: " +
           workspace_dir() +
           " (other locations need user approval). Prefer small, reversible "
           "steps and report what a tool did.\n"
           "Pick the simplest tool that does the job: use files to read, "
           "write or edit files (NEVER run python just to read or write a "
           "file), webfetch/websearch for web content, and python ONLY as a "
           "last resort for what the other tools cannot do (computation, "
           "data processing, extra packages).";
}

struct PermRule {
    std::vector<std::string> allow;  // empty = unrestricted (unless deny)
    std::vector<std::string> deny;
    bool ask = false;                // not-allowed calls ask instead of deny
};

struct ToolPolicy {
    PermRule read, write, net, run;
    std::vector<std::string> deny_always;  // floor: config cannot override

    // Parses a manifest `permissions` object (keys: read/write/net/run with
    // allow/deny/ask, plus deny_always). Missing keys keep safe defaults:
    // read/write unrestricted-with-ask, net open, run always asks.
    static ToolPolicy from_json(const nlohmann::json& j);

    // Merges a config.jsonc override over the manifest policy (same shape);
    // deny_always entries are never removed.
    void merge_override(const nlohmann::json& j);

    // `abs_path` canonical (canonical_path()); empty for net/run checks.
    Decision check(PermClass cls, const std::string& abs_path) const;

private:
    const PermRule& rule(PermClass c) const;
};

// Session-scoped "always allow" answers (never persisted): a granted path
// covers itself and everything below it.
class GrantStore {
public:
    void allow(PermClass cls, const std::string& canonical_path);
    bool granted(PermClass cls, const std::string& canonical_path) const;

    // Tool-level "always allow" for targetless run checks (e.g. the python
    // tool: there is no path to grant, the user approves the tool itself).
    // Scoped to the one tool - it never covers other run-class tools.
    void allow_tool(const std::string& tool_name);
    bool tool_allowed(const std::string& tool_name) const;

    // Snapshot of the live grants as (class name, path prefix) pairs, for
    // the /grants command; clear() revokes them all without a restart.
    std::vector<std::pair<std::string, std::string>> list() const;
    void clear();

private:
    mutable std::mutex mtx_;
    std::vector<std::pair<int, std::string>> grants_;  // (class, path prefix)
    std::vector<std::string> tool_grants_;             // tool names (run)
};

// Blocking ask callback implemented by the GUI modal / the CLI prompt.
class AskHandler {
public:
    virtual ~AskHandler() = default;
    // `detail` is a human-readable summary of what is about to happen
    // (tool, action, resolved paths / code). Returns Allow, Always (the
    // runner then records a session grant for the checked paths/URLs, or
    // for the tool itself when there was no target) or Deny.
    virtual Decision ask(const std::string& tool, const std::string& action,
                         const std::string& detail) = 0;
};

} // namespace krastor
