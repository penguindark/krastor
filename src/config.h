// Krastor — application configuration.
// Loads settings from a JSON file with environment variable fallback for the API
// key. The runtime config lives next to the executable (exe_dir()/config.json);
// a legacy config.json in the working directory is still read as a fallback and
// migrated on first save. See config.example.json for the schema.

#pragma once

#include <string>

namespace krastor {

struct LlmConfig {
    std::string base_url = "https://api.openai.com/v1";
    std::string api_key;            // resolved from config or environment
    std::string model = "gpt-4o-mini";
    // Selected entry in llm_models.json ("provider_id/model_id"). When set and
    // found, it overrides base_url/api_key/model (and context_size when the
    // file declares limit.context). See llm_models.h.
    std::string model_ref;
    std::string system_prompt =
        "You are Krastor, a helpful voice assistant. Answer concisely; "
        "your replies may be spoken aloud.";
    double temperature = 0.7;
    int max_tokens = 1024;
    int context_size = 131072;      // model context window (footer usage %)
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

struct GuiConfig {
    int font_size = 18;             // px, Ctrl +/- steps within [12, 30]
    std::string effect;             // shader effect name ("" = built-in gradient)
    double effect_intensity = 1.0;  // background brightness multiplier 0.2..3
    int window_w = 900;             // main window size, restored at startup
    int window_h = 640;
};

struct AgentConfig {
    std::string name = "Krastor";       // agent persona name (UI + window title)
    bool continuous = false;            // hands-free mic -> STT -> LLM -> TTS loop
    bool ignore_while_speaking = true;  // drop mic segments while thinking/speaking
};

struct Config {
    LlmConfig llm;
    TtsConfig tts;
    SttConfig stt;
    GuiConfig gui;
    AgentConfig agent;

    // Directory containing the running executable ("." when undetectable).
    static std::string exe_dir();

    // Canonical runtime config path: <exe_dir>/config.json.
    static std::string runtime_path();

    // Path to load from: the runtime path when it exists, else a "config.json"
    // in the working directory (legacy location, migrated on first save).
    static std::string default_path();

    // Loads config from `path`. Missing file is not an error (defaults apply);
    // malformed JSON throws std::runtime_error. The api key falls back to the
    // KRASTOR_API_KEY then OPENAI_API_KEY environment variables when empty.
    static Config load(const std::string& path);

    // Writes the current settings as JSON (same schema as load()). Used by the
    // GUI settings modal and by the auto-save on every validated change.
    // Atomic: serialized to `<path>.tmp` then renamed over the target, so a
    // crash can never leave a half-written file. Throws on write failure.
    void save(const std::string& path) const;
    void save() const { save(runtime_path()); }
};

} // namespace krastor
