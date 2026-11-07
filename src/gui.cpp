// Krastor — GUI implementation (GLFW + OpenGL3 + Nuklear).
// The window is the agent's main interface: a terminal-style chat panel, a text
// input, a floating icon-button cluster in the lower-right corner (send,
// attach image, Listen via Parakeet STT, test voice, stop - each with a
// tooltip), an auto-speak toggle (Piper TTS)
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
#include <condition_variable>
#include <cstdlib>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <future>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "config.h"
#include "conversation.h"
#include "gui.h"
#include "llm_client.h"
#include "llm_models.h"
#include "formatters.h"
#include "highlight.h"
#include "emoji.h"
#include "icons.h"
#include "stt.h"
#include "tts.h"
#include "voices.h"
#include "ansi.h"
#include "clipboard.h"
#include "effects.h"
#include "audio_analysis.h"
#include "audio_fx.h"
#include <algorithm>
#include "permissions.h"
#include "media.h"
#include "tool_registry.h"
#include "tool_runner.h"
#include "tool_loop.h"
#include "session_store.h"
#include "compactor.h"
#include "stb_image.h"

namespace krastor {

namespace {

// Font zoom ladder (Ctrl +/-): sizes baked into the atlas at startup, since
// Nuklear glyphs are static and there is no dynamic font scaling.
constexpr int kFontMin = 12, kFontMax = 30, kFontStep = 2, kFontDefault = 18;
// Smallest window size accepted from config/restore.
constexpr int kWinMinW = 480, kWinMinH = 360;

// ---------------------------------------------------------------- background
// The fullscreen background is drawn by the pluggable EffectEngine
// (src/effects.h): built-in animated gradient + user .frag plugins from
// data/shaders/, fed with the mic spectrum (src/audio_analysis.h).

// ------------------------------------------------------------- app state
struct ChatLine {
    std::string who;   // "you" | "krastor" | "sys"
    std::string text;
    std::string meta;  // optional dim stats sub-line (assistant replies)
    // Reasoning ("Thought") block shown above the reply, opencode-style:
    // a clickable "+ Thought · 5.4s" header toggles the dim body.
    // Tool calls reuse the same collapsible with a custom label
    // ("⚙ files · read"), so think_label defaults to "Thought".
    std::string thinking;    // raw reasoning text ("" = no block)
    std::string think_meta;  // duration suffix, e.g. "5.4s"
    std::string think_label; // "" = "Thought"
    bool thought_open = false;
    MediaRef media;          // image/sound block shown in the chat flow
    // File edit diff (files edit/write tool results): flat ops from
    // filetool + counts, rendered as a collapsible side-by-side block.
    nlohmann::json diff;     // null = no diff
    std::string diff_path;
    int diff_added = 0, diff_removed = 0;
    bool diff_open = true;
    ChatLine() = default;
    ChatLine(std::string w, std::string t, std::string m = {})
        : who(std::move(w)), text(std::move(t)), meta(std::move(m)) {}
};

// "4.2s" under a minute, "11m 30s" at/above (stats line formatting).
std::string fmt_elapsed(double ms)
{
    char buf[32];
    const double s = ms / 1000.0;
    if (s < 60.0)
        std::snprintf(buf, sizeof(buf), "%.1fs", s);
    else
        std::snprintf(buf, sizeof(buf), "%dm %02ds", static_cast<int>(s / 60.0),
                      static_cast<int>(std::fmod(s, 60.0)));
    return buf;
}

// 131072 -> "131,072" (context usage footer).
std::string fmt_thousands(long long v)
{
    const bool neg = v < 0;
    std::string s = std::to_string(neg ? -v : v);
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (i && (s.size() - i) % 3 == 0)
            out += ',';
        out += s[i];
    }
    return neg ? "-" + out : out;
}

// nk_image referencing a pixel sub-rect of a sprite atlas texture (the SVG
// icon atlas or the emoji atlas). Shared by the FAB cluster and button_icon.
struct nk_image atlas_image(unsigned tex, int tw, int th, int x, int y,
                             int w, int h)
{
    struct nk_image img = nk_image_id(static_cast<int>(tex));
    img.w = static_cast<nk_ushort>(tw);
    img.h = static_cast<nk_ushort>(th);
    img.region[0] = static_cast<nk_ushort>(x);
    img.region[1] = static_cast<nk_ushort>(y);
    img.region[2] = static_cast<nk_ushort>(w);
    img.region[3] = static_cast<nk_ushort>(h);
    return img;
}

// Icon+text button with the SVG atlas -> plain label fallback (same
// degradation chain as the floating action buttons). Nuklear's own
// text+image button centers the text over the FULL width and paints it
// under the icon on narrow buttons, so this draws the button manually:
// background + click via nk_do_button, icon at the left edge, text centered
// in the area right of the icon. The icon is sized from the button height,
// so it scales with the font zoom for free. Uses NK_LIB internals - the same
// access pattern the FAB cluster already relies on.
float text_w_px(const struct nk_user_font* f, const char* s, int n); // fwd
bool button_icon(nk_context* ctx, const IconAtlas& icons, const char* icon,
                 const char* label)
{
    const IconAtlas::Rect* r = icons.ok() ? icons.find(icon) : nullptr;
    if (!r)
        return nk_button_label(ctx, label) != 0;

    struct nk_rect slot;
    nk_panel_alloc_space(&slot, ctx);  // consume the layout slot
    nk_flags state = 0;
    struct nk_rect content;
    const bool clicked = nk_do_button(&state, &ctx->current->buffer, slot,
                                      &ctx->style.button, &ctx->input,
                                      NK_BUTTON_DEFAULT, &content) != 0;

    const struct nk_style_button* st = &ctx->style.button;
    const struct nk_image img =
        atlas_image(icons.texture(), icons.tex_w(), icons.tex_h(), r->x, r->y,
                    r->w, r->h);
    const float ih = slot.h - 2.0f * st->padding.y;
    nk_draw_image(&ctx->current->buffer,
                  nk_rect(slot.x + 2.0f * st->padding.x,
                          slot.y + (slot.h - ih) / 2.0f, ih, ih),
                  &img, nk_white);

    const int len = (int)std::strlen(label);
    const float fh = ctx->style.font->height;
    const float tw = text_w_px(ctx->style.font, label, len);
    const float tx0 = slot.x + 3.0f * st->padding.x + ih;
    const float avail = slot.x + slot.w - st->padding.x - tx0;
    const nk_color txt = (state & NK_WIDGET_STATE_HOVER)   ? st->text_hover
                         : (state & NK_WIDGET_STATE_ACTIVED) ? st->text_active
                                                             : st->text_normal;
    nk_draw_text(&ctx->current->buffer,
                 nk_rect(tx0 + (avail - tw) / 2.0f, slot.y + (slot.h - fh) / 2.0f,
                         tw > 1.0f ? tw : 1.0f, fh),
                 label, len, ctx->style.font, nk_rgba(0, 0, 0, 0), txt);
    return clicked;
}

// Icon-only button filling the current layout slot (top bar): the icon is
// centered and sized from the button height, so the bar stays clean and
// consistent at any font zoom. Falls back to a short text button when the
// SVG atlas is unavailable.
bool button_icon_only(nk_context* ctx, const IconAtlas& icons, const char* icon,
                      const char* fallback_text,
                      bool* hovered = nullptr,
                      struct nk_rect* slot_out = nullptr)
{
    const IconAtlas::Rect* r = icons.ok() ? icons.find(icon) : nullptr;
    if (!r) {
        const bool clicked = nk_button_label(ctx, fallback_text) != 0;
        if (hovered)
            *hovered = (ctx->last_widget_state & NK_WIDGET_STATE_HOVER) != 0;
        return clicked;
    }

    struct nk_rect slot;
    nk_panel_alloc_space(&slot, ctx);  // consume the layout slot
    if (slot_out)
        *slot_out = slot;
    nk_flags state = 0;
    struct nk_rect content;
    const bool clicked = nk_do_button(&state, &ctx->current->buffer, slot,
                                       &ctx->style.button, &ctx->input,
                                       NK_BUTTON_DEFAULT, &content) != 0;
    if (hovered)
        *hovered = (state & NK_WIDGET_STATE_HOVER) != 0;

    const struct nk_style_button* st = &ctx->style.button;
    const struct nk_image img =
        atlas_image(icons.texture(), icons.tex_w(), icons.tex_h(), r->x, r->y,
                    r->w, r->h);
    const float ih = slot.h - 2.0f * st->padding.y;
    const float iw = ih * (float)r->w / (float)(r->h > 0 ? r->h : 1);
    nk_draw_image(&ctx->current->buffer,
                  nk_rect(slot.x + (slot.w - iw) / 2.0f,
                          slot.y + (slot.h - ih) / 2.0f, iw, ih),
                  &img, nk_white);
    return clicked;
}

// Truncate s so it renders within max_w pixels (UTF-8 codepoint safe),
// appending ".." when shortened. Nuklear label buttons do NOT clip their
// text: a long session title used to bleed over the neighbouring columns
// and past the window border.
std::string fit_text(const struct nk_user_font* f, const std::string& s,
                     float max_w)
{
    if (!f || !f->width ||
        text_w_px(f, s.c_str(), (int)s.size()) <= max_w)
        return s;
    std::string out;
    size_t i = 0;
    while (i < s.size()) {
        size_t j = i + 1;  // next UTF-8 codepoint boundary
        while (j < s.size() &&
               (static_cast<unsigned char>(s[j]) & 0xC0) == 0x80)
            ++j;
        const std::string trial = s.substr(0, j) + "..";
        if (text_w_px(f, trial.c_str(), (int)trial.size()) > max_w)
            break;
        out = s.substr(0, j);
        i = j;
    }
    return out + "..";
}

// ---- UTF-8 codepoint helpers (selection maps on codepoints, not bytes, so
// Italian accents and emoji stay one position each) ----
int cp_len(unsigned char c)
{
    return c < 0x80 ? 1 : c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : 2;
}
int cp_count(const std::string& s)
{
    int n = 0;
    for (size_t i = 0; i < s.size(); i += cp_len(static_cast<unsigned char>(s[i])))
        ++n;
    return n;
}
// Byte offset of codepoint `cp` (clamped to the string end).
size_t cp_to_byte(const std::string& s, int cp)
{
    size_t i = 0;
    int n = 0;
    while (i < s.size() && n < cp) {
        i += cp_len(static_cast<unsigned char>(s[i]));
        ++n;
    }
    return i;
}
bool is_word_char(unsigned char c)
{
    return c >= 0x80 || std::isalnum(c) != 0 || c == '_';
}

// ---- Chat selection machinery ----
// The chat is custom-drawn, so selection is implemented on top of the layout
// pass: every frame the wrap in draw_chat_line records a hit-map (pixel rect +
// codepoint range per drawn token) and the mouse/keyboard map into it. The
// virtual document is the ANSI-stripped text of all chat lines joined by '\n';
// prefixes ("> ", "Krastor: ", "! ") and stats sub-lines are not selectable.
struct ChatSeg {
    float x0, x1;      // pixel span of the token
    int line;          // source ChatLine index
    int cp0, cp1;      // codepoint range within that line's plain text
};
struct ChatHitLine {
    float y, h;        // pixel span (window coordinates, scroll applied)
    int line;
    std::vector<ChatSeg> segs;
};
// Resolved selection bounds in (line, cp) space, lo <= hi.
struct SelRange {
    int lo_line, lo_cp, hi_line, hi_cp;
};

struct SettingsState {
    char name[128] = {0};
    // Selected llm_models.json entry ("provider/model"); URL/key/context come
    // from the file, so Settings no longer edits them directly.
    std::string model_ref;
    char system_prompt[4096] = {0};
    float temperature = 0.7f;
    int max_tokens = 1024;
    std::string voice;
    std::string status;
    bool open = false;
    bool dirty = false;   // LLM settings changed -> rebuild the client
    // Background effect plugins (display names; index 0 = built-in gradient).
    std::vector<std::string> effect_names;
    std::string effect = "gradient";
    float effect_intensity = 1.0f;   // brightness knob, debounced auto-save
    double intensity_due = 0.0;      // save timestamp, 0 = nothing pending

    void load_from(const Config& cfg, const VoiceRegistry& voices)
    {
        std::snprintf(name, sizeof(name), "%s", cfg.agent.name.c_str());
        model_ref = cfg.llm.model_ref;
        std::snprintf(system_prompt, sizeof(system_prompt), "%s",
                      cfg.llm.system_prompt.c_str());
        temperature = static_cast<float>(cfg.llm.temperature);
        max_tokens = cfg.llm.max_tokens;
        effect_intensity = static_cast<float>(cfg.gui.effect_intensity);
        const auto* v = voices.current();
        voice = v ? v->id : (voices.voices().empty() ? std::string() : voices.voices()[0].id);
    }
};

struct AppState : AskHandler {
    Config& cfg;
    VoiceRegistry& voices;
    Conversation conv;
    LlmClient llm;
    Tts tts;
    Stt stt;

    std::vector<ChatLine> lines;   // main thread only
    SettingsState settings;
    std::string name = "Krastor";  // agent persona (agent.name), live-editable
    LlmModelRegistry models;       // llm_models.json (Settings model combo)
    OutputFormatter fmt;           // raw reply -> ANSI-styled display text
    Highlighter hl;                // data/languages/*.json code-block packs
    EmojiAtlas emoji;              // color emoji sprite atlas (data/fonts)
    IconAtlas icons;               // SVG UI icon atlas (data/icons, LunaSVG)

    // Images pasted/attached to the message being composed (sent as vision
    // attachments together with the text).
    struct Attachment {
        int width, height;
        std::vector<unsigned char> png;
    };
    std::vector<Attachment> attachments;
    bool pending_image_paste = false;  // set by the clipboard paste hook

    enum class Worker { None, Thinking, Listening, Compacting } worker = Worker::None;
    std::future<LlmReply> llm_fut;
    std::future<CompactResult> compact_fut;  // /compact worker result
    std::shared_ptr<CancelToken> llm_cancel;  // Stop button aborts the request
    std::future<std::string> stt_fut;
    std::atomic<bool> speaking{false};
    // Decaying key-press pulse for the shader event feed (u_typing): set to
    // 1 whenever the input buffer changes, drained every frame.
    float typing_pulse = 0.0f;
    std::string input_prev;  // last frame's input text (typing detector)
    bool auto_speak = false;  // seeded from cfg.tts.auto_speak (off by default)
    int last_prompt_tokens = 0;  // context occupancy = last request's prompt size
    bool follow_bottom = true;   // chat auto-scrolls to newest unless the user
    float chat_bottom = 0.0f;    // last frame's max scroll offset (wheel clamp)
                                 // scrolled back up (wheel/keys clear the flag)

    // ---- auto-compaction (opencode v2 style) ----
    bool overflow_retried = false;     // one compact+retry per turn on overflow
    bool retry_after_compact = false;  // re-run the turn after overflow compact

    // ---- Inline media (see media.h) ----
    SoundPlayer player;  // mini sound player (own output device)
    // Loaded image textures in load order (oldest first) for LRU eviction;
    // blocks are owned by `lines`, these are just observers.
    std::vector<std::weak_ptr<MediaBlock>> tex_lru;

    // ---- Hands-free pipeline (mic -> STT -> chat -> LLM -> TTS) ----
    // The Stt feeder thread pushes closed VAD segments into `utterances`; a
    // pipeline worker recognizes them and pushes the text into `heard`, which
    // the render loop drains each frame and feeds to send(). Reusing send()
    // keeps the single conversation/LLM worker path intact.
    std::mutex ut_mtx;
    std::condition_variable ut_cv;
    std::vector<std::vector<float>> utterances;
    std::thread pipe_thread;
    std::atomic<bool> pipe_run{false};
    std::mutex heard_mtx;
    std::vector<std::string> heard;

    // ---- External tools (see tools_plan.md) ----
    GrantStore tool_grants;      // session-scoped "always allow" answers
    ToolRegistry tool_registry;  // tools/tool_*.jsonc (loaded in the ctor)
    ToolRunner tool_runner;
    // AskHandler: the LLM worker blocks in ask() until the GUI modal answers.
    struct PendingAsk {
        std::string tool, action, detail;
        Decision answer = Decision::Deny;
        bool answered = false;
    };
    std::mutex ask_mtx;
    std::condition_variable ask_cv;
    std::shared_ptr<PendingAsk> pending_ask;
    Decision ask(const std::string& tool, const std::string& action,
                 const std::string& detail) override
    {
        auto pa = std::make_shared<PendingAsk>();
        pa->tool = tool;
        pa->action = action;
        pa->detail = detail;
        {
            std::lock_guard<std::mutex> lk(ask_mtx);
            pending_ask = pa;
        }
        Decision ans;
        {
            std::unique_lock<std::mutex> lk(ask_mtx);
            ask_cv.wait(lk, [&] { return pa->answered; });
            ans = pa->answer;
            // Dismiss the modal: without this the dialog stays on screen
            // (and keeps blocking the chat) even after the worker resumed.
            if (pending_ask == pa)
                pending_ask.reset();
        }
        ask_cv.notify_all();
        return ans;
    }
    // Completed tool calls are queued by the worker and drained into chat
    // lines by poll() (main thread only touches `lines`).
    struct ToolEvent {
        std::string label, meta, body;
        // filetool edit/write results carry a structured diff: render it as
        // a side-by-side block instead of a plain collapsible (opencode-style).
        nlohmann::json diff;  // null = no diff
        std::string path;
        int added = 0, removed = 0;
    };
    std::mutex tev_mtx;
    std::vector<ToolEvent> tev_ready;

    // ---- Sessions (see sessions_plan.md) ----
    SessionStore store;
    std::string session_id, session_title;
    size_t saved_count = 0;      // messages persisted so far (autosave cursor)
    bool sessions_open = false;  // session manager modal
    bool help_open = false;      // help overlay (F1 or /help)
    int delete_confirm = -1;     // list row awaiting a delete confirmation
    char rename_buf[128] = {0};  // rename field in the session modal

    // Multiline chat input (Enter sends, Shift+Enter inserts a newline).
    // Large buffer so pasted code/long prompts are never silently truncated.
    char input[16384] = {0};

    // ---- Chat text selection ----
    // Virtual document: ANSI-stripped text of every chat line joined by '\n'.
    // Selection is stored as codepoint offsets into that document; the hit-map
    // (chat_hits) is rebuilt by chat_panel every frame.
    std::vector<std::string> doc_lines;
    std::vector<int> doc_start;   // codepoint offset where each line begins
    int doc_len = 0;
    std::vector<ChatHitLine> chat_hits;
    int sel_anchor = -1, sel_caret = -1;  // -1 = no selection
    bool chat_focused = false;    // click in the chat routes selection keys
    bool sel_dragging = false, sel_moved = false;
    double last_click_t = 0.0;    // for double/triple click detection
    int last_click_x = 0, last_click_y = 0, click_count = 0;
    int scroll_cmd = 0;  // 1=PageUp 2=PageDown 3=Home 4=End, consumed by chat_panel
    struct nk_rect fab_rect = {0, 0, 0, 0};  // floating action button strip
    // (clicks here must not start a chat selection)
    bool clear_edit_focus = false;  // chat click: drop the input box's edit focus
    bool uitest_sel_done = false;  // one-shot selection seed for screenshot tests
    // Clickable "Thought" header rects recorded during the last frame:
    // (ChatLine index, header rect in window coords). Clicking one toggles.
    std::vector<std::pair<int, struct nk_rect>> thought_rects;
    // Same for the "← Edit path" diff headers (click toggles the diff).
    std::vector<std::pair<int, struct nk_rect>> diff_rects;
    // Image blocks drawn last frame: (ChatLine index, image rect). A click
    // inside copies the image to the clipboard (like text selection copies).
    std::vector<std::pair<int, struct nk_rect>> image_rects;

    AppState(Config& c, VoiceRegistry& v)
        : cfg(c), voices(v), conv(c.llm.system_prompt), llm(c.llm), tts(v, c), stt(c),
          fmt(c), tool_runner(tool_grants, nlohmann::json{}),
          store(SessionStore::default_dir(c.sessions.dir))
    {
        name = c.agent.name;
        auto_speak = c.tts.auto_speak;
        // Model list for the Settings combo (URL/key/context come from this file).
        models.load(LlmModelRegistry::default_path());
        // External tools: manifests load only when the system is enabled and
        // the selected model declares capabilities.tools (function calling).
        if (c.tools.enabled && c.llm.supports_tools) {
            nlohmann::json ov = nlohmann::json::parse(
                c.tools.overrides_json.empty() ? "{}" : c.tools.overrides_json,
                nullptr, false);
            std::vector<std::string> tlog;
            tool_registry.load_dir(tools_dir(),
                                   ov.is_object() ? ov : nlohmann::json(), tlog);
            for (auto& l : tlog)
                lines.push_back({"sys", l});
            // Guard against double-append: the GUI persists the prompt into
            // config.json on every save, so a saved config already carries it.
            if (!tool_registry.empty() &&
                c.llm.system_prompt.find("provided tools to inspect") ==
                    std::string::npos)
                c.llm.system_prompt += tool_usage_prompt();
        }
        // Media usage (always on): the model shows images/sounds with the
        // chat tokens. conv was built from the plain prompt in the member
        // init list, so push the appended text into it too. Guard against
        // double-append when a saved config already contains it.
        if (c.llm.system_prompt.find("[image: ") == std::string::npos)
            c.llm.system_prompt += kMediaUsagePrompt;
        // ANSI color guidance (the chat renderer speaks SGR), same guard.
        if (c.llm.system_prompt.find("renders ANSI SGR colors") ==
                std::string::npos)
            c.llm.system_prompt += kAnsiUsagePrompt;
        conv.set_system_prompt(c.llm.system_prompt);
        // Syntax highlighting for fenced code blocks (data/languages/*.json).
        if (hl.load_default())
            fmt.set_highlighter([this](const std::string& code,
                                       const std::string& lang) {
                return hl.highlight(code, lang);
            });
        // Color emoji sprite atlas (generated by tools/gen_emoji_atlas.py).
        emoji.load_default();
        // SVG UI icons (data/icons/*.svg via LunaSVG, see src/icons.h).
        icons.load_default();
        lines.push_back({"sys", name + " ready. Type below (Enter sends, "
                                  "Shift+Enter = newline). Ctrl+V pastes text or "
                                  "an image. Settings = LLM/voice config."});
        // ---- session restore (sessions_plan.md): continue the most recent
        // session or start a fresh one; a restored history rebuilds the chat.
        {
            const auto l = store.list();
            if (!l.empty() && c.sessions.continue_last &&
                store.load(l.front().id, conv)) {
                session_id = l.front().id;
                session_title = l.front().title;
            }
            if (session_id.empty()) {
                session_id = store.create("New session", c.llm.model_ref);
                session_title = "New session";
            }
            saved_count = conv.messages().size();
            if (saved_count > 1)
                rebuild_lines_from_conv();
            else
                lines.push_back({"sys", "session: " + session_title});
        }
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
            // ANSI art sample: raw SGR colors must render as colors while
            // the markdown around them survives (line-level passthrough).
            // The last line has NO ESC bytes (as models often send): the
            // repair pass must turn it into a navy banner with white text.
            lines.push_back({"krastor", fmt.format(
                "### ANSI art check\n"
                "\x1b[48;2;255;215;0m        \x1b[48;2;230;180;0m        "
                "\x1b[48;2;200;140;0m        \x1b[0m amber blocks\n"
                "- markdown list item **still styled**\n"
                "[48;5;26m[38;5;231m[1m repaired banner [0m[48;5;26m no ESC [0m")});
            // Sample file-edit diff block (side-by-side renderer test).
            {
                ChatLine dl{"sys", "", ""};
                dl.diff_path = "C:\\dev\\demo\\app.cpp";
                dl.diff_added = 2;
                dl.diff_removed = 1;
                dl.diff = nlohmann::json::parse(
                    "{\"ops\":["
                    "{\"op\":\" \",\"old\":10,\"new\":10,\"text\":\"int main() {\"},"
                    "{\"op\":\"-\",\"old\":11,\"new\":0,\"text\":\"  return 0;\"},"
                    "{\"op\":\"+\",\"old\":0,\"new\":11,\"text\":\"  int x = compute(42);\"},"
                    "{\"op\":\"+\",\"old\":0,\"new\":12,\"text\":\"  return x;\"},"
                    "{\"op\":\" \",\"old\":12,\"new\":13,\"text\":\"}\"},"
                    "{\"op\":\" \",\"old\":30,\"new\":31,\"text\":\"// next hunk\"}"
                    "],\"added\":2,\"removed\":1,\"truncated\":false}");
                lines.push_back(std::move(dl));
            }
            // Filler so the panel is scrollable (wheel/keyboard scroll tests).
            for (int i = 1; i <= 40; ++i) {
                char buf[80];
                std::snprintf(buf, sizeof(buf),
                              "filler line %d - the quick brown fox jumps over "
                              "the lazy dog", i);
                lines.push_back({"sys", buf});
            }
            // Markdown formatter sample, placed
            // after the filler so it is visible in a bottom-pinned screenshot.
            lines.push_back({"krastor", fmt.format(
                "# Heading one\n"
                "Some **bold**, *italic* and `inline code` text.\n"
                "- first item\n"
                "- second **item**\n"
                "  - nested\n"
                "1. ordered\n"
                "> quoted text\n"
                "---\n"
                "| Hash | Descrizione | Timestamp |\n"
                "|---|---|:---:|\n"
                "| `bfd3305` | Print loaded config path at startup | 2026-10-25 09:16 |\n"
                "| `12ec6e3` | Code-block aesthetics + settings spacing | 2026-10-25 09:10 |")});
            // Wide table sample (long third column, inline code + em dash):
            // regression check for bar/row width alignment in the renderer.
            lines.push_back({"krastor", fmt.format(
                "| File | Dimensione | Cosa contiene |\n"
                "|---|---|---|\n"
                "| `cube_100x40.txt` | 9.005 B | ASCII puro, 40 righe × 100 caratteri, tre shade di block |\n"
                "| `cube_100x40_giallo.ansi.txt` | 38.567 B | ANSI, un codice colore per ogni carattere (ridondante) |\n"
                "| `cube_100x40_giallo.runs.ansi.txt` | 9.878 B | ANSI ottimizzato a \"run\" — questo è quello buono per il terminale |\n"
                "| `cube_100x40_giallo.svg` | 4.976 B | Render immagine (palette ambra) |\n"
                "| `cube_100x40_ansi_render.svg` | 4.976 B | Render immagine con i colori esatti dei codici ANSI |")});
            // Code-block sample with an interior blank line (background
            // continuity regression for append_block_bg).
            lines.push_back({"krastor", fmt.format(
                "```python\ndef count_primes(count):\n    primes = []\n    num = 2\n    while len(primes) < count:\n        # Controllo se il numero e primo\n        is_prime = True\n        for p in primes:\n            if p * p > num:\n                break\n        if is_prime:\n            primes.append(num)\n        num += 1\n    return primes\n\nprimi100 = count_primes(100)\nprint(\"I primi 100 numeri sono:\")\nfor i, p in enumerate(primi100, 1):\n    print(f\"{i}: {p}\")\n```")});
            // Collapsible Thought block sample.
            {
                ChatLine tl{"krastor", fmt.format("Answer after **thinking**."), ""};
                tl.thinking = "Let me consider the options...\n"
                              "Weighing trade-offs: step 1, step 2.";
                tl.think_meta = "5.4s";
                tl.thought_open = true;
                lines.push_back(std::move(tl));
            }
            // Color emoji sample.
            lines.push_back({"krastor",
                "Emoji: 😀🚀✅❤️🔥🐛✨🎉👍🤔 color test!"});
            // Auto-send one short message so the response stats line and the
            // context footer are visible in a screenshot without typing.
            auto_speak = false;  // keep the screenshot test silent
            send("Say hello in one short sentence.");
            // Attach the clipboard image (if any) AFTER the auto-send: send()
            // consumes and clears the attachments, so attaching before it would
            // leave no chip to screenshot.
            attach_clipboard_image();
            // Open the Settings modal for icon/modal screenshot tests.
            if (std::getenv("KRASTOR_UITEST_SETTINGS"))
                settings.open = true;
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
            try {
                tts.speak(text);
            } catch (const std::exception& e) {
                std::fprintf(stderr, "tts failed: %s\n", e.what());
            }
            speaking.store(false);  // always released, even on error
        }).detach();
    }

    void send(const std::string& text)
    {
        // Slash commands never reach the LLM (sessions_plan.md §4).
        if (!text.empty() && text[0] == '/') {
            handle_command(text);
            return;
        }
        if ((text.empty() && attachments.empty()) || worker != Worker::None)
            return;
        std::vector<std::string> b64;
        b64.reserve(attachments.size());
        for (const auto& a : attachments)
            b64.push_back(base64_encode(a.png.data(), a.png.size()));
        std::string shown = text;
        std::string model_text = text;
        if (!b64.empty()) {
            if (!shown.empty())
                shown += ' ';
            shown += "[+" + std::to_string(b64.size()) +
                     (b64.size() == 1 ? " image]" : " images]");
            // Tell the model where the images WILL be saved (autosave writes
            // them to the session's attachments/ at the end of the turn,
            // same numbering as attachment_paths): without this the agent
            // cannot re-find a pasted image later and claims it is gone.
            if (cfg.sessions.autosave && !session_id.empty()) {
                const auto paths =
                    store.attachment_paths(session_id, (int)b64.size());
                if (!paths.empty()) {
                    model_text += "\n[attached files saved at: ";
                    for (size_t k = 0; k < paths.size(); ++k) {
                        if (k)
                            model_text += ", ";
                        model_text += paths[k];
                    }
                    model_text += "]";
                }
            }
        }
        lines.push_back({"you", shown});
        conv.add_user(model_text, std::move(b64));
        attachments.clear();
        overflow_retried = false;  // fresh turn: the retry budget resets
        launch_llm();
    }

    // Starts the LLM worker on the CURRENT conversation (used by send() and
    // by the overflow-retry path after an automatic compaction).
    void launch_llm()
    {
        worker = Worker::Thinking;
        const LlmClient client(llm);          // snapshot: survives settings changes
        llm_cancel = std::make_shared<CancelToken>();
        auto cancel = llm_cancel;
        llm_fut = std::async(std::launch::async, [this, client, cancel] {
            const auto t_start = std::chrono::steady_clock::now();
            // Tool calls become collapsible chat blocks (⚙ label, args+output
            // body); they are queued here and drained into `lines` by poll().
            ToolLoopHooks hooks;
            hooks.on_done = [this](const ToolCallInfo& info,
                                   const ToolResult& tr) {
                ToolEvent ev;
                ev.label = "\xE2\x9A\x99 " + info.tool + " \xC2\xB7 " + info.action;
                if (tr.ok) {
                    char m[48];
                    std::snprintf(m, sizeof(m), "%s - %zu B",
                                  fmt_elapsed(static_cast<double>(tr.ms)).c_str(),
                                  tr.result.size());
                    ev.meta = m;
                } else {
                    ev.meta = tr.error.size() > 60
                                  ? tr.error.substr(0, 60) + "..."
                                  : tr.error;
                }
                ev.body = "args: " + info.args_json + "\n" +
                          (tr.ok ? "result: " + tr.result
                                 : "error: " + tr.error);
                if (!tr.stderr_text.empty())
                    ev.body += "\nlog: " + tr.stderr_text;
                // edit/write results: pull out the structured diff for the
                // side-by-side renderer (parse never throws: discarded on
                // malformed/truncated JSON, which just keeps the text block).
                if (tr.ok) {
                    const auto j = nlohmann::json::parse(tr.result, nullptr, false);
                    if (!j.is_discarded() && j.is_object() &&
                        j.contains("diff") && j["diff"].is_object() &&
                        j["diff"].contains("ops")) {
                        ev.diff = j["diff"];
                        ev.path = j.value("path", std::string());
                        ev.added = j["diff"].value("added", 0);
                        ev.removed = j["diff"].value("removed", 0);
                    }
                }
                std::lock_guard<std::mutex> lk(tev_mtx);
                tev_ready.push_back(std::move(ev));
            };
            // The worker owns `conv` while worker != None (send() is guarded),
            // so the loop can append assistant/tool messages directly.
            LlmReply r = agent_complete(client, conv, tool_registry,
                                        tool_runner, this, cancel.get(), hooks);
            r.elapsed_ms = std::chrono::duration<double, std::milli>(
                               std::chrono::steady_clock::now() - t_start).count();
            return r;
        });
    }

    // Stop button: abort the in-flight LLM request immediately and cut any
    // TTS playback. Safe to press at any time (no-ops when idle).
    void stop()
    {
        if ((worker == Worker::Thinking || worker == Worker::Compacting) &&
            llm_cancel) {
            llm_cancel->cancel();  // makes the blocking HTTP call return now
            lines.push_back({"sys", "(stopped)"});
        }
        tts.interrupt();  // no-op when nothing is playing
        player.stop();    // mini sound player too
    }

    // ------------------------------------------------------------- sessions
    // Persist everything appended since the last save (see sessions_plan.md);
    // auto-titles an untitled session from its first user message.
    void autosave_turn()
    {
        if (!cfg.sessions.autosave || session_id.empty())
            return;
        const auto fresh = conv.messages_since(saved_count);
        if (fresh.empty())
            return;
        // A JSON error here (invalid UTF-8 sneaking into the history) must
        // never take the GUI down: the cursor simply does not advance, so
        // the turn is retried on the next save.
        try {
            store.append(session_id, fresh);
        } catch (const std::exception& e) {
            lines.push_back({"sys", std::string("autosave failed: ") + e.what()});
            return;
        }
        saved_count = conv.messages().size();
        SessionMeta sm;
        if (store.meta_of(session_id, sm) && sm.title == "New session") {
            for (const auto& m : conv.messages())
                if (m.role == "user" && !m.content.empty()) {
                    session_title = SessionStore::title_from_text(m.content);
                    store.rename(session_id, session_title);
                    break;
                }
        }
    }

    // Rebuild the visible chat from the loaded conversation: user/assistant
    // turns, tool calls as collapsed blocks (like live ones), summaries as a
    // divider. Main thread only.
    void rebuild_lines_from_conv()
    {
        lines.clear();
        sel_anchor = sel_caret = -1;
        lines.push_back({"sys", "session: " + session_title});
        const auto& msgs = conv.messages();
        for (size_t i = 1; i < msgs.size(); ++i) {
            const auto& m = msgs[i];
            if (is_summary_message(m)) {
                lines.push_back({"sys", "---- compacted summary ----"});
                continue;
            }
            if (m.role == "user") {
                std::string t = m.content;
                if (!m.images.empty()) {
                    if (!t.empty())
                        t += " ";
                    t += "[+" + std::to_string(m.images.size()) +
                         (m.images.size() == 1 ? " image]" : " images]");
                }
                lines.push_back({"you", t});
            } else if (m.role == "assistant") {
                for (const auto& tc : m.tool_calls) {
                    ChatLine tl{"sys", "", ""};
                    std::string action = tc.name;
                    auto aj = nlohmann::json::parse(tc.arguments, nullptr, false);
                    if (aj.is_object() && aj.contains("action") &&
                        aj["action"].is_string())
                        action = aj["action"].get<std::string>();
                    tl.think_label = "\xE2\x9A\x99 " + tc.name + " \xC2\xB7 " + action;
                    tl.thinking = "args: " + tc.arguments;
                    lines.push_back(std::move(tl));
                }
                if (!m.content.empty()) {
                    // Same media-token split as live replies: restored
                    // sessions re-render their image/sound blocks (base64
                    // tokens are self-contained; file paths need the file).
                    for (auto& s : split_media_tokens(m.content)) {
                        if (s.media) {
                            ChatLine ml{"krastor", ""};
                            ml.media = std::move(s.media);
                            lines.push_back(std::move(ml));
                        } else {
                            lines.push_back({"krastor", fmt.format(s.text)});
                        }
                    }
                }
            } else if (m.role == "tool") {
                // edit/write results carry a structured diff: re-render it
                // as a diff block. The stored content may be truncated by
                // cap_text, so parse must not throw; on failure the result
                // simply folds into the tool block below as plain text.
                const auto j = nlohmann::json::parse(m.content, nullptr, false);
                const nlohmann::json* res = nullptr;
                if (!j.is_discarded() && j.is_object()) {
                    if (j.contains("diff") && j["diff"].is_object())
                        res = &j;
                    else if (j.contains("result") && j["result"].is_object() &&
                             j["result"].contains("diff"))
                        res = &j["result"];
                }
                if (res) {
                    ChatLine dl{"sys", "", ""};
                    dl.diff = (*res)["diff"];
                    dl.diff_path = res->value("path", std::string());
                    dl.diff_added = dl.diff.value("added", 0);
                    dl.diff_removed = dl.diff.value("removed", 0);
                    lines.push_back(std::move(dl));
                    continue;
                }
                // Fold the result into the last tool block above it.
                for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
                    if (it->who == "sys" && !it->think_label.empty()) {
                        it->thinking += "\nresult: " + m.content;
                        break;
                    }
                }
            }
        }
        follow_bottom = true;
    }

    // Resolve a list index ("3"/"#3"), exact id or unique id prefix.
    std::string resolve_session_arg(const std::string& arg)
    {
        std::string a = arg.empty() || arg[0] != '#' ? arg : arg.substr(1);
        if (a.empty())
            return {};
        const auto l = store.list();
        if (std::all_of(a.begin(), a.end(), [](char ch) {
                return std::isdigit(static_cast<unsigned char>(ch)) != 0;
            })) {
            const int i = std::atoi(a.c_str());
            if (i >= 1 && i <= static_cast<int>(l.size()))
                return l[i - 1].id;
            return {};
        }
        if (store.exists(a))
            return a;
        std::string hit;
        int n = 0;
        for (const auto& s : l)
            if (s.id.rfind(a, 0) == 0) { hit = s.id; ++n; }
        return n == 1 ? hit : std::string();
    }

    // The single switch path: modal, quick switcher, hotkey and commands all
    // funnel here. Refused while the worker owns the conversation.
    bool switch_to_session(const std::string& id)
    {
        if (worker != Worker::None || !store.exists(id))
            return false;
        autosave_turn();  // the outgoing session stays complete on disk
        player.stop();    // a sound from the old session must not carry over
        if (!store.load(id, conv)) {
            lines.push_back({"sys", "could not load session " + id});
            return false;
        }
        SessionMeta sm;
        store.meta_of(id, sm);
        session_id = id;
        session_title = sm.title;
        saved_count = conv.messages().size();
        rebuild_lines_from_conv();
        return true;
    }

    void new_session(const std::string& title = {})
    {
        if (worker != Worker::None)
            return;
        autosave_turn();
        conv.reset();
        session_title = title.empty() ? "New session" : title;
        session_id = store.create(session_title, cfg.llm.model_ref);
        saved_count = conv.messages().size();
        lines.push_back({"sys", "new session: " + session_title});
        follow_bottom = true;
    }

    void delete_session(const std::string& id)
    {
        if (worker != Worker::None || !store.exists(id))
            return;
        const bool was_current = id == session_id;
        store.remove(id);
        if (was_current) {
            conv.reset();
            session_id = store.create("New session", cfg.llm.model_ref);
            session_title = "New session";
            saved_count = conv.messages().size();
            lines.clear();
            lines.push_back({"sys", "session deleted - new session started"});
            follow_bottom = true;
        }
    }

    // /compact on a worker thread: the summarizing LLM calls must never block
    // the render loop. The worker owns `conv` while worker != None.
    void run_compact(const std::string& focus)
    {
        if (worker != Worker::None)
            return;
        worker = Worker::Compacting;
        llm_cancel = std::make_shared<CancelToken>();
        const LlmClient client(llm);  // snapshot: survives settings changes
        auto cancel = llm_cancel;
        lines.push_back({"sys", "compacting session..."});
        compact_fut = std::async(std::launch::async,
                                 [this, client, cancel, focus] {
            return compact_conversation(client, conv, cfg.llm.context_size,
                                        cfg.llm.max_tokens, focus, cancel.get());
        });
    }

    // opencode v2 auto-compaction: after a completed turn, if the last
    // request came within `compact_buffer` of the model's window, compact
    // automatically (the buffer is max(NN% of window, 16k), see compactor.h).
    void maybe_auto_compact()
    {
        if (!cfg.sessions.auto_compact || worker != Worker::None)
            return;
        const int ctx = cfg.llm.context_size;
        if (ctx <= 0 || last_prompt_tokens <= 0)
            return;
        const int buf = compact_buffer_tokens(cfg.sessions.compact_buffer, ctx);
        if (last_prompt_tokens > ctx - buf) {
            lines.push_back({"sys", "auto-compacting (context " +
                                    std::to_string(last_prompt_tokens) + "/" +
                                    std::to_string(ctx) + ")"});
            run_compact("auto");
        }
    }

    // Provider rejected the context as too long (wording varies per vendor).
    static bool is_context_overflow(const std::string& e)
    {
        std::string s = e;
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return (char)std::tolower(c); });
        const auto has = [&](const char* sub) { return s.find(sub) != std::string::npos; };
        return (has("context") && (has("too long") || has("exceed") ||
                                   has("length") || has("maximum"))) ||
               has("too many tokens") || has("prompt is too long");
    }

    // Clears the visible chat WITHOUT touching the session or the LLM
    // conversation: pure screen cleanup (/clean or the eraser FAB).
    void clean_screen()
    {
        lines.clear();
        sel_anchor = sel_caret = -1;
        lines.push_back({"sys", "(screen cleared - session intact)"});
        follow_bottom = true;
    }

    // ------------------------------------------------------------- media
    // Start playback for a sound block ("beep" is the built-in test tone).
    bool play_block(MediaBlock& m, std::string& err)
    {
        if (m.src == "beep") {
            int rate = 0;
            const auto pcm = make_beep_pcm(rate);
            return player.play_pcm(pcm, rate, m.src);
        }
        return player.play(m.src, err);
    }

    // /image <src> [w [h]] - sizes are read from the RIGHT so paths with
    // spaces survive; a size token is digits or -/*/auto.
    void show_image_cmd(const std::string& args)
    {
        std::vector<std::string> parts;
        {
            std::istringstream iss(args);
            for (std::string t; iss >> t;)
                parts.push_back(t);
        }
        auto size_like = [](const std::string& s) {
            return parse_media_size(s) >= 0;
        };
        int w = 0, h = 0;
        if (parts.size() >= 3 && size_like(parts[parts.size() - 1]) &&
            size_like(parts[parts.size() - 2])) {
            w = parse_media_size(parts[parts.size() - 2]);
            h = parse_media_size(parts.back());
            parts.resize(parts.size() - 2);
        } else if (parts.size() >= 2 && size_like(parts.back())) {
            w = parse_media_size(parts.back());
            parts.pop_back();
        }
        std::string src;
        for (const auto& p : parts) {
            if (!src.empty())
                src += ' ';
            src += p;
        }
        if (src.empty()) {
            lines.push_back({"sys", "usage: /image <file|clipboard|base64:...> "
                                    "[w [h]]  ('-' = auto size)"});
            return;
        }
        auto m = std::make_shared<MediaBlock>();
        m->kind = MediaBlock::Kind::Image;
        m->src = std::move(src);
        m->req_w = w;
        m->req_h = h;
        m->label = m->src.rfind("base64:", 0) == 0
                       ? "base64 image"
                       : m->src.substr(m->src.find_last_of("/\\") + 1);
        ChatLine cl{"sys", ""};
        cl.media = std::move(m);
        lines.push_back(std::move(cl));
        follow_bottom = true;
    }

    // /play <file|beep> - decodes and starts playback immediately; the chat
    // gets a mini-player block (play/stop + progress) for the sound.
    void play_cmd(const std::string& args)
    {
        if (args.empty()) {
            lines.push_back({"sys", "usage: /play <audio file|beep>"});
            return;
        }
        auto m = std::make_shared<MediaBlock>();
        m->kind = MediaBlock::Kind::Sound;
        m->src = args;
        m->label = args == "beep" ? "beep (test tone)"
                                  : args.substr(args.find_last_of("/\\") + 1);
        std::string err;
        if (!play_block(*m, err)) {
            lines.push_back({"sys", "play failed: " + (err.empty() ? "no audio output device" : err)});
            return;
        }
        ChatLine cl{"sys", ""};
        cl.media = std::move(m);
        lines.push_back(std::move(cl));
        follow_bottom = true;
    }

    // Slash commands typed in the input box (opencode-style). Returns true
    // when the text was consumed as a command (send() then skips the LLM).
    bool handle_command(const std::string& line)
    {
        auto trim = [](std::string s) {
            const size_t p = s.find_first_not_of(" \t");
            return p == std::string::npos ? std::string() : s.substr(p);
        };
        auto arg_of = [&](const char* cmd) {
            return line.size() > std::strlen(cmd)
                       ? trim(line.substr(std::strlen(cmd)))
                       : std::string();
        };
        if (line == "/sessions") {
            sessions_open = true;
            return true;
        }
        if (line == "/session") {
            SessionMeta sm;
            if (store.meta_of(session_id, sm))
                lines.push_back({"sys", "session " + sm.id + " - " + sm.title +
                                    " (" + std::to_string(sm.messages) + " msgs, " +
                                    std::to_string(sm.compactions) + " compactions)"});
            return true;
        }
        if (line.rfind("/session ", 0) == 0) {
            const std::string rest = arg_of("/session");
            const auto sub = [&](const char* s) {
                return rest.rfind(s, 0) == 0;
            };
            if (rest == "list") {
                // Text listing in the chat: ids are copyable, so the user can
                // follow with /session delete <id> or /session open <id>.
                const auto all = store.list();
                if (all.empty())
                    lines.push_back({"sys", "no saved sessions"});
                int i = 0;
                for (const auto& s : all)
                    lines.push_back({"sys", std::to_string(++i) + "  " + s.id +
                                            "  " + s.title + "  (" +
                                            std::to_string(s.messages) + " msgs, " +
                                            (s.updated.size() >= 16
                                                 ? s.updated.substr(0, 16)
                                                 : s.updated) + ")" +
                                            (s.id == session_id ? "  <- current"
                                                                : "")});
            } else if (rest == "new" || sub("new ")) {
                new_session(rest.size() > 4 ? trim(rest.substr(4)) : std::string());
            } else if (sub("open ")) {
                const std::string arg = trim(rest.substr(5));
                const std::string id = resolve_session_arg(arg);
                if (id.empty())
                    lines.push_back({"sys", "session not found: " + arg});
                else if (!switch_to_session(id))
                    lines.push_back({"sys", "cannot open session " + arg});
            } else if (sub("delete ")) {
                const std::string arg = trim(rest.substr(7));
                const std::string id = resolve_session_arg(arg);
                if (id.empty())
                    lines.push_back({"sys", "session not found: " + arg});
                else {
                    delete_session(id);
                    lines.push_back({"sys", "deleted session " + id});
                }
            } else if (sub("rename ")) {
                const std::string t = trim(rest.substr(7));
                if (!t.empty() && store.rename(session_id, t))
                    session_title = t;
            } else {
                lines.push_back({"sys", "usage: /session [list|new|open <x>|delete "
                                        "<x>|rename <t>] - modal: /sessions"});
            }
            return true;
        }
        if (line == "/clean") {
            clean_screen();
            return true;
        }
        if (line == "/voice") {  // toggle TTS output (same as the top-bar button)
            auto_speak = !auto_speak;
            cfg.tts.auto_speak = auto_speak;
            cfg.save();
            lines.push_back({"sys", std::string("voice output: ") +
                                    (auto_speak ? "ON" : "OFF")});
            return true;
        }
        if (line == "/mic") {  // toggle microphone capture (hands-free pipeline)
            if (hands_free()) {
                stop_hands_free();
                lines.push_back({"sys", "microphone capture: OFF"});
            } else {
                start_hands_free();  // reports its own success/failure line
            }
            return true;
        }
        if (line == "/compact" || line.rfind("/compact ", 0) == 0) {
            run_compact(arg_of("/compact"));
            return true;
        }
        if (line == "/title" || line.rfind("/title ", 0) == 0) {
            const std::string t = arg_of("/title");
            if (t.empty())
                lines.push_back({"sys", "current title: " + session_title});
            else if (store.rename(session_id, t))
                session_title = t;
            return true;
        }
        if (line == "/help") {
            help_open = true;
            return true;
        }
        if (line == "/image" || line.rfind("/image ", 0) == 0) {
            show_image_cmd(arg_of("/image"));
            return true;
        }
        if (line == "/play" || line.rfind("/play ", 0) == 0) {
            play_cmd(arg_of("/play"));
            return true;
        }
        lines.push_back({"sys", "unknown command: " + line +
                                "  (try /help)"});
        return true;
    }

    void start_listen()
    {
        if (worker != Worker::None || stt.streaming())
            return;
        worker = Worker::Listening;
        const int idx = cfg.stt.input_device;
        Stt& stt_ref = stt;
        stt_fut = std::async(std::launch::async,
                             [&stt_ref, idx] { return stt_ref.listen(10.0, idx); });
    }

    ~AppState()
    {
        stop_hands_free();
        // Never let a worker hang on a permission dialog at shutdown.
        {
            std::lock_guard<std::mutex> lk(ask_mtx);
            if (pending_ask) {
                pending_ask->answer = Decision::Deny;
                pending_ask->answered = true;
            }
        }
        ask_cv.notify_all();
    }

    bool hands_free() const { return pipe_run.load(); }

    // Starts the continuous cycle: mic stream -> VAD -> recognize -> send().
    // Validated by the mic actually opening; persisted to agent.continuous.
    void start_hands_free()
    {
        if (pipe_run.load())
            return;
        if (!stt.start_stream(cfg.stt.input_device, [this](std::vector<float> audio) {
                std::lock_guard<std::mutex> lk(ut_mtx);
                if (utterances.size() < 8)  // cap the recognition backlog
                    utterances.push_back(std::move(audio));
                ut_cv.notify_one();
            })) {
            lines.push_back({"sys", "hands-free: cannot open the microphone "
                                    "(check the device, CLI /devices)"});
            return;
        }
        pipe_run.store(true);
        pipe_thread = std::thread([this] {
            std::vector<float> audio;
            for (;;) {
                {
                    std::unique_lock<std::mutex> lk(ut_mtx);
                    ut_cv.wait(lk, [this] {
                        return !pipe_run.load() || !utterances.empty();
                    });
                    if (!pipe_run.load() && utterances.empty())
                        return;
                    audio = std::move(utterances.front());
                    utterances.erase(utterances.begin());
                }
                try {
                    const std::string text = stt.recognize(audio.data(), audio.size());
                    if (!text.empty()) {
                        std::lock_guard<std::mutex> lk(heard_mtx);
                        heard.push_back(text);
                    }
                } catch (const std::exception& e) {
                    // Never let an exception escape: it would terminate() the
                    // whole process. Log and keep the pipeline alive.
                    std::fprintf(stderr, "hands-free recognize failed: %s\n", e.what());
                }
            }
        });
        cfg.agent.continuous = true;
        try {
            cfg.save();  // validated: the mic stream is running
        } catch (const std::exception&) {
        }
        lines.push_back({"sys", "hands-free ON - just speak; I listen until you "
                                "toggle it off."});
    }

    void stop_hands_free()
    {
        stt.stop_stream();
        if (pipe_run.exchange(false)) {
            ut_cv.notify_all();
            if (pipe_thread.joinable())
                pipe_thread.join();
        }
        {
            std::lock_guard<std::mutex> lk(ut_mtx);
            utterances.clear();
        }
        if (cfg.agent.continuous) {
            cfg.agent.continuous = false;
            try {
                cfg.save();
            } catch (const std::exception&) {
            }
        }
    }

    // Polls finished worker tasks once per frame.
    void poll()
    {
        // Tool calls completed by the worker -> collapsible chat blocks.
        {
            std::vector<ToolEvent> evs;
            {
                std::lock_guard<std::mutex> lk(tev_mtx);
                evs.swap(tev_ready);
            }
            for (auto& e : evs) {
                if (!e.diff.is_null()) {
                    ChatLine cl{"sys", "", ""};
                    cl.diff = std::move(e.diff);
                    cl.diff_path = std::move(e.path);
                    cl.diff_added = e.added;
                    cl.diff_removed = e.removed;
                    lines.push_back(std::move(cl));
                    continue;
                }
                ChatLine cl{"sys", "", ""};
                cl.thinking = std::move(e.body);
                cl.think_label = std::move(e.label);
                cl.think_meta = std::move(e.meta);
                lines.push_back(std::move(cl));
            }
        }
        if (worker == Worker::Thinking &&
            llm_fut.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            const bool stopped = llm_cancel && llm_cancel->load();
            LlmReply reply;
            try {
                reply = llm_fut.get();
            } catch (const std::exception& e) {
                reply.ok = false;
                reply.error = e.what();
            }
            worker = Worker::None;
            if (stopped) {
                // The Stop button already printed "(stopped)"; the aborted
                // reply is discarded (the user turn stays in the history).
            } else if (reply.ok) {
                conv.add_assistant(reply.content);
                // Media tokens ([image: ...] / [sound: ...] on their own
                // line) split the reply into ordered text/media chat lines;
                // the tokens never reach markdown formatting or the TTS.
                auto segs = split_media_tokens(reply.content);
                std::vector<ChatLine> out;
                std::string speak_text;
                size_t first_text = static_cast<size_t>(-1);
                for (auto& s : segs) {
                    if (s.media) {
                        ChatLine ml{"krastor", ""};
                        ml.media = std::move(s.media);
                        out.push_back(std::move(ml));
                    } else {
                        if (first_text == static_cast<size_t>(-1))
                            first_text = out.size();
                        out.push_back({"krastor", fmt.format(s.text)});
                        if (!speak_text.empty())
                            speak_text += '\n';
                        speak_text += s.text;
                    }
                }
                if (out.empty())
                    out.push_back({"krastor", ""});
                ChatLine& head = out[first_text == static_cast<size_t>(-1)
                                         ? 0 : first_text];
                if (!reply.reasoning.empty()) {
                    head.thinking = reply.reasoning;
                    head.think_meta = fmt_elapsed(reply.elapsed_ms);
                }
                // Stats sub-line: "model - time - tok/s" ("~" when the server
                // omitted usage and the token counts are estimated).
                const double secs =
                    reply.elapsed_ms > 1.0 ? reply.elapsed_ms / 1000.0 : 0.001;
                char rate[48];
                std::snprintf(rate, sizeof(rate), "%s%.1f tok/s",
                              reply.tokens_estimated ? "~" : "",
                              reply.completion_tokens / secs);
                out.back().meta = reply.model + " - " +
                                  fmt_elapsed(reply.elapsed_ms) + " - " + rate;
                last_prompt_tokens = reply.prompt_tokens;
                for (auto& l : out)
                    lines.push_back(std::move(l));
                if (auto_speak)
                    speak_async(speak_text);
            } else {
                lines.push_back({"sys", "LLM error: " + reply.error});
            }
            // Turn complete (user + tool traffic + reply, or just the user
            // turn on error): persist it to the session folder.
            autosave_turn();
            if (reply.ok) {
                maybe_auto_compact();
            } else if (!overflow_retried && is_context_overflow(reply.error) &&
                       conv.turn_count() > 1) {
                // Overflow recovery (opencode v2): compact and re-run the
                // turn once; a second rejection is reported as a real error.
                overflow_retried = true;
                lines.push_back({"sys", "context overflow - compacting and "
                                        "retrying the turn..."});
                retry_after_compact = true;
                run_compact("overflow");
            }
        }
        if (worker == Worker::Compacting &&
            compact_fut.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            CompactResult cr;
            try {
                cr = compact_fut.get();
            } catch (const std::exception& e) {
                cr.ok = false;
                cr.error = e.what();
            }
            worker = Worker::None;
            if (cr.ok) {
                store.replace_log_after_compact(session_id,
                                                conv.messages_since(1));
                saved_count = conv.messages().size();
                rebuild_lines_from_conv();
                lines.push_back({"sys", "compacted " +
                                std::to_string(cr.messages_before) + " -> " +
                                std::to_string(cr.messages_after) +
                                " messages (~" + std::to_string(cr.est_tokens_before) +
                                " -> ~" + std::to_string(cr.est_tokens_after) +
                                " tokens)"});
                if (retry_after_compact) {
                    // Overflow recovery: re-run the turn on the compacted
                    // context (the user message is already in the history).
                    retry_after_compact = false;
                    launch_llm();
                }
            } else if (cr.error != "cancelled") {
                lines.push_back({"sys", "compact failed: " + cr.error});
                retry_after_compact = false;
            } else {
                retry_after_compact = false;
            }
            follow_bottom = true;
        }
        if (worker == Worker::Listening &&
            stt_fut.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            std::string one_shot;
            try {
                one_shot = stt_fut.get();
            } catch (const std::exception& e) {
                std::fprintf(stderr, "listen failed: %s\n", e.what());
            }
            worker = Worker::None;
            if (one_shot.empty())
                lines.push_back({"sys", "(nothing heard)"});
            else
                send(one_shot);
        }

        // Hands-free: drain transcriptions produced by the pipeline worker.
        {
            std::vector<std::string> ready;
            {
                std::lock_guard<std::mutex> lk(heard_mtx);
                ready.swap(heard);
            }
            for (const auto& text : ready) {
                const bool busy = worker != Worker::None || speaking.load();
                if (busy && cfg.agent.ignore_while_speaking) {
                    std::string snippet = text.size() > 48 ? text.substr(0, 48) + "..." : text;
                    lines.push_back({"sys", "(heard while busy, ignored: " +
                                            snippet + ")"});
                    continue;
                }
                if (speaking.load()) {
                    // Barge-in (guard off): the user spoke over the TTS —
                    // cut playback short and handle the utterance instead.
                    tts.interrupt();
                    lines.push_back({"sys", "(speech interrupted playback)"});
                }
                send(text);
            }
        }
    }
};

// ------------------------------------------------------------- chat document
// Rebuild the virtual selection document: every chat line with ANSI escapes
// stripped (exactly what the renderer draws), joined by '\n' in the offsets.
void rebuild_chat_doc(AppState& app)
{
    app.doc_lines.clear();
    app.doc_start.clear();
    app.doc_len = 0;
    for (const auto& l : app.lines) {
        app.doc_start.push_back(app.doc_len);
        std::string plain;
        for (const auto& r : ansi_parse(l.text, 0, 0, 0))
            plain += r.text;
        app.doc_len += cp_count(plain);
        app.doc_lines.push_back(std::move(plain));
        app.doc_len += 1;  // the joining '\n' (trimmed after the last line)
    }
    if (app.doc_len > 0)
        --app.doc_len;
}

// Resolve a document offset to (line, codepoint within that line).
void doc_resolve(const AppState& app, int off, int& line, int& cp)
{
    line = static_cast<int>(app.doc_lines.size()) - 1;
    for (int i = 0; i < static_cast<int>(app.doc_lines.size()); ++i)
        if (app.doc_start[i] > off) { line = i - 1; break; }
    if (line < 0)
        line = 0;
    cp = off - app.doc_start[line];
    const int len = cp_count(app.doc_lines[line]);
    cp = cp < 0 ? 0 : cp > len ? len : cp;
}

// Map a pixel position (window coordinates) to (line, cp) via the hit-map.
bool chat_hit_test(const AppState& app, float x, float y, int& line, int& cp)
{
    for (const auto& hl : app.chat_hits) {
        if (y < hl.y || y >= hl.y + hl.h)
            continue;
        line = hl.line;
        if (hl.segs.empty()) { cp = 0; return true; }
        if (x <= hl.segs.front().x0) { cp = hl.segs.front().cp0; return true; }
        for (const auto& s : hl.segs) {
            if (x < s.x1) {
                const float t = s.x1 > s.x0 ? (x - s.x0) / (s.x1 - s.x0) : 0.0f;
                const int c = s.cp0 + (int)std::lround(t * (s.cp1 - s.cp0));
                cp = c < s.cp0 ? s.cp0 : c > s.cp1 ? s.cp1 : c;
                return true;
            }
        }
        cp = hl.segs.back().cp1;
        return true;
    }
    return false;
}

// Extract document range [lo, hi) as plain text (raw UTF-8 preserved).
std::string chat_extract(const AppState& app, int lo, int hi)
{
    std::string out;
    for (size_t i = 0; i < app.doc_lines.size(); ++i) {
        const int ls = app.doc_start[i];
        const int le = ls + cp_count(app.doc_lines[i]);
        const int a = lo > ls ? lo : ls;
        const int b = hi < le ? hi : le;
        if (a < b) {
            const size_t b0 = cp_to_byte(app.doc_lines[i], a - ls);
            const size_t b1 = cp_to_byte(app.doc_lines[i], b - ls);
            out.append(app.doc_lines[i], b0, b1 - b0);
        }
        if (i + 1 < app.doc_lines.size() && lo <= le && le < hi)
            out += '\n';  // the joining newline sits at offset le
    }
    return out;
}

// Word bounds (codepoints) around `cp` within one document line.
void word_range(const AppState& app, int line, int cp, int& lo, int& hi)
{
    const std::string& s = app.doc_lines[line];
    std::vector<char> word;
    for (size_t i = 0; i < s.size(); i += cp_len(static_cast<unsigned char>(s[i])))
        word.push_back(is_word_char(static_cast<unsigned char>(s[i])) ? 1 : 0);
    const int n = static_cast<int>(word.size());
    const int c = cp < n ? cp : n - 1;
    if (n == 0 || c < 0 || !word[c]) { lo = hi = cp; return; }
    int a = c, b = c;
    while (a > 0 && word[a - 1]) --a;
    while (b < n && word[b]) ++b;
    lo = a;
    hi = b;
}

// ------------------------------------------------------------- settings modal
// Permission request modal for external tools (tools_plan.md section 4):
// the LLM worker is blocked in AppState::ask() until one of the three
// buttons answers the pending request.
void permission_modal(struct nk_context* ctx, AppState& app, int win_w, int win_h)
{
    std::shared_ptr<AppState::PendingAsk> pa;
    {
        std::lock_guard<std::mutex> lk(app.ask_mtx);
        pa = app.pending_ask;
    }
    // No request, or already answered: gone. (ask() clears pending_ask when
    // it wakes up; the answered check hides the dialog immediately even
    // while the worker is still busy running the approved tool.)
    if (!pa || pa->answered)
        return;

    nk_style_push_style_item(ctx, &ctx->style.window.fixed_background,
                             nk_style_item_color(nk_rgba(24, 14, 10, 245)));
    // Scale with the window (never larger than it, never absurdly small).
    const int mw = (std::max)(360, (std::min)(560, win_w - 80));
    const int mh = (std::max)(260, (std::min)(440, win_h - 100));
    const float row_h = ctx->style.font && ctx->style.font->height > 0.0f
                            ? ctx->style.font->height
                            : 18.0f;
    bool answered = false;
    if (nk_begin(ctx, "Tool permission",
                 nk_rect((win_w - mw) / 2.0f, (win_h - mh) / 2.0f,
                         (float)mw, (float)mh),
                 NK_WINDOW_TITLE | NK_WINDOW_BORDER | NK_WINDOW_NO_SCROLLBAR)) {
        nk_layout_row_dynamic(ctx, 26, 1);
        nk_label_colored(ctx, ("Tool '" + pa->tool + "' asks permission").c_str(),
                         NK_LEFT, nk_rgb(235, 160, 70));
        nk_layout_row_dynamic(ctx, 20, 1);
        nk_label(ctx, pa->action.c_str(), NK_LEFT);
        // The detail group gets EXACTLY the space left after the labels and
        // the button row. Layout accounting (nk_panel_layout): at_y advances
        // only when the NEXT row is declared, so the just-drawn row must be
        // subtracted through layout->row.height as well (it already includes
        // the row spacing). Ignoring that overestimated the space by one row:
        // outer scrollbar + buttons pushed off-screen.
        const struct nk_rect clip = nk_window_get_content_region(ctx);
        const struct nk_panel* lay = ctx->current->layout;
        const float remaining = clip.y + clip.h - (lay->at_y + lay->row.height);
        const float detail_h = (std::max)(
            60.0f, remaining - 32.0f - 3.0f * ctx->style.window.spacing.y);
        nk_layout_row_dynamic(ctx, detail_h, 1);
        if (nk_group_begin(ctx, "##perm_detail", NK_WINDOW_BORDER)) {
            // The scrollbar must live only inside this box: start each new
            // request at the top (groups keep their scroll across frames;
            // a group IS a window, so nk_window_set_scroll applies to it).
            nk_window_set_scroll(ctx, 0, 0);
            // One row per detail line: nk_label_wrap renders literal '\n'
            // as a "?" glyph, so the lines are split here instead.
            size_t pos = 0;
            while (pos <= pa->detail.size()) {
                const size_t nl = pa->detail.find('\n', pos);
                const std::string line = pa->detail.substr(
                    pos, nl == std::string::npos ? std::string::npos
                                                 : nl - pos);
                nk_layout_row_dynamic(ctx, row_h, 1);
                nk_label_wrap(ctx, line.c_str());
                if (nl == std::string::npos)
                    break;
                pos = nl + 1;
            }
            nk_group_end(ctx);
        }
        nk_layout_row_dynamic(ctx, 32, 3);
        if (nk_button_label(ctx, "Allow once")) {
            pa->answer = Decision::Allow;
            answered = true;
        }
        if (nk_button_label(ctx, "Always allow")) {
            pa->answer = Decision::Always;
            answered = true;
        }
        if (nk_button_label(ctx, "Deny")) {
            pa->answer = Decision::Deny;
            answered = true;
        }
    }
    nk_end(ctx);
    nk_style_pop_style_item(ctx);

    if (answered) {
        {
            std::lock_guard<std::mutex> lk(app.ask_mtx);
            pa->answered = true;
        }
        app.ask_cv.notify_all();
    }
}

// Help overlay: every GUI command and keyboard shortcut (F1 or /help).
// Same sizing discipline as the other modals: NO_SCROLLBAR on the window,
// the scrollable group gets exactly the remaining content height.
void help_modal(struct nk_context* ctx, AppState& app, int win_w, int win_h)
{
    if (!app.help_open)
        return;
    nk_style_push_style_item(ctx, &ctx->style.window.fixed_background,
                             nk_style_item_color(nk_rgba(16, 18, 24, 240)));
    const int mw = (std::max)(420, (std::min)(680, win_w - 60));
    const int mh = (std::max)(300, (std::min)(520, win_h - 80));
    if (!nk_begin(ctx, "Help",
                  nk_rect((win_w - mw) / 2.0f, (win_h - mh) / 2.0f,
                          (float)mw, (float)mh),
                  NK_WINDOW_TITLE | NK_WINDOW_BORDER | NK_WINDOW_CLOSABLE |
                      NK_WINDOW_NO_SCROLLBAR)) {
        nk_end(ctx);
        nk_window_show(ctx, "Help", NK_SHOWN);  // X-close quirk, see session_modal
        app.help_open = false;
        nk_style_pop_style_item(ctx);
        return;
    }
    const struct nk_rect clip = nk_window_get_content_region(ctx);
    const struct nk_panel* lay = ctx->current->layout;
    const float list_h = (std::max)(
        80.0f, clip.y + clip.h - (lay->at_y + lay->row.height) -
                   3.0f * ctx->style.window.spacing.y);
    nk_layout_row_dynamic(ctx, list_h, 1);
    if (nk_group_begin(ctx, "##help", NK_WINDOW_BORDER)) {
        nk_window_set_scroll(ctx, 0, 0);
        struct Row {
            const char* key;
            const char* desc;
        };
        const nk_color k = nk_rgb(110, 160, 220), d = nk_rgb(200, 200, 205);
        auto section = [&](const char* t) {
            nk_layout_row_dynamic(ctx, 24, 1);
            nk_label_colored(ctx, t, NK_LEFT, nk_rgb(235, 160, 70));
        };
        auto rows = [&](std::initializer_list<Row> v) {
            for (const auto& r : v) {
                nk_layout_row_begin(ctx, NK_DYNAMIC, 22, 2);
                nk_layout_row_push(ctx, 0.34f);
                nk_label_colored(ctx, r.key, NK_LEFT, k);
                nk_layout_row_push(ctx, 0.66f);
                nk_label_colored(ctx, r.desc, NK_LEFT, d);
                nk_layout_row_end(ctx);
            }
        };
        section("Chat");
        rows({{"Enter", "send the message"},
              {"Shift+Enter", "newline in the input box"},
              {"Ctrl+V", "paste text / attach clipboard image"},
              {"drag & drop", "attach an image file"},
              {"click + drag in chat", "select text (Ctrl+C copies it)"},
              {"Ctrl++ / Ctrl+-", "font zoom in / out"}});
        section("Sessions");
        rows({{"Ctrl+O", "open the session manager"},
              {"Ctrl+Shift+O", "cycle to the next recent session"},
              {"/sessions", "open the session manager"},
              {"/session list", "list sessions with their ids"},
              {"/session open <x>", "load a session (index, id or id prefix)"},
              {"/session delete <x>", "delete a session permanently"},
              {"/session new [title]", "start a new session"},
              {"/session rename <t>", "rename the current session"},
              {"/title [title]", "show or set the current title"},
              {"/compact [focus]", "summarize + shrink the session (LLM)"}});
        section("Voice & screen");
        rows({{"/voice", "toggle voice output (TTS)"},
              {"/mic", "toggle microphone capture (hands-free)"},
              {"/clean", "clear the screen, session untouched (eraser button)"},
              {"/help or F1", "this window"}});
        section("Media");
        rows({{"/image <src> [w [h]]", "show an image (file, clipboard or "
                                       "base64:...) - '-' = auto size"},
              {"/play <file|beep>", "play a sound (mini player in the chat)"},
              {"[image: src | w | h]", "reply token: inline image block"},
              {"[sound: src]", "reply token: sound block with play/stop"},
              {"click an image", "copy it to the clipboard (like text)"}});
        section("Floating buttons");
        rows({{"send", "send the input box text"},
              {"paperclip", "attach the clipboard image"},
              {"ear", "listen once from the mic (max 10 s)"},
              {"speaker", "test the TTS voice"},
              {"eraser", "clear the screen (keeps the session)"},
              {"stop", "cancel the running request + silence"}});
        nk_group_end(ctx);
    }
    nk_end(ctx);
    nk_style_pop_style_item(ctx);
}

// Session manager: list / switch / new / rename / delete (sessions_plan.md §5).
void session_modal(struct nk_context* ctx, AppState& app, int win_w, int win_h)
{
    if (!app.sessions_open)
        return;

    nk_style_push_style_item(ctx, &ctx->style.window.fixed_background,
                             nk_style_item_color(nk_rgba(16, 18, 24, 240)));
    const int mw = (std::max)(420, (std::min)(640, win_w - 60));
    const int mh = (std::max)(300, (std::min)(480, win_h - 80));
    const float row_h = ctx->style.font && ctx->style.font->height > 0.0f
                            ? ctx->style.font->height
                            : 18.0f;
    bool close = false;
    // X-click detection: same hidden-window quirk as the Settings modal
    // (nk_begin fails after the title X hides the window).
    if (!nk_begin(ctx, "Sessions",
                  nk_rect((win_w - mw) / 2.0f, (win_h - mh) / 2.0f,
                          (float)mw, (float)mh),
                  NK_WINDOW_TITLE | NK_WINDOW_BORDER | NK_WINDOW_CLOSABLE |
                  NK_WINDOW_NO_SCROLLBAR)) {
        nk_end(ctx);
        nk_window_show(ctx, "Sessions", NK_SHOWN);
        nk_style_pop_style_item(ctx);
        app.sessions_open = false;
        app.delete_confirm = -1;
        return;
    }

    // Scrollable list: title + date + counts per row, delete needs a second
    // confirming click on the same row. The group height is computed so the
    // whole content (header + list + footer) fits the window exactly: any
    // overflow would add a second scrollbar around the modal itself.
    nk_layout_row_dynamic(ctx, row_h + 4, 1);
    nk_label_colored(ctx, "Sessions - click a row to open it", NK_LEFT,
                     nk_rgb(110, 160, 220));
    // Exact remaining space (see the permission modal note about at_y +
    // row.height): the footer row stays inside the window, so only the list
    // itself scrolls.
    const struct nk_rect clip = nk_window_get_content_region(ctx);
    const struct nk_panel* lay = ctx->current->layout;
    const float remaining = clip.y + clip.h - (lay->at_y + lay->row.height);
    const float list_h = (std::max)(
        80.0f, remaining - 30.0f - 3.0f * ctx->style.window.spacing.y);
    nk_layout_row_dynamic(ctx, list_h, 1);
    if (nk_group_begin(ctx, "##sess_list", NK_WINDOW_BORDER)) {
        nk_window_set_scroll(ctx, 0, 0);  // always open at the top
        const auto sessions = app.store.list();
        int idx = 0;
        for (const auto& s : sessions) {
            // Two columns only: the id column overflowed the window (labels
            // do not clip); the full ids are available via /session list.
            nk_layout_row_begin(ctx, NK_DYNAMIC, row_h + 8, 2);
            const bool cur = s.id == app.session_id;
            nk_layout_row_push(ctx, 0.80f);
            const std::string label =
                (cur ? "> " : "  ") + s.title + "   " +
                (s.updated.size() >= 10 ? s.updated.substr(0, 10) : s.updated) +
                "  " + std::to_string(s.messages) + " msgs" +
                (s.compactions ? "  compacted x" + std::to_string(s.compactions)
                               : "");
            const float col_w = nk_window_get_content_region(ctx).w * 0.80f -
                                4.0f * ctx->style.button.padding.x;
            if (nk_button_label(
                    ctx, fit_text(ctx->style.font, label, col_w).c_str()) &&
                !cur)
                if (app.switch_to_session(s.id))
                    close = true;  // refused while the worker is busy
            nk_layout_row_push(ctx, 0.20f);
            const bool confirm = app.delete_confirm == idx;
            if (nk_button_label(ctx, confirm ? "sure?" : "delete")) {
                if (confirm) {
                    app.delete_session(s.id);
                    app.delete_confirm = -1;
                } else {
                    app.delete_confirm = idx;
                }
            }
            nk_layout_row_end(ctx);
            ++idx;
        }
        if (sessions.empty()) {
            nk_layout_row_dynamic(ctx, row_h + 6, 1);
            nk_label(ctx, "(no saved sessions)", NK_LEFT);
        }
        nk_group_end(ctx);
    }

    // Footer on a single row so it always stays visible inside the window:
    // new / rename field / apply / close.
    nk_layout_row_dynamic(ctx, 30, 4);
    if (nk_button_label(ctx, "New session")) {
        app.new_session();
        close = true;
    }
    nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, app.rename_buf,
                                   sizeof(app.rename_buf) - 1,
                                   nk_filter_default);
    if (nk_button_label(ctx, "Rename")) {
        if (app.rename_buf[0] &&
            app.store.rename(app.session_id, app.rename_buf)) {
            app.session_title = app.rename_buf;
            app.rename_buf[0] = '\0';
        }
    }
    if (nk_button_label(ctx, "Close"))
        close = true;
    nk_end(ctx);

    if (close) {
        app.sessions_open = false;
        app.delete_confirm = -1;
    }
    nk_style_pop_style_item(ctx);
}

bool settings_modal(struct nk_context* ctx, Config& cfg, VoiceRegistry& voices,
                    SettingsState& s, int win_w, int win_h, EffectEngine& fx,
                    const LlmModelRegistry& models, const IconAtlas& icons)
{
    if (!s.open)
        return false;

    const int mw = 520, mh = 680;
    // The modal needs its own opaque background: the main window's
    // fixed_background is fully transparent so the shader shows through.
    nk_style_push_style_item(ctx, &ctx->style.window.fixed_background,
                             nk_style_item_color(nk_rgba(16, 18, 24, 235)));
    // Horizontal breathing room from the window edges. The padding is cached
    // by nk_begin into the layout clip, so it MUST be pushed before opening
    // the window (spacing instead is read live per row, see below).
    nk_style_push_vec2(ctx, &ctx->style.window.padding, nk_vec2(14.0f, 6.0f));
    // NOTE: the title-bar X button in Nuklear 4.13.3 only sets NK_WINDOW_HIDDEN
    // (never NK_WINDOW_CLOSED), so nk_window_is_closed() can't detect it. When
    // the window is hidden, nk_begin() returns 0 with win->layout == 0 and any
    // widget call afterwards trips NK_ASSERT(ctx->current->layout). Detect the
    // X click here instead: treat "begin failed" as closed, un-hide the window
    // so the gear button can open it again next time.
    if (!nk_begin(ctx, "Settings", nk_rect((win_w - mw) / 2.0f, (win_h - mh) / 2.0f, mw, mh),
                  NK_WINDOW_TITLE | NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_CLOSABLE)) {
        nk_end(ctx);
        nk_window_show(ctx, "Settings", NK_SHOWN);
        nk_style_pop_vec2(ctx);       // padding
        nk_style_pop_style_item(ctx);
        s.open = false;
        s.status.clear();
        return false;
    }

    // Vertical breathing room between the fields (the default 4 px makes the
    // label/input pairs and the property rows visually collide).
    nk_style_push_vec2(ctx, &ctx->style.window.spacing, nk_vec2(6.0f, 12.0f));

    nk_layout_row_dynamic(ctx, 18, 1);
    nk_label(ctx, "Agent name", NK_LEFT);
    nk_layout_row_dynamic(ctx, 26, 1);
    nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, s.name,
                                   sizeof(s.name) - 1, nk_filter_default);

    nk_layout_row_dynamic(ctx, 16, 1);
    nk_label(ctx, "LLM Engine", NK_LEFT);
    nk_layout_row_dynamic(ctx, 18, 1);
    nk_label(ctx, "Model (from llm_models.json)", NK_LEFT);
    nk_layout_row_dynamic(ctx, 26, 1);
    {
        // The combo lists every provider/model in llm_models.json; the label is
        // the model's display name. URL, key and context come from the file.
        const LlmModelEntry* cur = models.find(s.model_ref);
        if (nk_combo_begin_label(ctx,
                cur ? cur->display.c_str()
                    : (s.model_ref.empty() ? "(none selected)" : s.model_ref.c_str()),
                nk_vec2(480, 240))) {
            nk_layout_row_dynamic(ctx, 24, 1);
            for (const auto& m : models.entries())
                if (nk_combo_item_label(ctx,
                        (m.display + "  -  " + m.ref).c_str(), NK_TEXT_LEFT))
                    s.model_ref = m.ref;
            nk_combo_end(ctx);
        }
        if (cur) {
            nk_layout_row_dynamic(ctx, 18, 1);
            nk_label_colored(ctx, cur->base_url.c_str(), NK_LEFT,
                             nk_rgb(110, 110, 120));
        } else if (models.entries().empty()) {
            nk_layout_row_dynamic(ctx, 18, 1);
            nk_label_colored(ctx, models.error().c_str(), NK_LEFT,
                             nk_rgb(220, 180, 90));
        }
    }

    nk_layout_row_dynamic(ctx, 18, 1);
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

    // Background effect plugins from data/shaders (validated = compiles).
    nk_layout_row_dynamic(ctx, 18, 1);
    nk_label(ctx, "Background effect", NK_LEFT);
    nk_layout_row_dynamic(ctx, 26, 1);
    if (nk_combo_begin_label(ctx, s.effect.c_str(), nk_vec2(480, 240))) {
        nk_layout_row_dynamic(ctx, 24, 1);
        for (const auto& name : s.effect_names)
            if (nk_combo_item_label(ctx, name.c_str(), NK_TEXT_LEFT) && name != s.effect) {
                if (fx.select(name, "data/shaders")) {
                    s.effect = name;
                    cfg.gui.effect = (name == "gradient") ? "" : name;
                    try {
                        cfg.save();  // validated: the plugin compiled
                        s.status = "effect applied: " + name;
                    } catch (const std::exception&) {
                    }
                } else {
                    s.status = "effect failed: " + fx.error();
                }
            }
        nk_combo_end(ctx);
    }

    // Effect brightness: multiplies the plugin output (u_intensity). Applied
    // live while dragging; auto-saved 500 ms after the last change.
    nk_layout_row_dynamic(ctx, 26, 1);
    nk_property_float(ctx, "Effect brightness", 0.2f, &s.effect_intensity, 3.0f,
                      0.05f, 0.05f);
    if (std::fabs(s.effect_intensity - cfg.gui.effect_intensity) > 1e-4) {
        cfg.gui.effect_intensity = s.effect_intensity;
        s.intensity_due = glfwGetTime() + 0.5;  // debounce the write
    }
    if (s.intensity_due > 0.0 && glfwGetTime() >= s.intensity_due) {
        s.intensity_due = 0.0;
        try {
            cfg.save();  // validated: the value is live on screen
            char buf[48];
            std::snprintf(buf, sizeof(buf), "effect brightness: %.2fx",
                          s.effect_intensity);
            s.status = buf;
        } catch (const std::exception&) {
        }
    }

    // Hands-free echo guard (saved immediately: a plain validated toggle).
    nk_layout_row_dynamic(ctx, 28, 1);
    {
        nk_bool ignore = cfg.agent.ignore_while_speaking ? nk_true : nk_false;
        if (nk_checkbox_label(ctx, "Ignore mic while thinking/speaking", &ignore)) {
            cfg.agent.ignore_while_speaking = ignore != 0;
            try {
                cfg.save();
            } catch (const std::exception&) {
            }
            s.status = std::string("ignore while busy: ") +
                       (cfg.agent.ignore_while_speaking ? "on" : "off");
        }
    }

    // External tools master switch (see tools_plan.md). The manifest scan
    // happens at startup, so toggling takes effect after a restart.
    nk_layout_row_dynamic(ctx, 28, 1);
    {
        nk_bool tools = cfg.tools.enabled ? nk_true : nk_false;
        if (nk_checkbox_label(ctx, "Enable external tools (restart)", &tools)) {
            cfg.tools.enabled = tools != 0;
            try {
                cfg.save();
            } catch (const std::exception&) {
            }
            s.status = std::string("external tools: ") +
                       (cfg.tools.enabled ? "on (after restart)" : "off");
        }
    }

    // Input DSP (applied live to the shared AudioFx pipeline + saved).
    nk_layout_row_dynamic(ctx, 28, 1);
    {
        nk_bool dn = cfg.stt.denoise ? nk_true : nk_false;
        if (nk_checkbox_label(ctx, "Noise suppression (mic)", &dn)) {
            cfg.stt.denoise = dn != 0;
            AudioFx::instance().set_denoise(cfg.stt.denoise);
            try {
                cfg.save();
            } catch (const std::exception&) {
            }
            s.status = std::string("noise suppression: ") +
                       (cfg.stt.denoise ? "on" : "off");
        }
    }
    nk_layout_row_dynamic(ctx, 28, 1);
    {
        nk_bool ec = cfg.stt.echo_cancel ? nk_true : nk_false;
        if (nk_checkbox_label(ctx, "Echo cancellation (mic hears TTS)", &ec)) {
            cfg.stt.echo_cancel = ec != 0;
            AudioFx::instance().set_echo_cancel(cfg.stt.echo_cancel);
            try {
                cfg.save();
            } catch (const std::exception&) {
            }
            s.status = std::string("echo cancellation: ") +
                       (cfg.stt.echo_cancel ? "on" : "off");
        }
    }

    nk_layout_row_dynamic(ctx, 26, 1);
    nk_property_float(ctx, "Temperature", 0.0f, &s.temperature, 2.0f, 0.05f, 0.01f);
    nk_layout_row_dynamic(ctx, 26, 1);
    nk_property_int(ctx, "Max tokens", 1, &s.max_tokens, 32000, 64, 1.0f);

    nk_layout_row_dynamic(ctx, 18, 1);
    nk_label(ctx, "System prompt", NK_LEFT);
    nk_layout_row_dynamic(ctx, 52, 1);
    nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, s.system_prompt,
                                   sizeof(s.system_prompt) - 1, nk_filter_default);

    if (!s.status.empty()) {
        nk_layout_row_dynamic(ctx, 20, 1);
        nk_label_colored(ctx, s.status.c_str(), NK_LEFT, nk_rgb(120, 200, 120));
    }

    nk_layout_row_dynamic(ctx, 30, 2);
    if (button_icon(ctx, icons, "check", "Save")) {
        cfg.agent.name = s.name;
        cfg.llm.model_ref = s.model_ref;
        if (!apply_model_ref(cfg.llm, models))
            s.status = s.model_ref.empty()
                           ? "no model selected - keeping current llm.base_url/model"
                           : "model not found in llm_models.json";
        cfg.llm.system_prompt = s.system_prompt;
        cfg.llm.temperature = s.temperature;
        cfg.llm.max_tokens = s.max_tokens;
        cfg.tts.default_voice = s.voice;
        if (!s.voice.empty())
            voices.select(s.voice);
        try {
            cfg.save();  // atomic write next to the executable
            s.status = "Saved to " + Config::runtime_path();
            s.dirty = true;
        } catch (const std::exception& e) {
            s.status = std::string("Save failed: ") + e.what();
        }
    }
    if (button_icon(ctx, icons, "x", "Close")) {
        s.open = false;
        s.status.clear();
    }

    nk_style_pop_vec2(ctx);   // spacing
    nk_end(ctx);
    nk_style_pop_vec2(ctx);   // padding
    nk_style_pop_style_item(ctx);
    return true;
}

// ------------------------------------------------------------- chat panel
// Pixel width of a UTF-8 string in the given font (nuklear exposes this only
// through the font callback; there is no public nk_text_width in 4.13.3).
float text_w_px(const struct nk_user_font* f, const char* s, int n)
{
    return f && f->width ? f->width(f->userdata, f->height, s, n) : 0.0f;
}

// nk_fill_rect truncates x/y/w/h to integers, so with fractional layout
// coordinates (fractional font metrics, DPI scaling) adjacent fills leave
// 1px dark seams - the "black border around spaces" on colored lines.
// Round the rect outward so it covers the region's exact pixel extent.
void fill_px(struct nk_command_buffer* cv, float x, float y, float w, float h,
             struct nk_color c)
{
    const float x0 = std::floor(x), y0 = std::floor(y);
    const float x1 = std::ceil(x + w), y1 = std::ceil(y + h);
    nk_fill_rect(cv, nk_rect(x0, y0, x1 - x0, y1 - y0), 0, c);
}

// Decodes the UTF-8 codepoint at `i` (len receives its byte length).
char32_t utf8_next(const std::string& s, size_t i, size_t& len)
{
    const unsigned char c = static_cast<unsigned char>(s[i]);
    const auto cont = [&](size_t k) {
        return i + k < s.size()
                   ? static_cast<unsigned char>(s[i + k]) & 0x3F : 0u;
    };
    if (c < 0x80) { len = 1; return c; }
    if ((c & 0xE0) == 0xC0) { len = 2; return (char32_t(c & 0x1F) << 6) | cont(1); }
    if ((c & 0xF0) == 0xE0) { len = 3; return (char32_t(c & 0x0F) << 12) | (cont(1) << 6) | cont(2); }
    if ((c & 0xF8) == 0xF0) { len = 4; return (char32_t(c & 0x07) << 18) | (cont(1) << 12) | (cont(2) << 6) | cont(3); }
    len = 1;
    return c;
}

// Draws one token: plain text via nk_draw_text; codepoints present in the
// color emoji atlas as sprite quads (nk_draw_image, square advance = line
// height). The glfw backend binds the texture per draw command, so text and
// emoji batches interleave freely. Returns the actual pixel advance.
float draw_token_mixed(struct nk_command_buffer* canvas, float tx, float y,
                       float row_h, const std::string& text,
                       const struct nk_user_font* f, struct nk_color fg,
                       const EmojiAtlas* em)
{
    if (!em || !em->ok() || text.empty()) {
        const float rw = text_w_px(f, text.data(), (int)text.size());
        nk_draw_text(canvas, nk_rect(tx, y, rw + 2.0f, row_h), text.data(),
                     (int)text.size(), f, nk_rgba(0, 0, 0, 0), fg);
        return rw;
    }
    float x = tx;
    std::string run;
    auto flush = [&]() {
        if (run.empty())
            return;
        const float rw = text_w_px(f, run.data(), (int)run.size());
        nk_draw_text(canvas, nk_rect(x, y, rw + 2.0f, row_h), run.data(),
                     (int)run.size(), f, nk_rgba(0, 0, 0, 0), fg);
        x += rw;
        run.clear();
    };
    size_t i = 0;
    while (i < text.size()) {
        size_t len = 1;
        const char32_t cp = utf8_next(text, i, len);
        const EmojiAtlas::Rect* r = em->find(cp);
        if (r) {
            flush();
            struct nk_image img = nk_image_id(static_cast<int>(em->texture()));
            img.w = static_cast<nk_ushort>(em->tex_w());
            img.h = static_cast<nk_ushort>(em->tex_h());
            img.region[0] = static_cast<nk_ushort>(r->x);
            img.region[1] = static_cast<nk_ushort>(r->y);
            img.region[2] = static_cast<nk_ushort>(r->w);
            img.region[3] = static_cast<nk_ushort>(r->h);
            nk_draw_image(canvas, nk_rect(x, y, row_h, row_h), &img,
                          nk_rgb(255, 255, 255));
            x += row_h;
        } else {
            if (cp != 0xFE0F)  // emoji presentation selector: invisible
                run.append(text, i, len);
        }
        i += len;
    }
    flush();
    return x - tx;
}

// One layout token (a word or a run of spaces) carrying its ANSI style.
// Newlines are represented by a token with force_break = true.
// cp_begin is the token's first codepoint within the body text (-1 for the
// role prefix and other non-selectable tokens).
struct Word {
    std::string text;
    struct nk_color fg;
    struct nk_color bg;   // a == 0 -> no background
    bool force_break;
    int cp_begin = -1;
};

// Splits styled runs into space/word/newline tokens, preserving indentation
// (runs of spaces are kept as their own tokens so code blocks stay aligned).
// runs[0] is the role prefix (not selectable); the remaining runs carry the
// body's codepoint numbering used for the selection hit-map.
std::vector<Word> tokenize_runs(const std::vector<AnsiRun>& runs)
{
    std::vector<Word> toks;
    bool first_run = true;
    // Cumulative codepoint counter across ALL body runs (the role prefix is
    // not selectable and does not advance it): the selection maps document
    // offsets onto these numbers, so numbering must be continuous - a
    // per-run reset made styled replies (inline code, bold) highlight
    // scattered words instead of the dragged range.
    int lcp = 0;
    for (const auto& r : runs) {
        const bool prefix_run = first_run;
        first_run = false;
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
                toks.push_back({"", fg, bg, true, prefix_run ? -1 : lcp});
                ++i;
                if (!prefix_run)
                    ++lcp;
            } else if (ch == ' ') {
                size_t j = i;
                while (j < r.text.size() && r.text[j] == ' ')
                    ++j;
                toks.push_back({r.text.substr(i, j - i), fg, bg, false,
                                prefix_run ? -1 : lcp});
                if (!prefix_run)
                    lcp += static_cast<int>(j - i);
                i = j;
            } else {
                size_t j = i;
                while (j < r.text.size() && r.text[j] != ' ' && r.text[j] != '\n')
                    ++j;
                toks.push_back({r.text.substr(i, j - i), fg, bg, false,
                                prefix_run ? -1 : lcp});
                if (!prefix_run)
                    lcp += cp_count(r.text.substr(i, j - i));
                i = j;
            }
        }
    }
    return toks;
}

// Splits a single word wider than max_w into UTF-8-safe chunks that fit.
// Returns (chunk, codepoint offset of the chunk within the original word).
std::vector<std::pair<std::string, int>> split_wide(const std::string& w,
                                                   float max_w,
                                                   const struct nk_user_font* f)
{
    std::vector<std::pair<std::string, int>> out;
    size_t i = 0;
    int cp_off = 0;
    while (i < w.size()) {
        const int start_cp = cp_off;
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
            ++cp_off;
        }
        out.push_back({w.substr(i, j - i), start_cp});
        i = j;
    }
    return out;
}

// Draws one chat message (prefix + ANSI-styled body) word-wrapped at text_w,
// with diff-style full-line backgrounds. Returns the pixel height consumed.
// When src_line >= 0, every drawn body token is recorded into `hits` for the
// selection hit-map, and the selection `sel` (if any) paints a highlight.
float draw_chat_line(struct nk_context* ctx, const std::string& prefix,
                     const std::string& body, struct nk_color default_color,
                     float text_w, int src_line,
                     std::vector<ChatHitLine>* hits, const SelRange* sel,
                     const EmojiAtlas* em = nullptr)
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
        for (auto& pr : split_wide(w.text, text_w, f)) {
            Word t{std::move(pr.first), w.fg, w.bg, false,
                   w.cp_begin < 0 ? -1 : w.cp_begin + pr.second};
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
        // Diff-style full-line background only when every token on the visual
        // line carries the same background; a single token without one (a table
        // border, plain cell text next to an inline-code cell) means the line
        // is not a full-line background, so paint per-token backgrounds instead
        // and inline highlights (bg: X, `code`) do not bleed across the line.
        struct nk_color bg = nk_rgba(0, 0, 0, 0);
        bool uniform = true;
        for (const auto& w : ln) {
            if (w.text.empty())
                continue;
            if (!w.bg.a) {
                uniform = false;
                break;
            }
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
            fill_px(canvas, area.x, y, text_w, row_h, bg);
        ChatHitLine hit_line;
        hit_line.y = y;
        hit_line.h = row_h;
        hit_line.line = src_line;
        float tx = area.x;
        for (const auto& w : ln) {
            const float ww = text_w_px(f, w.text.data(), (int)w.text.size());
            const int ncp = w.cp_begin >= 0 ? cp_count(w.text) : 0;
            if (!uniform && w.bg.a)
                fill_px(canvas, tx, y, ww, row_h, w.bg);
            // Selection highlight: translucent accent behind the selected
            // portion of this token (monospace: cp->x is linear).
            if (sel && src_line >= 0 && ncp > 0) {
                const int a = w.cp_begin, b = w.cp_begin + ncp;
                const bool after_lo = src_line > sel->lo_line ||
                                      (src_line == sel->lo_line && b > sel->lo_cp);
                const bool before_hi = src_line < sel->hi_line ||
                                       (src_line == sel->hi_line && a < sel->hi_cp);
                if (after_lo && before_hi) {
                    const int ha = src_line == sel->lo_line && sel->lo_cp > a
                                       ? sel->lo_cp : a;
                    const int hb = src_line == sel->hi_line && sel->hi_cp < b
                                       ? sel->hi_cp : b;
                    if (hb > ha) {
                        const float px = ww / ncp;
                        fill_px(canvas, tx + (ha - a) * px, y, (hb - ha) * px,
                                row_h, nk_rgba(60, 110, 200, 115));
                    }
                }
            }
            const float adv = draw_token_mixed(canvas, tx, y, row_h, w.text, f,
                                               w.fg, em);
            if (hits && src_line >= 0 && ncp > 0)
                hit_line.segs.push_back({tx, tx + ww, src_line, w.cp_begin,
                                         w.cp_begin + ncp});
            tx += adv > 0.0f ? adv : ww;
        }
        if (hits && src_line >= 0)
            hits->push_back(std::move(hit_line));
        y += row_h;
    }
    return total_h;
}

// ---------------------------------------------------------------- diff block
// opencode-style collapsible file-edit block: a clickable header
// ("← Edit path +N −M") over a side-by-side diff (old | new) when the chat
// is wide enough, otherwise a unified single-column diff. Diff rows never
// wrap: long lines are clipped with an ellipsis so every row is exactly one
// line tall (cheap to render, easy to scan).

// Clip one line to `max_w` pixels (UTF-8 aware), appending "..." when cut.
std::string clip_line_w(const struct nk_user_font* f, const std::string& s,
                        float max_w)
{
    if (max_w < 8.0f || text_w_px(f, s.data(), (int)s.size()) <= max_w)
        return s;
    std::string out;
    float w = 0.0f;
    const float dots = text_w_px(f, "...", 3);
    size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        size_t n = 1;
        if ((c & 0xE0) == 0xC0) n = 2;
        else if ((c & 0xF0) == 0xE0) n = 3;
        else if ((c & 0xF8) == 0xF0) n = 4;
        if (i + n > s.size())
            break;
        const std::string ch = s.substr(i, n);
        const float cw = text_w_px(f, ch.data(), (int)ch.size());
        if (w + cw + dots > max_w)
            break;
        w += cw;
        out += ch;
        i += n;
    }
    return out + "...";
}

// One rendered diff row: left/right cells; kind drives the backgrounds.
struct DiffRow {
    int old_no = 0, new_no = 0;
    std::string old_text, new_text;
    char kind = ' ';  // ' ' context | '-' del | '+' add | '~' changed pair
    bool gap = false;  // hunk separator (line numbers jumped)
};

// Build display rows from filetool's flat op list: a run of '-'/'+' ops is
// paired row-by-row into the side-by-side view; a context line whose numbers
// jump over untouched lines inserts a gap separator.
std::vector<DiffRow> diff_rows(const nlohmann::json& diff)
{
    std::vector<DiffRow> rows;
    std::vector<const nlohmann::json*> dels, adds;
    int last_old = 0, last_new = 0;
    bool first = true;
    auto flush = [&]() {
        const size_t n = (std::max)(dels.size(), adds.size());
        for (size_t k = 0; k < n; ++k) {
            DiffRow r;
            if (k < dels.size()) {
                r.old_no = (*dels[k]).value("old", 0);
                r.old_text = (*dels[k]).value("text", std::string());
                last_old = r.old_no;
            }
            if (k < adds.size()) {
                r.new_no = (*adds[k]).value("new", 0);
                r.new_text = (*adds[k]).value("text", std::string());
                last_new = r.new_no;
            }
            r.kind = (k < dels.size() && k < adds.size())
                         ? '~'
                         : (k < dels.size() ? '-' : '+');
            rows.push_back(std::move(r));
        }
        dels.clear();
        adds.clear();
    };
    for (const auto& op : diff["ops"]) {
        const std::string o = op.value("op", std::string(" "));
        if (o == "-") {
            dels.push_back(&op);
            continue;
        }
        if (o == "+") {
            adds.push_back(&op);
            continue;
        }
        flush();
        const int on = op.value("old", 0), nn = op.value("new", 0);
        if (!first && ((last_old > 0 && on > last_old + 1) ||
                       (last_new > 0 && nn > last_new + 1))) {
            DiffRow g;
            g.gap = true;
            rows.push_back(g);
        }
        first = false;
        DiffRow r;
        r.old_no = on;
        r.new_no = nn;
        r.old_text = r.new_text = op.value("text", std::string());
        last_old = on;
        last_new = nn;
        rows.push_back(std::move(r));
    }
    flush();
    return rows;
}

// Header + (when open) the diff panes. Returns the pixel height consumed;
// the header rect is recorded into `rects` so a click can toggle it.
float draw_diff_block(struct nk_context* ctx, const ChatLine& l, float text_w,
                      int src_line, const EmojiAtlas* em,
                      std::vector<std::pair<int, struct nk_rect>>* rects)
{
    const struct nk_user_font* f = ctx->style.font;
    const float row_h = f && f->height > 0.0f ? f->height : 18.0f;
    text_w = text_w < 40.0f ? 40.0f : text_w;

    // Keep at most the last two path components so the header stays short.
    std::string path = l.diff_path;
    const size_t s1 = path.find_last_of("/\\");
    if (s1 != std::string::npos && s1 > 0) {
        const size_t s2 = path.find_last_of("/\\", s1 - 1);
        if (s2 != std::string::npos)
            path = "..." + path.substr(s2);
    }
    const std::string head =
        std::string(l.diff_open ? "- " : "+ ") +
        "\x1b[33mEdit \x1b[0m" + path + " \x1b[32m+" +
        std::to_string(l.diff_added) + " \x1b[31m\xE2\x88\x92" +
        std::to_string(l.diff_removed) + "\x1b[0m";
    float total = draw_chat_line(ctx, "\xE2\x86\x90 ", head,
                                 nk_rgb(230, 150, 60), text_w, -1, nullptr,
                                 nullptr, em);
    rects->push_back({src_line, nk_widget_bounds(ctx)});
    if (!l.diff_open || !l.diff.is_object() || !l.diff.contains("ops") ||
        !l.diff["ops"].is_array())
        return total;

    const auto rows = diff_rows(l.diff);
    const bool side = text_w >= 640.0f;
    const float gutter = 4.0f * text_w_px(f, "0", 1) + 6.0f;
    const float gap_w = 10.0f;
    const float pane_w = side ? (text_w - gap_w) / 2.0f : text_w;
    const struct nk_color c_ctx = nk_rgb(170, 170, 175);
    const struct nk_color c_del = nk_rgb(246, 142, 142);
    const struct nk_color c_add = nk_rgb(140, 227, 158);
    const struct nk_color c_num = nk_rgb(95, 95, 105);
    const struct nk_color bg_del = nk_rgba(110, 36, 40, 120);
    const struct nk_color bg_add = nk_rgba(24, 84, 44, 120);
    constexpr int kMaxViewRows = 240;

    struct nk_command_buffer* cv = nk_window_get_canvas(ctx);
    auto dim_note = [&](const std::string& s) {
        nk_layout_row_dynamic(ctx, row_h, 1);
        const struct nk_rect a = nk_widget_bounds(ctx);
        draw_token_mixed(cv, a.x + 6, a.y, row_h, s, f, c_num, em);
        total += row_h;
    };
    // Unified row: mark + dim line number + clipped text on a full-width bg.
    auto uni_row = [&](char mark, int no, const std::string& text,
                       struct nk_color fg, struct nk_color bg) {
        nk_layout_row_dynamic(ctx, row_h, 1);
        const struct nk_rect a = nk_widget_bounds(ctx);
        if (bg.a)
            fill_px(cv, a.x, a.y, text_w, row_h, bg);
        const std::string ns = no ? std::to_string(no) : std::string();
        const float nw = text_w_px(f, ns.data(), (int)ns.size());
        draw_token_mixed(cv, a.x + 2, a.y, row_h, std::string(1, mark), f, fg,
                         em);
        draw_token_mixed(cv, a.x + 18 + gutter - nw, a.y, row_h, ns, f, c_num,
                         em);
        draw_token_mixed(cv, a.x + 18 + gutter, a.y, row_h,
                         clip_line_w(f, text, text_w - gutter - 24), f, fg, em);
        total += row_h;
    };

    int shown = 0;
    for (const auto& r : rows) {
        if (shown >= kMaxViewRows) {
            dim_note("... " + std::to_string(rows.size() - kMaxViewRows) +
                     " more lines");
            break;
        }
        ++shown;
        if (r.gap) {
            dim_note("\xC2\xB7 \xC2\xB7 \xC2\xB7");  // · · ·
            continue;
        }
        if (!side) {
            // unified: a changed pair becomes a del row followed by an add row
            if (r.kind == '-' || r.kind == '~')
                uni_row('-', r.old_no, r.old_text, c_del, bg_del);
            if (r.kind == '+' || r.kind == '~')
                uni_row('+', r.new_no, r.new_text, c_add, bg_add);
            if (r.kind == ' ')
                uni_row(' ', r.old_no, r.old_text, c_ctx, nk_rgba(0, 0, 0, 0));
            continue;
        }
        nk_layout_row_dynamic(ctx, row_h, 1);
        const struct nk_rect a = nk_widget_bounds(ctx);
        const float rx = a.x + pane_w + gap_w;
        if (r.kind == '-' || r.kind == '~')
            fill_px(cv, a.x, a.y, pane_w, row_h, bg_del);
        if (r.kind == '+' || r.kind == '~')
            fill_px(cv, rx, a.y, pane_w, row_h, bg_add);
        const std::string ons = r.old_no ? std::to_string(r.old_no) : "";
        const std::string nns = r.new_no ? std::to_string(r.new_no) : "";
        draw_token_mixed(cv, a.x + gutter - 6 -
                                 text_w_px(f, ons.data(), (int)ons.size()),
                         a.y, row_h, ons, f, c_num, em);
        draw_token_mixed(cv, rx + gutter - 6 -
                                 text_w_px(f, nns.data(), (int)nns.size()),
                         a.y, row_h, nns, f, c_num, em);
        draw_token_mixed(cv, a.x + gutter, a.y, row_h,
                         clip_line_w(f, r.old_text, pane_w - gutter - 6), f,
                         (r.kind == '-' || r.kind == '~') ? c_del : c_ctx, em);
        draw_token_mixed(cv, rx + gutter, a.y, row_h,
                         clip_line_w(f, r.new_text, pane_w - gutter - 6), f,
                         (r.kind == '+' || r.kind == '~') ? c_add : c_ctx, em);
        total += row_h;
    }
    if (l.diff.value("truncated", false))
        dim_note("... diff too large, showing the first part");
    return total;
}

// ------------------------------------------------------------- media blocks
// Image textures are uploaded lazily on the render thread at first draw (the
// bytes come from disk, the clipboard or a base64 token; workers never touch
// GL). Eviction is a simple LRU over the load order.
constexpr size_t kMaxImageTextures = 40;

void media_unload(MediaBlock& m)
{
    if (m.tex) {
        glDeleteTextures(1, &m.tex);
        m.tex = 0;
    }
    m.loaded = false;
}

void media_ensure_loaded(AppState& app, const MediaRef& m)
{
    if (m->kind != MediaBlock::Kind::Image || m->loaded || m->failed)
        return;
    if (m->bytes.empty()) {
        std::string err;
        if (!resolve_media_bytes(m->src, m->bytes, err)) {
            m->failed = true;
            m->error = err;
            return;
        }
    }
    int w = 0, h = 0, ch = 0;
    stbi_uc* px = stbi_load_from_memory(m->bytes.data(),
                                        (int)m->bytes.size(), &w, &h, &ch, 4);
    if (!px) {
        m->failed = true;
        m->error = "cannot decode image";
        if (m->src != "clipboard")
            m->bytes.clear();
        return;
    }
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, px);
    stbi_image_free(px);
    // The clipboard is volatile: keep its bytes for re-upload after eviction.
    // Files/base64 are re-resolvable from src, so free the RAM copy.
    if (m->src != "clipboard")
        m->bytes.clear();
    m->tex = tex;
    m->nat_w = w;
    m->nat_h = h;
    m->loaded = true;
    // LRU bookkeeping: drop dead refs, then unload the oldest live texture
    // when over budget (a big chat of images cannot grow VRAM forever).
    app.tex_lru.erase(std::remove_if(app.tex_lru.begin(), app.tex_lru.end(),
                                     [](const std::weak_ptr<MediaBlock>& w) {
                                         return w.expired();
                                     }),
                      app.tex_lru.end());
    app.tex_lru.push_back(m);
    size_t live = 0;
    for (const auto& w : app.tex_lru)
        if (!w.expired())
            ++live;
    if (live > kMaxImageTextures) {
        for (const auto& w : app.tex_lru) {
            auto b = w.lock();
            if (b && b->tex && b != m) {
                media_unload(*b);
                break;
            }
        }
    }
}

// Image block: one flow row sized by media_fit (always contained inside the
// text area, ratio preserved - see media.h). The drawn rect is recorded for
// the click-to-copy handler (rects are from the previous frame, like the
// Thought headers).
float draw_media_image(nk_context* ctx, AppState& app, const MediaRef& m,
                       float text_w, float max_h, int src_line)
{
    media_ensure_loaded(app, m);
    if (m->failed)
        return draw_chat_line(ctx, "! ", "image failed (" + m->error +
                                             "): " + m->src,
                              nk_rgb(235, 110, 110), text_w, -1, nullptr,
                              nullptr, &app.emoji);
    float w = 0.0f, h = 0.0f;
    media_fit(*m, text_w, max_h, w, h);
    nk_layout_row_dynamic(ctx, h + 4.0f, 1);
    const struct nk_rect slot = nk_widget_bounds(ctx);
    const struct nk_image img =
        atlas_image(m->tex, m->nat_w, m->nat_h, 0, 0, m->nat_w, m->nat_h);
    const struct nk_rect drawn = nk_rect(slot.x, slot.y, w, h);
    nk_draw_image(&ctx->current->buffer, drawn, &img, nk_white);
    app.image_rects.push_back({src_line, drawn});
    return h + 4.0f;
}

// Sound block: mini player row - Play/Stop button, name (+progress text),
// progress bar. Clicks never push chat lines (the chat loop is iterating):
// playback errors land in the block and show in its label.
float draw_media_sound(nk_context* ctx, AppState& app, const MediaRef& m,
                       float text_w)
{
    const float h = ctx->style.font->height + 8.0f;
    const bool active = app.player.playing_src(m->src);
    nk_layout_row_begin(ctx, NK_DYNAMIC, h, 3);
    nk_layout_row_push(ctx, 0.18f);
    const bool clicked = button_icon(ctx, app.icons, active ? "square" : "play",
                                     active ? "Stop" : "Play");
    nk_layout_row_push(ctx, 0.47f);
    std::string label = m->label;
    if (!m->error.empty())
        label = m->label + " - " + m->error;
    else if (active) {
        char t[48];
        std::snprintf(t, sizeof(t), "  %.0f / %.0fs", app.player.position_s(),
                      app.player.duration_s());
        label += t;
    }
    nk_label_colored(ctx,
                     fit_text(ctx->style.font, label, 0.47f * text_w).c_str(),
                     NK_LEFT, nk_rgb(200, 200, 205));
    nk_layout_row_push(ctx, 0.35f);
    const double dur = active ? app.player.duration_s() : 0.0;
    nk_size cur = 0, max = 100;
    if (dur > 0.0)
        cur = (nk_size)(app.player.position_s() / dur * 100.0);
    nk_progress(ctx, &cur, max, nk_false);
    nk_layout_row_end(ctx);
    if (clicked) {
        if (active) {
            app.player.stop();
        } else {
            m->error.clear();
            std::string err;
            if (!app.play_block(*m, err))
                m->error = err.empty() ? "no audio output device" : err;
        }
    }
    return h;
}

void chat_panel(struct nk_context* ctx, AppState& app, float height)
{
    // This Nuklear has no "fill remaining space" row: the group gets an
    // explicitly computed height (see caller). A full-width static item would
    // overflow the padded row and wrap, pushing the group off-screen, so use a
    // dynamic-ratio row (one column at ratio 1.0) with the fixed pixel height.
    nk_layout_row_begin(ctx, NK_DYNAMIC, height, 1);
    nk_layout_row_push(ctx, 1.0f);
    // Translucent panel background so the shader effect shows through.
    nk_style_push_color(ctx, &ctx->style.window.background, nk_rgba(14, 16, 22, 105));
    if (nk_group_begin(ctx, "##chat", NK_WINDOW_BORDER)) {
        const float spacing = ctx->style.window.spacing.y;
        // Usable text width inside the group: window width minus group padding,
        // border and the vertical scrollbar.
        const float text_w = nk_window_get_width(ctx) -
                             ctx->style.window.scrollbar_size.x - 20.0f;
        const float row_h = ctx->style.font && ctx->style.font->height > 0.0f
                                ? ctx->style.font->height : 18.0f;

        // The Settings modal overlaps the chat but is a separate window: while
        // it is open, mouse input on it must not register as chat input.
        bool perm_pending = false;
        {
            std::lock_guard<std::mutex> lk(app.ask_mtx);
            perm_pending = static_cast<bool>(app.pending_ask);
        }
        const bool modal_blocks = app.settings.open || perm_pending;

        // Mouse wheel over the chat: leave follow-bottom mode and scroll
        // manually (3 text lines per notch). The delta is consumed so
        // Nuklear's own 10%-of-height step does not stack on top of it.
        // Groups in 4.13.3 are panels inside the parent window, NOT separate
        // windows: nk_window_get_bounds() would return the MAIN window's rect
        // (making every click anywhere look "over the chat"). The group's own
        // rect is the panel bounds.
        struct nk_rect gb = ctx->current->layout->bounds;
        gb.w -= ctx->style.window.scrollbar_size.x;  // exclude the scrollbar
        // UITEST bypasses the hover gate: the agent harness cannot move the
        // real cursor over the window (posted WM_MOUSEMOVE is overridden by
        // live input), so wheel injection would never scroll otherwise.
        const bool uitest_hover =
            std::getenv("KRASTOR_UITEST") != nullptr ||
            nk_input_is_mouse_hovering_rect(&ctx->input, gb);
        if (ctx->input.mouse.scroll_delta.y != 0.0f && uitest_hover &&
            !modal_blocks) {
            app.follow_bottom = false;
            nk_uint sx = 0, sy = 0;
            // nk_group_get_scroll (NOT nk_window_get_scroll): inside a group,
            // ctx->current is still the main window, whose scrollbar.y is not
            // the group's offset. The group's live offset lives in the parent
            // window's value store, which both group scroll accessors use.
            nk_group_get_scroll(ctx, "##chat", &sx, &sy);
            float off = (float)sy - ctx->input.mouse.scroll_delta.y * row_h * 3.0f;
            if (off < 0.0f)
                off = 0.0f;
            // Clamp at the bottom too (previous frame's value): without this
            // the wheel pushed the offset past the end BEFORE the content was
            // drawn, so every tick flashed an over-scrolled frame (the bounce).
            if (off > app.chat_bottom)
                off = app.chat_bottom;
            nk_group_set_scroll(ctx, "##chat", 0, (nk_uint)off);
            ctx->input.mouse.scroll_delta.y = 0.0f;  // consumed
        }

        // ---- selection: mouse (uses the hit-map built during last frame) ----
        struct nk_input* in = &ctx->input;
        const bool over = nk_input_is_mouse_hovering_rect(in, gb);
        struct nk_mouse_button* L = &in->mouse.buttons[NK_BUTTON_LEFT];
        // Clicks on the floating action button strip are not selection clicks.
        const bool on_fab = app.fab_rect.w > 0.0f &&
                            nk_input_is_mouse_hovering_rect(in, app.fab_rect);
        if (L->clicked && over && !modal_blocks && !on_fab) {
            app.chat_focused = true;
            // A click on an image block copies it to the clipboard (like a
            // text selection copy) and is not a selection click.
            bool copied = false;
            for (const auto& ir : app.image_rects) {
                const struct nk_rect& r = ir.second;
                if (in->mouse.pos.x >= r.x && in->mouse.pos.x < r.x + r.w &&
                    in->mouse.pos.y >= r.y && in->mouse.pos.y < r.y + r.h) {
                    const MediaRef& m = app.lines[ir.first].media;
                    if (m && !m->failed) {
                        std::vector<unsigned char> bytes = m->bytes;
                        std::string err;
                        if (bytes.empty())
                            resolve_media_bytes(m->src, bytes, err);
                        if (!bytes.empty() && copy_image_to_clipboard(bytes, err))
                            app.lines.push_back({"sys", "image copied to clipboard"});
                        else
                            app.lines.push_back({"sys", "image copy failed: " + err});
                    }
                    copied = true;
                    break;
                }
            }
            // A click on a "Thought" header toggles that block and is not a
            // selection click.
            bool toggled = copied;
            for (const auto& tr : app.thought_rects) {
                const struct nk_rect& r = tr.second;
                if (in->mouse.pos.x >= r.x && in->mouse.pos.x < r.x + r.w &&
                    in->mouse.pos.y >= r.y && in->mouse.pos.y < r.y + r.h) {
                    app.lines[tr.first].thought_open =
                        !app.lines[tr.first].thought_open;
                    toggled = true;
                    break;
                }
            }
            // Same for the "← Edit path" diff headers.
            for (const auto& dr : app.diff_rects) {
                const struct nk_rect& r = dr.second;
                if (in->mouse.pos.x >= r.x && in->mouse.pos.x < r.x + r.w &&
                    in->mouse.pos.y >= r.y && in->mouse.pos.y < r.y + r.h) {
                    app.lines[dr.first].diff_open =
                        !app.lines[dr.first].diff_open;
                    toggled = true;
                    break;
                }
            }
            if (toggled) {
                app.sel_dragging = false;
            } else {
            // Release the text-edit focus (next frame) so keyboard goes to the
            // selection keys instead of moving the invisible edit caret.
            app.clear_edit_focus = true;
            int line = 0, cp = 0;
            const bool hit =
                chat_hit_test(app, in->mouse.pos.x, in->mouse.pos.y, line, cp);
            const double now = glfwGetTime();
            const bool again =
                now - app.last_click_t < 0.4 &&
                std::fabs(in->mouse.pos.x - app.last_click_x) < 8.0 &&
                std::fabs(in->mouse.pos.y - app.last_click_y) < 8.0;
            app.click_count = again ? app.click_count + 1 : 1;
            app.last_click_t = now;
            app.last_click_x = (int)in->mouse.pos.x;
            app.last_click_y = (int)in->mouse.pos.y;
            if (hit) {
                const int off = app.doc_start[line] + cp;
                if (app.click_count >= 3) {  // triple-click: whole source line
                    app.sel_anchor = app.doc_start[line];
                    app.sel_caret = app.doc_start[line] +
                                    cp_count(app.doc_lines[line]);
                } else if (app.click_count == 2) {  // double-click: word
                    int wl = 0, wr = 0;
                    word_range(app, line, cp, wl, wr);
                    app.sel_anchor = app.doc_start[line] + wl;
                    app.sel_caret = app.doc_start[line] + wr;
                } else {
                    app.sel_anchor = app.sel_caret = off;
                }
                app.sel_dragging = true;
                app.sel_moved = false;
            } else {
                app.sel_anchor = app.sel_caret = -1;
                app.sel_dragging = false;
            }
            }
        } else if (L->down && app.sel_dragging && !modal_blocks &&
                   app.sel_anchor >= 0) {
            int line = 0, cp = 0;
            if (chat_hit_test(app, in->mouse.pos.x, in->mouse.pos.y, line, cp)) {
                const int off = app.doc_start[line] + cp;
                if (off != app.sel_caret) {
                    app.sel_caret = off;
                    app.sel_moved = true;
                    app.follow_bottom = false;  // auto-pin pauses while selecting
                }
            }
        } else if (!L->down && app.sel_dragging) {
            app.sel_dragging = false;
            // A plain click (no drag, not part of a multi-click) clears.
            if (!app.sel_moved && app.click_count == 1)
                app.sel_anchor = app.sel_caret = -1;
        }

        // Rebuild the virtual document and resolve the selection bounds; the
        // hit-map (chat_hits) is refilled by draw_chat_line below.
        rebuild_chat_doc(app);
        if (std::getenv("KRASTOR_UITEST") && !app.uitest_sel_done &&
            app.doc_lines.size() > 2) {
            // Seed a selection across the first styled lines and scroll to the
            // top so the highlight is visible in a screenshot.
            app.uitest_sel_done = true;
            app.sel_anchor = app.doc_start[1];
            app.sel_caret = app.doc_start[2] + 20;
            app.chat_focused = true;  // so posted Ctrl+A/Ctrl+C hotkeys apply
            app.follow_bottom = false;
            app.scroll_cmd = 3;  // Home
        }
        app.chat_hits.clear();
        SelRange sel{};
        bool has_sel = false;
        if (app.sel_anchor >= 0 && app.sel_caret >= 0 &&
            app.sel_anchor != app.sel_caret) {
            const int lo = app.sel_anchor < app.sel_caret ? app.sel_anchor
                                                         : app.sel_caret;
            const int hi = app.sel_anchor < app.sel_caret ? app.sel_caret
                                                         : app.sel_anchor;
            doc_resolve(app, lo, sel.lo_line, sel.lo_cp);
            doc_resolve(app, hi, sel.hi_line, sel.hi_cp);
            has_sel = true;
        }

        float content_h = 0.0f;
        int src = 0;
        app.thought_rects.clear();
        app.diff_rects.clear();
        app.image_rects.clear();
        // Images are always contained inside the chat text area: the max
        // block height is the visible group height (no scroll needed to see
        // a whole image), the max width is text_w (media_fit does the rest).
        const float media_max_h =
            height - 2.0f * ctx->style.window.padding.y - 8.0f;
        for (const auto& l : app.lines) {
            const std::string prefix = l.who == "you" ? "> " :
                                       l.who == "krastor" ? app.name + ": " : "! ";
            struct nk_color col = nk_rgb(160, 160, 160);
            if (l.who == "you") col = nk_rgb(120, 180, 255);
            else if (l.who == "krastor") col = nk_rgb(130, 220, 150);
            // File-edit diff (files edit/write): its own collapsible block,
            // no plain text row of its own.
            if (!l.diff.is_null()) {
                content_h += draw_diff_block(ctx, l, text_w, src, &app.emoji,
                                             &app.diff_rects) +
                             spacing;
                ++src;
                continue;
            }
            // Collapsible block (opencode-style "Thought · Ns"; tool calls
            // reuse it with a "⚙ tool · action" label).
            if (!l.thinking.empty()) {
                std::string head = std::string(l.thought_open ? "- " : "+ ") +
                                   (l.think_label.empty() ? "Thought"
                                                          : l.think_label);
                if (!l.think_meta.empty())
                    head += " \xC2\xB7 " + l.think_meta;
                content_h += draw_chat_line(ctx, "", head, nk_rgb(230, 150, 60),
                                            text_w, -1, nullptr, nullptr,
                                            &app.emoji) +
                             spacing;
                app.thought_rects.push_back({src, nk_widget_bounds(ctx)});
                if (l.thought_open)
                    content_h += draw_chat_line(
                        ctx, "  \xE2\x94\x82 ", l.thinking, nk_rgb(150, 150, 160),
                        text_w, -1, nullptr, nullptr, &app.emoji) +
                                 spacing;
            }
            // Media blocks (image/sound) get their own row in the flow.
            if (l.media)
                content_h +=
                    (l.media->kind == MediaBlock::Kind::Image
                         ? draw_media_image(ctx, app, l.media, text_w,
                                            media_max_h, src)
                         : draw_media_sound(ctx, app, l.media, text_w)) +
                    spacing;
            // Pure tool blocks carry no text line of their own.
            if (!l.text.empty() || (l.thinking.empty() && !l.media))
                content_h += draw_chat_line(ctx, prefix, l.text, col, text_w, src,
                                            &app.chat_hits, has_sel ? &sel : nullptr,
                                            &app.emoji) +
                             spacing;
            // Dim stats sub-line under the reply (model - time - tok/s).
            if (!l.meta.empty())
                content_h += draw_chat_line(ctx, "  \xC2\xB7 ", l.meta,
                                             nk_rgb(110, 110, 120), text_w,
                                             -1, nullptr, nullptr,
                                             &app.emoji) +
                             spacing;
            ++src;
        }
        // Follow-bottom pinning: keep the newest line in view while following.
        // The group keeps its own persistent scroll offset (nk_window_set_scroll
        // would scroll the outer window). The bottom offset is taken from the
        // group's actual layout end - the same value Nuklear's own scrollbar
        // clamps to - rather than from the content_h sum: if the two ever
        // diverge, clamping to the smaller one would leave the last lines
        // unreachable below the clamp. Scrolling up with the wheel leaves the
        // view where the user left it; reaching the bottom re-enables follow.
        const float visible = height - 2.0f * ctx->style.window.padding.y - 8.0f;
        struct nk_panel* gp = ctx->current->layout;
        const float est_bottom = content_h > visible ? content_h - visible : 0.0f;
        // panel_end finalizes at_y with the last row height; Nuklear's own
        // scrollbar max is (at_y + row.height) - bounds.y - bounds.h.
        const float layout_bottom =
            gp->at_y + gp->row.height - gp->bounds.y - gp->bounds.h;
        const float bottom = layout_bottom > est_bottom ? layout_bottom : est_bottom;
        // Keyboard scroll commands (PageUp/PageDown/Home/End, set by the main
        // loop when the chat has focus).
        if (app.scroll_cmd) {
            nk_uint sx = 0, sy = 0;
            nk_group_get_scroll(ctx, "##chat", &sx, &sy);
            float off = (float)sy;
            const float page = visible > row_h * 2.0f ? visible - row_h : visible;
            switch (app.scroll_cmd) {
                case 1: off -= page; break;
                case 2: off += page; break;
                case 3: off = 0.0f; break;
                case 4: off = bottom; app.follow_bottom = true; break;
            }
            app.scroll_cmd = 0;
            if (!app.follow_bottom) {
                if (off < 0.0f) off = 0.0f;
                if (off > bottom) off = bottom;
                nk_group_set_scroll(ctx, "##chat", 0, (nk_uint)off);
            }
        }
        if (app.follow_bottom) {
            nk_group_set_scroll(ctx, "##chat", 0, (nk_uint)bottom);
        }
        nk_group_end(ctx);
        // Post-end bookkeeping on the persistent offset:
        // - clamp any overshoot (Nuklear's own clamp stops working once the
        //   scrollbar auto-hides) so it can never be rendered;
        // - wheel/drag reaching the bottom re-enables the follow pin;
        // - a scrollbar drag that pulled the view up while pinned must
        //   release the pin, otherwise the next frame snaps back to the
        //   bottom and the drag is impossible.
        {
            nk_uint sx = 0, sy = 0;
            nk_group_get_scroll(ctx, "##chat", &sx, &sy);
            if ((float)sy > bottom)
                nk_group_set_scroll(ctx, "##chat", 0, (nk_uint)bottom);
            else if ((float)sy >= bottom - 4.0f)
                app.follow_bottom = true;
            else if (app.follow_bottom)
                app.follow_bottom = false;  // drag pulled the view up
        }
        app.chat_bottom = bottom;
        nk_layout_row_end(ctx);
    }
    nk_style_pop_color(ctx);
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

    // Restore the persisted window size (clamped to a usable minimum).
    cfg.gui.window_w = cfg.gui.window_w < kWinMinW ? kWinMinW : cfg.gui.window_w;
    cfg.gui.window_h = cfg.gui.window_h < kWinMinH ? kWinMinH : cfg.gui.window_h;
    GLFWwindow* win =
        glfwCreateWindow(cfg.gui.window_w, cfg.gui.window_h, "Krastor", nullptr, nullptr);
    if (!win) {
        std::fprintf(stderr, "gui: failed to create window (OpenGL 3.3 core required)\n");
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(win);
    glfwSwapInterval(1);  // vsync

    // Restore the persisted position (-1 = let the OS place the window).
    // Guard against a stale position from a disconnected monitor: only apply
    // it when the title-bar area lands on some monitor's work area.
    if (cfg.gui.window_x >= 0 && cfg.gui.window_y >= 0) {
        const int mx = cfg.gui.window_x, my = cfg.gui.window_y;
        int mon_n = 0;
        GLFWmonitor** mons = glfwGetMonitors(&mon_n);
        for (int m = 0; m < mon_n; ++m) {
            int wx = 0, wy = 0, ww = 0, wh = 0;
            glfwGetMonitorWorkarea(mons[m], &wx, &wy, &ww, &wh);
            if (mx >= wx - 50 && mx + 200 <= wx + ww + 50 &&
                my >= wy - 50 && my + 100 <= wy + wh + 50) {
                glfwSetWindowPos(win, mx, my);
                break;
            }
        }
    }

    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
        std::fprintf(stderr, "gui: gladLoadGLLoader failed\n");
        glfwTerminate();
        return 1;
    }

    EffectEngine fx;
    fx.init();
    SpectrumAnalyzer spec;
    std::vector<float> spec_window(SpectrumAnalyzer::kWindow, 0.0f);
    const double t0 = glfwGetTime();

    static struct nk_glfw device;
    nk_glfw3_init(&device, win, NK_GLFW3_INSTALL_CALLBACKS);
    struct nk_font_atlas* atlas = nullptr;
    nk_glfw3_font_stash_begin(&device, &atlas);
    // Monospace font with box-drawing/extended glyphs for terminal-style output;
    // fall back to the built-in font when the file is missing. Bake a ladder of
    // sizes so Ctrl +/- can swap the style font at runtime.
    std::vector<char> font_buf;  // must outlive the stash (atlas borrows it)
    {
        std::ifstream f("data/fonts/jetbrains-mono.ttf", std::ios::binary);
        if (f)
            font_buf.assign((std::istreambuf_iterator<char>(f)),
                            std::istreambuf_iterator<char>());
    }
    std::vector<struct nk_font*> fonts;  // index i -> kFontMin + i*kFontStep px
    // Glyph ranges baked into the atlas: ASCII + Latin-1 (Italian accents) +
    // general punctuation (curly quotes, dashes, ellipsis) + box drawing and
    // block elements (the │ ─ bars used by quotes/tables/rules, the ▓▒░
    // shades used by ANSI art). Without this Nuklear bakes ASCII only and
    // everything else renders as "?".
    static const nk_rune kGlyphRanges[] = {
        32, 0x00FF, 0x2000, 0x205B, 0x2500, 0x259F, 0
    };
    for (int px = kFontMin; px <= kFontMax; px += kFontStep) {
        struct nk_font_config fcfg = nk_font_config((float)px);
        fcfg.range = kGlyphRanges;
        struct nk_font* fn = font_buf.empty()
            ? nk_font_atlas_add_default(atlas, (float)px, &fcfg)
            : nk_font_atlas_add_from_memory(atlas, font_buf.data(), font_buf.size(),
                                            (float)px, &fcfg);
        if (fn)
            fonts.push_back(fn);
    }
    nk_glfw3_font_stash_end(&device);
    font_buf.clear();
    font_buf.shrink_to_fit();

    // Font zoom state: style.font drives every widget (4.13.3 has no per-widget
    // fonts), so swapping it rescales the whole UI.
    auto font_index_for = [&](int px) {
        int i = (px - kFontMin + kFontStep / 2) / kFontStep;
        if (i < 0) i = 0;
        if (!fonts.empty() && i >= (int)fonts.size()) i = (int)fonts.size() - 1;
        return i;
    };
    int font_px = cfg.gui.font_size;
    double win_save_due = 0.0;  // debounced auto-save of the window size
    if (!fonts.empty())
        nk_style_set_font(&device.ctx, &fonts[font_index_for(font_px)]->handle);
    bool prev_plus = false, prev_minus = false, prev_zero = false;
    double font_save_due = 0.0;  // debounce timestamp for auto-save
    // The main window is fully transparent so the shader background shows
    // through everywhere; readability comes from the chat group's own panel
    // (see chat_panel) and the opaque Settings modal. NOTE: the window body
    // is painted from style.window.fixed_background; style.window.background
    // only colors dynamic panels (groups, popups).
    device.ctx.style.window.fixed_background = nk_style_item_color(nk_rgba(16, 18, 24, 0));
    device.ctx.style.window.background = nk_rgba(16, 18, 24, 200);
    // Checkbox readability: the vendored dark color table fills the CHECKED
    // cursor with the window color (45,45,45) - the checked box looked like
    // an empty hole and the unchecked gray one looked "filled", i.e. the
    // state read inverted. A bright accent square = checked, unmistakable.
    device.ctx.style.checkbox.cursor_normal =
        nk_style_item_color(nk_rgb(120, 180, 255));
    device.ctx.style.checkbox.cursor_hover =
        nk_style_item_color(nk_rgb(150, 200, 255));

    AppState app(cfg, voices);
    app.settings.load_from(cfg, voices);
    app.tts.init();
    // Effect plugin list from data/shaders + restore the persisted selection
    // (falling back to the built-in gradient when it no longer compiles).
    for (const auto& e : fx.scan("data/shaders"))
        app.settings.effect_names.push_back(e.name);
    if (!cfg.gui.effect.empty() && !fx.select(cfg.gui.effect, "data/shaders")) {
        std::fprintf(stderr, "gui: stored effect '%s' failed: %s — using built-in\n",
                     cfg.gui.effect.c_str(), fx.error().c_str());
        cfg.gui.effect.clear();
    }
    app.settings.effect = fx.current();
    // Restore hands-free mode (loads the STT models synchronously: the first
    // frame is delayed a few seconds, then everything runs on worker threads).
    if (cfg.agent.continuous)
        app.start_hands_free();
    glfwSetWindowTitle(win, app.name.c_str());
    // Override the backend paste plugin so Ctrl+V can also grab clipboard images.
    g_paste_app = &app;
    device.ctx.clip.paste = krastor_paste;
    glfwSetDropCallback(win, drop_callback);

    while (!glfwWindowShouldClose(win)) {
        int w = 0, h = 0;
        glfwGetWindowSize(win, &w, &h);

        // Window resized or moved by the user: remember it (debounced, like
        // font zoom). Position is skipped while iconified (Windows reports a
        // parked off-screen position for minimized windows).
        const bool iconified = glfwGetWindowAttrib(win, GLFW_ICONIFIED) != 0;
        if (!iconified) {
            int px = 0, py = 0;
            glfwGetWindowPos(win, &px, &py);
            if (px != cfg.gui.window_x || py != cfg.gui.window_y) {
                cfg.gui.window_x = px;
                cfg.gui.window_y = py;
                win_save_due = glfwGetTime() + 0.5;
            }
        }
        if (w != cfg.gui.window_w || h != cfg.gui.window_h) {
            cfg.gui.window_w = w;
            cfg.gui.window_h = h;
            win_save_due = glfwGetTime() + 0.5;
        }
        if (win_save_due > 0.0 && glfwGetTime() >= win_save_due) {
            win_save_due = 0.0;
            try {
                cfg.save();
            } catch (const std::exception&) {
            }
        }

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

        // ---- Ctrl +/-/0 font zoom (edge-detected; debounced auto-save) ----
        if (!fonts.empty()) {
            const bool ctrl = glfwGetKey(win, GLFW_KEY_LEFT_CONTROL) ||
                              glfwGetKey(win, GLFW_KEY_RIGHT_CONTROL);
            const bool plus = ctrl && (glfwGetKey(win, GLFW_KEY_EQUAL) ||
                                       glfwGetKey(win, GLFW_KEY_KP_ADD));
            const bool minus = ctrl && (glfwGetKey(win, GLFW_KEY_MINUS) ||
                                        glfwGetKey(win, GLFW_KEY_KP_SUBTRACT));
            const bool zero = ctrl && glfwGetKey(win, GLFW_KEY_0);
            int new_px = font_px;
            if (plus && !prev_plus)
                new_px += kFontStep;
            else if (minus && !prev_minus)
                new_px -= kFontStep;
            else if (zero && !prev_zero)
                new_px = kFontDefault;
            prev_plus = plus;
            prev_minus = minus;
            prev_zero = zero;
            if (new_px != font_px) {
                font_px = new_px < kFontMin ? kFontMin
                        : new_px > kFontMax ? kFontMax
                                            : new_px;
                nk_style_set_font(&device.ctx, &fonts[font_index_for(font_px)]->handle);
                cfg.gui.font_size = font_px;
                font_save_due = glfwGetTime() + 0.5;  // debounce the write
            }
        }
        if (font_save_due > 0.0 && glfwGetTime() >= font_save_due) {
            font_save_due = 0.0;
            try {
                cfg.save();  // validated: the new size was applied without error
            } catch (const std::exception&) {
            }
        }

        // ---- session hotkeys: Ctrl+O opens the manager, Ctrl+Shift+O cycles
        // through the recent sessions (LRU order of the store list) ----
        {
            const bool ctrl = glfwGetKey(win, GLFW_KEY_LEFT_CONTROL) ||
                              glfwGetKey(win, GLFW_KEY_RIGHT_CONTROL);
            static bool prev_o = false;
            const bool o = ctrl && glfwGetKey(win, GLFW_KEY_O) != 0;
            if (o && !prev_o && app.worker == AppState::Worker::None) {
                const bool shift = glfwGetKey(win, GLFW_KEY_LEFT_SHIFT) ||
                                   glfwGetKey(win, GLFW_KEY_RIGHT_SHIFT);
                if (shift) {
                    const auto l = app.store.list();
                    if (l.size() > 1) {
                        size_t i = 0;
                        for (; i < l.size(); ++i)
                            if (l[i].id == app.session_id)
                                break;
                        if (i < l.size())
                            app.switch_to_session(l[(i + 1) % l.size()].id);
                    }
                } else {
                    app.sessions_open = !app.sessions_open;
                }
            }
            prev_o = o;
            // F1 toggles the help overlay (edge-detected like the others).
            static bool prev_f1 = false;
            const bool f1 = glfwGetKey(win, GLFW_KEY_F1) != 0;
            if (f1 && !prev_f1)
                app.help_open = !app.help_open;
            prev_f1 = f1;
        }

        // ---- chat selection & scroll hotkeys (only while the chat has focus,
        // i.e. after clicking in it; edge-detected like the font zoom) ----
        if (app.chat_focused && !app.settings.open && app.doc_len > 0) {
            struct KeyState {
                bool l, r, u, d, home, end, pgup, pgdn, a, c, esc;
            };
            static KeyState kprev{};
            const bool shift = glfwGetKey(win, GLFW_KEY_LEFT_SHIFT) ||
                               glfwGetKey(win, GLFW_KEY_RIGHT_SHIFT);
            const bool ctrl = glfwGetKey(win, GLFW_KEY_LEFT_CONTROL) ||
                              glfwGetKey(win, GLFW_KEY_RIGHT_CONTROL);
            auto kd = [&](int key) { return glfwGetKey(win, key) != 0; };
            auto pressed = [](bool now, bool prev) { return now && !prev; };
            KeyState kn{};
            kn.l = kd(GLFW_KEY_LEFT); kn.r = kd(GLFW_KEY_RIGHT);
            kn.u = kd(GLFW_KEY_UP); kn.d = kd(GLFW_KEY_DOWN);
            kn.home = kd(GLFW_KEY_HOME); kn.end = kd(GLFW_KEY_END);
            kn.pgup = kd(GLFW_KEY_PAGE_UP); kn.pgdn = kd(GLFW_KEY_PAGE_DOWN);
            kn.a = ctrl && kd(GLFW_KEY_A);
            kn.c = ctrl && kd(GLFW_KEY_C);
            kn.esc = kd(GLFW_KEY_ESCAPE);

            auto clamp_doc = [&](int v) {
                return v < 0 ? 0 : v > app.doc_len ? app.doc_len : v;
            };
            auto start_sel = [&]() {
                if (app.sel_anchor < 0)
                    app.sel_anchor = app.sel_caret = app.doc_len;
            };

            if (shift && pressed(kn.l, kprev.l)) {
                start_sel();
                app.sel_caret = clamp_doc(app.sel_caret - 1);
            }
            if (shift && pressed(kn.r, kprev.r)) {
                start_sel();
                app.sel_caret = clamp_doc(app.sel_caret + 1);
            }
            if (shift && (pressed(kn.home, kprev.home) ||
                          pressed(kn.end, kprev.end))) {
                start_sel();
                int line = 0, cp = 0;
                doc_resolve(app, app.sel_caret, line, cp);
                app.sel_caret = pressed(kn.home, kprev.home)
                                    ? app.doc_start[line]
                                    : app.doc_start[line] +
                                          cp_count(app.doc_lines[line]);
            }
            if (shift && (pressed(kn.u, kprev.u) || pressed(kn.d, kprev.d))) {
                start_sel();
                const bool up = pressed(kn.u, kprev.u);
                int line = 0, cp = 0;
                doc_resolve(app, app.sel_caret, line, cp);
                // Move one visual line: the hit-map holds every visual line of
                // every chat line (off-screen ones included, they are laid out
                // and clipped), in document order.
                int idx = -1;
                for (int i = 0; i < (int)app.chat_hits.size(); ++i) {
                    const auto& hl = app.chat_hits[i];
                    if (hl.line != line)
                        continue;
                    if (hl.segs.empty() ||
                        (cp >= hl.segs.front().cp0 && cp <= hl.segs.back().cp1)) {
                        idx = i;
                        break;
                    }
                }
                const int tgt = idx + (up ? -1 : 1);
                if (idx >= 0 && tgt >= 0 && tgt < (int)app.chat_hits.size()) {
                    const auto& cur = app.chat_hits[idx];
                    const auto& tl = app.chat_hits[tgt];
                    const int cur_base = cur.segs.empty() ? 0 : cur.segs.front().cp0;
                    const int span = tl.segs.empty() ? 0
                                                    : tl.segs.back().cp1 -
                                                          tl.segs.front().cp0;
                    const int off = cp - cur_base;
                    app.sel_caret = app.doc_start[tl.line] +
                                    (off < span ? off : span);
                } else {
                    const int len = cp_count(app.doc_lines[line]);
                    app.sel_caret = up ? app.doc_start[line]
                                       : app.doc_start[line] + len;
                }
            }
            if (pressed(kn.a, kprev.a)) {
                app.sel_anchor = 0;
                app.sel_caret = app.doc_len;
            }
            if (pressed(kn.c, kprev.c) && app.sel_anchor >= 0 &&
                app.sel_caret >= 0 && app.sel_anchor != app.sel_caret) {
                const int lo = app.sel_anchor < app.sel_caret ? app.sel_anchor
                                                             : app.sel_caret;
                const int hi = app.sel_anchor < app.sel_caret ? app.sel_caret
                                                             : app.sel_anchor;
                const std::string txt = chat_extract(app, lo, hi);
                if (!txt.empty())
                    glfwSetClipboardString(win, txt.c_str());  // raw UTF-8
            }
            if (pressed(kn.esc, kprev.esc)) {
                app.sel_anchor = app.sel_caret = -1;
                app.chat_focused = false;
            }
            if (!shift) {  // plain scroll keys (Shift+Home/End select instead)
                if (pressed(kn.pgup, kprev.pgup)) {
                    app.follow_bottom = false;
                    app.scroll_cmd = 1;
                }
                if (pressed(kn.pgdn, kprev.pgdn)) {
                    app.follow_bottom = false;
                    app.scroll_cmd = 2;
                }
                if (pressed(kn.home, kprev.home)) {
                    app.follow_bottom = false;
                    app.scroll_cmd = 3;
                }
                if (pressed(kn.end, kprev.end))
                    app.scroll_cmd = 4;
            }
            kprev = kn;
        }

        // Spectrum feed: live mic while listening, synthetic animation
        // otherwise; then shift one column into the history ring and check
        // whether the active shader file changed on disk.
        if ((app.worker == AppState::Worker::Listening || app.stt.streaming()) &&
            app.stt.latest(spec_window.data(), spec_window.size()) == spec_window.size())
            spec.feed(spec_window.data());
        else
            spec.synthetic(glfwGetTime() - t0);
        spec.advance();
        fx.check_reload();

        nk_glfw3_new_frame(&device);
        struct nk_context* ctx = &device.ctx;

        // ---- main surface (transparent: the shader shows through) ----
        // NK_WINDOW_BACKGROUND is essential: this window covers the whole
        // viewport, so without the flag any click (e.g. the chat input below a
        // modal) moves it to the top of Nuklear's stack; its fullscreen bounds
        // then contain every point, so the modals above can never regain the
        // top and Nuklear marks them NK_WINDOW_ROM - every widget inside,
        // including the title-bar X, stops responding (frozen-modal bug).
        nk_begin(ctx, "Krastor", nk_rect(0, 0, (float)w, (float)h),
                 NK_WINDOW_BACKGROUND);
        {
            // A chat click requested keyboard focus: drop the input box's edit
            // focus (the edit lives in this window, so unfocus must run here).
            if (app.clear_edit_focus) {
                nk_edit_unfocus(ctx);
                app.clear_edit_focus = false;
            }
            // All fixed row heights scale with the font so the layout follows
            // the Ctrl +/- zoom.
            const float fh = ctx->style.font && ctx->style.font->height > 0.0f
                                 ? ctx->style.font->height : 18.0f;
            // Compact top bar: short buttons (the icons size themselves from
            // the button height) and columns only as wide as the labels need,
            // so the chat gets the rest.
            const float top_h = fh * 1.7f;
            const float input_h = fh * 3.7f;
            const float status_h = fh * 1.2f;
            const float chip_h = fh * 1.45f;
            const float fab_sz = fh * 1.45f;  // floating action button size

            // Top bar: title | session quick switcher | hands-free | speak | settings.
            // The three action buttons are icon-only (centered, sized from the
            // row height): no labels, no overlap, consistent at any font zoom.
            nk_layout_row_begin(ctx, NK_DYNAMIC, top_h, 5);
            nk_layout_row_push(ctx, 0.22f);
            nk_label(ctx, app.name.c_str(), NK_MIDDLE_LEFT);
            nk_layout_row_push(ctx, 0.40f);
            {
                // One-click switch over the most recent sessions; the combo
                // lives inside the main window, so it cannot ROM the UI the
                // way a child window would (see the fab comment below).
                if (nk_combo_begin_label(ctx, app.session_title.c_str(),
                                         nk_vec2(360.0f, 280.0f))) {
                    nk_layout_row_dynamic(ctx, 24, 1);
                    int shown = 0;
                    for (const auto& s : app.store.list()) {
                        if (++shown > 10)
                            break;
                        const std::string item = s.title + "  -  " +
                            (s.updated.size() >= 16 ? s.updated.substr(0, 16)
                                                    : s.updated);
                        if (nk_combo_item_label(ctx, item.c_str(), NK_TEXT_LEFT) &&
                            s.id != app.session_id)
                            app.switch_to_session(s.id);
                    }
                    if (nk_combo_item_label(ctx, "+ new session", NK_TEXT_LEFT))
                        app.new_session();
                    nk_combo_end(ctx);
                }
            }
            // Tooltips for the icon-only buttons: hover + slot rect are
            // captured here, the bubble is drawn at the end of the frame
            // (same place as the FAB tooltip) so the chat group cannot
            // paint over it.
            int top_tip = -1;
            struct nk_rect top_tip_rect = {0, 0, 0, 0};
            const char* top_tip_text = "";
            auto top_hover = [&](int i, bool hov, const struct nk_rect& rc,
                                 const char* tip) {
                if (hov) {
                    top_tip = i;
                    top_tip_rect = rc;
                    top_tip_text = tip;
                }
            };
            bool hov = false;
            struct nk_rect rc;
            nk_layout_row_push(ctx, 0.12f);
            if (button_icon_only(ctx, app.icons,
                                 app.hands_free() ? "mic" : "mic-off", "HF",
                                 &hov, &rc)) {
                if (app.hands_free())
                    app.stop_hands_free();
                else
                    app.start_hands_free();
            }
            top_hover(0, hov, rc,
                      app.hands_free() ? "Hands-free: ON (/mic)"
                                       : "Hands-free: OFF (/mic)");
            nk_layout_row_push(ctx, 0.12f);
            if (button_icon_only(ctx, app.icons,
                                 app.auto_speak ? "volume-2" : "volume-x",
                                 "SPK", &hov, &rc)) {
                app.auto_speak = !app.auto_speak;
                app.cfg.tts.auto_speak = app.auto_speak;
                app.cfg.save();
            }
            top_hover(1, hov, rc,
                      app.auto_speak ? "Voice output: ON (/voice)"
                                     : "Voice output: OFF (/voice)");
            nk_layout_row_push(ctx, 0.12f);
            if (button_icon_only(ctx, app.icons, "settings", "Cfg", &hov, &rc))
                app.settings.open = !app.settings.open;
            top_hover(2, hov, rc, "Settings");
            nk_layout_row_end(ctx);

            // Chat history: this Nuklear has no "fill remaining" row, so the
            // group height is the window height minus the other rows (all
            // font-proportional) plus row spacing/padding.
            const float attach_h = app.attachments.empty() ? 0.0f : chip_h + 4.0f;
            const float chat_h = (float)h - top_h - input_h - status_h -
                                 attach_h - 40.0f;
            // Floating action button strip: lower-right corner, floating over
            // the chat just above the input box. Computed before chat_panel so
            // the selection handler can ignore clicks on it.
            const float fab_gap = 6.0f, fab_pad = 6.0f;
            const struct nk_rect fab_strip = nk_rect(
                (float)w - (7.0f * fab_sz + 6.0f * fab_gap + 2.0f * fab_pad) - 10.0f,
                (float)h - (status_h + input_h + attach_h + 14.0f) -
                    (fab_sz + 2.0f * fab_pad),
                7.0f * fab_sz + 6.0f * fab_gap + 2.0f * fab_pad,
                fab_sz + 2.0f * fab_pad);
            app.fab_rect = fab_strip;
            chat_panel(ctx, app, chat_h > 60.0f ? chat_h : 60.0f);

            // ---- floating action buttons (icon-only, tooltips on hover) ----
            // Drawn directly into the main window's command buffer at absolute
            // coordinates. A separate Nuklear window would permanently ROM the
            // main window (4.13.3 gives input only to the topmost window, and
            // the full-size main window always overlaps any child window),
            // which killed every other button in the UI.
            {
                nk_fill_rect(&ctx->current->buffer, fab_strip, 8.0f,
                             nk_rgba(22, 24, 32, 225));
                struct FabBtn {
                    const char* icon;  // data/icons/<name>.svg (preferred)
                    char32_t cp;       // emoji fallback when the SVG is missing
                    const char* tip;
                    const char* fb;
                    bool enabled;
                };
                const bool busy = app.worker != AppState::Worker::None;
                const FabBtn fabs[7] = {
                    {"send", 0x1F680, "Send", "Send", !busy},
                    {"paperclip", 0x1F4CE, "Attach image from clipboard", "Img", true},
                    {"ear", 0x1F442, "Listen (mic, max 10 s)", "Mic", !busy},
                    {"volume-2", 0x1F3B5, "Test voice", "Voice", true},
                    {"eraser", 0x1F9F9, "Clear screen (keeps the session)", "Clr", true},
                    {"help", 0x2753, "Help: commands & shortcuts (F1)", "Help", true},
                    {"square", 0x274C, "Stop: cancel request + silence", "Stop", true},
                };
                bool fab_click[7] = {false, false, false, false, false, false, false};
                int fab_hover = -1;
                for (int i = 0; i < 7; ++i) {
                    const struct nk_rect slot = nk_rect(
                        fab_strip.x + fab_pad + (float)i * (fab_sz + fab_gap),
                        fab_strip.y + fab_pad, fab_sz, fab_sz);
                    nk_flags state = 0;
                    // Icon source chain: SVG atlas -> emoji atlas -> text.
                    const IconAtlas::Rect* ir =
                        app.icons.ok() ? app.icons.find(fabs[i].icon) : nullptr;
                    const EmojiAtlas::Rect* r =
                        app.emoji.ok() ? app.emoji.find(fabs[i].cp) : nullptr;
                    if (ir) {
                        fab_click[i] = nk_do_button_image(
                            &state, &ctx->current->buffer, slot,
                            atlas_image(app.icons.texture(), app.icons.tex_w(),
                                        app.icons.tex_h(), ir->x, ir->y, ir->w,
                                        ir->h),
                            NK_BUTTON_DEFAULT, &ctx->style.button,
                            &ctx->input) != 0;
                    } else if (r) {
                        fab_click[i] = nk_do_button_image(
                            &state, &ctx->current->buffer, slot,
                            atlas_image(app.emoji.texture(), app.emoji.tex_w(),
                                        app.emoji.tex_h(), r->x, r->y, r->w,
                                        r->h),
                            NK_BUTTON_DEFAULT, &ctx->style.button,
                            &ctx->input) != 0;
                    } else {  // no atlas: plain text buttons
                        fab_click[i] = nk_do_button_text(
                            &state, &ctx->current->buffer, slot, fabs[i].fb,
                            (int)std::strlen(fabs[i].fb), NK_TEXT_CENTERED,
                            NK_BUTTON_DEFAULT, &ctx->style.button,
                            &ctx->input, ctx->style.font) != 0;
                    }
                    if (!fabs[i].enabled)  // busy: grey the icon out
                        nk_fill_rect(&ctx->current->buffer, slot, 3.0f,
                                     nk_rgba(16, 18, 24, 160));
                    if (nk_input_is_mouse_hovering_rect(&ctx->input, slot))
                        fab_hover = i;
                }
                // Tooltip: small rounded bubble centered above the button.
                if (fab_hover >= 0) {
                    const char* t = fabs[fab_hover].tip;
                    const int tl = (int)std::strlen(t);
                    const float tw = text_w_px(ctx->style.font, t, tl) + 18.0f;
                    const float th = fh * 1.4f;
                    const float cx = fab_strip.x + fab_pad +
                                     ((float)fab_hover + 0.5f) * (fab_sz + fab_gap);
                    float tx = cx - tw / 2.0f;
                    if (tx < 4.0f) tx = 4.0f;
                    if (tx + tw > (float)w - 4.0f) tx = (float)w - 4.0f - tw;
                    const struct nk_rect tr =
                        nk_rect(tx, fab_strip.y - th - 6.0f, tw, th);
                    nk_fill_rect(&ctx->current->buffer, tr, 4.0f,
                                 nk_rgba(16, 18, 24, 245));
                    nk_draw_text(&ctx->current->buffer,
                                 nk_rect(tr.x + (tr.w - text_w_px(ctx->style.font, t, tl)) / 2.0f,
                                         tr.y + (tr.h - fh) / 2.0f, tw, fh),
                                 t, tl, ctx->style.font, nk_rgba(0, 0, 0, 0),
                                 nk_rgb(232, 236, 242));
                }
                // Top-bar tooltip: same bubble as the FABs, below the hovered
                // action button (mic / speak / settings).
                if (top_tip >= 0) {
                    const char* t = top_tip_text;
                    const int tl = (int)std::strlen(t);
                    const float tw = text_w_px(ctx->style.font, t, tl) + 18.0f;
                    const float th = fh * 1.4f;
                    float tx =
                        top_tip_rect.x + top_tip_rect.w / 2.0f - tw / 2.0f;
                    if (tx < 4.0f)
                        tx = 4.0f;
                    if (tx + tw > (float)w - 4.0f)
                        tx = (float)w - 4.0f - tw;
                    const struct nk_rect tr =
                        nk_rect(tx, top_tip_rect.y + top_tip_rect.h + 6.0f,
                                tw, th);
                    nk_fill_rect(&ctx->current->buffer, tr, 4.0f,
                                 nk_rgba(16, 18, 24, 245));
                    nk_draw_text(&ctx->current->buffer,
                                 nk_rect(tr.x + (tr.w - text_w_px(ctx->style.font, t, tl)) / 2.0f,
                                         tr.y + (tr.h - fh) / 2.0f, tw, fh),
                                 t, tl, ctx->style.font, nk_rgba(0, 0, 0, 0),
                                 nk_rgb(232, 236, 242));
                }
                if (fab_click[1])
                    app.attach_clipboard_image();
                if (fab_click[4])
                    app.clean_screen();
                if (fab_click[5])
                    app.help_open = !app.help_open;
                if (fab_click[6])
                    app.stop();
                if (fab_click[0] && app.worker == AppState::Worker::None &&
                    app.input[0]) {
                    app.send(app.input);
                    app.input[0] = '\0';
                }
                if (fab_click[2])
                    app.start_listen();
                if (fab_click[3])
                    app.speak_async("Ciao, la voce funziona.");
            }

            // Attachment chips: one per pasted image, click a chip to remove it.
            if (!app.attachments.empty()) {
                nk_layout_row_begin(ctx, NK_STATIC, chip_h, (int)app.attachments.size());
                size_t remove_idx = SIZE_MAX;
                for (size_t i = 0; i < app.attachments.size(); ++i) {
                    // With the icon atlas the leading "[x]" becomes the x
                    // icon; without it the text hint stays (same fallback
                    // chain as every other icon button).
                    const bool has_x = app.icons.ok() && app.icons.find("x");
                    char chip[64];
                    std::snprintf(chip, sizeof(chip), has_x ? "%dx%d" : "[x] %dx%d",
                                  app.attachments[i].width, app.attachments[i].height);
                    nk_layout_row_push(ctx, fh * 6.5f);
                    if (button_icon(ctx, app.icons, "x", chip))
                        remove_idx = i;
                }
                nk_layout_row_end(ctx);
                if (remove_idx != SIZE_MAX)
                    app.attachments.erase(app.attachments.begin() + (long)remove_idx);
            }

            // Input: OpenCode-style full-width box - dark field, blue accent
            // bar on the left edge (Enter sends, Shift+Enter newline).
            nk_layout_row_begin(ctx, NK_DYNAMIC, input_h, 1);
            nk_layout_row_push(ctx, 1.0f);
            // Slot rect of the next (full-width) widget: 4.13.3 has no
            // nk_window_get_cursor, but after row_begin layout->at_y is the
            // row's top and the first column starts at the content padding.
            const struct nk_rect input_box = nk_rect(
                ctx->current->bounds.x + ctx->style.window.padding.x,
                ctx->current->layout->at_y, nk_window_get_width(ctx), input_h);
            nk_style_push_color(ctx, &ctx->style.edit.border_color, nk_rgb(45, 50, 62));
            nk_style_push_style_item(ctx, &ctx->style.edit.normal,
                                     nk_style_item_color(nk_rgba(9, 11, 16, 255)));
            nk_style_push_style_item(ctx, &ctx->style.edit.hover,
                                     nk_style_item_color(nk_rgba(9, 11, 16, 255)));
            nk_style_push_style_item(ctx, &ctx->style.edit.active,
                                     nk_style_item_color(nk_rgba(12, 15, 21, 255)));
            nk_style_push_color(ctx, &ctx->style.edit.text_normal, nk_rgb(232, 236, 242));
            nk_style_push_color(ctx, &ctx->style.edit.cursor_normal, nk_rgb(120, 180, 255));
            nk_style_push_color(ctx, &ctx->style.edit.cursor_text_normal,
                                nk_rgb(120, 180, 255));
            nk_style_push_vec2(ctx, &ctx->style.edit.padding, nk_vec2(10.0f, 6.0f));
            // Visible caret: 2 px wide accent-blue bar (Nuklear's default is a
            // 1 px line that is nearly invisible on the dark field).
            nk_style_push_float(ctx, &ctx->style.edit.cursor_size, 2.0f);
            const nk_flags edit_state = nk_edit_string_zero_terminated(
                ctx,
                (nk_flags)NK_EDIT_BOX | (nk_flags)NK_EDIT_SIG_ENTER |
                    (nk_flags)NK_EDIT_CTRL_ENTER_NEWLINE,
                app.input, sizeof(app.input) - 1, nk_filter_default);
            // Accent bar painted over the field's left edge (after the widget,
            // so it sits on top of its background).
            nk_fill_rect(&ctx->current->buffer,
                         nk_rect(input_box.x, input_box.y, 3.0f, input_box.h),
                         0, nk_rgb(120, 180, 255));
            nk_style_pop_float(ctx);       // cursor_size
            nk_style_pop_vec2(ctx);
            nk_style_pop_color(ctx);      // cursor_text_normal
            nk_style_pop_color(ctx);      // cursor_normal
            nk_style_pop_color(ctx);      // text_normal
            nk_style_pop_style_item(ctx); // active
            nk_style_pop_style_item(ctx); // hover
            nk_style_pop_style_item(ctx); // normal
            nk_style_pop_color(ctx);      // border_color
            // Typing focus back on the input releases the chat selection keys.
            if ((edit_state & NK_EDIT_ACTIVE) != 0)
                app.chat_focused = false;
            // A clipboard image was "pasted" while this field was focused.
            if (app.pending_image_paste) {
                app.pending_image_paste = false;
                app.attach_clipboard_image();
            }
            nk_layout_row_end(ctx);

            // Enter sends (the action buttons live in the floating cluster
            // drawn after nk_end).
            if ((edit_state & NK_EDIT_COMMITED) != 0 &&
                app.worker == AppState::Worker::None) {
                app.send(app.input);
                app.input[0] = '\0';
            }

            // Status line + context usage footer.
            nk_layout_row_dynamic(ctx, status_h, 1);
            std::string status_buf;
            if (app.worker == AppState::Worker::Thinking)
                status_buf = "thinking...";
            else if (app.worker == AppState::Worker::Compacting)
                status_buf = "compacting session...";
            else if (app.worker == AppState::Worker::Listening)
                status_buf = "listening... speak now (max 10 s)";
            else if (app.speaking.load()) status_buf = "speaking...";
            else if (app.hands_free()) {
                status_buf = "hands-free: listening - just speak"
                             "  (ignore while busy: ";
                status_buf += cfg.agent.ignore_while_speaking ? "on" : "off";
                status_buf += ")";
            }
            else if (const Voice* v = voices.current()) {
                status_buf = "voice: " + v->id + "  |  mic: " +
                             (cfg.stt.input_device < 0 ? std::string("default")
                                                       : std::to_string(cfg.stt.input_device)) +
                             "  (CLI /devices, /mic)";
            }
            // ctx: <last prompt tokens> / <model window> — the last request's
            // prompt size is the current context occupancy. Amber past 85%.
            {
                const int total = cfg.llm.context_size > 0 ? cfg.llm.context_size : 1;
                const int pct = static_cast<int>(100.0 * app.last_prompt_tokens / total);
                if (!status_buf.empty())
                    status_buf += "  |  ";
                status_buf += "ctx: " + fmt_thousands(app.last_prompt_tokens) +
                              " / " + fmt_thousands(total) +
                              " (" + std::to_string(pct) + "%)";
            }
            if (100.0 * app.last_prompt_tokens > 85.0 * cfg.llm.context_size)
                nk_label_colored(ctx, status_buf.c_str(), NK_MIDDLE_LEFT,
                                 nk_rgb(220, 180, 90));
            else
                nk_label(ctx, status_buf.c_str(), NK_MIDDLE_LEFT);
        }
        nk_end(ctx);

        settings_modal(ctx, cfg, voices, app.settings, w, h, fx, app.models,
                       app.icons);
        // Session manager (list/switch/new/rename/delete, sessions_plan.md).
        session_modal(ctx, app, w, h);
        // Help overlay (F1 or /help): commands + shortcuts.
        help_modal(ctx, app, w, h);
        // External-tool permission dialog (blocks the worker until answered).
        permission_modal(ctx, app, w, h);

        // A saved name change propagates to the top bar and the window title
        // (the title also follows the current session).
        if (cfg.agent.name != app.name)
            app.name = cfg.agent.name;
        {
            static std::string last_win_title;
            const std::string want = app.name + " \xE2\x80\x94 " + app.session_title;
            if (want != last_win_title) {
                glfwSetWindowTitle(win, want.c_str());
                last_win_title = want;
            }
        }

        // ---- render: shader background, then the UI ----
        int fb_w = 0, fb_h = 0;
        glfwGetFramebufferSize(win, &fb_w, &fb_h);
        glViewport(0, 0, fb_w, fb_h);
        glClearColor(0.04f, 0.05f, 0.07f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        // Shader event feed: audio + assistant state + input activity.
        if (app.input != app.input_prev) {  // typed/pasted/edited this frame
            app.typing_pulse = 1.0f;
            app.input_prev = app.input;
        }
        app.typing_pulse = app.typing_pulse > 0.001f ? app.typing_pulse * 0.90f
                                                    : 0.0f;
        krastor::EffectInput fxin;
        fxin.level = spec.level();
        fxin.hist = spec.history();
        fxin.hist_w = SpectrumAnalyzer::kHistory;
        fxin.hist_bands = SpectrumAnalyzer::kBands;
        fxin.hist_head = spec.head();
        fxin.listening = app.worker == AppState::Worker::Listening ||
                         app.stt.streaming();
        fxin.speaking = app.speaking.load();
        fxin.thinking = app.worker == AppState::Worker::Thinking ||
                        app.worker == AppState::Worker::Compacting;
        fxin.typing = app.typing_pulse;
        fxin.intensity = static_cast<float>(cfg.gui.effect_intensity);
        fx.draw(fb_w, fb_h, float(glfwGetTime() - t0), fxin);
        nk_glfw3_render(&device, NK_ANTI_ALIASING_ON, 512 * 1024, 128 * 1024);

        glfwSwapBuffers(win);
    }

    // Final save so a resize (or any debounced change) in the last 500 ms
    // before a clean exit is not lost.
    try {
        cfg.save();
    } catch (const std::exception&) {
    }

    nk_glfw3_shutdown(&device);
    glfwTerminate();
    return 0;
}

// ---- --gui-test: modal stacking regression (headless Nuklear) ----
// Reproduces the frozen-settings-modal bug: the app's main window covers the
// whole viewport, so when a click lands on it (e.g. the chat input) while a
// modal is open, Nuklear's window activation moves the fullscreen window to
// the top of the stack; its bounds then contain every point, so the modal can
// never regain the top and gets marked NK_WINDOW_ROM - every widget inside it
// (including the title-bar X) stops responding. The fix is to submit the base
// window with NK_WINDOW_BACKGROUND. This test drives a real nk_context with
// synthetic mouse frames and asserts the modal button keeps receiving clicks.
namespace {

float stack_font_width(nk_handle, float, const char* text, int len)
{ return static_cast<float>(len) * 6.0f; }
void stack_font_query(nk_handle, float height, struct nk_user_font_glyph* glyph,
                      nk_rune, nk_rune)
{
    glyph->uv[0] = nk_vec2(0, 0);
    glyph->uv[1] = nk_vec2(1, 1);
    glyph->offset = nk_vec2(0, 0);
    glyph->width = 6.0f;
    glyph->height = height;
    glyph->xadvance = 6.0f;
}

} // namespace

int run_gui_stack_test()
{
    int failed = 0;
    auto check = [&failed](bool ok, const char* what) {
        std::printf("%s: %s\n", ok ? "ok  " : "FAIL", what);
        if (!ok)
            ++failed;
    };

    struct nk_context ctx;
    struct nk_user_font font;
    nk_zero(&font, sizeof(font));
    font.height = 12.0f;
    font.width = stack_font_width;
    font.query = stack_font_query;
    if (!nk_init_default(&ctx, &font)) {
        std::printf("FAIL: nk_init_default\n");
        return 1;
    }
    nk_style_set_font(&ctx, &font);

    int hits = 0;
    // One UI pass: fullscreen base window + modal with one big button.
    auto ui_frame = [&]() {
        nk_begin(&ctx, "Base", nk_rect(0, 0, 800, 600), NK_WINDOW_BACKGROUND);
        nk_end(&ctx);
        nk_begin(&ctx, "Modal", nk_rect(300, 100, 200, 300),
                 NK_WINDOW_TITLE | NK_WINDOW_BORDER | NK_WINDOW_CLOSABLE);
        nk_layout_row_dynamic(&ctx, 200, 1);
        if (nk_button_label(&ctx, "hit"))
            ++hits;
        nk_end(&ctx);
        nk_clear(&ctx);  // ends the frame (advances ctx->seq, like the backends do)
    };
    // A real click spans two frames: Nuklear 4.13 buttons trigger on press
    // (nk_input_is_mouse_pressed requires down && clicked in the same frame).
    auto click_at = [&](int x, int y) {
        nk_input_begin(&ctx);
        nk_input_motion(&ctx, x, y);
        nk_input_button(&ctx, NK_BUTTON_LEFT, x, y, nk_true);
        nk_input_end(&ctx);
        ui_frame();
        nk_input_begin(&ctx);
        nk_input_motion(&ctx, x, y);
        nk_input_button(&ctx, NK_BUTTON_LEFT, x, y, nk_false);
        nk_input_end(&ctx);
        ui_frame();
    };

    click_at(400, 250);   // sanity: modal button responds while on top
    check(hits == 1, "modal button responds before the trigger click");
    click_at(50, 550);    // click the base window outside the modal (trigger)
    click_at(400, 250);   // modal must STILL respond (before the fix: ROM)
    check(hits == 2, "modal still responds after clicking the fullscreen base");
    click_at(50, 550);
    click_at(400, 250);
    check(hits == 3, "modal keeps responding after repeated base clicks");

    nk_free(&ctx);
    std::printf("gui-test: %d failed - %s\n", failed, failed ? "FAIL" : "PASS");
    return failed ? 1 : 0;
}

} // namespace krastor
