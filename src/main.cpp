// Krastor — LLM agent with voice input/output and real-time graphics interface.
// Phase 3: core agent CLI — config, OpenAI-compatible LLM chat loop and runtime
// TTS voice selection. STT/TTS audio and the GUI arrive in Phases 4/5.
// See project.md (master description) and src/development.md (this folder).

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include "clipboard.h"
#include "compactor.h"
#include "config.h"
#include "audio_fx.h"
#include "conversation.h"
#include "formatters.h"
#include "highlight.h"
#include "gui.h"
#include "icons.h"
#include "llm_client.h"
#include "llm_models.h"
#include "media.h"
#include "permissions.h"
#include "session_store.h"
#include "stt.h"
#include "tool_loop.h"
#include "tool_registry.h"
#include "tool_runner.h"
#include "tts.h"
#include "tts_text.h"
#include "voices.h"

namespace {

void print_help()
{
    std::cout <<
        "\nCommands:\n"
        "  /help              show this help\n"
        "  /voices            list available TTS voices\n"
        "  /voice <id>        switch TTS voice at runtime (e.g. /voice en-us-lessac)\n"
        "  /say <text>        speak the text with the current TTS voice\n"
        "  /listen [sec]      record the microphone and transcribe one utterance\n"
        "                     (Parakeet EN+IT auto-detect; default 20 s max)\n"
        "  /devices           list capture (microphone) devices with their index\n"
        "  /mic [sec] [idx]   live microphone level meter (diagnose no-signal)\n"
        "  /transcribe <file> transcribe an audio file (wav/mp3/flac)\n"
        "  /reset             clear the conversation (starts a new session)\n"
        "  /sessions          list saved sessions\n"
        "  /session list      same as /sessions\n"
        "  /session           show the current session\n"
        "  /session new [t]   start a new session (optional title)\n"
        "  /session open <x>  load a session (list index, id or id prefix)\n"
        "  /session delete <x>  delete a session permanently\n"
        "  /session rename <t>  rename the current session\n"
        "  /compact [focus]   summarize + shrink the session with the LLM\n"
        "  /agent             list the configured agents (config.jsonc)\n"
        "  /agent use <id>    switch agent (persona, model_ref, thinking, color)\n"
        "  /grants            list this session's always-allow tool permissions\n"
        "  /grants clear      revoke all always-allow tool permissions\n"
        "  /config            show the active LLM configuration\n"
        "  /exit              quit\n"
        "Anything else is sent to the LLM.\n\n";
}

void print_voice(const krastor::VoiceRegistry& voices)
{
    const auto* v = voices.current();
    std::cout << (v ? "current TTS voice: " + v->id + " [" + v->language + "]"
                    : "no TTS voice discovered")
              << '\n';
}

// ---- session command helpers (CLI, see sessions_plan.md) ----

// Resolve a `/session open|delete` argument: list index ("3" or "#3"),
// exact id, or unique id prefix. Empty = not found/ambiguous.
std::string resolve_session(const krastor::SessionStore& store,
                            const std::string& arg)
{
    std::string a = arg;
    if (!a.empty() && a[0] == '#')
        a.erase(0, 1);
    if (a.empty())
        return {};
    const auto l = store.list();
    if (std::all_of(a.begin(), a.end(),
                    [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; })) {
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

void print_sessions(const krastor::SessionStore& store)
{
    const auto l = store.list();
    if (l.empty()) {
        std::cout << "no sessions (" << store.dir() << ")\n";
        return;
    }
    std::cout << "sessions in " << store.dir() << ":\n";
    int i = 0;
    for (const auto& s : l)
        std::cout << "  " << ++i << "  " << s.id << "  " << s.updated << "  "
                  << s.messages << " msgs"
                  << (s.compactions ? "  compacted x" + std::to_string(s.compactions)
                                    : "")
                  << "  " << s.title << "\n";
}

// Trim leading whitespace/tabs of a command argument.
std::string trim_arg(std::string s)
{
    const size_t p = s.find_first_not_of(" \t");
    return p == std::string::npos ? std::string() : s.substr(p);
}

void list_voices(const krastor::VoiceRegistry& voices)
{
    if (voices.voices().empty()) {
        std::cout << "no voices found (expected models/tts-piper-* folders)\n";
        return;
    }
    std::cout << "TTS voices:\n";
    for (const auto& v : voices.voices())
        std::cout << (v.id == (voices.current() ? voices.current()->id : "") ? " * " : "   ")
                  << v.id << "  (" << v.language << ")\n";
}

// Headless self-test for the SVG icon pipeline (LunaSVG + IconAtlas):
// rasterizes data/icons/*.svg without GL, asserts every icon drew a sane
// amount of ink (line-art strokes: not blank, not a filled square) and
// dumps the atlas to icon_atlas_test.png for visual inspection.
// Exit code 0 = all good, 1 = failure.
int run_icon_test()
{
    krastor::IconAtlas icons;
    const std::string exe_dir = krastor::Config::exe_dir();
    bool loaded = icons.rasterize(exe_dir + "/data/icons");
    if (!loaded)
        loaded = icons.rasterize("data/icons");
    if (!loaded) {
        std::cerr << "icon-test: FAIL - no icons found in data/icons\n";
        return 1;
    }
    std::cout << "icon-test: " << icons.names().size() << " icons, atlas "
              << icons.tex_w() << "x" << icons.tex_h() << '\n';
    int failures = 0;
    for (const auto& name : icons.names()) {
        const double cov = icons.coverage(name);
        const bool sane = cov > 0.02 && cov < 0.75;
        std::printf("  %-12s coverage %5.1f%%  %s\n", name.c_str(),
                    cov * 100.0, sane ? "OK" : "FAIL");
        if (!sane)
            ++failures;
    }
    const std::string png = "icon_atlas_test.png";
    if (icons.dump_png(png))
        std::cout << "icon-test: atlas written to " << png << '\n';
    else
        std::cout << "icon-test: WARNING - could not write the atlas PNG\n";
    if (failures) {
        std::cerr << "icon-test: FAIL - " << failures << " icon(s) out of range\n";
        return 1;
    }
    std::cout << "icon-test: PASS\n";
    return 0;
}

float rms(const float* p, size_t n)
{
    double s = 0.0;
    for (size_t i = 0; i < n; ++i)
        s += static_cast<double>(p[i]) * p[i];
    return n ? static_cast<float>(std::sqrt(s / n)) : 0.0f;
}

// Minimal 44-byte-header IEEE-float mono WAV writer (test artifacts).
bool write_wav(const char* path, const std::vector<float>& samples, int rate)
{
    std::FILE* f = std::fopen(path, "wb");
    if (!f)
        return false;
    const uint32_t data = static_cast<uint32_t>(samples.size() * 4);
    auto put32 = [&](uint32_t v) { std::fwrite(&v, 1, 4, f); };
    auto put16 = [&](uint16_t v) { std::fwrite(&v, 1, 2, f); };
    std::fwrite("RIFF", 1, 4, f);
    put32(36 + data);
    std::fwrite("WAVEfmt ", 1, 8, f);
    put32(16); put16(3); put16(1);
    put32(static_cast<uint32_t>(rate));
    put32(static_cast<uint32_t>(rate) * 4);
    put16(4); put16(32);
    std::fwrite("data", 1, 4, f);
    put32(data);
    std::fwrite(samples.data(), 1, data, f);
    std::fclose(f);
    return true;
}

// Headless self-test for the input DSP chain (see src/audio_fx.h):
//  1. noise suppression: stationary noise must drop by >6 dB after RNNoise;
//  2. echo cancellation: a synthetic echo (far-end delayed by the nominal
//     pipeline offset) must drop by >6 dB after the Speex AEC converges.
// Dumps fx_test_in.wav / fx_test_out.wav in the CWD for listening.
// Exit code 0 = both PASS, 1 = failure.
int run_fx_test()
{
    auto& fx = krastor::AudioFx::instance();
    constexpr int kRate = 16000;
    constexpr int kFrame = 160;  // 10 ms
    int failures = 0;

    // ---- 1. noise suppression --------------------------------------------
    fx.set_echo_cancel(false);
    fx.set_denoise(true);
    {
        std::mt19937 rng(42);
        std::uniform_real_distribution<float> noise(-0.02f, 0.02f);
        auto gen = [&](int i) {
            float s = noise(rng);  // stationary hiss over the whole clip
            if (i >= kRate / 2 && i < 2 * kRate + kRate / 2) {
                const double t = i / static_cast<double>(kRate);
                const double two_pi = 6.283185307179586;
                s += static_cast<float>(
                    0.25 * std::sin(two_pi * 180.0 * t) *
                    (0.6 + 0.4 * std::sin(two_pi * 3.0 * t)));
            }
            return s;
        };
        const int total = 3 * kRate;
        std::vector<float> in(static_cast<size_t>(total)), out;
        for (int i = 0; i < total; ++i)
            in[static_cast<size_t>(i)] = gen(i);
        const std::vector<float> in_copy = in;
        for (int off = 0; off + kFrame <= total; off += kFrame) {
            float buf[kFrame];
            const int got = fx.process(in.data() + off, buf, kFrame);
            out.insert(out.end(), buf, buf + got);
        }
        // Tail 0.3 s: noise-only in the input; compare against the matching
        // (delayed) tail of the output.
        const size_t tail = kFrame * 3;
        const float before = rms(in_copy.data() + in_copy.size() - tail, tail);
        const float after = out.size() >= tail ? rms(out.data() + out.size() - tail, tail) : 1e9f;
        const bool pass = before > 0.0f && after < before * 0.5f;
        std::printf("fx-test denoise: noise RMS %.4f -> %.4f  %s\n", before,
                    after, pass ? "OK" : "FAIL");
        if (!pass)
            ++failures;
        write_wav("fx_test_in.wav", in_copy, kRate);
        write_wav("fx_test_out.wav", out, kRate);
        std::cout << "fx-test: wrote fx_test_in.wav / fx_test_out.wav\n";
    }

    // ---- 2. echo cancellation ---------------------------------------------
    fx.set_denoise(false);
    fx.set_echo_cancel(true);
    {
        std::mt19937 rng(7);
        std::uniform_real_distribution<float> farnd(-0.3f, 0.3f);
        std::uniform_real_distribution<float> mic_noise(-0.002f, 0.002f);
        const int delay = 1600;  // 100 ms acoustic path = AudioFx nominal
        const int total = 4 * kRate;
        std::vector<float> ref(static_cast<size_t>(total));
        for (auto& s : ref)
            s = farnd(rng);
        std::vector<float> mic(static_cast<size_t>(total), 0.0f);
        for (int i = delay; i < total; ++i)
            mic[static_cast<size_t>(i)] =
                0.8f * ref[static_cast<size_t>(i - delay)] + mic_noise(rng);

        std::vector<float> out;
        float buf[kFrame];
        for (int off = 0; off + kFrame <= total; off += kFrame) {
            fx.push_playback(ref.data() + off, kFrame, kRate);
            const int got = fx.process(mic.data() + off, buf, kFrame);
            out.insert(out.end(), buf, buf + got);
        }
        // Last 2 s: after the AEC has converged. Echo-only input vs output.
        const size_t tail = static_cast<size_t>(kRate) * 2;
        const float before = rms(mic.data() + mic.size() - tail, tail);
        const float after = out.size() >= tail ? rms(out.data() + out.size() - tail, tail) : 1e9f;
        const bool pass = before > 0.0f && after < before * 0.5f;
        std::printf("fx-test echo:   echo RMS %.4f -> %.4f  %s\n", before,
                    after, pass ? "OK" : "FAIL");
        if (!pass)
            ++failures;

        // Smoke: far-end pushed at a Piper-like 22050 Hz must stream through
        // the internal resampler without hangs or runaway buffering.
        std::vector<float> ref22(static_cast<size_t>(22050));
        for (auto& s : ref22)
            s = farnd(rng);
        std::vector<float> out22;
        for (size_t off = 0; off + kFrame <= ref22.size(); off += kFrame) {
            fx.push_playback(ref22.data() + off, kFrame, 22050);
            std::vector<float> silent(kFrame, 0.0f);
            const int got = fx.process(silent.data(), buf, kFrame);
            out22.insert(out22.end(), buf, buf + got);
        }
        const bool smoke = out22.size() > kRate;  // ~1 s of output flowed
        std::printf("fx-test ref22k: %zu samples out  %s\n", out22.size(),
                    smoke ? "OK" : "FAIL");
        if (!smoke)
            ++failures;
    }

    if (failures) {
        std::cerr << "fx-test: FAIL (" << failures << " stage(s))\n";
        return 1;
    }
    std::cout << "fx-test: PASS\n";
    return 0;
}

} // namespace

// Headless self-test for the TTS text filter (see src/tts_text.h):
// table-driven input -> expected plain-speech text. Exit 0 = all PASS.
int run_tts_filter_test()
{
    struct Case {
        const char* in;
        const char* out;
    };
    static const Case cases[] = {
        {"Hello world", "Hello world"},
        {"Hello **bold** and *it* and `code`", "Hello bold and it and code"},
        {"## Title\n### Sub", "Title\nSub"},
        {"- one\n- two\n1. three", "one\ntwo\nthree"},
        {"> quoted text", "quoted text"},
        {"```python\nprint('hi')\n```\nDone.", "Done."},
        {"```\nonly code\n```", ""},
        {"~~~\ncode\n~~~\nok", "ok"},
        {"[Krastor](https://example.com) rocks", "Krastor rocks"},
        {"visit https://example.com today", "visit today"},
        {"| a | b |\n|---|---|\n| 1 | 2 |", "a, b\n1, 2"},
        {"---\ntext", "text"},
        {"<b>bold</b> text", "bold text"},
        {"Ciao \xF0\x9F\x8C\x8E!", "Ciao!"},
        {"snake_case_name stays", "snake_case_name stays"},
        {"![img](x.png) alt text", "alt text"},
        {"multi\n\n\n\nblank", "multi\nblank"},
        {"~~strike~~", "strike"},
        {"1) first\n2) second", "first\nsecond"},
        {"a < b stays ok", "a < b stays ok"},
    };
    int failures = 0;
    for (const auto& c : cases) {
        const std::string got = krastor::tts_filter(c.in);
        const bool ok = got == c.out;
        if (!ok)
            ++failures;
        std::printf("  %-42s %s\n", c.in, ok ? "OK" : "FAIL");
        if (!ok)
            std::printf("      expected [%s] got [%s]\n", c.out, got.c_str());
    }
    if (failures) {
        std::cerr << "tts-filter-test: FAIL (" << failures << " case(s))\n";
        return 1;
    }
    std::cout << "tts-filter-test: PASS (" << (sizeof(cases) / sizeof(cases[0]))
              << " cases)\n";
    return 0;
}

// Debug/self-test helper: run one tool call directly (no LLM involved),
// printing everything the runner saw. Usage:
//   krastor --tool-run <tool> <json-args>
int run_tool_run(int argc, char** argv, int& i)
{
    if (i + 2 >= argc) {
        std::cerr << "usage: --tool-run <tool> <json-args>\n";
        return 2;
    }
    const std::string name = argv[++i];
    const std::string args = argv[++i];
    krastor::GrantStore grants;
    krastor::ToolRegistry reg;
    std::vector<std::string> log;
    reg.load_dir(krastor::tools_dir(), nlohmann::json{}, log);
    for (auto& l : log)
        std::cout << l << "\n";
    const krastor::ToolManifest* m = reg.find(name);
    if (!m) {
        std::cerr << "tool not found: " << name << "\n";
        return 1;
    }
    struct AutoAsk : krastor::AskHandler {
        krastor::Decision ask(const std::string&, const std::string&,
                              const std::string&) override
        {
            return krastor::Decision::Allow;  // debug mode: always allow
        }
    } ask;
    krastor::ToolRunner runner(grants, nlohmann::json{});
    const auto r = runner.run(*m, args, &ask, nullptr);
    std::cout << "ok=" << (r.ok ? "true" : "false") << " ms=" << r.ms << "\n"
              << "result: " << r.result << "\n"
              << "error: " << r.error << "\n"
              << "stderr: " << r.stderr_text << "\n";
    return r.ok ? 0 : 1;
}

// Headless self-test of the external tools system (no LLM involved):
// exercises the runner + permission gate + every built-in tool. Exit code =
// number of failed checks. Usage: krastor --tool-test
int run_tool_test()
{
    int failed = 0;
    auto check = [&](const std::string& name, bool pass,
                     const std::string& info = {}) {
        std::cout << (pass ? "PASS " : "FAIL ") << name
                  << (pass || info.empty() ? "" : " - " + info) << "\n";
        if (!pass)
            ++failed;
    };

    // utf8_clean: tool output must reach the JSON layer as valid UTF-8
    // (crash of 2026-11-07: cap_text cut a codepoint, autosave threw).
    check("utf8_clean keeps valid", krastor::utf8_clean("caf\xc3\xa9 \xe2\x82\xac") ==
                                        "caf\xc3\xa9 \xe2\x82\xac");
    check("utf8_clean strips cp1252", krastor::utf8_clean("caf\xe9") == "caf?");
    check("utf8_clean cuts truncated tail",
          krastor::utf8_clean("ok\xe2\x82") == "ok??");

    // tool usage prompt: sandbox + the "python is the last resort" rule.
    {
        const std::string up = krastor::tool_usage_prompt();
        check("tool prompt: sandbox + python last resort",
              up.find(krastor::workspace_dir()) != std::string::npos &&
                  up.find("last resort") != std::string::npos &&
                  up.find("NEVER run python just to read") != std::string::npos);
    }

    // ANSI color guidance for the model + line-level markdown passthrough:
    // ESC lines survive verbatim, the rest of the message stays formatted.
    {
        check("ansi prompt: SGR guidance present",
              std::string(krastor::kAnsiUsagePrompt).
                  find("renders ANSI SGR colors") != std::string::npos);
        const std::string mixed = krastor::md_to_ansi(
            "## Head\n\x1b[31mraw red\x1b[0m\nplain **bold**");
        check("md passthrough is line-level",
              mixed.find("\x1b[31mraw red\x1b[0m") != std::string::npos &&
                  mixed.find("## ") == std::string::npos &&
                  mixed.find("Head") != std::string::npos);
        check("utf8_clean keeps ESC bytes",
              krastor::utf8_clean("\x1b[32mgreen\x1b[0m").find('\x1b') !=
                  std::string::npos);
        // Models that drop the ESC byte (control chars lost in generation):
        // bare SGR sequences are repaired into real escapes at format time.
        const std::string fixed = krastor::md_to_ansi(
            "[48;5;26m[38;5;231m  METEO [0m\nplain text");
        check("ansi_repair restores dropped ESC bytes",
              fixed.find("\x1b[48;5;26m\x1b[38;5;231m  METEO \x1b[0m") !=
                  std::string::npos);
        check("ansi_repair leaves prose alone",
              krastor::md_to_ansi("wait [5m] ok").find('\x1b') ==
                  std::string::npos);
        check("ansi_repair keeps escaped sequences untouched",
              krastor::md_to_ansi("\x1b[31mred\x1b[0m") == "\x1b[31mred\x1b[0m");
        // ANSI payload pasted inside a language-less fence (the model wraps
        // colored tool output in ```) is repaired too: the highlighter never
        // runs there, so without this the bare SGR text renders literally.
        const std::string fenced = krastor::md_to_ansi(
            "```\n[48;5;26m[1m SASSARI [0m\n[48;5;236m alba [0m\n```");
        check("md repairs ESC-less ANSI inside a fence",
              fenced.find("\x1b[48;5;26m") != std::string::npos);
        // A code sample that merely mentions bracketed numbers stays literal:
        // one repairable line is below the two-line block threshold.
        const std::string sample = krastor::md_to_ansi(
            "```\nputs(\"[31m[0m red\");\nint a[5];\n```");
        check("md keeps literal SGR-looking code in fences",
              sample.find("\x1b[31m") == std::string::npos &&
                  sample.find("[31m") != std::string::npos);
    }

    krastor::GrantStore grants;
    krastor::ToolRegistry reg;
    std::vector<std::string> log;
    reg.load_dir(krastor::tools_dir(), nlohmann::json{}, log);
    for (auto& l : log)
        std::cout << l << "\n";

    struct AutoAsk : krastor::AskHandler {
        krastor::Decision ask(const std::string&, const std::string&,
                              const std::string&) override
        {
            return krastor::Decision::Allow;
        }
    } ask_allow;
    struct DenyAsk : krastor::AskHandler {
        krastor::Decision ask(const std::string&, const std::string&,
                              const std::string&) override
        {
            return krastor::Decision::Deny;
        }
    } ask_deny;
    krastor::ToolRunner runner(grants, nlohmann::json{});

    const auto* files = reg.find("files");
    const auto* web = reg.find("webfetch");
    const auto* search = reg.find("websearch");
    const auto* py = reg.find("python");
    check("registry: files tool loaded", files != nullptr);
    check("registry: webfetch tool loaded", web != nullptr);
    check("registry: websearch tool loaded", search != nullptr);
    check("registry: python tool loaded", py != nullptr);
    if (!files || !web || !search || !py) {
        std::cout << "tool-test: " << failed << " failed\n";
        return failed;
    }

    // --- files: sandbox write / read / edit / list ----------------------------
    const std::string tag =
        "selftest_" +
        std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count()) +
        ".txt";
    auto fr = [&](const std::string& args, krastor::AskHandler* ah = nullptr) {
        return runner.run(*files, args, ah ? ah : &ask_allow, nullptr);
    };
    auto r = fr("{\"action\":\"write\",\"path\":\"" + tag +
                "\",\"content\":\"ciao mondo\",\"overwrite\":true}");
    check("files: write in sandbox", r.ok, r.error);
    r = fr("{\"action\":\"read\",\"path\":\"" + tag + "\"}");
    check("files: read back", r.ok && r.result.find("ciao mondo") != std::string::npos,
          r.error);
    r = fr("{\"action\":\"edit\",\"path\":\"" + tag +
           "\",\"find\":\"mondo\",\"replace\":\"pianeta\"}");
    check("files: edit", r.ok && r.result.find("\"replacements\":1") != std::string::npos,
          r.error);
    // edit results carry a structured diff for the GUI side-by-side renderer.
    {
        const auto j = nlohmann::json::parse(r.result, nullptr, false);
        const bool has_diff = !j.is_discarded() && j.contains("diff") &&
                              j["diff"].is_object() &&
                              j["diff"].contains("ops");
        check("files: edit result carries diff", has_diff);
        if (has_diff) {
            int minus = 0, plus = 0;
            for (const auto& op : j["diff"]["ops"]) {
                if (op.value("op", "") == "-")
                    ++minus;
                else if (op.value("op", "") == "+")
                    ++plus;
            }
            check("files: edit diff counts one -/+ pair",
                  minus == 1 && plus == 1 &&
                      j["diff"].value("added", 0) == 1 &&
                      j["diff"].value("removed", 0) == 1,
                  "-" + std::to_string(minus) + " +" + std::to_string(plus));
        }
    }
    r = fr("{\"action\":\"read\",\"path\":\"" + tag + "\",\"start\":5,\"end\":12}");
    check("files: byte-range read", r.ok && r.result.find("pianeta") != std::string::npos,
          r.error);
    r = fr("{\"action\":\"list\",\"path\":\".\"}");
    check("files: list workspace", r.ok && r.result.find(tag) != std::string::npos,
          r.error);
    // overwrite-write results carry a diff too: one added line over the old
    // content ("ciao pianeta" stays as context, "riga due" is the addition).
    r = fr("{\"action\":\"write\",\"path\":\"" + tag +
           "\",\"content\":\"ciao pianeta\\nriga due\",\"overwrite\":true}");
    {
        const auto j = nlohmann::json::parse(r.result, nullptr, false);
        check("files: overwrite write carries diff",
              r.ok && !j.is_discarded() && j.contains("diff") &&
                  j["diff"].value("added", 0) == 1 &&
                  j["diff"].value("removed", 0) == 0,
              r.error);
    }
    // --- files: delete always asks, even inside the sandbox --------------------
    r = fr("{\"action\":\"delete\",\"path\":\"" + tag + "\"}", &ask_deny);
    check("files: delete asks -> user deny honoured", !r.ok,
          "delete must confirm even in the sandbox");
    r = fr("{\"action\":\"delete\",\"path\":\"" + tag + "\"}");
    check("files: delete after confirm", r.ok, r.error);
    r = fr("{\"action\":\"stat\",\"path\":\"" + tag + "\"}");
    check("files: deleted file is gone",
          r.ok && r.result.find("\"exists\":false") != std::string::npos, r.error);
    r = fr("{\"action\":\"delete\",\"path\":\"C:/Windows\"}");
    check("files: deny floor blocks delete of C:/Windows", !r.ok,
          "must be blocked before asking");
    // --- files: permission floors ----------------------------------------------
    r = fr("{\"action\":\"write\",\"path\":\"C:/Windows/evil_selftest.txt\","
           "\"content\":\"x\",\"overwrite\":true}");
    check("files: deny floor blocks C:/Windows", !r.ok, "write must be blocked");
    r = fr("{\"action\":\"read\",\"path\":\"" +
           krastor::Config::exe_dir() + "/../.git/config\"}");
    check("files: deny floor blocks .git", !r.ok, "read must be blocked");
    r = fr("{\"action\":\"write\",\"path\":\"../outside_selftest.txt\","
           "\"content\":\"x\",\"overwrite\":true}", &ask_deny);
    check("files: ask outside sandbox -> user deny", !r.ok,
          "must ask and honour the deny");

    // --- webfetch ----------------------------------------------------------------
    r = runner.run(*web, "{\"url\":\"https://example.com\"}", &ask_allow, nullptr);
    if (r.error.rfind("fetch failed", 0) == 0)
        std::cout << "SKIP webfetch (no network)\n";
    else
        check("webfetch: example.com text",
              r.ok && r.result.find("Example Domain") != std::string::npos,
              r.error);
    r = runner.run(*web, "{\"url\":\"file:///c:/windows/win.ini\"}",
                   &ask_allow, nullptr);
    check("webfetch: file:// scheme blocked", !r.ok, "must be blocked");

    // --- websearch ---------------------------------------------------------------
    // DDG answers with an anti-bot challenge page intermittently (per IP):
    // a refusal is a SKIP, not a FAIL - the protocol check runs when it works.
    r = runner.run(*search, "{\"query\":\"C++ programming language\",\"max_results\":3}",
                   &ask_allow, nullptr);
    if (r.error.find("fetch failed") != std::string::npos ||
        r.error.find("engine may have refused") != std::string::npos)
        std::cout << "SKIP websearch (engine refused or no network)\n";
    else
        check("websearch: results with title+url",
              r.ok && r.result.find("\"url\"") != std::string::npos,
              r.error);

    // --- python -------------------------------------------------------------------
    r = runner.run(*py, "{\"code\":\"print(6*7)\"}", &ask_allow, nullptr);
    check("python: inline code", r.ok && r.result.find("42") != std::string::npos,
          r.error);
    r = runner.run(*py, "{\"code\":\"raise RuntimeError('boom')\"}",
                   &ask_allow, nullptr);
    check("python: traceback captured",
          r.ok && r.result.find("boom") != std::string::npos, r.error);
    r = runner.run(*py, "{\"code\":\"print(1)\"}", &ask_deny, nullptr);
    check("python: run asks -> user deny honoured", !r.ok,
          "run.ask must gate execution");

    std::cout << "tool-test: " << (failed ? std::to_string(failed) : "0")
              << " failed" << (failed ? "" : " - PASS") << "\n";
    return failed;
}

// Headless self-test for the session store (sessions_plan.md): folder-per-
// session round-trip with images and tool calls, crash tolerance, rename,
// list ordering, compaction archive, delete. Exit code = failed checks.
int run_session_test()
{
    namespace fs = std::filesystem;
    int failed = 0;
    auto check = [&](const char* name, bool ok, const std::string& err = {}) {
        std::cout << (ok ? "PASS " : "FAIL ") << name
                  << (ok || err.empty() ? "" : " - " + err) << "\n";
        if (!ok)
            ++failed;
    };

    std::error_code ec;
    const fs::path dir = fs::temp_directory_path() / "krastor_session_test";
    fs::remove_all(dir, ec);
    krastor::SessionStore store(dir.string());

    // --- create / meta ----------------------------------------------------------
    const std::string id = store.create("test session", "test/model");
    check("create returns id", !id.empty());
    check("folder exists", store.exists(id));
    check("rejects path traversal", !store.exists("../evil"));
    krastor::SessionMeta m;
    check("meta readable", store.meta_of(id, m) && m.title == "test session" &&
                              m.model_ref == "test/model");

    // --- append + load round-trip (image, tool calls, tool result) --------------
    const std::string png = "fake-png-bytes-1234";
    const std::string b64 =
        krastor::base64_encode(reinterpret_cast<const unsigned char*>(png.data()),
                               png.size());
    krastor::Conversation conv("SYS");
    conv.add_user("hello", {b64});
    conv.add_assistant("", {{"call1", "files", "{\"action\":\"list\"}"}});
    conv.add_tool("call1", "[]");
    conv.add_assistant("done");
    check("append turn", store.append(id, conv.messages_since(1)));
    check("attachment file written",
          fs::exists(dir / id / "attachments" / "img-1.png"));

    // --- predicted attachment paths (the GUI tells the model these paths) ---
    const std::string id_pred = store.create("attach predict", "test/model");
    const auto predicted = store.attachment_paths(id_pred, 2);
    check("attachment_paths predicts the next slots",
          predicted.size() == 2 &&
              fs::path(predicted[0]) == dir / id_pred / "attachments" / "img-1.png" &&
              fs::path(predicted[1]) == dir / id_pred / "attachments" / "img-2.png");
    krastor::Conversation conv2("SYS");
    conv2.add_user("two images", {b64, b64});
    check("append predicted-slot turn", store.append(id_pred, conv2.messages_since(1)));
    check("predicted paths are the written files",
          fs::exists(predicted[0]) && fs::exists(predicted[1]));
    store.remove(id_pred);  // keep the session-count checks below unaffected

    krastor::Conversation back("SYS");
    check("load", store.load(id, back, &m));
    const auto& ms = back.messages();
    check("system kept + 4 messages", ms.size() == 5);
    check("user text restored", ms.size() == 5 && ms[1].content == "hello");
    check("image round-trip",
          ms.size() == 5 && ms[1].images.size() == 1 && ms[1].images[0] == b64);
    check("tool_calls round-trip",
          ms.size() == 5 && ms[2].tool_calls.size() == 1 &&
              ms[2].tool_calls[0].id == "call1" &&
              ms[2].tool_calls[0].name == "files" &&
              ms[2].tool_calls[0].arguments == "{\"action\":\"list\"}");
    check("tool result round-trip",
          ms.size() == 5 && ms[3].role == "tool" &&
              ms[3].tool_call_id == "call1" && ms[3].content == "[]");
    krastor::SessionMeta m2;
    store.meta_of(id, m2);
    check("meta message count", m2.messages == 4);

    // --- incremental append (messages_since) -------------------------------------
    conv.add_user("second");
    check("incremental append", store.append(id, conv.messages_since(5)));
    krastor::Conversation back2("SYS");
    store.load(id, back2);
    check("5 messages after append", back2.messages().size() == 6);

    // --- crash tolerance: half-written last line is skipped ----------------------
    {
        std::ofstream f(dir / id / "log.jsonl", std::ios::app);
        f << "{\"role\":\"user\",\"content\":\"trunc";  // no newline, broken JSON
    }
    krastor::Conversation back3("SYS");
    check("load survives corrupt tail", store.load(id, back3));
    check("corrupt line skipped", back3.messages().size() == 6);

    // --- rename + list -------------------------------------------------------------
    check("rename", store.rename(id, "renamed"));
    auto l = store.list();
    check("list shows one", l.size() == 1 && l[0].title == "renamed");
    const std::string id2 = store.create("second session", "");
    l = store.list();
    check("list shows two", l.size() == 2);
    // `updated` has second resolution: wait past the boundary so the rename
    // below is unambiguously the newest activity.
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    store.rename(id, "touched last");  // deterministic: id is now newest
    l = store.list();
    check("list newest first", !l.empty() && l[0].id == id);

    // --- compaction rewrite + archive ----------------------------------------------
    std::vector<krastor::Message> fresh;
    krastor::Message s;
    s.role = "user";
    s.content = std::string(krastor::kSummaryMarker) + " — compacted] text";
    fresh.push_back(s);
    fresh.push_back(conv.messages()[5]);  // keep one tail message
    check("compact rewrite", store.replace_log_after_compact(id, fresh));
    check("archive kept",
          fs::exists(dir / id / "archive" / "pre-compact-1.jsonl"));
    krastor::Conversation back4("SYS");
    check("compacted log loads", store.load(id, back4, &m2));
    check("compacted content", back4.messages().size() == 3 &&
                                   krastor::is_summary_message(back4.messages()[1]));
    check("meta compactions", m2.compactions == 1 && m2.messages == 2);

    // --- delete ----------------------------------------------------------------------
    check("delete", store.remove(id));
    check("deleted gone", !store.exists(id));
    check("other session survives", store.exists(id2));
    fs::remove_all(dir, ec);

    // --- compactor helpers (opencode v2 template / clipping / buffer) ------------------
    {
        const std::string p_new = krastor::compactor_test_prompt(false);
        const std::string p_upd = krastor::compactor_test_prompt(true);
        check("compact prompt: structured template",
              p_new.find("## Objective") != std::string::npos &&
                  p_new.find("## Work State") != std::string::npos &&
                  p_new.find("### Blocked") != std::string::npos &&
                  p_new.find("## Relevant Files") != std::string::npos);
        check("compact prompt: update mode wording",
              p_upd.find("Update the existing checkpoint") != std::string::npos &&
                  p_new.find("Update the existing checkpoint") == std::string::npos);

        std::vector<krastor::Message> msgs;
        krastor::Message u;
        u.role = "user";
        u.content = "fai X";
        krastor::Message a;
        a.role = "assistant";
        a.tool_calls.push_back({"c1", "files", "{\"action\":\"read\"}"});
        krastor::Message t;
        t.role = "tool";
        t.content = std::string("big output ") + std::string(6000, 'x');
        msgs = {u, a, t};
        const auto lv0 = krastor::compactor_test_transcript(msgs, 0);
        const auto lv1 = krastor::compactor_test_transcript(msgs, 1);
        const auto lv2 = krastor::compactor_test_transcript(msgs, 2);
        const auto lv3 = krastor::compactor_test_transcript(msgs, 3);
        const auto lv4 = krastor::compactor_test_transcript(msgs, 4);
        check("transcript L0 keeps tool output",
              lv0.find("big output") != std::string::npos &&
                  lv0.find("[clipped]") == std::string::npos);
        check("transcript L1 clips long tool output",
              lv1.find("big output") != std::string::npos &&
                  lv1.find("[clipped]") != std::string::npos);
        check("transcript L2 drops tool output",
              lv2.find("big output") == std::string::npos &&
                  lv2.find("[tool result omitted]") != std::string::npos &&
                  lv2.find("assistant calls files") != std::string::npos);
        check("transcript L3 user text only",
              lv3.find("fai X") != std::string::npos &&
                  lv3.find("[tool") == std::string::npos &&
                  lv3.find("assistant calls") == std::string::npos);
        check("transcript L4 omits everything", lv4.empty());

        check("compact buffer: 15% of 200k",
              krastor::compact_buffer_tokens("15%", 200000) == 30000);
        check("compact buffer: 16k floor on small windows",
              krastor::compact_buffer_tokens("15%", 64000) == 16000);
        check("compact buffer: raw tokens floored",
              krastor::compact_buffer_tokens("5000", 200000) == 16000);
        check("compact buffer: half-window cap on tiny windows",
              krastor::compact_buffer_tokens("15%", 20000) == 10000);
        check("compact buffer: garbage falls back to 15%",
              krastor::compact_buffer_tokens("abc", 200000) == 30000);
    }

    std::cout << "session-test: " << (failed ? std::to_string(failed) : "0")
              << " failed" << (failed ? "" : " - PASS") << "\n";
    return failed;
}

// Headless self-test for the media system (src/media.h): token parsing,
// size parsing, reply splitting, fit-inside scaling and WAV decoding.
// Exit code = number of failed checks. Usage: krastor --media-test
int run_media_test()
{
    using namespace krastor;
    int failed = 0;
    auto check = [&](const char* name, bool ok, const std::string& err = {}) {
        if (!ok) {
            ++failed;
            std::cout << "  FAIL " << name << (err.empty() ? "" : " - " + err)
                      << "\n";
        }
    };

    // ---- size fields ----
    check("size auto empty", parse_media_size("") == 0);
    check("size dash", parse_media_size("-") == 0);
    check("size star", parse_media_size("*") == 0);
    check("size auto word", parse_media_size("auto") == 0);
    check("size number", parse_media_size(" 400 ") == 400);
    check("size invalid", parse_media_size("abc") == -1);

    // ---- tokens ----
    MediaBlock mb;
    check("image token full",
          parse_media_token("  [image: C:\\a\\b.png | 400 | 300] ", mb) &&
              mb.kind == MediaBlock::Kind::Image && mb.src == "C:\\a\\b.png" &&
              mb.req_w == 400 && mb.req_h == 300 && mb.label == "b.png");
    check("image token auto sizes",
          parse_media_token("[image: x.png]", mb) && mb.req_w == 0 &&
              mb.req_h == 0);
    check("image token one size",
          parse_media_token("[image: x.png | 250]", mb) && mb.req_w == 250 &&
              mb.req_h == 0);
    check("image token dash",
          parse_media_token("[image: x.png | - | 120]", mb) && mb.req_w == 0 &&
              mb.req_h == 120);
    check("image token base64",
          parse_media_token("[image: base64:AAAA]", mb) &&
              mb.src == "base64:AAAA" && mb.label == "base64 image");
    check("image token no src", !parse_media_token("[image:  | 400]", mb));
    check("image token bad size", !parse_media_token("[image: a.png | zz]", mb));
    check("sound token",
          parse_media_token("[sound: C:\\s\\ding.wav]", mb) &&
              mb.kind == MediaBlock::Kind::Sound && mb.src == "C:\\s\\ding.wav");
    check("not a token", !parse_media_token("see [image: a.png] inline", mb));

    // ---- reply splitting ----
    const auto segs = split_media_tokens(
        "before\n[image: a.png | 100]\nmiddle\n[sound: b.wav]\nafter");
    check("split count", segs.size() == 5, std::to_string(segs.size()));
    check("split text 0", segs.size() == 5 && segs[0].text == "before");
    check("split media 1",
          segs.size() == 5 && segs[1].media && segs[1].media->src == "a.png");
    check("split text 2", segs.size() == 5 && segs[2].text == "middle");
    check("split media 3",
          segs.size() == 5 && segs[3].media &&
              segs[3].media->kind == MediaBlock::Kind::Sound);
    check("split text 4", segs.size() == 5 && segs[4].text == "after");
    const auto plain = split_media_tokens("no media here");
    check("split plain", plain.size() == 1 && plain[0].text == "no media here");
    std::string speak;
    for (const auto& s : split_media_tokens("l1\n[image: a.png]\nl2")) {
        if (s.media)
            continue;
        if (!speak.empty())
            speak += '\n';
        speak += s.text;
    }
    check("tokens stripped from speak text", speak == "l1\nl2", speak);

    // ---- fit-inside (native 1000x500, box 400x300) ----
    MediaBlock img;
    img.nat_w = 1000;
    img.nat_h = 500;
    float w = 0, h = 0;
    media_fit(img, 400, 300, w, h);  // auto: width-limited (400x200)
    check("fit auto", w > 399.0f && w < 401.0f && h > 199.0f && h < 201.0f,
          std::to_string((int)w) + "x" + std::to_string((int)h));
    img.req_w = 800;  // over the box: must be scaled back inside
    media_fit(img, 400, 300, w, h);
    check("fit req_w clamped", w <= 400.5f && h <= 300.5f && w > 399.0f,
          std::to_string((int)w) + "x" + std::to_string((int)h));
    img = MediaBlock{};
    img.nat_w = 1000;
    img.nat_h = 500;
    img.req_h = 100;  // only height: width follows the aspect
    media_fit(img, 400, 300, w, h);
    check("fit req_h aspect", w > 199.0f && w < 201.0f && h > 99.0f &&
                                  h < 101.0f,
          std::to_string((int)w) + "x" + std::to_string((int)h));
    img.req_w = 100;
    img.req_h = 50;  // both, fits: no scaling
    media_fit(img, 400, 300, w, h);
    check("fit exact", w > 99.0f && w < 101.0f && h > 49.0f && h < 51.0f);
    img.req_w = 5000;
    img.req_h = 4000;  // both, way over: contained, ratio kept
    media_fit(img, 400, 300, w, h);
    check("fit both clamped", w <= 400.5f && h <= 300.5f &&
                                  (std::fabs(w / h - 5.0 / 4.0) < 0.02f),
          std::to_string((int)w) + "x" + std::to_string((int)h));

    // ---- audio: beep buffer + WAV decode ----
    int rate = 0;
    const auto beep = make_beep_pcm(rate);
    check("beep pcm", rate == 48000 && beep.size() > 1000);
    // Build a tiny PCM16 mono 8 kHz WAV in memory and decode it.
    {
        std::vector<unsigned char> wav;
        auto put32 = [&](uint32_t v) {
            wav.push_back((unsigned char)v);
            wav.push_back((unsigned char)(v >> 8));
            wav.push_back((unsigned char)(v >> 16));
            wav.push_back((unsigned char)(v >> 24));
        };
        auto put16 = [&](uint16_t v) {
            wav.push_back((unsigned char)v);
            wav.push_back((unsigned char)(v >> 8));
        };
        const int samples = 800;
        wav.insert(wav.end(), {'R', 'I', 'F', 'F'});
        put32(36 + (uint32_t)samples * 2);
        wav.insert(wav.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
        put32(16);
        put16(1);          // PCM
        put16(1);          // mono
        put32(8000);       // rate
        put32(16000);      // byte rate
        put16(2);          // block align
        put16(16);         // bits
        wav.insert(wav.end(), {'d', 'a', 't', 'a'});
        put32((uint32_t)samples * 2);
        for (int i = 0; i < samples; ++i)
            put16((uint16_t)(1000 * (i / 40 % 2 ? 1 : -1)));
        std::vector<float> pcm;
        int drate = 0, dch = 0;
        std::string err;
        check("wav decode",
              decode_audio_bytes(wav, pcm, drate, dch, err) && drate == 8000 &&
                  dch == 2 && pcm.size() == (size_t)samples * 2,
              err);
    }

    std::cout << "media-test: " << (failed ? std::to_string(failed) : "0")
              << " failed" << (failed ? "" : " - PASS") << "\n";
    return failed;
}

// Config self-test (multiagent_plan.md): JSONC parsing with comments, the
// two-level override (state.json over config.jsonc), the agents map
// (synthesized base, fields, unknown-active fallback, circular cycle),
// hex colors, system-prompt composition, thinking-effort precedence and the
// save() round-trip (state.json must never carry the agents map).
// Usage: krastor --config-test
int run_config_test()
{
    namespace fs = std::filesystem;
    int failed = 0;
    auto check = [&](const char* name, bool ok, const std::string& err = {}) {
        std::cout << (ok ? "PASS " : "FAIL ") << name
                  << (ok || err.empty() ? "" : " - " + err) << "\n";
        if (!ok)
            ++failed;
    };

    std::error_code ec;
    const fs::path dir = fs::temp_directory_path() / "krastor_config_test";
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    // An empty state.json beside the config shadows the real exe-dir state,
    // so the test never picks up the developer's own runtime state.
    { std::ofstream(dir / "state.json") << "{}\n"; }

    // --- JSONC: comments + agents map ------------------------------------------
    {
        std::ofstream f(dir / "config.jsonc");
        f << "{\n"
             "  // base model\n"
             "  \"llm\": { \"model\": \"m1\", /* inline */ \"model_ref\": \"p/m1\" },\n"
             "  \"gui\": { \"font_size\": 22 },\n"
             "  \"agents\": {\n"
             "    \"poeta\": { \"name\": \"Poeta\", \"color\": \"#E5A93C\",\n"
             "                 \"system_prompt\": \"Sei un poeta.\", \"thinking\": \"max\" },\n"
             "    \"sec\": { \"color\": \"#FF0000\" }\n"
             "  },\n"
             "  \"agent\": { \"active\": \"poeta\" }\n"
             "}\n";
    }
    krastor::Config cfg = krastor::Config::load((dir / "config.jsonc").string());
    check("jsonc comments parse",
          cfg.llm.model == "m1" && cfg.gui.font_size == 22);
    check("agents parsed + base synthesized",
          cfg.agents.size() == 3 && cfg.agents[0].id == "krastor" &&
              cfg.agents[1].id == "poeta" && cfg.agents[2].id == "sec");
    const krastor::AgentDef* poeta = cfg.find_agent("poeta");
    check("agent fields", poeta && poeta->name == "Poeta" &&
                              poeta->color == "#E5A93C" &&
                              poeta->thinking == "max" &&
                              poeta->system_prompt == "Sei un poeta.");
    check("active from config.jsonc", cfg.agent.active == "poeta");
    check("base agent synthesized from agent.name",
          cfg.find_agent("krastor")->name == "Krastor");

    // --- state.json overlay (app-owned, takes precedence) ----------------------
    {
        std::ofstream f(dir / "state.json");
        f << "{ \"gui\": { \"font_size\": 26 }, \"agent\": { \"active\": \"sec\" } }\n";
    }
    cfg = krastor::Config::load((dir / "config.jsonc").string());
    check("state.json overrides config.jsonc",
          cfg.gui.font_size == 26 && cfg.agent.active == "sec");
    check("agents survive the state overlay", cfg.agents.size() == 3);

    // --- circular cycle ----------------------------------------------------------
    check("next_agent circular",
          cfg.next_agent("sec")->id == "krastor" &&
          cfg.next_agent("krastor")->id == "poeta" &&
          cfg.next_agent("poeta")->id == "sec");

    // --- unknown active falls back to the base agent -----------------------------
    fs::remove(dir / "state.json", ec);
    {
        std::ofstream f(dir / "config.jsonc");  // trunc: minimal config
        f << "{ \"agent\": { \"active\": \"ghost\" } }\n";
    }
    { std::ofstream(dir / "state.json") << "{}\n"; }
    cfg = krastor::Config::load((dir / "config.jsonc").string());
    check("unknown active falls back to krastor",
          cfg.agent.active == "krastor" && cfg.agents.size() == 1);

    // --- hex colors ---------------------------------------------------------------
    int r = 0, g = 0, b = 0;
    krastor::parse_hex_color("#E5A93C", r, g, b);
    check("hex color parsed", r == 0xE5 && g == 0xA9 && b == 0x3C);
    krastor::parse_hex_color("zz", r, g, b);
    check("bad hex falls back to azzurro", r == 120 && g == 180 && b == 255);

    // --- system-prompt composition -------------------------------------------------
    krastor::Config c2;
    c2.llm.system_prompt = "BASE.";
    const std::string p1 = krastor::compose_system_prompt(c2, nullptr, false);
    check("compose appends media + ansi suffixes",
          p1.rfind("BASE.", 0) == 0 &&
          p1.find("[image: ") != std::string::npos &&
          p1.find("renders ANSI SGR colors") != std::string::npos);
    krastor::AgentDef ag;
    ag.id = "x";
    ag.system_prompt = "AGENT.";
    const std::string p2 = krastor::compose_system_prompt(c2, &ag, false);
    check("agent prompt replaces the base",
          p2.rfind("AGENT.", 0) == 0 && p2.find("BASE.") == std::string::npos);
    krastor::Config c3;
    c3.llm.system_prompt = p1;  // already-suffixed base (old saved configs)
    check("suffixes never doubled",
          krastor::compose_system_prompt(c3, nullptr, false) == p1);

    // --- thinking precedence: agent > model entry -----------------------------------
    {
        std::ofstream f(dir / "llm_models.jsonc");
        f << "{ \"provider\": { \"p\": { \"name\": \"P\","
             " \"settings\": { \"baseURL\": \"http://x/v1\", \"apiKey\": \"k\" },"
             " \"models\": { \"m1\": { \"limit\": { \"context\": 4096 },"
             " \"thinking\": \"medium\" } } } } }\n";
    }
    krastor::LlmModelRegistry reg;
    check("registry loads", reg.load((dir / "llm_models.jsonc").string()));
    krastor::LlmConfig base;
    base.model_ref = "p/m1";
    check("apply_model_ref sets the model thinking default",
          krastor::apply_model_ref(base, reg) && base.reasoning_effort == "medium" &&
              base.context_size == 4096);
    krastor::AgentDef ag2;
    ag2.id = "t";
    ag2.thinking = "max";
    krastor::LlmConfig eff;
    krastor::apply_agent_llm(eff, base, &ag2, reg);
    check("agent thinking overrides the model default",
          eff.reasoning_effort == "max");
    krastor::apply_agent_llm(eff, base, nullptr, reg);
    check("no agent keeps the model default", eff.reasoning_effort == "medium");

    // --- colors: user-only scheme + markdown palette override -----------------
    {
        std::ofstream f(dir / "config.jsonc");
        f << "{ \"colors\": { \"text\": \"#FF0000\", \"md_code\": \"#00FF00\","
             " \"block_bg\": \"#111111\" }, \"chat\": { \"pad_x\": 20 } }\n";
    }
    { std::ofstream(dir / "state.json")
        << "{ \"colors\": { \"text\": \"#00FF00\" }, \"chat\": { \"pad_x\": 99 } }\n"; }
    cfg = krastor::Config::load((dir / "config.jsonc").string());
    check("colors parsed from config.jsonc", cfg.colors.text == "#FF0000");
    check("colors are user-only (state.json ignored)",
          cfg.colors.text == "#FF0000");
    check("colors.block_bg parsed", cfg.colors.block_bg == "#111111");
    check("chat layout parsed from config.jsonc", cfg.chat.pad_x == 20.0f);
    check("chat layout is user-only (state.json ignored)",
          cfg.chat.pad_x == 20.0f);
    check("chat layout keeps defaults for unset keys",
          cfg.chat.pad_y == 10.0f && cfg.chat.bar_w == 3.0f &&
              cfg.chat.bar_gap == 14.0f && cfg.chat.block_gap == 10.0f);
    check("md_* overrides the markdown palette",
          krastor::md_to_ansi("a `b`", nullptr, cfg.colors.md)
              .find("\x1b[38;2;0;255;0m") != std::string::npos);
    check("default palette keeps the opencode-ish code color",
          krastor::md_to_ansi("a `b`")
              .find("\x1b[38;2;210;200;120m") != std::string::npos);

    // --- save() round-trip: state schema, no agents map ------------------------------
    {
        std::ofstream f(dir / "config.jsonc");
        f << "{ \"gui\": { \"font_size\": 22 }, \"agents\": { \"a\": {} } }\n";
    }
    krastor::Config c4 = krastor::Config::load((dir / "config.jsonc").string());
    c4.gui.font_size = 30;
    const std::string sp = (dir / "state2.json").string();
    c4.save(sp);
    check("save() round-trips through load()", [&] {
        { std::ofstream(dir / "state.json") << "{}\n"; }
        std::ofstream(dir / "config.jsonc", std::ios::trunc)
            << "{ \"gui\": { \"font_size\": 22 }, \"agents\": { \"a\": {} } }\n";
        // simulate the two-level layout: state beside the config
        fs::copy_file(sp, dir / "state.json", fs::copy_options::overwrite_existing, ec);
        const krastor::Config c5 =
            krastor::Config::load((dir / "config.jsonc").string());
        return c5.gui.font_size == 30;
    }());
    {
        std::ifstream f(sp);
        const auto j = nlohmann::json::parse(f, nullptr, false);
        check("save() never writes the agents map",
              !j.is_discarded() && !j.contains("agents") &&
                  j.contains("gui") && j["gui"]["font_size"] == 30);
    }

    fs::remove_all(dir, ec);
    std::cout << "config-test: " << failed << " failed - "
              << (failed ? "FAIL" : "PASS") << "\n";
    return failed;
}

int main(int argc, char** argv)
{
#ifdef _WIN32
    // Correct rendering of accented Italian text in the console.
    SetConsoleOutputCP(CP_UTF8);
#endif

    // An uncaught exception on any thread would abort() with no trace (the GUI
    // has no console when double-clicked). Log the real cause next to the exe.
    std::set_terminate([] {
        const std::string log_path = krastor::Config::exe_dir() + "/krastor_error.log";
        std::FILE* f = std::fopen(log_path.c_str(), "a");
        auto report = [&](const std::string& what) {
            std::fprintf(stderr, "FATAL: %s\n", what.c_str());
            if (f) {
                std::fprintf(f, "uncaught: %s\n", what.c_str());
                std::fclose(f);
            }
        };
        if (const auto e = std::current_exception()) {
            try {
                std::rethrow_exception(e);
            } catch (const std::exception& ex) {
                report(ex.what());
            } catch (...) {
                report("non-standard exception");
            }
        } else {
            report("unknown termination (joinable thread destroyed?)");
        }
        std::abort();
    });

    // Args: [--gui] [--icon-test] [--fx-test] [--tts-filter-test] [--gui-test]
    //       [--config-test] [--session-test] [--media-test] [--session <id>]
    //       [--new-session] [--agent <id>]
    //       [--delete-session <id>] [--compact [focus]] [config.jsonc]
    bool use_gui = false;
    std::string config_path;  // empty -> default (exe dir, legacy CWD fallback)
    std::string session_flag, delete_session_flag;
    std::string agent_flag;   // --agent <id>: start on that config.jsonc agent
    bool new_session_flag = false, list_sessions_flag = false,
         compact_flag = false;
    std::string compact_focus;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--gui") use_gui = true;
        else if (a == "--icon-test") return run_icon_test();
        else if (a == "--fx-test") return run_fx_test();
        else if (a == "--tts-filter-test") return run_tts_filter_test();
        else if (a == "--tool-run") return run_tool_run(argc, argv, i);
        else if (a == "--tool-test") return run_tool_test();
        else if (a == "--config-test") return run_config_test();
        else if (a == "--session-test") return run_session_test();
        else if (a == "--media-test") return run_media_test();
        else if (a == "--gui-test") return krastor::run_gui_stack_test();
        else if (a == "--new-session") new_session_flag = true;
        else if (a == "--list-sessions") list_sessions_flag = true;
        else if (a == "--agent" && i + 1 < argc) agent_flag = argv[++i];
        else if (a == "--session" && i + 1 < argc) session_flag = argv[++i];
        else if (a == "--delete-session" && i + 1 < argc)
            delete_session_flag = argv[++i];
        else if (a == "--compact") {
            compact_flag = true;
            if (i + 1 < argc && argv[i + 1][0] != '-')
                compact_focus = argv[++i];
        }
        else config_path = a;
    }
    if (config_path.empty())
        config_path = krastor::Config::default_path();
    std::cout << "Krastor v0.4.0 (Phase 4 voice I/O)\n";

    krastor::Config cfg = [&] {
        try {
            return krastor::Config::load(config_path);
        } catch (const std::exception& e) {
            std::cerr << "config error: " << e.what() << '\n';
            ::exit(1);
        }
    }();
    // Show which file was actually loaded: a stale ./config.json in the working
    // directory is ignored once the exe-dir config exists (see default_path()).
    std::cout << "config: " << config_path << '\n';
    // A legacy working-directory config (config.json/.jsonc): persist everything
    // into state.json next to the executable so the app has a complete mutable
    // copy there (the user file itself is never rewritten).
    if (config_path == "config.json" || config_path == "config.jsonc") {
        try {
            cfg.save();
        } catch (const std::exception& e) {
            std::cerr << "warning: config migration failed: " << e.what() << '\n';
        }
    }
    // Input DSP toggles (RNNoise denoise / SpeexDSP echo cancel) drive the
    // shared AudioFx pipeline used by the mic capture and the TTS tap.
    krastor::AudioFx::instance().set_denoise(cfg.stt.denoise);
    krastor::AudioFx::instance().set_echo_cancel(cfg.stt.echo_cancel);

    // llm_models.jsonc (opencode.jsonc format): when llm.model_ref names an
    // entry there, its URL/key/model/context override the explicit fields.
    krastor::LlmModelRegistry models;
    if (!models.load(krastor::LlmModelRegistry::default_path()))
        std::cout << "note: " << models.error()
                  << " — using llm.base_url/model from the config\n";
    else if (krastor::apply_model_ref(cfg.llm, models))
        std::cout << "model: " << cfg.llm.model_ref << " -> " << cfg.llm.base_url
                  << "\n";
    // Multi-agent: the active agent (--agent flag or agent.active) overrides
    // the global llm config (model_ref + thinking). base_llm keeps the
    // untouched global so /agent use can switch back cleanly.
    const krastor::LlmConfig base_llm = cfg.llm;
    if (!agent_flag.empty()) {
        if (!cfg.find_agent(agent_flag)) {
            std::cerr << "unknown agent '" << agent_flag
                      << "' (config.jsonc \"agents\")\n";
            return 1;
        }
        cfg.agent.active = agent_flag;
    }
    krastor::apply_agent_llm(cfg.llm, base_llm,
                             cfg.find_agent(cfg.agent.active), models);

    // ---- session CLI flags (sessions_plan.md) --------------------------------
    // Handled before the heavy voice stack: pure store operations exit here.
    krastor::SessionStore store(krastor::SessionStore::default_dir(cfg.sessions.dir));
    if (list_sessions_flag) {
        print_sessions(store);
        return 0;
    }
    if (!delete_session_flag.empty()) {
        const std::string id = resolve_session(store, delete_session_flag);
        if (id.empty()) {
            std::cerr << "session not found: " << delete_session_flag << "\n";
            return 1;
        }
        std::cout << (store.remove(id) ? "deleted session " + id + "\n"
                                       : "delete failed\n");
        return store.exists(id);
    }
    if (compact_flag) {
        std::string id = session_flag.empty()
                             ? (store.list().empty() ? std::string()
                                                     : store.list().front().id)
                             : resolve_session(store, session_flag);
        if (id.empty()) {
            std::cerr << "no session to compact (--session <id>)\n";
            return 1;
        }
        krastor::Conversation conversation(cfg.llm.system_prompt);
        if (!store.load(id, conversation)) {
            std::cerr << "cannot load session " << id << "\n";
            return 1;
        }
        krastor::LlmClient llm(cfg.llm);
        std::cout << "compacting " << id << "...\n";
        const auto cr = krastor::compact_conversation(
            llm, conversation, cfg.llm.context_size, cfg.llm.max_tokens,
            compact_focus, nullptr,
            [](const std::string& s) { std::cout << "[compact] " << s << "\n"; });
        if (!cr.ok) {
            std::cerr << "compact failed: " << cr.error << "\n";
            return 1;
        }
        store.replace_log_after_compact(id, conversation.messages_since(1));
        std::cout << "compacted " << cr.messages_before << " -> "
                  << cr.messages_after << " messages (~" << cr.est_tokens_before
                  << " -> ~" << cr.est_tokens_after << " tokens, "
                  << cr.llm_calls << " LLM call(s))\n";
        return 0;
    }

    krastor::VoiceRegistry voices;
    if (voices.discover(cfg.tts.models_dir)) {
        if (!cfg.tts.default_voice.empty() && !voices.select(cfg.tts.default_voice))
            std::cerr << "warning: default_voice '" << cfg.tts.default_voice
                      << "' not found, using first discovered voice\n";
    }

    if (use_gui) {
        // The GUI owns the effective config per agent (apply_agent recomputes
        // it from the global one on every switch), so hand it the global back.
        cfg.llm = base_llm;
        return krastor::run_gui(cfg, voices);
    }

    print_voice(voices);

    krastor::Tts tts(voices, cfg);
    tts.init();
    krastor::Stt stt(cfg);

    if (cfg.llm.api_key.empty())
        std::cout << "note: no API key configured (config.jsonc llm.api_key or "
                     "KRASTOR_API_KEY) — LLM calls will fail\n";

    krastor::LlmClient llm(cfg.llm);
    // External tools (see tools_plan.md): the CLI asks interactively on stdin.
    krastor::GrantStore tool_grants;
    krastor::ToolRegistry tool_registry;
    krastor::ToolRunner tool_runner(tool_grants, nlohmann::json{});
    struct ConsoleAsk : krastor::AskHandler {
        krastor::Decision ask(const std::string& tool, const std::string& action,
                              const std::string& detail) override
        {
            std::cout << "\n[tool] '" << tool << "' wants to " << action
                      << ":\n" << detail << "\nAllow once? [y/N/a] "
                      << std::flush;
            std::string a;
            if (!std::getline(std::cin, a))
                return krastor::Decision::Deny;
            if (a == "y" || a == "Y")
                return krastor::Decision::Allow;
            if (a == "a" || a == "A")
                return krastor::Decision::Always;
            return krastor::Decision::Deny;
        }
    } tool_ask;
    bool tools_active = false;
    if (cfg.tools.enabled && cfg.llm.supports_tools) {
        nlohmann::json ov = nlohmann::json::parse(
            cfg.tools.overrides_json.empty() ? "{}" : cfg.tools.overrides_json,
            nullptr, false);
        std::vector<std::string> tlog;
        tool_registry.load_dir(krastor::tools_dir(),
                               ov.is_object() ? ov : nlohmann::json(), tlog);
        for (auto& l : tlog)
            std::cout << l << "\n";
        tools_active = !tool_registry.empty();
    }
    // System prompt: the active agent's persona (or the global one) plus the
    // usage suffixes (tool sandbox, media tokens, ANSI colors), each guarded
    // against double-append — the same composition the GUI uses per agent.
    std::string sys_prompt = krastor::compose_system_prompt(
        cfg, cfg.find_agent(cfg.agent.active), tools_active);
    krastor::OutputFormatter fmt(cfg);
    krastor::Highlighter hl;
    if (hl.load_default())
        fmt.set_highlighter([&hl](const std::string& code, const std::string& lang) {
            return hl.highlight(code, lang);
        });
    krastor::Conversation conversation(sys_prompt);

    // ---- session selection (sessions_plan.md) ---------------------------------
    // --session <id> > --new-session > continue the most recent > fresh session.
    std::string session_id;
    std::string session_title;
    std::string pending_session_agent;  // agent recorded in a restored session
    if (!session_flag.empty() && !new_session_flag) {
        session_id = resolve_session(store, session_flag);
        if (session_id.empty()) {
            std::cerr << "warning: session '" << session_flag
                      << "' not found, starting a new one\n";
        }
    }
    if (session_id.empty() && !new_session_flag && cfg.sessions.continue_last) {
        // Prefer the session active on the previous run; fall back to the
        // most recently updated one.
        session_id = cfg.sessions.last_id;
        if (session_id.empty() || !store.exists(session_id)) {
            const auto l = store.list();
            session_id = l.empty() ? std::string{} : l.front().id;
        }
    }
    if (!session_id.empty() && store.load(session_id, conversation)) {
        krastor::SessionMeta sm;
        store.meta_of(session_id, sm);
        session_title = sm.title;
        std::cout << "session: " << session_id << " \"" << session_title
                  << "\" (" << sm.messages << " msgs restored)\n";
        // The session's agent comes back with it (an explicit --agent wins).
        if (agent_flag.empty() && !sm.agent.empty())
            pending_session_agent = sm.agent;
    } else {
        session_id = store.create("New session", cfg.llm.model_ref,
                                  cfg.agent.active);
        session_title = "New session";
        std::cout << "session: " << session_id << " (new)\n";
    }
    size_t saved_count = conversation.messages().size();
    // Record the active session so the next run resumes exactly this one.
    auto remember_session = [&] {
        cfg.sessions.last_id = session_id;
        cfg.save();
    };
    remember_session();

    // Switch the whole CLI stack to an agent: effective llm config, client,
    // system prompt and the session's recorded agent. Shared by /agent use
    // and the session-restore above.
    auto apply_cli_agent = [&](const std::string& id) -> bool {
        const krastor::AgentDef* a = cfg.find_agent(id);
        if (!a)
            return false;
        cfg.agent.active = a->id;
        krastor::apply_agent_llm(cfg.llm, base_llm, a, models);
        llm = krastor::LlmClient(cfg.llm);
        sys_prompt = krastor::compose_system_prompt(cfg, a, tools_active);
        conversation.set_system_prompt(sys_prompt);
        if (!session_id.empty())
            store.set_agent(session_id, a->id);
        return true;
    };
    if (!pending_session_agent.empty() &&
        apply_cli_agent(pending_session_agent))
        std::cout << "agent: " << cfg.agent.active << " (from session)\n";

    // Persist everything appended since the last save; auto-title the session
    // from its first user message while it is still untitled.
    auto autosave = [&] {
        if (!cfg.sessions.autosave || session_id.empty())
            return;
        const auto fresh = conversation.messages_since(saved_count);
        if (fresh.empty())
            return;
        store.append(session_id, fresh);
        saved_count = conversation.messages().size();
        krastor::SessionMeta sm;
        if (store.meta_of(session_id, sm) && sm.title == "New session") {
            for (const auto& msg : conversation.messages())
                if (msg.role == "user" && !msg.content.empty()) {
                    session_title =
                        krastor::SessionStore::title_from_text(msg.content);
                    store.rename(session_id, session_title);
                    break;
                }
        }
    };

    print_help();

    std::string line;
    while (true) {
        std::cout << "> " << std::flush;
        if (!std::getline(std::cin, line))
            break;  // EOF (Ctrl+Z/Ctrl+D)
        if (line.empty())
            continue;

        if (line == "/devices") {
            std::cout << krastor::Stt::list_input_devices();
            continue;
        }
        if (line.rfind("/mic", 0) == 0) {
            int sec = 5, idx = cfg.stt.input_device;
            std::istringstream iss(line.substr(4));
            if (!(iss >> sec) || sec <= 0) sec = 5;
            iss >> idx;  // optional second argument
            const double peak = stt.monitor(sec, idx);
            if (peak >= 0.0)
                std::cout << (peak < 0.01
                                  ? "[mic] NO SIGNAL — wrong device, muted mic, or "
                                    "Windows blocks desktop-app mic access "
                                    "(Settings > Privacy & security > Microphone)\n"
                                  : peak < 0.05
                                        ? "[mic] very quiet — raise the mic volume/gain\n"
                                        : "[mic] signal OK — this device hears you\n");
            continue;
        }
        if (line.rfind("/listen", 0) == 0) {
            int sec = 20, idx = cfg.stt.input_device;
            std::istringstream iss(line.substr(7));
            if (!(iss >> sec) || sec <= 0) sec = 20;
            iss >> idx;  // optional second argument
            std::cout << "[stt] listening... (speak now, up to " << sec << "s)\n";
            const std::string heard = stt.listen(sec, idx);
            if (heard.empty()) { std::cout << "[stt] nothing heard\n"; continue; }
            std::cout << "[stt] " << heard << "\n\n";
            line = heard;  // fall through: the transcription goes to the LLM
        } else if (line.rfind("/transcribe", 0) == 0) {
            std::string arg = line.size() > 11 ? line.substr(11) : "";
            arg.erase(0, arg.find_first_not_of(" \t"));
            if (arg.empty()) { std::cout << "usage: /transcribe <audio file>\n"; continue; }
            const std::string text = stt.transcribe_file(arg);
            std::cout << (text.empty() ? "(no speech recognized)" : text) << "\n";
            continue;
        }

        if (line == "/exit" || line == "/quit")
            break;
        if (line == "/help") { print_help(); continue; }
        if (line == "/voices") { list_voices(voices); continue; }
        if (line == "/reset") {
            conversation.reset();
            session_id = store.create("New session", cfg.llm.model_ref,
                                      cfg.agent.active);
            session_title = "New session";
            remember_session();
            saved_count = conversation.messages().size();
            std::cout << "(conversation cleared, new session " << session_id
                      << ")\n";
            continue;
        }
        if (line == "/sessions") { print_sessions(store); continue; }
        if (line.rfind("/session", 0) == 0) {
            const std::string rest = line.size() > 8 ? line.substr(8) : "";
            std::istringstream iss(trim_arg(rest));
            std::string sub;
            iss >> sub;
            if (sub.empty() || sub == "info") {
                krastor::SessionMeta sm;
                if (store.meta_of(session_id, sm))
                    std::cout << "session: " << sm.id << "\n  title: " << sm.title
                              << "\n  created: " << sm.created
                              << "\n  updated: " << sm.updated
                              << "\n  messages: " << sm.messages
                              << "\n  compactions: " << sm.compactions << "\n";
                else
                    std::cout << "no current session\n";
            } else if (sub == "list") {
                print_sessions(store);
            } else if (sub == "new") {
                const std::string title = trim_arg(rest.substr(rest.find("new") + 3));
                autosave();  // flush the outgoing session first
                conversation.reset();
                session_id = store.create(title.empty() ? "New session" : title,
                                          cfg.llm.model_ref, cfg.agent.active);
                session_title = title.empty() ? "New session" : title;
                remember_session();
                saved_count = conversation.messages().size();
                std::cout << "new session " << session_id << "\n";
            } else if (sub == "open") {
                std::string arg;
                iss >> arg;
                const std::string id = resolve_session(store, arg);
                if (id.empty()) {
                    std::cout << "session not found: " << arg << " (see /sessions)\n";
                } else {
                    autosave();  // keep the outgoing session complete on disk
                    if (store.load(id, conversation)) {
                        krastor::SessionMeta sm;
                        store.meta_of(id, sm);
                        session_id = id;
                        session_title = sm.title;
                        remember_session();
                        saved_count = conversation.messages().size();
                        std::cout << "opened session " << id << " \""
                                  << session_title << "\"\n";
                        if (!sm.agent.empty() && sm.agent != cfg.agent.active &&
                            apply_cli_agent(sm.agent))
                            std::cout << "agent: " << cfg.agent.active
                                      << " (from session)\n";
                    } else {
                        std::cout << "cannot load session " << id << "\n";
                    }
                }
            } else if (sub == "delete") {
                std::string arg;
                iss >> arg;
                const std::string id = resolve_session(store, arg);
                if (id.empty())
                    std::cout << "session not found: " << arg
                              << " (see /sessions)\n";
                else if (store.remove(id)) {
                    std::cout << "deleted session " << id << "\n";
                    if (id == session_id) {  // the ground vanished under us
                        conversation.reset();
                        session_id = store.create("New session", cfg.llm.model_ref,
                                                  cfg.agent.active);
                        session_title = "New session";
                        remember_session();
                        saved_count = conversation.messages().size();
                        std::cout << "started a new session " << session_id << "\n";
                    }
                } else
                    std::cout << "delete failed\n";
            } else if (sub == "rename") {
                const std::string title =
                    trim_arg(rest.substr(rest.find("rename") + 6));
                if (title.empty())
                    std::cout << "usage: /session rename <title>\n";
                else if (store.rename(session_id, title)) {
                    session_title = title;
                    std::cout << "renamed to \"" << title << "\"\n";
                }
            } else
                std::cout << "usage: /session [info|new|open|delete|rename]\n";
            continue;
        }
        if (line.rfind("/title", 0) == 0) {
            const std::string title = trim_arg(line.substr(6));
            if (title.empty())
                std::cout << "current title: " << session_title << "\n";
            else if (store.rename(session_id, title)) {
                session_title = title;
                std::cout << "renamed to \"" << title << "\"\n";
            }
            continue;
        }
        if (line.rfind("/compact", 0) == 0) {
            const std::string focus = trim_arg(line.substr(8));
            std::cout << "compacting session " << session_id << "...\n";
            const auto cr = krastor::compact_conversation(
                llm, conversation, cfg.llm.context_size, cfg.llm.max_tokens,
                focus, nullptr,
                [](const std::string& s) { std::cout << "[compact] " << s << "\n"; });
            if (!cr.ok) {
                std::cout << "compact failed: " << cr.error << "\n";
            } else {
                store.replace_log_after_compact(session_id,
                                                conversation.messages_since(1));
                saved_count = conversation.messages().size();
                std::cout << "compacted " << cr.messages_before << " -> "
                          << cr.messages_after << " messages (~"
                          << cr.est_tokens_before << " -> ~"
                          << cr.est_tokens_after << " tokens, " << cr.llm_calls
                          << " LLM call(s))\n";
            }
            continue;
        }
        if (line == "/agent" || line.rfind("/agent ", 0) == 0) {
            std::string arg = line.size() > 6 ? line.substr(6) : "";
            arg.erase(0, arg.find_first_not_of(" \t"));
            if (arg.empty() || arg == "list") {
                std::cout << "agents (config.jsonc \"agents\"):\n";
                for (const auto& a : cfg.agents)
                    std::cout << (a.id == cfg.agent.active ? "  * " : "    ")
                              << a.id << "  ("
                              << (a.name.empty() ? a.id : a.name)
                              << ", " << a.color << ")\n";
                std::cout << "switch with: /agent use <id>\n";
            } else if (arg.rfind("use ", 0) == 0) {
                const std::string id = trim_arg(arg.substr(4));
                if (apply_cli_agent(id))
                    std::cout << "agent -> " << cfg.agent.active << " ["
                              << cfg.llm.model << "]\n";
                else
                    std::cout << "unknown agent '" << id
                              << "' (see /agent list)\n";
            } else
                std::cout << "usage: /agent [list|use <id>]\n";
            continue;
        }
        if (line == "/config") {
            std::cout << "agent:    " << cfg.agent.active << "\n"
                      << "base_url: " << cfg.llm.base_url << "\n"
                      << "model:    " << cfg.llm.model << "\n"
                      << "thinking: "
                      << (cfg.llm.reasoning_effort.empty()
                              ? "(model default)"
                              : cfg.llm.reasoning_effort)
                      << "\n"
                      << "api_key:  " << (cfg.llm.api_key.empty() ? "(not set)" : "(set)") << "\n";
            print_voice(voices);
            continue;
        }
        if (line.rfind("/voice", 0) == 0) {
            std::string arg = line.size() > 6 ? line.substr(6) : "";
            // trim leading spaces
            arg.erase(0, arg.find_first_not_of(" \t"));
            if (arg.empty()) { print_voice(voices); continue; }
            if (voices.select(arg))
                std::cout << "TTS voice switched -> " << voices.current()->id << '\n';
            else
                std::cout << "unknown voice '" << arg << "' (see /voices)\n";
            continue;
        }
        if (line.rfind("/say", 0) == 0) {
            std::string arg = line.size() > 4 ? line.substr(4) : "";
            arg.erase(0, arg.find_first_not_of(" \t"));
            if (arg.empty()) { std::cout << "usage: /say <text>\n"; continue; }
            tts.speak(arg);
            continue;
        }
        if (line == "/grants" || line.rfind("/grants ", 0) == 0) {
            std::string arg = line.size() > 7 ? line.substr(7) : "";
            arg.erase(0, arg.find_first_not_of(" \t"));
            if (arg == "clear") {
                tool_grants.clear();
                std::cout << "all tool permissions revoked\n";
            } else {
                auto gs = tool_grants.list();
                if (gs.empty())
                    std::cout << "no always-allow grants this session\n";
                else {
                    std::cout << "always-allow grants (session):\n";
                    for (const auto& g : gs)
                        std::cout << "  " << g.first << "  " << g.second << '\n';
                    std::cout << "revoke all with /grants clear\n";
                }
            }
            continue;
        }
        if (line[0] == '/') {
            std::cout << "unknown command (try /help)\n";
            continue;
        }

        conversation.add_user(line);
        std::cout << "...thinking..." << std::flush;
        krastor::ToolLoopHooks hooks;
        hooks.on_call = [](const krastor::ToolCallInfo& info) {
            std::cout << "\r[tool] " << info.tool << " - " << info.action
                      << "..." << std::string(20, ' ') << "\n" << std::flush;
        };
        hooks.on_done = [](const krastor::ToolCallInfo& info,
                           const krastor::ToolResult& tr) {
            std::cout << "[tool] " << info.tool << " - " << info.action << ": "
                      << (tr.ok ? "ok (" + std::to_string(tr.ms) + " ms)"
                                : "FAILED - " + tr.error)
                      << "\n" << std::flush;
        };
        auto reply = krastor::agent_complete(llm, conversation, tool_registry,
                                             tool_runner, &tool_ask, nullptr,
                                             hooks);
        std::cout << "\r" << std::string(40, ' ') << "\r";  // clear the placeholder
        if (reply.ok) {
            conversation.add_assistant(reply.content);
            std::cout << fmt.format(reply.content) << "\n\n";
        } else {
            // The user turn stays in history; the model can still see the question.
            std::cerr << "LLM error: " << reply.error << "\n\n";
        }
        autosave();  // persist the whole turn (user + tool traffic + reply)
    }
    std::cout << "bye.\n";
    return 0;
}
