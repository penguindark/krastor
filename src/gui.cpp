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
#include <mutex>
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
#include "stt.h"
#include "tts.h"
#include "voices.h"
#include "ansi.h"
#include "clipboard.h"
#include "effects.h"
#include "audio_analysis.h"

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
    std::string thinking;    // raw reasoning text ("" = no block)
    std::string think_meta;  // duration suffix, e.g. "5.4s"
    bool thought_open = false;
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

struct AppState {
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
    std::shared_ptr<CancelToken> llm_cancel;  // Stop button aborts the request
    std::future<std::string> stt_fut;
    std::atomic<bool> speaking{false};
    bool auto_speak = true;
    int last_prompt_tokens = 0;  // context occupancy = last request's prompt size
    bool follow_bottom = true;   // chat auto-scrolls to newest unless the user
                                 // scrolled back up (wheel/keys clear the flag)

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

    AppState(Config& c, VoiceRegistry& v)
        : cfg(c), voices(v), conv(c.llm.system_prompt), llm(c.llm), tts(v, c), stt(c),
          fmt(c)
    {
        name = c.agent.name;
        // Model list for the Settings combo (URL/key/context come from this file).
        models.load(LlmModelRegistry::default_path());
        // Syntax highlighting for fenced code blocks (data/languages/*.json).
        if (hl.load_default())
            fmt.set_highlighter([this](const std::string& code,
                                       const std::string& lang) {
                return hl.highlight(code, lang);
            });
        // Color emoji sprite atlas (generated by tools/gen_emoji_atlas.py).
        emoji.load_default();
        lines.push_back({"sys", name + " ready. Type below (Enter sends, "
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
        llm_cancel = std::make_shared<CancelToken>();
        auto cancel = llm_cancel;
        llm_fut = std::async(std::launch::async, [client, snapshot, cancel] {
            const auto t_start = std::chrono::steady_clock::now();
            LlmReply r = client.complete(snapshot, cancel.get());
            r.elapsed_ms = std::chrono::duration<double, std::milli>(
                               std::chrono::steady_clock::now() - t_start).count();
            return r;
        });
    }

    // Stop button: abort the in-flight LLM request immediately and cut any
    // TTS playback. Safe to press at any time (no-ops when idle).
    void stop()
    {
        if (worker == Worker::Thinking && llm_cancel) {
            llm_cancel->cancel();  // makes the blocking HTTP call return now
            lines.push_back({"sys", "(stopped)"});
        }
        tts.interrupt();  // no-op when nothing is playing
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

    ~AppState() { stop_hands_free(); }

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
                ChatLine line{"krastor", fmt.format(reply.content), ""};
                if (!reply.reasoning.empty()) {
                    line.thinking = reply.reasoning;
                    line.think_meta = fmt_elapsed(reply.elapsed_ms);
                }
                // Stats sub-line: "model - time - tok/s" ("~" when the server
                // omitted usage and the token counts are estimated).
                const double secs =
                    reply.elapsed_ms > 1.0 ? reply.elapsed_ms / 1000.0 : 0.001;
                char rate[48];
                std::snprintf(rate, sizeof(rate), "%s%.1f tok/s",
                              reply.tokens_estimated ? "~" : "",
                              reply.completion_tokens / secs);
                line.meta = reply.model + " - " + fmt_elapsed(reply.elapsed_ms) +
                            " - " + rate;
                last_prompt_tokens = reply.prompt_tokens;
                lines.push_back(std::move(line));
                if (auto_speak)
                    speak_async(reply.content);
            } else {
                lines.push_back({"sys", "LLM error: " + reply.error});
            }
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
bool settings_modal(struct nk_context* ctx, Config& cfg, VoiceRegistry& voices,
                    SettingsState& s, int win_w, int win_h, EffectEngine& fx,
                    const LlmModelRegistry& models)
{
    if (!s.open)
        return false;

    const int mw = 520, mh = 620;
    // The modal needs its own opaque background: the main window's
    // fixed_background is fully transparent so the shader shows through.
    nk_style_push_style_item(ctx, &ctx->style.window.fixed_background,
                             nk_style_item_color(nk_rgba(16, 18, 24, 235)));
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
        nk_style_pop_style_item(ctx);
        s.open = false;
        s.status.clear();
        return false;
    }

    // Vertical breathing room between the fields (the default 4 px makes the
    // label/input pairs and the property rows visually collide).
    nk_style_push_vec2(ctx, &ctx->style.window.spacing, nk_vec2(4.0f, 8.0f));

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
    nk_layout_row_dynamic(ctx, 24, 1);
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
    if (nk_button_label(ctx, "Save")) {
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
    if (nk_button_label(ctx, "Close")) {
        s.open = false;
        s.status.clear();
    }

    nk_style_pop_vec2(ctx);
    nk_end(ctx);
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
        int lcp = 0;  // codepoints consumed within this run
        while (i < r.text.size()) {
            const char ch = r.text[i];
            if (ch == '\n') {
                toks.push_back({"", fg, bg, true, prefix_run ? -1 : lcp});
                ++i;
                ++lcp;
            } else if (ch == ' ') {
                size_t j = i;
                while (j < r.text.size() && r.text[j] == ' ')
                    ++j;
                toks.push_back({r.text.substr(i, j - i), fg, bg, false,
                                prefix_run ? -1 : lcp});
                lcp += static_cast<int>(j - i);
                i = j;
            } else {
                size_t j = i;
                while (j < r.text.size() && r.text[j] != ' ' && r.text[j] != '\n')
                    ++j;
                toks.push_back({r.text.substr(i, j - i), fg, bg, false,
                                prefix_run ? -1 : lcp});
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
        ChatHitLine hit_line;
        hit_line.y = y;
        hit_line.h = row_h;
        hit_line.line = src_line;
        float tx = area.x;
        for (const auto& w : ln) {
            const float ww = text_w_px(f, w.text.data(), (int)w.text.size());
            const int ncp = w.cp_begin >= 0 ? cp_count(w.text) : 0;
            if (!uniform && w.bg.a)
                nk_fill_rect(canvas, nk_rect(tx, y, ww, row_h), 0, w.bg);
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
                        nk_fill_rect(canvas,
                                     nk_rect(tx + (ha - a) * px, y,
                                             (hb - ha) * px, row_h),
                                     0, nk_rgba(60, 110, 200, 115));
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
        const bool modal_blocks = app.settings.open;

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
            // A click on a "Thought" header toggles that block and is not a
            // selection click.
            bool toggled = false;
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
        for (const auto& l : app.lines) {
            const std::string prefix = l.who == "you" ? "> " :
                                       l.who == "krastor" ? app.name + ": " : "! ";
            struct nk_color col = nk_rgb(160, 160, 160);
            if (l.who == "you") col = nk_rgb(120, 180, 255);
            else if (l.who == "krastor") col = nk_rgb(130, 220, 150);
            // Collapsible reasoning block (opencode-style "Thought · Ns").
            if (!l.thinking.empty()) {
                std::string head = std::string(l.thought_open ? "- " : "+ ") +
                                   "Thought";
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
        // would scroll the outer window). The bottom offset is computed
        // explicitly instead of relying on Nuklear's clamp, which stops running
        // once the scrollbar auto-hides. Scrolling up with the wheel leaves the
        // view where the user left it; reaching the bottom re-enables follow.
        const float visible = height - 2.0f * ctx->style.window.padding.y - 8.0f;
        const float bottom = content_h > visible ? content_h - visible : 0.0f;
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
        } else {
            nk_uint sx = 0, sy = 0;
            nk_group_get_scroll(ctx, "##chat", &sx, &sy);
            if ((float)sy >= bottom - 4.0f)
                app.follow_bottom = true;
        }
        nk_group_end(ctx);
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
    // general punctuation (curly quotes, dashes, ellipsis) + box drawing
    // (the │ ─ bars used by quotes/tables/rules). Without this Nuklear bakes
    // ASCII only and everything else renders as "?".
    static const nk_rune kGlyphRanges[] = {
        32, 0x00FF, 0x2000, 0x205B, 0x2500, 0x257F, 0
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

        // Window resized by the user: remember it (debounced, like font zoom).
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
        nk_begin(ctx, "Krastor", nk_rect(0, 0, (float)w, (float)h), 0);
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
            const float top_h = fh * 2.4f;
            const float input_h = fh * 3.7f;
            const float status_h = fh * 1.2f;
            const float chip_h = fh * 1.45f;
            const float fab_sz = fh * 1.45f;  // floating action button size

            // Top bar: title | hands-free | speak toggle | settings
            nk_layout_row_begin(ctx, NK_DYNAMIC, top_h, 4);
            nk_layout_row_push(ctx, 0.34f);
            nk_label(ctx, app.name.c_str(), NK_MIDDLE_LEFT);
            nk_layout_row_push(ctx, 0.28f);
            if (nk_button_label(ctx, app.hands_free() ? "Hands-free: ON"
                                                      : "Hands-free: OFF")) {
                if (app.hands_free())
                    app.stop_hands_free();
                else
                    app.start_hands_free();
            }
            nk_layout_row_push(ctx, 0.18f);
            if (nk_button_label(ctx, app.auto_speak ? "Speak: ON" : "Speak: OFF"))
                app.auto_speak = !app.auto_speak;
            nk_layout_row_push(ctx, 0.20f);
            if (nk_button_label(ctx, "Settings"))
                app.settings.open = !app.settings.open;
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
                (float)w - (5.0f * fab_sz + 4.0f * fab_gap + 2.0f * fab_pad) - 10.0f,
                (float)h - (status_h + input_h + attach_h + 14.0f) -
                    (fab_sz + 2.0f * fab_pad),
                5.0f * fab_sz + 4.0f * fab_gap + 2.0f * fab_pad,
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
                    char32_t cp;
                    const char* tip;
                    const char* fb;
                    bool enabled;
                };
                const bool busy = app.worker != AppState::Worker::None;
                const FabBtn fabs[5] = {
                    {0x1F680, "Send", "Send", !busy},
                    {0x1F4CE, "Attach image from clipboard", "Img", true},
                    {0x1F442, "Listen (mic, max 10 s)", "Mic", !busy},
                    {0x1F3B5, "Test voice", "Voice", true},
                    {0x274C, "Stop: cancel request + silence", "Stop", true},
                };
                bool fab_click[5] = {false, false, false, false, false};
                int fab_hover = -1;
                for (int i = 0; i < 5; ++i) {
                    const struct nk_rect slot = nk_rect(
                        fab_strip.x + fab_pad + (float)i * (fab_sz + fab_gap),
                        fab_strip.y + fab_pad, fab_sz, fab_sz);
                    nk_flags state = 0;
                    const EmojiAtlas::Rect* r =
                        app.emoji.ok() ? app.emoji.find(fabs[i].cp) : nullptr;
                    if (r) {
                        struct nk_image img =
                            nk_image_id(static_cast<int>(app.emoji.texture()));
                        img.w = static_cast<nk_ushort>(app.emoji.tex_w());
                        img.h = static_cast<nk_ushort>(app.emoji.tex_h());
                        img.region[0] = static_cast<nk_ushort>(r->x);
                        img.region[1] = static_cast<nk_ushort>(r->y);
                        img.region[2] = static_cast<nk_ushort>(r->w);
                        img.region[3] = static_cast<nk_ushort>(r->h);
                        fab_click[i] = nk_do_button_image(
                            &state, &ctx->current->buffer, slot, img,
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
                if (fab_click[1])
                    app.attach_clipboard_image();
                if (fab_click[4])
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
                    char chip[64];
                    std::snprintf(chip, sizeof(chip), "[x] %dx%d",
                                  app.attachments[i].width, app.attachments[i].height);
                    nk_layout_row_push(ctx, fh * 6.5f);
                    if (nk_button_label(ctx, chip))
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

        settings_modal(ctx, cfg, voices, app.settings, w, h, fx, app.models);

        // A saved name change propagates to the top bar and the window title.
        if (cfg.agent.name != app.name) {
            app.name = cfg.agent.name;
            glfwSetWindowTitle(win, app.name.c_str());
        }

        // ---- render: shader background, then the UI ----
        int fb_w = 0, fb_h = 0;
        glfwGetFramebufferSize(win, &fb_w, &fb_h);
        glViewport(0, 0, fb_w, fb_h);
        glClearColor(0.04f, 0.05f, 0.07f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        fx.draw(fb_w, fb_h, float(glfwGetTime() - t0), spec.level(),
                spec.history(), SpectrumAnalyzer::kHistory, SpectrumAnalyzer::kBands,
                spec.head(), app.worker == AppState::Worker::Listening ||
                                 app.stt.streaming(),
                app.speaking.load(), static_cast<float>(cfg.gui.effect_intensity));
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

} // namespace krastor
