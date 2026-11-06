// Krastor — external tool registry implementation (see tool_registry.h).

#include "tool_registry.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "config.h"

namespace krastor {

namespace fs = std::filesystem;

std::string tools_dir()
{
    const fs::path exe = fs::path(Config::exe_dir());
    std::error_code ec;
    fs::path sibling = exe.parent_path() / "tools";
    if (fs::is_directory(sibling, ec))
        return sibling.string();
    fs::path next = exe / "tools";
    if (fs::is_directory(next, ec))
        return next.string();
    return {};
}

namespace {

bool parse_manifest(const fs::path& file, const nlohmann::json& j,
                    ToolManifest& out, std::string& err)
{
    out.dir = file.parent_path().string();
    out.name = j["name"].get<std::string>();
    out.description = j.value("description", std::string());
    out.exe = (file.parent_path() / fs::path(j["exe"].get<std::string>()))
                  .string();
    out.enabled = j.value("enabled", true);
    out.timeout_ms = j.value("timeout_ms", 15000);
    out.input_schema =
        j.contains("input_schema") ? j["input_schema"] : nlohmann::json{
                                                              {"type", "object"}};
    out.permissions = j.contains("permissions") ? j["permissions"]
                                                : nlohmann::json();
    out.cwd = j.value("cwd", std::string("tools"));
    out.setup_hint = j.value("setup_hint", std::string());
    if (j.contains("args") && j["args"].is_array())
        for (const auto& a : j["args"])
            if (a.is_string())
                out.args.push_back(
                    (file.parent_path() / fs::path(a.get<std::string>()))
                        .string());
    if (j.contains("path_args") && j["path_args"].is_array())
        for (const auto& a : j["path_args"])
            if (a.is_string())
                out.path_args.push_back(a.get<std::string>());
    if (j.contains("net_args") && j["net_args"].is_array())
        for (const auto& a : j["net_args"])
            if (a.is_string())
                out.net_args.push_back(a.get<std::string>());
    if (j.contains("ask_actions") && j["ask_actions"].is_array())
        for (const auto& a : j["ask_actions"])
            if (a.is_string())
                out.ask_actions.push_back(a.get<std::string>());
    if (j.contains("action_class") && j["action_class"].is_object())
        for (const auto& [k, v] : j["action_class"].items())
            if (v.is_string())
                out.action_class[k] = v.get<std::string>();
    (void)err;
    return true;
}

} // namespace

void ToolRegistry::load_dir(const std::string& dir,
                            const nlohmann::json& overrides,
                            std::vector<std::string>& log)
{
    tools_.clear();
    if (dir.empty()) {
        log.push_back("tools: no tools/ folder found - external tools disabled");
        return;
    }
    std::vector<fs::path> files;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec))
        if (e.is_regular_file() && e.path().extension() == ".jsonc" &&
            e.path().filename().string().rfind("tool_", 0) == 0)
            files.push_back(e.path());
    std::sort(files.begin(), files.end());

    for (const auto& file : files) {
        std::ifstream f(file, std::ios::binary);
        // ignore_comments: manifests are JSONC (// and /* */ allowed);
        // allow_exceptions=false -> malformed input comes back as discarded.
        const nlohmann::json j = nlohmann::json::parse(f, nullptr, false, true);
        if (j.is_discarded()) {
            log.push_back("tools: " + file.filename().string() +
                          ": invalid JSON");
            continue;
        }
        if (!j.is_object() || !j.contains("name") || !j["name"].is_string() ||
            !j.contains("exe") || !j["exe"].is_string()) {
            log.push_back("tools: " + file.filename().string() +
                          ": missing name/exe");
            continue;
        }
        ToolManifest m;
        std::string err;
        if (!parse_manifest(file, j, m, err)) {
            log.push_back("tools: " + file.filename().string() + ": " + err);
            continue;
        }
        // config.json overrides: tools.<name>.enabled / .permissions
        if (overrides.is_object() && overrides.contains(m.name)) {
            const auto& ov = overrides[m.name];
            if (ov.is_object()) {
                if (ov.contains("enabled"))
                    m.enabled = ov["enabled"].get<bool>();
                if (ov.contains("permissions"))
                    m.permissions = ov["permissions"];
            }
        }
        if (!m.enabled) {
            log.push_back("tools: " + m.name + " disabled");
            continue;
        }
        std::error_code fec;
        if (!fs::is_regular_file(m.exe, fec)) {
            log.push_back("tools: " + m.name + ": executable missing - " +
                          m.exe +
                          (m.setup_hint.empty() ? "" : " - " + m.setup_hint));
            continue;
        }
        tools_.push_back(std::move(m));
    }
    if (!tools_.empty()) {
        std::string names;
        for (const auto& t : tools_)
            names += (names.empty() ? "" : ", ") + t.name;
        log.push_back("tools: loaded " + names);
    }
}

const ToolManifest* ToolRegistry::find(const std::string& name) const
{
    for (const auto& t : tools_)
        if (t.name == name)
            return &t;
    return nullptr;
}

std::string ToolRegistry::tools_json() const
{
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& t : tools_)
        arr.push_back({{"type", "function"},
                       {"function",
                        {{"name", t.name},
                         {"description", t.description},
                         {"parameters", t.input_schema}}}});
    return arr.dump();
}

} // namespace krastor
