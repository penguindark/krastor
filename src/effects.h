// Krastor — pluggable GLSL background effects.
// Effects are plain GLSL 330 fragment shaders in data/shaders/*.frag, compiled
// at runtime on top of a shared fullscreen-triangle vertex shader and a fixed
// uniform contract (time, resolution, mic level + spectrum history texture).
// The active plugin is hot-reloaded when its file changes on disk; a plugin
// that fails to compile never replaces the current one.

#pragma once

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

#include <glad/glad.h>

namespace krastor {

struct EffectInfo {
    std::string name;    // display name (header comment or file stem)
    std::string path;    // .frag path ("" for the built-in effect)
    bool builtin = false;
};

// Everything a frame's event feed carries into the shader. Bundled in a
// struct so future events (token streaming, notifications, ...) extend the
// struct + uniform contract without touching draw()'s signature.
struct EffectInput {
    float level = 0.0f;                 // smoothed mic RMS 0..1
    const unsigned char* hist = nullptr;  // hist_w x hist_bands R8 ring
    int hist_w = 0, hist_bands = 0, hist_head = 0;
    bool listening = false;             // hands-free mic capture active
    bool speaking = false;              // TTS playback running
    bool thinking = false;              // LLM request in flight (incl. tools)
    float typing = 0.0f;                // decaying keypress pulse 0..1
    float intensity = 1.0f;             // global brightness (Settings)
};

class EffectEngine {
public:
    // Compiles the built-in fallback effect. Requires a current GL context.
    void init();

    // Lists available effects: "off" first, then built-in, then *.frag files
    // in `dir`.
    std::vector<EffectInfo> scan(const std::string& dir) const;

    // Compiles and activates the named effect ("off" disables the pass,
    // "" or built-in name = fallback). On failure keeps the previous effect
    // and records error().
    bool select(const std::string& name, const std::string& dir);

    const std::string& current() const { return current_; }
    bool enabled() const { return enabled_; }
    const std::string& error() const { return error_; }

    // Recompiles the active plugin if its file changed. True when reloaded.
    bool check_reload();

    // Draws the fullscreen background (no-op while "off").
    void draw(int w, int h, float t, const EffectInput& in);

private:
    GLuint compile_prog(const std::string& frag_source);
    bool load_file(const std::string& path, std::string& out) const;

    GLuint prog_ = 0;
    GLuint vao_ = 0;
    GLuint tex_ = 0;
    int tex_w_ = 0, tex_h_ = 0;
    std::string current_ = "gradient";
    bool enabled_ = true;
    std::string active_path_;
    std::string error_;
    std::filesystem::file_time_type mtime_{};
};

} // namespace krastor
