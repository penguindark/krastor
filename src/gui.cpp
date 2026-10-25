// Krastor — GUI implementation (GLFW + OpenGL3 + Nuklear).
// The window is the agent's main interface: a terminal-style chat panel, a text
// input with Send, a Listen button (Parakeet STT), an auto-speak toggle (Piper TTS)
// and a gear icon opening the settings modal. Slow work (LLM HTTP, STT, TTS) runs
// on worker threads so the render loop never blocks. A GLSL fragment shader draws
// an animated background behind the UI.
// This translation unit owns the Nuklear and GLFW-GL3 backend implementations.

#include <glad/glad.h>
#include <GLFW/glfw3.h>

#define NK_INCLUDE_DEFAULT_ALLOCATOR
#define NK_INCLUDE_VERTEX_BUFFER_OUTPUT
#define NK_INCLUDE_FONT_BAKING
#define NK_INCLUDE_DEFAULT_FONT
#define NK_IMPLEMENTATION
#define NK_GLFW_GL3_IMPLEMENTATION
#include "nuklear.h"
#include "nuklear_glfw_gl3.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "config.h"
#include "conversation.h"
#include "gui.h"
#include "llm_client.h"
#include "stt.h"
#include "tts.h"
#include "voices.h"

namespace krastor {

namespace {

constexpr int kWinW = 900;
constexpr int kWinH = 600;
constexpr const char* kConfigPath = "config.json";

// ---------------------------------------------------------------- background
// Fullscreen animated gradient drawn with a fragment shader before Nuklear.
struct BgShader {
    GLuint prog = 0;
    GLuint vao = 0;
    bool ok = false;
    std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();

    void init()
    {
        const char* vs =
            "#version 330 core\n"
            "void main() {\n"
            "  vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
            "  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
            "}\n";
        const char* fs =
            "#version 330 core\n"
            "uniform float u_time;\n"
            "uniform vec2 u_res;\n"
            "out vec4 frag_color;\n"
            "void main() {\n"
            "  vec2 uv = gl_FragCoord.xy / u_res;\n"
            "  float v = 0.5 + 0.5 * sin(uv.x * 3.0 + u_time * 0.25)\n"
            "                             * sin(uv.y * 2.0 - u_time * 0.18);\n"
            "  float vig = smoothstep(1.2, 0.35, length(uv - 0.5));\n"
            "  vec3 col = mix(vec3(0.045, 0.050, 0.075),\n"
            "                 vec3(0.090, 0.120, 0.190), v) * (0.55 + 0.45 * vig);\n"
            "  frag_color = vec4(col, 1.0);\n"
            "}\n";
        auto compile = [](GLenum type, const char* src) -> GLuint {
            GLuint s = glCreateShader(type);
            glShaderSource(s, 1, &src, nullptr);
            glCompileShader(s);
            GLint ok = 0;
            glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
            if (!ok) {
                char log[512];
                glGetShaderInfoLog(s, sizeof(log), nullptr, log);
                std::fprintf(stderr, "gui: shader compile failed: %s\n", log);
                glDeleteShader(s);
                return 0;
            }
            return s;
        };
        const GLuint v = compile(GL_VERTEX_SHADER, vs);
        const GLuint f = compile(GL_FRAGMENT_SHADER, fs);
        if (!v || !f)
            return;
        prog = glCreateProgram();
        glAttachShader(prog, v);
        glAttachShader(prog, f);
        glLinkProgram(prog);
        glDeleteShader(v);
        glDeleteShader(f);
        GLint linked = 0;
        glGetProgramiv(prog, GL_LINK_STATUS, &linked);
        glGenVertexArrays(1, &vao);
        ok = linked != 0;
    }

    void draw(int w, int h) const
    {
        if (!ok)
            return;
        const float t = std::chrono::duration<float>(std::chrono::steady_clock::now() - t0).count();
        glUseProgram(prog);
        glUniform1f(glGetUniformLocation(prog, "u_time"), t);
        glUniform2f(glGetUniformLocation(prog, "u_res"), (float)w, (float)h);
        glBindVertexArray(vao);
        glDrawArrays(GL_TRIANGLES, 0, 3);  // big triangle covering the screen
        glUseProgram(0);
    }
};

// ------------------------------------------------------------- gear icon
void draw_gear(struct nk_command_buffer* c, struct nk_rect r, struct nk_color col)
{
    const float cx = r.x + r.w * 0.5f;
    const float cy = r.y + r.h * 0.5f;
    const float rad = (r.w < r.h ? r.w : r.h) * 0.42f;
    for (int i = 0; i < 8; ++i) {
        const float a = static_cast<float>(i) * 3.14159265f / 4.0f;
        const float dx = std::cos(a), dy = std::sin(a);
        nk_stroke_line(c, cx + dx * rad * 0.55f, cy + dy * rad * 0.55f,
                          cx + dx * rad * 1.00f, cy + dy * rad * 1.00f, 3.0f, col);
    }
    nk_fill_circle(c, nk_rect(cx - rad * 0.62f, cy - rad * 0.62f,
                              rad * 1.24f, rad * 1.24f), col);
    nk_fill_circle(c, nk_rect(cx - rad * 0.26f, cy - rad * 0.26f,
                              rad * 0.52f, rad * 0.52f), nk_rgb(45, 45, 48));
}

// ------------------------------------------------------------- app state
struct ChatLine {
    std::string who;   // "you" | "krastor" | "sys"
    std::string text;
};

struct SettingsState {
    char base_url[512] = {0};
    char model[256] = {0};
    char system_prompt[4096] = {0};
    float temperature = 0.7f;
    int max_tokens = 1024;
    std::string voice;
    std::string status;
    bool open = false;
    bool dirty = false;   // LLM settings changed -> rebuild the client

    void load_from(const Config& cfg, const VoiceRegistry& voices)
    {
        std::snprintf(base_url, sizeof(base_url), "%s", cfg.llm.base_url.c_str());
        std::snprintf(model, sizeof(model), "%s", cfg.llm.model.c_str());
        std::snprintf(system_prompt, sizeof(system_prompt), "%s",
                      cfg.llm.system_prompt.c_str());
        temperature = static_cast<float>(cfg.llm.temperature);
        max_tokens = cfg.llm.max_tokens;
        const auto* v = voices.current();
        voice = v ? v->id : (voices.voices().empty() ? std::string() : voices.voices()[0].id);
    }
};

struct AppState {
    Config& cfg;
    VoiceRegistry& voices;
    Conversation conv;
    LlmClient llm;
    Tts tts;
    Stt stt;

    std::vector<ChatLine> lines;   // main thread only
    SettingsState settings;

    enum class Worker { None, Thinking, Listening } worker = Worker::None;
    std::future<LlmReply> llm_fut;
    std::future<std::string> stt_fut;
    std::atomic<bool> speaking{false};
    bool auto_speak = true;
    char input[1024] = {0};

    AppState(Config& c, VoiceRegistry& v)
        : cfg(c), voices(v), conv(c.llm.system_prompt), llm(c.llm), tts(v, c), stt(c)
    {
        lines.push_back({"sys", "Krastor ready. Type below or click Listen, "
                                "then Enter to send. Gear = settings."});
    }

    void speak_async(const std::string& text)
    {
        if (text.empty() || speaking.load())
            return;
        speaking.store(true);
        std::thread([this, text] {
            tts.speak(text);
            speaking.store(false);
        }).detach();
    }

    void send(const std::string& text)
    {
        if (text.empty() || worker != Worker::None)
            return;
        lines.push_back({"you", text});
        conv.add_user(text);
        worker = Worker::Thinking;
        const LlmClient client(llm);          // snapshot: survives settings changes
        const Conversation snapshot = conv;   // copy for the worker thread
        llm_fut = std::async(std::launch::async, [client, snapshot] {
            return client.complete(snapshot);
        });
    }

    void start_listen()
    {
        if (worker != Worker::None)
            return;
        worker = Worker::Listening;
        const int idx = cfg.stt.input_device;
        Stt& stt_ref = stt;
        stt_fut = std::async(std::launch::async,
                             [&stt_ref, idx] { return stt_ref.listen(10.0, idx); });
    }

    // Polls finished worker tasks once per frame.
    void poll()
    {
        if (worker == Worker::Thinking &&
            llm_fut.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            const LlmReply reply = llm_fut.get();
            worker = Worker::None;
            if (reply.ok) {
                conv.add_assistant(reply.content);
                lines.push_back({"krastor", reply.content});
                if (auto_speak)
                    speak_async(reply.content);
            } else {
                lines.push_back({"sys", "LLM error: " + reply.error});
            }
        }
        if (worker == Worker::Listening &&
            stt_fut.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            const std::string heard = stt_fut.get();
            worker = Worker::None;
            if (heard.empty())
                lines.push_back({"sys", "(nothing heard)"});
            else
                send(heard);
        }
    }
};

// ------------------------------------------------------------- settings modal
bool settings_modal(struct nk_context* ctx, Config& cfg, VoiceRegistry& voices,
                    SettingsState& s, int win_w, int win_h)
{
    if (!s.open)
        return false;

    const int mw = 520, mh = 430;
    nk_begin(ctx, "Settings", nk_rect((win_w - mw) / 2.0f, (win_h - mh) / 2.0f, mw, mh),
             NK_WINDOW_TITLE | NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_CLOSABLE);

    nk_layout_row_dynamic(ctx, 22, 1);
    nk_label(ctx, "LLM Engine", NK_LEFT);
    nk_layout_row_dynamic(ctx, 26, 1);
    nk_label(ctx, "Base URL", NK_LEFT);
    nk_layout_row_dynamic(ctx, 26, 1);
    nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, s.base_url,
                                   sizeof(s.base_url) - 1, nk_filter_default);
    nk_layout_row_dynamic(ctx, 26, 1);
    nk_label(ctx, "Model", NK_LEFT);
    nk_layout_row_dynamic(ctx, 26, 1);
    nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, s.model,
                                   sizeof(s.model) - 1, nk_filter_default);

    nk_layout_row_dynamic(ctx, 26, 1);
    nk_label(ctx, "TTS Voice", NK_LEFT);
    nk_layout_row_dynamic(ctx, 26, 1);
    if (nk_combo_begin_label(ctx, s.voice.empty() ? "(none)" : s.voice.c_str(),
                             nk_vec2(480, 200))) {
        nk_layout_row_dynamic(ctx, 24, 1);
        for (const auto& v : voices.voices())
            if (nk_combo_item_label(ctx, v.id.c_str(), NK_TEXT_LEFT))
                s.voice = v.id;
        nk_combo_end(ctx);
    }

    nk_layout_row_dynamic(ctx, 26, 1);
    nk_property_float(ctx, "Temperature", 0.0f, &s.temperature, 2.0f, 0.05f, 0.01f);
    nk_layout_row_dynamic(ctx, 26, 1);
    nk_property_int(ctx, "Max tokens", 1, &s.max_tokens, 32000, 64, 1.0f);

    nk_layout_row_dynamic(ctx, 22, 1);
    nk_label(ctx, "System prompt", NK_LEFT);
    nk_layout_row_dynamic(ctx, 52, 1);
    nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, s.system_prompt,
                                   sizeof(s.system_prompt) - 1, nk_filter_default);

    if (!s.status.empty()) {
        nk_layout_row_dynamic(ctx, 20, 1);
        nk_label_colored(ctx, s.status.c_str(), NK_LEFT, nk_rgb(120, 200, 120));
    }

    nk_layout_row_dynamic(ctx, 30, 2);
    if (nk_button_label(ctx, "Save")) {
        cfg.llm.base_url = s.base_url;
        cfg.llm.model = s.model;
        cfg.llm.system_prompt = s.system_prompt;
        cfg.llm.temperature = s.temperature;
        cfg.llm.max_tokens = s.max_tokens;
        cfg.tts.default_voice = s.voice;
        if (!s.voice.empty())
            voices.select(s.voice);
        try {
            cfg.save(kConfigPath);
            s.status = "Saved to " + std::string(kConfigPath);
            s.dirty = true;
        } catch (const std::exception& e) {
            s.status = std::string("Save failed: ") + e.what();
        }
    }
    if (nk_button_label(ctx, "Close")) {
        s.open = false;
        s.status.clear();
    }

    const bool closed_by_x = nk_window_is_closed(ctx, "Settings") != 0;
    nk_end(ctx);
    if (closed_by_x) {
        s.open = false;
        s.status.clear();
        return false;
    }
    return true;
}

// ------------------------------------------------------------- chat panel
// Rough wrapped height for a line of text in the chat panel.
float chat_line_height(const std::string& text)
{
    const int visual_rows = static_cast<int>(text.size()) / 70 + 1;
    return 18.0f * visual_rows + 6.0f;
}

void chat_panel(struct nk_context* ctx, AppState& app)
{
    nk_layout_row_dynamic(ctx, 0, 1);  // fill the remaining space
    if (nk_group_begin(ctx, "##chat", NK_WINDOW_BORDER)) {
        for (const auto& l : app.lines) {
            nk_layout_row_dynamic(ctx, chat_line_height(l.text), 1);
            struct nk_color col = nk_rgb(160, 160, 160);
            if (l.who == "you") col = nk_rgb(120, 180, 255);
            else if (l.who == "krastor") col = nk_rgb(130, 220, 150);
            const std::string prefix = l.who == "you" ? "> " :
                                       l.who == "krastor" ? "Krastor: " : "! ";
            nk_label_colored_wrap(ctx, (prefix + l.text).c_str(), col);
        }
        // Pin the view to the newest line.
        nk_window_set_scroll(ctx, 0, 1000000);
        nk_group_end(ctx);
    }
}

} // namespace

int run_gui(Config& cfg, VoiceRegistry& voices)
{
    if (!glfwInit()) {
        std::fprintf(stderr, "gui: glfwInit failed\n");
        return 1;
    }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);

    GLFWwindow* win = glfwCreateWindow(kWinW, kWinH, "Krastor", nullptr, nullptr);
    if (!win) {
        std::fprintf(stderr, "gui: failed to create window (OpenGL 3.3 core required)\n");
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(win);
    glfwSwapInterval(1);  // vsync

    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
        std::fprintf(stderr, "gui: gladLoadGLLoader failed\n");
        glfwTerminate();
        return 1;
    }

    BgShader bg;
    bg.init();

    static struct nk_glfw device;
    nk_glfw3_init(&device, win, NK_GLFW3_INSTALL_CALLBACKS);
    struct nk_font_atlas* atlas = nullptr;
    nk_glfw3_font_stash_begin(&device, &atlas);
    struct nk_font* font = nk_font_atlas_add_default(atlas, 18, nullptr);
    nk_glfw3_font_stash_end(&device);
    if (font)
        nk_style_set_font(&device.ctx, &font->handle);
    // Semi-transparent panels so the shader background shows through.
    device.ctx.style.window.background = nk_rgba(16, 18, 24, 210);

    AppState app(cfg, voices);
    app.settings.load_from(cfg, voices);
    app.tts.init();

    while (!glfwWindowShouldClose(win)) {
        int w = 0, h = 0;
        glfwGetWindowSize(win, &w, &h);

        glfwPollEvents();
        app.poll();
        if (app.settings.dirty) {
            app.llm = LlmClient(cfg.llm);
            app.settings.dirty = false;
        }

        nk_glfw3_new_frame(&device);
        struct nk_context* ctx = &device.ctx;

        // ---- main surface (transparent: the shader shows through) ----
        nk_begin(ctx, "Krastor", nk_rect(0, 0, (float)w, (float)h), 0);
        {
            // Top bar: title | speak toggle | gear
            nk_layout_row_begin(ctx, NK_DYNAMIC, 44, 4);
            nk_layout_row_push(ctx, 0.66f);
            nk_label(ctx, "Krastor", NK_MIDDLE_LEFT);
            nk_layout_row_push(ctx, 0.16f);
            if (nk_button_label(ctx, app.auto_speak ? "Speak: ON" : "Speak: OFF"))
                app.auto_speak = !app.auto_speak;
            nk_layout_row_push(ctx, 0.08f);
            nk_spacer(ctx);
            nk_layout_row_push(ctx, 0.10f);
            const struct nk_rect gear_rect = nk_widget_bounds(ctx);
            if (nk_button_label(ctx, " "))
                app.settings.open = !app.settings.open;
            draw_gear(nk_window_get_canvas(ctx), gear_rect, nk_rgb(225, 225, 225));
            nk_layout_row_end(ctx);

            // Chat history
            chat_panel(ctx, app);

            // Input row
            nk_layout_row_begin(ctx, NK_DYNAMIC, 34, 4);
            nk_layout_row_push(ctx, 0.56f);
            const nk_flags edit_state = nk_edit_string_zero_terminated(
                ctx, NK_EDIT_FIELD, app.input, sizeof(app.input) - 1, nk_filter_default);
            nk_layout_row_push(ctx, 0.12f);
            const bool send_click =
                nk_button_label(ctx, app.worker == AppState::Worker::None ? "Send" : "...");
            nk_layout_row_push(ctx, 0.16f);
            const bool listen_click =
                nk_button_label(ctx, app.worker == AppState::Worker::Listening
                                        ? "Listening..." : "Listen");
            nk_layout_row_push(ctx, 0.16f);
            if (nk_button_label(ctx, app.speaking.load() ? "Speaking..." : "Test voice"))
                app.speak_async("Ciao, la voce funziona.");
            nk_layout_row_end(ctx);

            if ((send_click || (edit_state & NK_EDIT_COMMITED) != 0) &&
                app.worker == AppState::Worker::None) {
                app.send(app.input);
                app.input[0] = '\0';
            }
            if (listen_click)
                app.start_listen();

            // Status line
            nk_layout_row_dynamic(ctx, 22, 1);
            std::string status_buf;
            const char* status = "";
            if (app.worker == AppState::Worker::Thinking) status = "thinking...";
            else if (app.worker == AppState::Worker::Listening)
                status = "listening... speak now (max 10 s)";
            else if (app.speaking.load()) status = "speaking...";
            else if (const Voice* v = voices.current()) {
                status_buf = "voice: " + v->id + "  |  mic: " +
                             (cfg.stt.input_device < 0 ? std::string("default")
                                                       : std::to_string(cfg.stt.input_device)) +
                             "  (CLI /devices, /mic)";
                status = status_buf.c_str();
            }
            if (*status)
                nk_label(ctx, status, NK_MIDDLE_LEFT);
        }
        nk_end(ctx);

        settings_modal(ctx, cfg, voices, app.settings, w, h);

        // ---- render: shader background, then the UI ----
        int fb_w = 0, fb_h = 0;
        glfwGetFramebufferSize(win, &fb_w, &fb_h);
        glViewport(0, 0, fb_w, fb_h);
        glClearColor(0.04f, 0.05f, 0.07f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        bg.draw(fb_w, fb_h);
        nk_glfw3_render(&device, NK_ANTI_ALIASING_ON, 4096, 4096 * 3);

        glfwSwapBuffers(win);
    }

    nk_glfw3_shutdown(&device);
    glfwTerminate();
    return 0;
}

} // namespace krastor
