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

class EffectEngine {
public:
    // Compiles the built-in fallback effect. Requires a current GL context.
    void init();

    // Lists available effects: built-in first, then *.frag files in `dir`.
    std::vector<EffectInfo> scan(const std::string& dir) const;

    // Compiles and activates the named effect ("" or built-in name = fallback).
    // On failure keeps the previous effect and records error().
    bool select(const std::string& name, const std::string& dir);

    const std::string& current() const { return current_; }
    const std::string& error() const { return error_; }

    // Recompiles the active plugin if its file changed. True when reloaded.
    bool check_reload();

    // Draws the fullscreen background. `bands`/`level`/`hist` come from the
    // SpectrumAnalyzer (hist = history ring, hist_w x hist_bands, R8).
    // `intensity` multiplies the plugin output (global brightness knob).
    void draw(int w, int h, float t, float level, const unsigned char* hist,
              int hist_w, int hist_bands, int hist_head, bool listening,
              bool speaking, float intensity = 1.0f);

private:
    GLuint compile_prog(const std::string& frag_source);
    bool load_file(const std::string& path, std::string& out) const;

    GLuint prog_ = 0;
    GLuint vao_ = 0;
    GLuint tex_ = 0;
    int tex_w_ = 0, tex_h_ = 0;
    std::string current_ = "gradient";
    std::string active_path_;
    std::string error_;
    std::filesystem::file_time_type mtime_{};
};

} // namespace krastor
