// Krastor — application configuration.
// Two-level layout, so hand-written comments survive forever:
//   config.jsonc  user-owned, JSONC (// and /* */ comments); the app NEVER
//                 writes it. Lives next to the executable
//                 (exe_dir()/config.jsonc); legacy config.json names in the
//                 exe dir or working directory are still read as fallback.
//   state.json    app-owned (Config::save()): every runtime-mutable setting
//                 (font zoom, window geometry, Settings edits, active agent).
//                 Read after config.jsonc, so it takes precedence.
// Precedence: defaults < config.jsonc < state.json < env API key.
// See config.example.jsonc for the schema.

#pragma once

#include <string>
#include <vector>

#include "md_format.h"  // MdPalette (markdown colors of the "colors" scheme)

namespace krastor {

struct LlmConfig {
    std::string base_url = "https://api.openai.com/v1";
    std::string api_key;            // resolved from config or environment
    std::string model = "gpt-4o-mini";
    // Selected entry in llm_models.jsonc ("provider_id/model_id"). When set and
    // found, it overrides base_url/api_key/model (and context_size when the
    // file declares limit.context). See llm_models.h.
    std::string model_ref;
    std::string system_prompt =
        "You are Krastor, a helpful voice assistant. Answer concisely; "
        "your replies may be spoken aloud.";
    double temperature = 0.7;
    int max_tokens = 1024;
    int context_size = 131072;      // model context window (footer usage %)
    // Set from llm_models.jsonc capabilities.tools by apply_model_ref():
    // external tools are only offered to models that support function calling.
    bool supports_tools = false;
    // "low"|"medium"|"max" — sent as body["reasoning_effort"] when non-empty
    // (OpenAI-compat thinking knob). Filled by apply_model_ref() from the
    // model entry's "thinking", overridden by the active agent's thinking.
    std::string reasoning_effort;
};

// One selectable agent (config.jsonc "agents" map). Empty fields inherit:
// system_prompt -> llm.system_prompt, model_ref -> llm.model_ref,
// thinking -> the model entry's thinking (llm_models.jsonc).
struct AgentDef {
    std::string id;
    std::string name;           // display name (top bar, title); default = id
    std::string color = "#78B4FF";  // accent color: input bar, caret, thinking
    std::string system_prompt;
    std::string model_ref;
    std::string thinking;
    // Optional "permissions" object (same shape as the tools permission
    // overrides): merged over the tool policy while this agent is active,
    // so an agent can run read-only. Raw JSON; empty = no override.
    std::string permissions_json;
};

struct TtsConfig {
    std::string models_dir = "models";
    std::string default_voice;      // empty -> first discovered voice
    double speed = 1.0;
    // Speak every assistant reply automatically (GUI top-bar toggle, /voice).
    // Off by default: the user enables it when they want hands-free audio.
    bool auto_speak = false;
};

struct SttConfig {
    // Index into the capture-device list shown by the CLI `/devices` command.
    // -1 = system default microphone.
    int input_device = -1;
    // Input DSP (see src/audio_fx.h): RNNoise noise suppression and SpeexDSP
    // acoustic echo cancellation (uses the TTS playback as far-end reference).
    bool denoise = true;
    bool echo_cancel = true;
};

struct GuiConfig {
    int font_size = 18;             // px, Ctrl +/- steps within [12, 30]
    std::string effect;             // shader effect name ("" = built-in gradient)
    double effect_intensity = 1.0;  // background brightness multiplier 0.2..3
    int window_w = 900;             // main window size, restored at startup
    int window_h = 640;
    int window_x = -1;              // main window position (-1 = OS default)
    int window_y = -1;
    bool format_output = true;      // Markdown/syntax formatting of LLM replies
    int history_size = 100;         // input history lines kept (0 = off)
    bool media_autoplay = true;     // [sound: ...] from the model starts now
};

// Basic chat color scheme (config.jsonc "colors", "#rrggbb"). User-owned
// like the agents map: parsed from config.jsonc only, never written back.
// Defaults are the opencode dark palette (white text). The user-message
// marker bar is not here: it always uses the active agent's color.
struct ColorScheme {
    std::string text = "#FFFFFF";          // default chat text
    std::string system = "#A0A0A0";        // "! " system/error lines
    std::string meta = "#6E6E78";          // dim stats sub-line
    std::string thinking = "#E6963C";      // "+ Thought · Ns" header
    std::string thinking_body = "#9696A0"; // expanded thought text
    std::string block_bg = "#16181F";      // chat block panel background
    MdPalette md;                          // markdown palette ("colors"."md_*")
};

// Chat block layout in px (config.jsonc "chat", user-owned like colors):
// every block is [bar bar_w][bar_gap][panel: content with pad_x/pad_y
// padding], blocks are separated by block_gap.
struct ChatLayout {
    float bar_w = 3.0f;      // colored left bar width
    float bar_gap = 14.0f;   // bar -> panel horizontal gap
    float pad_x = 12.0f;     // panel border -> text, horizontal
    float pad_y = 10.0f;     // panel border -> text, vertical
    float block_gap = 10.0f; // vertical gap between blocks
};

struct AgentConfig {
    std::string name = "Krastor";       // base persona name (UI + window title)
    std::string active;                 // id of the selected agent in `agents`
    bool continuous = false;            // hands-free mic -> STT -> LLM -> TTS loop
    bool ignore_while_speaking = true;  // drop mic segments while thinking/speaking
    int max_tool_rounds = 16;           // tool-loop cap per user message (see tool_loop)
};

struct ToolsConfig {
    // Global switch for the external tools system (see tools_plan.md).
    // Per-tool overrides (enabled/permissions) live in the tool manifests
    // and can be patched here as "tools.<name>.{enabled,permissions}".
    bool enabled = true;
    std::string overrides_json;  // raw "tools" object minus "enabled"
};

struct SessionsConfig {
    // Persistent chat sessions (see sessions_plan.md).
    std::string dir;                  // "" -> sessions/ next to the executable
    bool autosave = true;             // persist every completed turn
    bool continue_last = true;        // startup resumes the most recent session
    bool auto_compact = true;         // compact automatically near the limit
    std::string compact_buffer = "15%";  // free room kept ("NN%" or tokens)
    // state.json only: id of the session that was active when the app last
    // ran. Startup prefers it over "most recently updated", so switching to
    // an older session and closing restarts on THAT session.
    std::string last_id;
};

struct Config {
    LlmConfig llm;
    TtsConfig tts;
    SttConfig stt;
    GuiConfig gui;
    AgentConfig agent;
    ColorScheme colors;
    ChatLayout chat;
    ToolsConfig tools;
    SessionsConfig sessions;
    // Selectable agents from config.jsonc "agents" (file order). When the
    // file defines none, a base agent is synthesized from agent.name +
    // llm.system_prompt so the UI always has at least one entry.
    std::vector<AgentDef> agents;

    // Directory containing the running executable ("." when undetectable).
    static std::string exe_dir();

    // Canonical user config path: <exe_dir>/config.jsonc.
    static std::string runtime_path();

    // App-owned mutable state: <exe_dir>/state.json (written by save()).
    static std::string state_path();

    // Path to load from: exe_dir()/config.jsonc, then the legacy names
    // exe_dir()/config.json, ./config.jsonc, ./config.json (in that order);
    // falls back to the canonical path (created on first save).
    static std::string default_path();

    // Loads config from `path` (JSONC: // and /* */ comments allowed),
    // then overlays state.json — the one next to `path`, else exe_dir()'s —
    // on top. Missing files are not errors (defaults apply); malformed JSON
    // in `path` throws std::runtime_error (a broken state.json is ignored).
    // The api key falls back to the KRASTOR_API_KEY then OPENAI_API_KEY
    // environment variables when empty.
    static Config load(const std::string& path);

    // Writes the runtime-mutable settings (the state.json schema: same keys
    // as load() understands, minus the user-only "agents" map). Used by the
    // GUI settings modal and by the auto-save on every validated change.
    // Atomic: serialized to `<path>.tmp` then renamed over the target, so a
    // crash can never leave a half-written file. Throws on write failure.
    void save(const std::string& path) const;
    void save() const { save(state_path()); }

    // The agent with `id`, or nullptr. An empty id matches the active agent.
    const AgentDef* find_agent(const std::string& id) const;
    // Circular next agent after `id` (never null when agents is non-empty).
    const AgentDef* next_agent(const std::string& id) const;
};

// "#rrggbb" (or "rrggbb") -> rgb components; anything invalid yields the
// fallback (the base agent's azzurro 120,180,255).
void parse_hex_color(const std::string& hex, int& r, int& g, int& b);

// The system prompt actually sent for `agent`: the agent's own prompt (or the
// global llm.system_prompt when empty) plus the usage suffixes (tool sandbox,
// media tokens, ANSI colors), each guarded so an already-suffixed base is
// never doubled. `tools_active` = tool registry loaded and model supports
// function calling. Shared by GUI and CLI so switching agents recomposes the
// exact same prompt a fresh start would build.
std::string compose_system_prompt(const Config& cfg, const AgentDef* agent,
                                  bool tools_active);

} // namespace krastor
