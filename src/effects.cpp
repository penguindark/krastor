// Krastor — shader effect plugin engine implementation.

#include "effects.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace krastor {

namespace {

// Uniform contract prepended to every plugin source. Plugins must not declare
// #version or out_color themselves; they write to `frag_color`.
constexpr const char* kHeader =
    "#version 330 core\n"
    "uniform vec2 u_resolution;   // framebuffer size in px\n"
    "uniform float u_time;        // seconds since start\n"
    "uniform float u_level;       // smoothed mic RMS 0..1 (0 when mic closed)\n"
    "uniform sampler2D u_spectrum;// R8 texture: x = time ring, y = freq bands\n"
    "uniform float u_spec_head;   // newest column of u_spectrum.x (0..1)\n"
    "uniform float u_listening;   // 1 while hands-free listening is active\n"
    "uniform float u_speaking;    // 1 while TTS playback runs\n"
    "out vec4 frag_color;\n";

// Built-in fallback: the original animated gradient.
constexpr const char* kBuiltinSource =
    "void main() {\n"
    "  vec2 uv = gl_FragCoord.xy / u_resolution;\n"
    "  float v = 0.5 + 0.5 * sin(uv.x * 3.0 + u_time * 0.25)\n"
    "                             * sin(uv.y * 2.0 - u_time * 0.18);\n"
    "  float vig = smoothstep(1.2, 0.35, length(uv - 0.5));\n"
    "  vec3 col = mix(vec3(0.045, 0.050, 0.075),\n"
    "                 vec3(0.090, 0.120, 0.190), v) * (0.55 + 0.45 * vig);\n"
    "  frag_color = vec4(col, 1.0);\n"
    "}\n";

constexpr const char* kBuiltinName = "gradient";

const char* kVertex =
    "#version 330 core\n"
    "void main() {\n"
    "  vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
    "  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";

// Display name from an optional first-line comment:
//   // krastor-effect: Ring Spectrogram
std::string effect_name_from_file(const std::string& path, const std::string& src)
{
    std::istringstream in(src);
    std::string line;
    if (std::getline(in, line)) {
        const auto pos = line.find("krastor-effect:");
        if (pos != std::string::npos) {
            std::string name = line.substr(pos + 15);
            const auto start = name.find_first_not_of(" \t");
            if (start != std::string::npos)
                name = name.substr(start);
            while (!name.empty() && (name.back() == ' ' || name.back() == '\r'))
                name.pop_back();
            if (!name.empty())
                return name;
        }
    }
    return std::filesystem::path(path).stem().string();
}

} // namespace

bool EffectEngine::load_file(const std::string& path, std::string& out) const
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

GLuint EffectEngine::compile_prog(const std::string& frag_source)
{
    auto compile = [this](GLenum type, const char* src) -> GLuint {
        const GLuint s = glCreateShader(type);
        glShaderSource(s, 1, &src, nullptr);
        glCompileShader(s);
        GLint ok = 0;
        glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[1024];
            glGetShaderInfoLog(s, sizeof(log), nullptr, log);
            error_ = log;
            std::fprintf(stderr, "effects: shader compile failed:\n%s\n", log);
            glDeleteShader(s);
            return 0;
        }
        return s;
    };
    const GLuint v = compile(GL_VERTEX_SHADER, kVertex);
    if (!v)
        return 0;
    const GLuint f = compile(GL_FRAGMENT_SHADER, frag_source.c_str());
    if (!f) {
        glDeleteShader(v);
        return 0;
    }
    const GLuint p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    glLinkProgram(p);
    glDeleteShader(v);
    glDeleteShader(f);
    GLint linked = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[512];
        glGetProgramInfoLog(p, sizeof(log), nullptr, log);
        error_ = log;
        glDeleteProgram(p);
        return 0;
    }
    return p;
}

void EffectEngine::init()
{
    glGenVertexArrays(1, &vao_);
    glGenTextures(1, &tex_);
    const GLuint p = compile_prog(std::string(kHeader) + kBuiltinSource);
    if (p) {
        prog_ = p;
        current_ = kBuiltinName;
        active_path_.clear();
        error_.clear();
    }
}

std::vector<EffectInfo> EffectEngine::scan(const std::string& dir) const
{
    std::vector<EffectInfo> out{{kBuiltinName, "", true}};
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec))
        return out;
    std::vector<EffectInfo> files;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        if (!e.is_regular_file() || e.path().extension() != ".frag")
            continue;
        std::string src;
        if (!load_file(e.path().string(), src))
            continue;
        files.push_back({effect_name_from_file(e.path().string(), src),
                         e.path().string(), false});
    }
    std::sort(files.begin(), files.end(),
              [](const EffectInfo& a, const EffectInfo& b) { return a.name < b.name; });
    out.insert(out.end(), files.begin(), files.end());
    return out;
}

bool EffectEngine::select(const std::string& name, const std::string& dir)
{
    error_.clear();
    if (name.empty() || name == kBuiltinName) {
        const GLuint p = compile_prog(std::string(kHeader) + kBuiltinSource);
        if (!p)
            return false;
        glDeleteProgram(prog_);
        prog_ = p;
        current_ = kBuiltinName;
        active_path_.clear();
        return true;
    }
    for (const auto& e : scan(dir)) {
        if (e.builtin || e.name != name)
            continue;
        std::string src;
        if (!load_file(e.path, src)) {
            error_ = "cannot read " + e.path;
            return false;
        }
        const GLuint p = compile_prog(std::string(kHeader) + src);
        if (!p)
            return false;  // keep the previous effect
        glDeleteProgram(prog_);
        prog_ = p;
        current_ = name;
        active_path_ = e.path;
        std::error_code ec;
        mtime_ = std::filesystem::last_write_time(e.path, ec);
        return true;
    }
    error_ = "effect not found: " + name;
    return false;
}

bool EffectEngine::check_reload()
{
    if (active_path_.empty())
        return false;
    std::error_code ec;
    const auto mt = std::filesystem::last_write_time(active_path_, ec);
    if (ec || mt == mtime_)
        return false;
    std::string src;
    if (!load_file(active_path_, src))
        return false;
    const GLuint p = compile_prog(std::string(kHeader) + src);
    if (!p)
        return false;  // broken edit: keep the last good version running
    glDeleteProgram(prog_);
    prog_ = p;
    mtime_ = mt;
    error_.clear();
    std::fprintf(stderr, "effects: hot-reloaded %s\n", active_path_.c_str());
    return true;
}

void EffectEngine::draw(int w, int h, float t, float level,
                        const unsigned char* hist, int hist_w, int hist_bands,
                        int hist_head, bool listening, bool speaking)
{
    if (!prog_)
        return;
    // (Re)allocate + upload the spectrum history texture.
    glBindTexture(GL_TEXTURE_2D, tex_);
    if (hist_w != tex_w_ || hist_bands != tex_h_) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, hist_w, hist_bands, 0, GL_RED,
                     GL_UNSIGNED_BYTE, hist);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        tex_w_ = hist_w;
        tex_h_ = hist_bands;
    } else if (hist) {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, hist_w, hist_bands, GL_RED,
                        GL_UNSIGNED_BYTE, hist);
    }
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex_);

    glUseProgram(prog_);
    glUniform2f(glGetUniformLocation(prog_, "u_resolution"), float(w), float(h));
    glUniform1f(glGetUniformLocation(prog_, "u_time"), t);
    glUniform1f(glGetUniformLocation(prog_, "u_level"), level);
    glUniform1i(glGetUniformLocation(prog_, "u_spectrum"), 0);
    glUniform1f(glGetUniformLocation(prog_, "u_spec_head"),
                hist_w ? float(hist_head) / float(hist_w) : 0.0f);
    glUniform1f(glGetUniformLocation(prog_, "u_listening"), listening ? 1.0f : 0.0f);
    glUniform1f(glGetUniformLocation(prog_, "u_speaking"), speaking ? 1.0f : 0.0f);
    glBindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glUseProgram(0);
}

} // namespace krastor
