// Krastor — external tool registry (see tools_plan.md sections 1-2).
// Scans the tools/ folder (sibling of the executable, fallback: next to it)
// for `tool_<name>.jsonc` manifests. Each manifest describes one external
// executable: what it does (LLM-facing description), its input JSON Schema
// and its permission policy.

#pragma once

#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace krastor {

struct ToolManifest {
    std::string name;          // unique id, exposed to the LLM as function name
    std::string description;   // LLM-facing "what this tool does"
    std::string dir;           // tools folder (absolute)
    std::string exe;           // absolute path of the executable
    std::vector<std::string> args;  // fixed argv prefix (resolved vs tools dir)
    std::string setup_hint;    // shown when exe is missing ("run setup.ps1 ...")
    bool enabled = true;
    int timeout_ms = 15000;
    std::vector<std::string> path_args;    // args validated as fs paths
    std::vector<std::string> net_args;     // args validated as net targets (URLs)
    nlohmann::json input_schema;           // JSON Schema (forwarded to the LLM)
    nlohmann::json permissions;            // raw policy block (see permissions.h)
    std::map<std::string, std::string> action_class;  // action -> read/write/net/run
    std::string cwd = "tools";             // "tools" | "workspace" | absolute dir
};

class ToolRegistry {
public:
    // Loads every `tool_*.jsonc` in `dir` (sorted by name for determinism).
    // `overrides` is the optional config.json "tools" object: per-tool
    // `enabled` and `permissions` overrides. Unreadable/malformed manifests
    // are skipped with a message appended to `log`.
    void load_dir(const std::string& dir, const nlohmann::json& overrides,
                  std::vector<std::string>& log);

    const std::vector<ToolManifest>& tools() const { return tools_; }
    const ToolManifest* find(const std::string& name) const;

    // OpenAI `tools` array for the chat request ("[]" when none enabled).
    std::string tools_json() const;
    bool empty() const { return tools_.empty(); }

private:
    std::vector<ToolManifest> tools_;
};

// tools/ next to the exe's folder (dev layout: bin/../tools), fallback
// exe_dir()/tools for portable installs. Empty when neither exists.
std::string tools_dir();

} // namespace krastor
