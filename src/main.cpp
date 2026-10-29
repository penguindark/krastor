// Krastor — LLM agent with voice input/output and real-time graphics interface.
// Phase 3: core agent CLI — config, OpenAI-compatible LLM chat loop and runtime
// TTS voice selection. STT/TTS audio and the GUI arrive in Phases 4/5.
// See project.md (master description) and src/development.md (this folder).

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include "config.h"
#include "audio_fx.h"
#include "conversation.h"
#include "formatters.h"
#include "highlight.h"
#include "gui.h"
#include "icons.h"
#include "llm_client.h"
#include "llm_models.h"
#include "stt.h"
#include "tts.h"
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
        "  /reset             clear the conversation (keeps system prompt)\n"
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

    // Args: [--gui] [--icon-test] [--fx-test] [config.json]
    bool use_gui = false;
    std::string config_path;  // empty -> default (exe dir, legacy CWD fallback)
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--gui") use_gui = true;
        else if (a == "--icon-test") return run_icon_test();
        else if (a == "--fx-test") return run_fx_test();
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
    // directory is ignored once bin/config.json exists (see default_path()).
    std::cout << "config: " << config_path << '\n';
    // Migrate a legacy working-directory config to the canonical location next
    // to the executable so future auto-saves have a complete file to update.
    if (config_path == "config.json") {
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

    // llm_models.json (opencode.jsonc format): when llm.model_ref names an
    // entry there, its URL/key/model/context override the explicit fields.
    krastor::LlmModelRegistry models;
    if (!models.load(krastor::LlmModelRegistry::default_path()))
        std::cout << "note: " << models.error()
                  << " — using llm.base_url/model from config.json\n";
    else if (krastor::apply_model_ref(cfg.llm, models))
        std::cout << "model: " << cfg.llm.model_ref << " -> " << cfg.llm.base_url
                  << "\n";

    krastor::VoiceRegistry voices;
    if (voices.discover(cfg.tts.models_dir)) {
        if (!cfg.tts.default_voice.empty() && !voices.select(cfg.tts.default_voice))
            std::cerr << "warning: default_voice '" << cfg.tts.default_voice
                      << "' not found, using first discovered voice\n";
    }

    if (use_gui)
        return krastor::run_gui(cfg, voices);

    print_voice(voices);

    krastor::Tts tts(voices, cfg);
    tts.init();
    krastor::Stt stt(cfg);

    if (cfg.llm.api_key.empty())
        std::cout << "note: no API key configured (config.json llm.api_key or "
                     "KRASTOR_API_KEY) — LLM calls will fail\n";

    krastor::LlmClient llm(cfg.llm);
    krastor::OutputFormatter fmt(cfg);
    krastor::Highlighter hl;
    if (hl.load_default())
        fmt.set_highlighter([&hl](const std::string& code, const std::string& lang) {
            return hl.highlight(code, lang);
        });
    krastor::Conversation conversation(cfg.llm.system_prompt);
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
        if (line == "/reset") { conversation.reset(); std::cout << "(conversation cleared)\n"; continue; }
        if (line == "/config") {
            std::cout << "base_url: " << cfg.llm.base_url << "\n"
                      << "model:    " << cfg.llm.model << "\n"
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
        if (line[0] == '/') {
            std::cout << "unknown command (try /help)\n";
            continue;
        }

        conversation.add_user(line);
        std::cout << "...thinking..." << std::flush;
        auto reply = llm.complete(conversation);
        std::cout << "\r" << std::string(40, ' ') << "\r";  // clear the placeholder
        if (reply.ok) {
            conversation.add_assistant(reply.content);
            std::cout << fmt.format(reply.content) << "\n\n";
        } else {
            // The user turn stays in history; the model can still see the question.
            std::cerr << "LLM error: " << reply.error << "\n\n";
        }
    }
    std::cout << "bye.\n";
    return 0;
}
