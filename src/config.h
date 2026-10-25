// Krastor — application configuration.
// Loads settings from a JSON file (config.json by default) with environment
// variable fallback for the API key. See config.example.json for the schema.

#pragma once

#include <string>

namespace krastor {

struct LlmConfig {
    std::string base_url = "https://api.openai.com/v1";
    std::string api_key;            // resolved from config or environment
    std::string model = "gpt-4o-mini";
    std::string system_prompt =
        "You are Krastor, a helpful voice assistant. Answer concisely; "
        "your replies may be spoken aloud.";
    double temperature = 0.7;
    int max_tokens = 1024;
};

struct TtsConfig {
    std::string models_dir = "models";
    std::string default_voice;      // empty -> first discovered voice
    double speed = 1.0;
};

struct SttConfig {
    // Index into the capture-device list shown by the CLI `/devices` command.
    // -1 = system default microphone.
    int input_device = -1;
};

struct Config {
    LlmConfig llm;
    TtsConfig tts;
    SttConfig stt;

    // Loads config from `path`. Missing file is not an error (defaults apply);
    // malformed JSON throws std::runtime_error. The api key falls back to the
    // KRASTOR_API_KEY then OPENAI_API_KEY environment variables when empty.
    static Config load(const std::string& path);

    // Writes the current settings to `path` as JSON (same schema as load()).
    // Used by the GUI settings modal. Throws std::runtime_error on write failure.
    void save(const std::string& path) const;
};

} // namespace krastor
