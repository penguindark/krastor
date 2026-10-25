// Krastor — GUI implementation (GLFW + OpenGL3 + Nuklear).
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

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "gui.h"
#include "config.h"
#include "voices.h"

namespace krastor {

namespace {

constexpr int kWinW = 900;
constexpr int kWinH = 600;
constexpr const char* kConfigPath = "config.json";

// Draws a simple gear icon (8 teeth + body + hole) centered in `r`.
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

// Editable state for the settings modal (fixed buffers for nk_edit_string).
struct SettingsState {
    char base_url[512] = {0};
    char model[256] = {0};
    char system_prompt[4096] = {0};
    float temperature = 0.7f;
    int max_tokens = 1024;
    std::string voice;      // currently selected voice id
    std::string status;     // transient feedback ("Saved", errors)
    bool open = false;

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

// The settings modal. Returns true if it is currently open.
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

    static struct nk_glfw device;
    nk_glfw3_init(&device, win, NK_GLFW3_INSTALL_CALLBACKS);
    struct nk_font_atlas* atlas = nullptr;
    nk_glfw3_font_stash_begin(&device, &atlas);
    struct nk_font* font = nk_font_atlas_add_default(atlas, 18, nullptr);
    nk_glfw3_font_stash_end(&device);
    if (font)
        nk_style_set_font(&device.ctx, &font->handle);

    SettingsState settings;
    settings.load_from(cfg, voices);

    while (!glfwWindowShouldClose(win)) {
        int w = 0, h = 0;
        glfwGetWindowSize(win, &w, &h);

        glfwPollEvents();
        nk_glfw3_new_frame(&device);
        struct nk_context* ctx = &device.ctx;

        // Base surface filling the window, with the gear in the top-right.
        nk_begin(ctx, "Krastor", nk_rect(0, 0, (float)w, (float)h), NK_WINDOW_BACKGROUND);
        {
            nk_layout_row_begin(ctx, NK_DYNAMIC, 48, 3);
            nk_layout_row_push(ctx, 0.82f);
            nk_label(ctx, "Krastor", NK_MIDDLE_LEFT);
            nk_layout_row_push(ctx, 0.08f);
            nk_spacer(ctx);
            nk_layout_row_push(ctx, 0.10f);
            const struct nk_rect gear_rect = nk_widget_bounds(ctx);
            if (nk_button_label(ctx, " "))
                settings.open = !settings.open;
            draw_gear(nk_window_get_canvas(ctx), gear_rect, nk_rgb(225, 225, 225));
            nk_layout_row_end(ctx);

            nk_layout_row_dynamic(ctx, 30, 1);
            nk_label(ctx, "Click the gear (top-right) to open settings.", NK_MIDDLE_CENTER);
        }
        nk_end(ctx);

        settings_modal(ctx, cfg, voices, settings, w, h);

        int fb_w = 0, fb_h = 0;
        glfwGetFramebufferSize(win, &fb_w, &fb_h);
        glViewport(0, 0, fb_w, fb_h);
        glClearColor(0.10f, 0.10f, 0.12f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        nk_glfw3_render(&device, NK_ANTI_ALIASING_ON, 4096, 4096 * 3);

        glfwSwapBuffers(win);
    }

    nk_glfw3_shutdown(&device);
    glfwTerminate();
    return 0;
}

} // namespace krastor
