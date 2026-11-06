#include "config.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

#include <nlohmann/json.hpp>

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
    return exe_dir() + "/config.json";
}

std::string Config::default_path()
{
    std::error_code ec;
    if (std::filesystem::exists(runtime_path(), ec))
        return runtime_path();
    std::ifstream legacy("config.json");
    if (legacy)
        return "config.json";  // legacy location; migrated to exe dir on first save
    return runtime_path();      // will be created next to the exe on first save
}

Config Config::load(const std::string& path)
{
    Config cfg;

    std::ifstream file(path);
    if (file) {
        nlohmann::json root;
        try {
            file >> root;
        } catch (const nlohmann::json::exception& e) {
            throw std::runtime_error("Invalid JSON in " + path + ": " + e.what());
        }

        if (root.contains("llm")) {
            const auto& j = root["llm"];
            cfg.llm.base_url = j.value("base_url", cfg.llm.base_url);
            cfg.llm.api_key = j.value("api_key", cfg.llm.api_key);
            cfg.llm.model = j.value("model", cfg.llm.model);
            cfg.llm.model_ref = j.value("model_ref", cfg.llm.model_ref);
            cfg.llm.system_prompt = j.value("system_prompt", cfg.llm.system_prompt);
            cfg.llm.temperature = j.value("temperature", cfg.llm.temperature);
            cfg.llm.max_tokens = j.value("max_tokens", cfg.llm.max_tokens);
            cfg.llm.context_size = j.value("context_size", cfg.llm.context_size);
        }
        if (root.contains("tts")) {
            const auto& j = root["tts"];
            cfg.tts.models_dir = j.value("models_dir", cfg.tts.models_dir);
            cfg.tts.default_voice = j.value("default_voice", cfg.tts.default_voice);
            cfg.tts.speed = j.value("speed", cfg.tts.speed);
        }
        if (root.contains("stt")) {
            const auto& j = root["stt"];
            cfg.stt.input_device = j.value("input_device", cfg.stt.input_device);
            cfg.stt.denoise = j.value("denoise", cfg.stt.denoise);
            cfg.stt.echo_cancel = j.value("echo_cancel", cfg.stt.echo_cancel);
        }
        if (root.contains("gui")) {
            const auto& j = root["gui"];
            cfg.gui.font_size = j.value("font_size", cfg.gui.font_size);
            cfg.gui.effect = j.value("effect", cfg.gui.effect);
            cfg.gui.effect_intensity =
                j.value("effect_intensity", cfg.gui.effect_intensity);
            cfg.gui.window_w = j.value("window_w", cfg.gui.window_w);
            cfg.gui.window_h = j.value("window_h", cfg.gui.window_h);
            cfg.gui.format_output = j.value("format_output", cfg.gui.format_output);
        }
        if (root.contains("agent")) {
            const auto& j = root["agent"];
            cfg.agent.name = j.value("name", cfg.agent.name);
            cfg.agent.continuous = j.value("continuous", cfg.agent.continuous);
            cfg.agent.ignore_while_speaking =
                j.value("ignore_while_speaking", cfg.agent.ignore_while_speaking);
        }
        if (root.contains("tools") && root["tools"].is_object()) {
            const auto& j = root["tools"];
            cfg.tools.enabled = j.value("enabled", cfg.tools.enabled);
            nlohmann::json ov = j;  // per-tool overrides = everything else
            ov.erase("enabled");
            if (!ov.empty())
                cfg.tools.overrides_json = ov.dump();
        }
    }

    if (cfg.llm.api_key.empty()) {
        cfg.llm.api_key = env("KRASTOR_API_KEY");
        if (cfg.llm.api_key.empty())
            cfg.llm.api_key = env("OPENAI_API_KEY");
    }
    return cfg;
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
    };
    root["tts"] = {
        {"models_dir", tts.models_dir},
        {"default_voice", tts.default_voice},
        {"speed", tts.speed},
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
        {"format_output", gui.format_output},
    };
    root["agent"] = {
        {"name", agent.name},
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
