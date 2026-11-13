// Krastor — external tool runner (see development.md).
// Spawns a tool executable, writes the JSON args (+ effective _policy) on
// stdin, captures stdout/stderr with a hard timeout and an output cap, and
// parses the {"ok":...} envelope. Every call passes the permission gate:
// deny_always/deny/allow lists first, Ask -> AskHandler dialog.

#pragma once

#include <map>
#include <string>

#include "permissions.h"
#include "tool_registry.h"

namespace krastor {

class CancelToken;

struct ToolResult {
    bool ok = false;
    std::string result;   // "result" field as text (JSON dump or raw stdout)
    std::string error;    // human-readable failure reason
    std::string stderr_text;  // tool log (for the GUI block / CLI verbose)
    long long ms = 0;
};

class ToolRunner {
public:
    ToolRunner(GrantStore& grants, const nlohmann::json& config_tools)
        : grants_(grants)
    {
        if (config_tools.is_object() && config_tools.contains("permissions"))
            global_permissions_ = config_tools["permissions"];
    }

    // Runs one tool call. `args_json` is the LLM-provided arguments object.
    // `ask` may be null: calls that would ask are then denied.
    // Never throws: failures come back as ToolResult::error.
    ToolResult run(const ToolManifest& tool, const std::string& args_json,
                   AskHandler* ask, CancelToken* cancel);

    // Per-agent permission overrides (config.jsonc agents.<id>.permissions):
    // merged last, over the manifest and the global config override, so the
    // active agent can tighten (e.g. read-only) but never widen the floor.
    void set_agent_permissions(const nlohmann::json& j)
    {
        if (agent_permissions_ == j)
            return;
        agent_permissions_ = j;
        cache_.clear();
    }

private:
    // Policy for a manifest (manifest block + global config override), cached.
    const ToolPolicy& policy_for(const ToolManifest& tool);

    GrantStore& grants_;
    nlohmann::json global_permissions_;
    nlohmann::json agent_permissions_;
    std::map<std::string, std::pair<ToolPolicy, nlohmann::json>> cache_;
};

} // namespace krastor
