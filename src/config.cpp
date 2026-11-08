#include "config.h"

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include "md_format.h"     // kAnsiUsagePrompt
#include "media.h"         // kMediaUsagePrompt
#include "permissions.h"   // tool_usage_prompt()

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace krastor {

namespace {

std::string env(const char* name)
{
    const char* value = std::getenv(name);
    return value ? value : "";
}

// One JSONC object merged into `cfg` (every key optional). Called for
// config.jsonc (user_only = true: the user-owned "agents" map and "colors"
// scheme are read) and again for state.json (false: those sections are
// user-owned and never live in the state file).
void apply_json(Config& cfg, const nlohmann::json& root, bool user_only)
{
    if (root.contains("llm") && root["llm"].is_object()) {
        const auto& j = root["llm"];
        cfg.llm.base_url = j.value("base_url", cfg.llm.base_url);
        cfg.llm.api_key = j.value("api_key", cfg.llm.api_key);
        cfg.llm.model = j.value("model", cfg.llm.model);
        cfg.llm.model_ref = j.value("model_ref", cfg.llm.model_ref);
        cfg.llm.system_prompt = j.value("system_prompt", cfg.llm.system_prompt);
        cfg.llm.temperature = j.value("temperature", cfg.llm.temperature);
        cfg.llm.max_tokens = j.value("max_tokens", cfg.llm.max_tokens);
        cfg.llm.context_size = j.value("context_size", cfg.llm.context_size);
        cfg.llm.reasoning_effort =
            j.value("reasoning_effort", cfg.llm.reasoning_effort);
    }
    if (root.contains("tts") && root["tts"].is_object()) {
        const auto& j = root["tts"];
        cfg.tts.models_dir = j.value("models_dir", cfg.tts.models_dir);
        cfg.tts.default_voice = j.value("default_voice", cfg.tts.default_voice);
        cfg.tts.speed = j.value("speed", cfg.tts.speed);
        cfg.tts.auto_speak = j.value("auto_speak", cfg.tts.auto_speak);
    }
    if (root.contains("stt") && root["stt"].is_object()) {
        const auto& j = root["stt"];
        cfg.stt.input_device = j.value("input_device", cfg.stt.input_device);
        cfg.stt.denoise = j.value("denoise", cfg.stt.denoise);
        cfg.stt.echo_cancel = j.value("echo_cancel", cfg.stt.echo_cancel);
    }
    if (root.contains("gui") && root["gui"].is_object()) {
        const auto& j = root["gui"];
        cfg.gui.font_size = j.value("font_size", cfg.gui.font_size);
        cfg.gui.effect = j.value("effect", cfg.gui.effect);
        cfg.gui.effect_intensity =
            j.value("effect_intensity", cfg.gui.effect_intensity);
        cfg.gui.window_w = j.value("window_w", cfg.gui.window_w);
        cfg.gui.window_h = j.value("window_h", cfg.gui.window_h);
        cfg.gui.window_x = j.value("window_x", cfg.gui.window_x);
        cfg.gui.window_y = j.value("window_y", cfg.gui.window_y);
        cfg.gui.format_output = j.value("format_output", cfg.gui.format_output);
    }
    if (root.contains("agent") && root["agent"].is_object()) {
        const auto& j = root["agent"];
        cfg.agent.name = j.value("name", cfg.agent.name);
        cfg.agent.active = j.value("active", cfg.agent.active);
        cfg.agent.continuous = j.value("continuous", cfg.agent.continuous);
        cfg.agent.ignore_while_speaking =
            j.value("ignore_while_speaking", cfg.agent.ignore_while_speaking);
    }
    if (user_only && root.contains("agents") && root["agents"].is_object()) {
        cfg.agents.clear();
        for (const auto& [id, a] : root["agents"].items()) {
            AgentDef d;
            d.id = id;
            if (a.is_object()) {
                d.name = a.value("name", id);
                d.color = a.value("color", d.color);
                d.system_prompt = a.value("system_prompt", d.system_prompt);
                d.model_ref = a.value("model_ref", d.model_ref);
                d.thinking = a.value("thinking", d.thinking);
            }
            cfg.agents.push_back(std::move(d));
        }
    }
    if (user_only && root.contains("colors") && root["colors"].is_object()) {
        const auto& j = root["colors"];
        cfg.colors.text = j.value("text", cfg.colors.text);
        cfg.colors.system = j.value("system", cfg.colors.system);
        cfg.colors.meta = j.value("meta", cfg.colors.meta);
        cfg.colors.thinking = j.value("thinking", cfg.colors.thinking);
        cfg.colors.thinking_body =
            j.value("thinking_body", cfg.colors.thinking_body);
        cfg.colors.block_bg = j.value("block_bg", cfg.colors.block_bg);
        // md_* keys feed the markdown palette ("md_code" -> palette "code").
        for (const auto& [k, v] : j.items())
            if (v.is_string() && k.size() > 3 && k.rfind("md_", 0) == 0)
                cfg.colors.md.set(k.substr(3), v.get<std::string>());
    }
    if (user_only && root.contains("chat") && root["chat"].is_object()) {
        const auto& j = root["chat"];
        cfg.chat.bar_w = j.value("bar_w", cfg.chat.bar_w);
        cfg.chat.bar_gap = j.value("bar_gap", cfg.chat.bar_gap);
        cfg.chat.pad_x = j.value("pad_x", cfg.chat.pad_x);
        cfg.chat.pad_y = j.value("pad_y", cfg.chat.pad_y);
        cfg.chat.block_gap = j.value("block_gap", cfg.chat.block_gap);
    }
    if (root.contains("tools") && root["tools"].is_object()) {
        const auto& j = root["tools"];
        cfg.tools.enabled = j.value("enabled", cfg.tools.enabled);
        nlohmann::json ov = j;  // per-tool overrides = everything else
        ov.erase("enabled");
        if (!ov.empty())
            cfg.tools.overrides_json = ov.dump();
    }
    if (root.contains("sessions") && root["sessions"].is_object()) {
        const auto& j = root["sessions"];
        cfg.sessions.dir = j.value("dir", cfg.sessions.dir);
        cfg.sessions.autosave = j.value("autosave", cfg.sessions.autosave);
        cfg.sessions.continue_last =
            j.value("continue_last", cfg.sessions.continue_last);
        cfg.sessions.auto_compact =
            j.value("auto_compact", cfg.sessions.auto_compact);
        cfg.sessions.compact_buffer =
            j.value("compact_buffer", cfg.sessions.compact_buffer);
        cfg.sessions.last_id = j.value("last_id", cfg.sessions.last_id);
    }
}

// Parses a whole JSONC file. `tolerant` = return false on any error instead of
// throwing (used for state.json: a broken state must never stop the app).
bool parse_file(const std::string& path, nlohmann::json& out, bool tolerant)
{
    std::ifstream file(path);
    if (!file)
        return false;
    try {
        // ignore_comments = true: JSONC (// and /* */ comments).
        out = nlohmann::json::parse(file, nullptr, true, true);
    } catch (const nlohmann::json::exception& e) {
        if (tolerant)
            return false;
        throw std::runtime_error("Invalid JSON in " + path + ": " + e.what());
    }
    return out.is_object();
}

std::string dir_of(const std::string& path)
{
    const size_t pos = path.find_last_of("/\\");
    return pos == std::string::npos ? std::string(".") : path.substr(0, pos);
}

} // namespace

std::string Config::exe_dir()
{
#ifdef _WIN32
    char buf[MAX_PATH * 4];
    const DWORD n = GetModuleFileNameA(nullptr, buf, DWORD(sizeof(buf)) - 1);
    std::string p = (n > 0 && n < sizeof(buf)) ? std::string(buf, n) : std::string();
#else
    char buf[4096];
    const ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    std::string p = (n > 0) ? std::string(buf, static_cast<size_t>(n)) : std::string();
#endif
    const size_t pos = p.find_last_of("/\\");
    return pos == std::string::npos ? std::string(".") : p.substr(0, pos);
}

std::string Config::runtime_path()
{
    return exe_dir() + "/config.jsonc";
}

std::string Config::state_path()
{
    return exe_dir() + "/state.json";
}

std::string Config::default_path()
{
    std::error_code ec;
    if (std::filesystem::exists(runtime_path(), ec))
        return runtime_path();
    // Legacy names, still readable: the old runtime config next to the exe,
    // then the working-directory files (migrated on first save).
    const std::string legacy_exe = exe_dir() + "/config.json";
    if (std::filesystem::exists(legacy_exe, ec))
        return legacy_exe;
    std::error_code ec2;
    if (std::filesystem::exists("config.jsonc", ec2))
        return "config.jsonc";
    if (std::filesystem::exists("config.json", ec2))
        return "config.json";
    return runtime_path();  // will be created next to the exe on first save
}

Config Config::load(const std::string& path)
{
    Config cfg;

    nlohmann::json root;
    if (parse_file(path, root, false))
        apply_json(cfg, root, true);

    // state.json (app-owned) overrides config.jsonc: the copy next to the
    // config file first (keeps portable setups together), else the canonical
    // one next to the executable.
    {
        const std::string beside = dir_of(path) + "/state.json";
        nlohmann::json state;
        if ((beside != state_path() && parse_file(beside, state, true)) ||
            parse_file(state_path(), state, true))
            apply_json(cfg, state, false);
    }

    // Base agent: the UI always has one. Synthesized when the file defined no
    // "krastor" entry; empty fields inherit the global llm config at use time.
    if (!cfg.find_agent("krastor")) {
        AgentDef base;
        base.id = "krastor";
        base.name = cfg.agent.name;
        cfg.agents.insert(cfg.agents.begin(), base);
    }
    if (!cfg.find_agent(cfg.agent.active))
        cfg.agent.active = cfg.find_agent("krastor")->id;

    if (cfg.llm.api_key.empty()) {
        cfg.llm.api_key = env("KRASTOR_API_KEY");
        if (cfg.llm.api_key.empty())
            cfg.llm.api_key = env("OPENAI_API_KEY");
    }
    return cfg;
}

const AgentDef* Config::find_agent(const std::string& id) const
{
    const std::string want = id.empty() ? agent.active : id;
    for (const auto& a : agents)
        if (a.id == want)
            return &a;
    return nullptr;
}

const AgentDef* Config::next_agent(const std::string& id) const
{
    if (agents.empty())
        return nullptr;
    size_t idx = 0;
    for (size_t i = 0; i < agents.size(); ++i)
        if (agents[i].id == id)
            idx = i;
    return &agents[(idx + 1) % agents.size()];
}

void parse_hex_color(const std::string& hex, int& r, int& g, int& b)
{
    r = 120; g = 180; b = 255;  // fallback: the base agent's azzurro
    std::string s = hex;
    if (!s.empty() && s[0] == '#')
        s.erase(0, 1);
    if (s.size() != 6)
        return;
    int v = 0;
    for (char c : s) {
        if (!std::isxdigit(static_cast<unsigned char>(c)))
            return;
        v = v * 16 + (std::isdigit(static_cast<unsigned char>(c))
                          ? c - '0'
                          : (std::tolower(static_cast<unsigned char>(c)) - 'a' + 10));
    }
    r = (v >> 16) & 0xff;
    g = (v >> 8) & 0xff;
    b = v & 0xff;
}

std::string compose_system_prompt(const Config& cfg, const AgentDef* agent,
                                  bool tools_active)
{
    std::string p = (agent && !agent->system_prompt.empty())
                        ? agent->system_prompt
                        : cfg.llm.system_prompt;
    // Same guarded appends the GUI/CLI startup used: a base prompt that
    // already carries a suffix (older saved configs) is never doubled.
    if (tools_active &&
        p.find("provided tools to inspect") == std::string::npos)
        p += tool_usage_prompt();
    if (p.find("[image: ") == std::string::npos)
        p += kMediaUsagePrompt;
    if (p.find("renders ANSI SGR colors") == std::string::npos)
        p += kAnsiUsagePrompt;
    return p;
}

void Config::save(const std::string& path) const
{
    nlohmann::json root;
    root["llm"] = {
        {"base_url", llm.base_url},
        {"api_key", llm.api_key},
        {"model", llm.model},
        {"model_ref", llm.model_ref},
        {"system_prompt", llm.system_prompt},
        {"temperature", llm.temperature},
        {"max_tokens", llm.max_tokens},
        {"context_size", llm.context_size},
        {"reasoning_effort", llm.reasoning_effort},
    };
    root["tts"] = {
        {"models_dir", tts.models_dir},
        {"default_voice", tts.default_voice},
        {"speed", tts.speed},
        {"auto_speak", tts.auto_speak},
    };
    root["stt"] = {
        {"input_device", stt.input_device},
        {"denoise", stt.denoise},
        {"echo_cancel", stt.echo_cancel},
    };
    root["gui"] = {
        {"font_size", gui.font_size},
        {"effect", gui.effect},
        {"effect_intensity", gui.effect_intensity},
        {"window_w", gui.window_w},
        {"window_h", gui.window_h},
        {"window_x", gui.window_x},
        {"window_y", gui.window_y},
        {"format_output", gui.format_output},
    };
    root["agent"] = {
        {"name", agent.name},
        {"active", agent.active},
        {"continuous", agent.continuous},
        {"ignore_while_speaking", agent.ignore_while_speaking},
    };
    root["tools"] = {{"enabled", tools.enabled}};
    if (!tools.overrides_json.empty()) {
        // Re-embed the per-tool overrides verbatim (round-trip via load()).
        auto ov = nlohmann::json::parse(tools.overrides_json, nullptr, false);
        if (!ov.is_discarded() && ov.is_object())
            for (const auto& [k, v] : ov.items())
                root["tools"][k] = v;
    }
    root["sessions"] = {
        {"dir", sessions.dir},
        {"autosave", sessions.autosave},
        {"continue_last", sessions.continue_last},
        {"auto_compact", sessions.auto_compact},
        {"compact_buffer", sessions.compact_buffer},
        {"last_id", sessions.last_id},
    };
    // NOTE: the "agents" map and the "colors" scheme are deliberately NOT
    // written: they live only in config.jsonc, where the user's comments are
    // preserved forever.

    // Atomic write: serialize to a temp file, then rename over the target
    // (std::filesystem::rename replaces an existing file on both toolchains).
    const std::string tmp = path + ".tmp";
    {
        std::ofstream file(tmp, std::ios::trunc);
        if (!file)
            throw std::runtime_error("Cannot write config to " + path);
        file << root.dump(2) << '\n';
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        std::filesystem::remove(tmp);
        throw std::runtime_error("Cannot write config to " + path + ": " + ec.message());
    }
}

} // namespace krastor
