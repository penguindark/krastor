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
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
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
#include "ansi.h"
#include "clipboard.h"

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

    // Images pasted/attached to the message being composed (sent as vision
    // attachments together with the text).
    struct Attachment {
        int width, height;
        std::vector<unsigned char> png;
    };
    std::vector<Attachment> attachments;
    bool pending_image_paste = false;  // set by the clipboard paste hook

    enum class Worker { None, Thinking, Listening } worker = Worker::None;
    std::future<LlmReply> llm_fut;
    std::future<std::string> stt_fut;
    std::atomic<bool> speaking{false};
    bool auto_speak = true;
    // Multiline chat input (Enter sends, Shift+Enter inserts a newline).
    // Large buffer so pasted code/long prompts are never silently truncated.
    char input[16384] = {0};

    AppState(Config& c, VoiceRegistry& v)
        : cfg(c), voices(v), conv(c.llm.system_prompt), llm(c.llm), tts(v, c), stt(c)
    {
        lines.push_back({"sys", "Krastor ready. Type below (Enter sends, "
                                "Shift+Enter = newline). Ctrl+V pastes text or "
                                "an image. Settings = LLM/voice config."});
        // Screenshot-test hook: seed a styled sample line and attach whatever
        // image (if any) is on the clipboard, so renderers can be verified.
        if (std::getenv("KRASTOR_UITEST")) {
            const std::string E(1, '\x1b');
            lines.push_back({"krastor",
                "ANSI: " + E + "[31mRED" + E + "[0m " + E + "[32mGREEN" + E + "[0m " +
                E + "[94mBRIGHT-BLUE" + E + "[0m " + E + "[1mBOLD" + E + "[0m bg:" +
                E + "[41m X " + E + "[0m diff:" + E + "[48;2;60;16;16m+added" + E +
                "[0m " + E + "[48;2;16;50;24m+more" + E + "[0m box:" +
                E + "[95m+--|" + E + "[0m"});
            lines.push_back({"krastor",
                E + "[48;2;20;60;30m+  full-width green diff line" + E + "[0m"});
            lines.push_back({"krastor",
                E + "[48;2;80;20;20m-  full-width red diff line" + E + "[0m"});
            attach_clipboard_image();
        }
    }

    // Grabs a clipboard bitmap (if any) and queues it as an attachment.
    // Returns true when an image was attached.
    bool attach_clipboard_image()
    {
        PastedImage img = clipboard_get_image();
        if (!img.ok())
            return false;
        attachments.push_back({img.width, img.height, std::move(img.png)});
        return true;
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
        if ((text.empty() && attachments.empty()) || worker != Worker::None)
            return;
        std::vector<std::string> b64;
        b64.reserve(attachments.size());
        for (const auto& a : attachments)
            b64.push_back(base64_encode(a.png.data(), a.png.size()));
        std::string shown = text;
        if (!b64.empty()) {
            if (!shown.empty())
                shown += ' ';
            shown += "[+" + std::to_string(b64.size()) +
                     (b64.size() == 1 ? " image]" : " images]");
        }
        lines.push_back({"you", shown});
        conv.add_user(text, std::move(b64));
        attachments.clear();
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
// Pixel width of a UTF-8 string in the given font (nuklear exposes this only
// through the font callback; there is no public nk_text_width in 4.13.3).
float text_w_px(const struct nk_user_font* f, const char* s, int n)
{
    return f && f->width ? f->width(f->userdata, f->height, s, n) : 0.0f;
}

// One layout token (a word or a run of spaces) carrying its ANSI style.
// Newlines are represented by a token with force_break = true.
struct Word {
    std::string text;
    struct nk_color fg;
    struct nk_color bg;   // a == 0 -> no background
    bool force_break;
};

// Splits styled runs into space/word/newline tokens, preserving indentation
// (runs of spaces are kept as their own tokens so code blocks stay aligned).
std::vector<Word> tokenize_runs(const std::vector<AnsiRun>& runs)
{
    std::vector<Word> toks;
    for (const auto& r : runs) {
        struct nk_color fg = nk_rgba(r.r, r.g, r.b, 255);
        if (r.bold) {  // no bold font face: brighten instead
            fg.r = static_cast<nk_byte>(fg.r > 195 ? 255 : fg.r + 60);
            fg.g = static_cast<nk_byte>(fg.g > 195 ? 255 : fg.g + 60);
            fg.b = static_cast<nk_byte>(fg.b > 195 ? 255 : fg.b + 60);
        }
        const struct nk_color bg =
            r.has_bg ? nk_rgba(r.br, r.bg, r.bb, 255) : nk_rgba(0, 0, 0, 0);
        size_t i = 0;
        while (i < r.text.size()) {
            const char ch = r.text[i];
            if (ch == '\n') {
                toks.push_back({"", fg, bg, true});
                ++i;
            } else if (ch == ' ') {
                size_t j = i;
                while (j < r.text.size() && r.text[j] == ' ')
                    ++j;
                toks.push_back({r.text.substr(i, j - i), fg, bg, false});
                i = j;
            } else {
                size_t j = i;
                while (j < r.text.size() && r.text[j] != ' ' && r.text[j] != '\n')
                    ++j;
                toks.push_back({r.text.substr(i, j - i), fg, bg, false});
                i = j;
            }
        }
    }
    return toks;
}

// Splits a single word wider than max_w into UTF-8-safe chunks that fit.
std::vector<std::string> split_wide(const std::string& w, float max_w,
                                    const struct nk_user_font* f)
{
    std::vector<std::string> out;
    size_t i = 0;
    while (i < w.size()) {
        size_t j = i;
        float x = 0.0f;
        while (j < w.size()) {
            // advance one UTF-8 codepoint
            size_t k = j + 1;
            while (k < w.size() && (static_cast<unsigned char>(w[k]) & 0xC0) == 0x80)
                ++k;
            const float cw = text_w_px(f, w.data() + j, (int)(k - j));
            if (x + cw > max_w && j > i)
                break;
            x += cw;
            j = k;
        }
        out.push_back(w.substr(i, j - i));
        i = j;
    }
    return out;
}

// Draws one chat message (prefix + ANSI-styled body) word-wrapped at text_w,
// with diff-style full-line backgrounds. Returns the pixel height consumed.
float draw_chat_line(struct nk_context* ctx, const std::string& prefix,
                     const std::string& body, struct nk_color default_color,
                     float text_w)
{
    const struct nk_user_font* f = ctx->style.font;
    const float row_h = f && f->height > 0.0f ? f->height : 18.0f;
    text_w = text_w < 40.0f ? 40.0f : text_w;

    std::vector<AnsiRun> runs;
    runs.push_back({prefix, default_color.r, default_color.g, default_color.b,
                    false, 0, 0, 0, false});
    auto body_runs = ansi_parse(body, default_color.r, default_color.g, default_color.b);
    runs.insert(runs.end(), body_runs.begin(), body_runs.end());

    // Tokenize, expanding overlong words into pre-split chunks.
    std::vector<Word> toks;
    for (auto& w : tokenize_runs(runs)) {
        if (w.force_break || w.text.empty()) {
            toks.push_back(std::move(w));
            continue;
        }
        const float ww = text_w_px(f, w.text.data(), (int)w.text.size());
        if (ww <= text_w) {
            toks.push_back(std::move(w));
            continue;
        }
        bool first = true;
        for (auto& chunk : split_wide(w.text, text_w, f)) {
            Word t{std::move(chunk), w.fg, w.bg, false};
            if (!first) t.force_break = true;  // chunk continues on next line
            toks.push_back(std::move(t));
            first = false;
        }
    }

    // Greedy word wrap into visual lines.
    std::vector<std::vector<Word>> lines;
    lines.emplace_back();
    float x = 0.0f;
    for (auto& w : toks) {
        if (w.force_break) {
            lines.emplace_back();
            x = 0.0f;
            continue;
        }
        const float ww = text_w_px(f, w.text.data(), (int)w.text.size());
        if (!lines.back().empty() && x + ww > text_w) {
            lines.emplace_back();
            x = 0.0f;
            if (w.text == std::string(w.text.size(), ' '))
                continue;  // drop spaces at a wrap point
        }
        x += ww;
        lines.back().push_back(std::move(w));
    }
    if (lines.size() == 1 && lines[0].empty())
        lines[0].push_back({" ", default_color, nk_rgba(0, 0, 0, 0), false});

    // Reserve the space, then draw tokens manually at absolute positions so
    // each word keeps its own color (nk_label cannot do styled runs).
    const float total_h = static_cast<float>(lines.size()) * row_h + 6.0f;
    nk_layout_row_dynamic(ctx, total_h, 1);
    const struct nk_rect area = nk_widget_bounds(ctx);
    struct nk_command_buffer* canvas = nk_window_get_canvas(ctx);
    float y = area.y;
    for (const auto& ln : lines) {
        // Diff-style full-line background only when every colored token on the
        // visual line shares one background color; otherwise paint per-token
        // backgrounds so inline highlights (bg: X) do not bleed across the line.
        struct nk_color bg = nk_rgba(0, 0, 0, 0);
        bool uniform = true;
        for (const auto& w : ln) {
            if (!w.bg.a)
                continue;
            if (!bg.a)
                bg = w.bg;
            else if (bg.r != w.bg.r || bg.g != w.bg.g || bg.b != w.bg.b) {
                uniform = false;
                break;
            }
        }
        if (!bg.a)
            uniform = false;
        if (uniform)
            nk_fill_rect(canvas, nk_rect(area.x, y, text_w, row_h), 0, bg);
        float tx = area.x;
        for (const auto& w : ln) {
            const float ww = text_w_px(f, w.text.data(), (int)w.text.size());
            if (!uniform && w.bg.a)
                nk_fill_rect(canvas, nk_rect(tx, y, ww, row_h), 0, w.bg);
            nk_draw_text(canvas, nk_rect(tx, y, ww + 2.0f, row_h),
                         w.text.data(), (int)w.text.size(), f,
                         nk_rgba(0, 0, 0, 0), w.fg);
            tx += ww;
        }
        y += row_h;
    }
    return total_h;
}

void chat_panel(struct nk_context* ctx, AppState& app, float height)
{
    // This Nuklear has no "fill remaining space" row: the group gets an
    // explicitly computed height (see caller). A full-width static item would
    // overflow the padded row and wrap, pushing the group off-screen, so use a
    // dynamic-ratio row (one column at ratio 1.0) with the fixed pixel height.
    nk_layout_row_begin(ctx, NK_DYNAMIC, height, 1);
    nk_layout_row_push(ctx, 1.0f);
    if (nk_group_begin(ctx, "##chat", NK_WINDOW_BORDER)) {
        const float spacing = ctx->style.window.spacing.y;
        // Usable text width inside the group: window width minus group padding,
        // border and the vertical scrollbar.
        const float text_w = nk_window_get_width(ctx) -
                             ctx->style.window.scrollbar_size.x - 20.0f;
        float content_h = 0.0f;
        for (const auto& l : app.lines) {
            const std::string prefix = l.who == "you" ? "> " :
                                       l.who == "krastor" ? "Krastor: " : "! ";
            struct nk_color col = nk_rgb(160, 160, 160);
            if (l.who == "you") col = nk_rgb(120, 180, 255);
            else if (l.who == "krastor") col = nk_rgb(130, 220, 150);
            content_h += draw_chat_line(ctx, prefix, l.text, col, text_w) + spacing;
        }
        // Pin the view to the newest line. The group keeps its own persistent
        // scroll offset (nk_window_set_scroll would scroll the outer window).
        // Compute the bottom offset explicitly instead of relying on Nuklear's
        // clamp, which stops running once the scrollbar auto-hides.
        const float visible = height - 2.0f * ctx->style.window.padding.y - 8.0f;
        const float bottom = content_h > visible ? content_h - visible : 0.0f;
        nk_group_set_scroll(ctx, "##chat", 0, (nk_uint)bottom);
        nk_group_end(ctx);
        nk_layout_row_end(ctx);
    }
}

} // namespace

// --------------------------------------------------------- clipboard paste
// Replaces the glfw backend's paste plugin: text pastes normally, but when the
// clipboard holds no text we check for a bitmap and queue it as an image
// attachment (Ctrl+V in the input field pastes text OR attaches an image).
static AppState* g_paste_app = nullptr;

// ------------------------------------------------------------- drag & drop
// Files dropped on the window are queued by the GLFW drop callback (runs on the
// event-loop thread inside glfwPollEvents) and consumed once per frame.
static std::mutex g_drop_mtx;
static std::vector<std::string> g_dropped;

static void drop_callback(GLFWwindow*, int count, const char** paths)
{
    std::lock_guard<std::mutex> lk(g_drop_mtx);
    for (int i = 0; i < count; ++i)
        g_dropped.emplace_back(paths[i]);
}

static bool has_ext_ci(const std::string& s, std::initializer_list<const char*> exts)
{
    size_t dot = s.find_last_of('.');
    if (dot == std::string::npos)
        return false;
    std::string ext = s.substr(dot);
    for (auto& c : ext)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (const char* e : exts)
        if (ext == e)
            return true;
    return false;
}

static void krastor_paste(nk_handle usr, struct nk_text_edit* edit)
{
    struct nk_glfw* glfw = (struct nk_glfw*)usr.ptr;
    const char* text = glfwGetClipboardString(glfw->win);
    if (text && *text) {
        nk_textedit_paste(edit, text, nk_strlen(text));
        return;
    }
    if (g_paste_app)
        g_paste_app->pending_image_paste = true;
}

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
    // Monospace font with box-drawing/extended glyphs for terminal-style output;
    // fall back to the built-in font when the file is missing.
    struct nk_font* font = nullptr;
    {
        std::ifstream f("data/fonts/jetbrains-mono.ttf", std::ios::binary);
        if (f) {
            std::vector<char> buf((std::istreambuf_iterator<char>(f)),
                                  std::istreambuf_iterator<char>());
            if (!buf.empty())
                font = nk_font_atlas_add_from_memory(atlas, buf.data(), buf.size(),
                                                     18, nullptr);
        }
    }
    if (!font)
        font = nk_font_atlas_add_default(atlas, 18, nullptr);
    nk_glfw3_font_stash_end(&device);
    if (font)
        nk_style_set_font(&device.ctx, &font->handle);
    // Semi-transparent panels so the shader background shows through.
    // NOTE: the window body is painted from style.window.fixed_background;
    // style.window.background only colors dynamic panels (groups, popups).
    device.ctx.style.window.fixed_background = nk_style_item_color(nk_rgba(16, 18, 24, 200));
    device.ctx.style.window.background = nk_rgba(16, 18, 24, 200);

    AppState app(cfg, voices);
    app.settings.load_from(cfg, voices);
    app.tts.init();
    // Override the backend paste plugin so Ctrl+V can also grab clipboard images.
    g_paste_app = &app;
    device.ctx.clip.paste = krastor_paste;
    glfwSetDropCallback(win, drop_callback);

    while (!glfwWindowShouldClose(win)) {
        int w = 0, h = 0;
        glfwGetWindowSize(win, &w, &h);

        glfwPollEvents();
        app.poll();
        if (app.settings.dirty) {
            app.llm = LlmClient(cfg.llm);
            app.settings.dirty = false;
        }

        // Consume files dropped on the window: images become attachments,
        // text files are appended to the input field, anything else is refused.
        {
            std::vector<std::string> dropped;
            {
                std::lock_guard<std::mutex> lk(g_drop_mtx);
                dropped.swap(g_dropped);
            }
            for (const auto& path : dropped) {
                if (has_ext_ci(path, {".png", ".jpg", ".jpeg", ".bmp", ".gif", ".tga"})) {
                    PastedImage img = load_image_file(path);
                    if (img.ok()) {
                        app.attachments.push_back(
                            {img.width, img.height, std::move(img.png)});
                        app.lines.push_back({"sys", "attached image: " + path});
                    } else {
                        app.lines.push_back({"sys", "cannot read image: " + path});
                    }
                } else if (has_ext_ci(path, {".txt", ".md", ".log", ".json", ".jsonc",
                                             ".xml", ".html", ".css", ".js", ".ts",
                                             ".c", ".h", ".cpp", ".hpp", ".py",
                                             ".ps1", ".sh", ".yml", ".yaml", ".ini"})) {
                    std::ifstream f(path, std::ios::binary);
                    std::string text((std::istreambuf_iterator<char>(f)),
                                     std::istreambuf_iterator<char>());
                    const size_t room = sizeof(app.input) - 1;
                    if (text.size() > room)
                        text.resize(room);  // keep within the input buffer
                    std::string add = (app.input[0] ? "\n" : "") + text;
                    if (std::strlen(app.input) + add.size() < room)
                        std::strncat(app.input, add.c_str(), room - std::strlen(app.input));
                    app.lines.push_back({"sys", "inserted file text: " + path});
                } else {
                    app.lines.push_back({"sys", "unsupported file type: " + path});
                }
            }
        }

        nk_glfw3_new_frame(&device);
        struct nk_context* ctx = &device.ctx;

        // ---- main surface (transparent: the shader shows through) ----
        nk_begin(ctx, "Krastor", nk_rect(0, 0, (float)w, (float)h), 0);
        {
            // Top bar: title | speak toggle | settings
            nk_layout_row_begin(ctx, NK_DYNAMIC, 44, 3);
            nk_layout_row_push(ctx, 0.62f);
            nk_label(ctx, "Krastor", NK_MIDDLE_LEFT);
            nk_layout_row_push(ctx, 0.18f);
            if (nk_button_label(ctx, app.auto_speak ? "Speak: ON" : "Speak: OFF"))
                app.auto_speak = !app.auto_speak;
            nk_layout_row_push(ctx, 0.20f);
            if (nk_button_label(ctx, "Settings"))
                app.settings.open = !app.settings.open;
            nk_layout_row_end(ctx);

            // Chat history: this Nuklear has no "fill remaining" row, so the
            // group height is the window height minus the other fixed rows
            // (top bar 44 + input 66 + status 22 + attachment row) plus
            // row spacing/padding.
            const float attach_h = app.attachments.empty() ? 0.0f : 30.0f;
            const float chat_h = (float)h - 44.0f - 66.0f - 22.0f - attach_h - 34.0f;
            chat_panel(ctx, app, chat_h > 60.0f ? chat_h : 60.0f);

            // Attachment chips: one per pasted image, click a chip to remove it.
            if (!app.attachments.empty()) {
                nk_layout_row_begin(ctx, NK_STATIC, 26, (int)app.attachments.size());
                size_t remove_idx = SIZE_MAX;
                for (size_t i = 0; i < app.attachments.size(); ++i) {
                    char chip[64];
                    std::snprintf(chip, sizeof(chip), "[x] %dx%d",
                                  app.attachments[i].width, app.attachments[i].height);
                    nk_layout_row_push(ctx, 118.0f);
                    if (nk_button_label(ctx, chip))
                        remove_idx = i;
                }
                nk_layout_row_end(ctx);
                if (remove_idx != SIZE_MAX)
                    app.attachments.erase(app.attachments.begin() + (long)remove_idx);
            }

            // Input row: multiline box (Enter sends, Shift+Enter newline).
            nk_layout_row_begin(ctx, NK_DYNAMIC, 66, 5);
            nk_layout_row_push(ctx, 0.44f);
            const nk_flags edit_state = nk_edit_string_zero_terminated(
                ctx,
                (nk_flags)NK_EDIT_BOX | (nk_flags)NK_EDIT_SIG_ENTER |
                    (nk_flags)NK_EDIT_CTRL_ENTER_NEWLINE,
                app.input, sizeof(app.input) - 1, nk_filter_default);
            // A clipboard image was "pasted" while this field was focused.
            if (app.pending_image_paste) {
                app.pending_image_paste = false;
                app.attach_clipboard_image();
            }
            nk_layout_row_push(ctx, 0.12f);
            const bool send_click =
                nk_button_label(ctx, app.worker == AppState::Worker::None ? "Send" : "...");
            nk_layout_row_push(ctx, 0.08f);
            const bool img_click = nk_button_label(ctx, "Img");
            nk_layout_row_push(ctx, 0.18f);
            const bool listen_click =
                nk_button_label(ctx, app.worker == AppState::Worker::Listening
                                        ? "Listening..." : "Listen");
            nk_layout_row_push(ctx, 0.18f);
            if (nk_button_label(ctx, app.speaking.load() ? "Speaking..." : "Test voice"))
                app.speak_async("Ciao, la voce funziona.");
            nk_layout_row_end(ctx);

            if (img_click)
                app.attach_clipboard_image();

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
        nk_glfw3_render(&device, NK_ANTI_ALIASING_ON, 512 * 1024, 128 * 1024);

        glfwSwapBuffers(win);
    }

    nk_glfw3_shutdown(&device);
    glfwTerminate();
    return 0;
}

} // namespace krastor
