#include "config.h"

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>

#include <nlohmann/json.hpp>

namespace krastor {

namespace {

std::string env(const char* name)
{
    const char* value = std::getenv(name);
    return value ? value : "";
}

} // namespace

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
            cfg.llm.system_prompt = j.value("system_prompt", cfg.llm.system_prompt);
            cfg.llm.temperature = j.value("temperature", cfg.llm.temperature);
            cfg.llm.max_tokens = j.value("max_tokens", cfg.llm.max_tokens);
        }
        if (root.contains("tts")) {
            const auto& j = root["tts"];
            cfg.tts.models_dir = j.value("models_dir", cfg.tts.models_dir);
            cfg.tts.default_voice = j.value("default_voice", cfg.tts.default_voice);
            cfg.tts.speed = j.value("speed", cfg.tts.speed);
        }
    }

    if (cfg.llm.api_key.empty()) {
        cfg.llm.api_key = env("KRASTOR_API_KEY");
        if (cfg.llm.api_key.empty())
            cfg.llm.api_key = env("OPENAI_API_KEY");
    }
    return cfg;
}

} // namespace krastor
