// Krastor — local TTS implementation.
// sherpa-onnx C API (Piper/VITS) + miniaudio playback. The miniaudio
// implementation itself is compiled once in libs/miniaudio_impl.c (C).

#include "tts.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <thread>
#include <vector>

#include "config.h"
#include "voices.h"

#ifdef _WIN32

#include <miniaudio.h>
#include <sherpa-onnx/c-api/c-api.h>

namespace krastor {

namespace fs = std::filesystem;

struct Tts::Impl {
    VoiceRegistry& voices;
    const Config& cfg;

    const SherpaOnnxOfflineTts* tts = nullptr;
    std::string loaded_folder;  // voice folder currently loaded into `tts`
    ma_engine engine{};
    bool engine_ok = false;

    Impl(VoiceRegistry& v, const Config& c) : voices(v), cfg(c) {}
    ~Impl()
    {
        if (tts)
            SherpaOnnxDestroyOfflineTts(tts);
        if (engine_ok)
            ma_engine_uninit(&engine);
    }

    // Loads (or reloads after a runtime voice switch) the Piper model.
    bool ensure_voice()
    {
        const Voice* v = voices.current();
        if (!v) {
            std::fprintf(stderr, "tts: no voice selected (see /voices)\n");
            return false;
        }
        if (tts && loaded_folder == v->folder)
            return true;

        if (tts) {
            SherpaOnnxDestroyOfflineTts(tts);
            tts = nullptr;
        }

        const fs::path dir = fs::path(cfg.tts.models_dir) / v->folder;
        std::error_code ec;
        fs::path model;
        for (const auto& e : fs::directory_iterator(dir, ec))
            if (!ec && e.is_regular_file() && e.path().extension() == ".onnx")
                model = e.path();
        const fs::path tokens = dir / "tokens.txt";
        const fs::path data_dir = fs::path(cfg.tts.models_dir) / "espeak-ng-data";
        if (model.empty() || !fs::is_regular_file(tokens, ec)) {
            std::fprintf(stderr, "tts: model/tokens not found in %s\n",
                         dir.string().c_str());
            return false;
        }

        std::printf("[tts] loading voice '%s' ...\n", v->id.c_str());
        // Keep the path strings alive for the whole Create call.
        const std::string s_model = model.string();
        const std::string s_tokens = tokens.string();
        const std::string s_data = data_dir.string();
        const std::string s_provider = "cpu";

        SherpaOnnxOfflineTtsConfig c{};
        c.model.vits.model = s_model.c_str();
        c.model.vits.tokens = s_tokens.c_str();
        c.model.vits.data_dir = s_data.c_str();  // espeak-ng frontend
        c.model.vits.noise_scale = 0.667f;
        c.model.vits.noise_scale_w = 0.8f;
        c.model.vits.length_scale = 1.0f;
        c.model.num_threads = 2;
        c.model.debug = 0;
        c.model.provider = s_provider.c_str();

        tts = SherpaOnnxCreateOfflineTts(&c);
        if (!tts) {
            std::fprintf(stderr, "tts: failed to load %s\n", s_model.c_str());
            return false;
        }
        loaded_folder = v->folder;
        return true;
    }

    // Blocking mono float playback through the default output device.
    // miniaudio's decoder expects a container, so the raw float PCM is wrapped
    // in a minimal 44-byte WAV header (IEEE float, mono) in memory.
    void play(const float* samples, int n, int sample_rate)
    {
        const size_t data_bytes = sizeof(float) * static_cast<size_t>(n);
        std::vector<uint8_t> wav(44 + data_bytes);
        auto put32 = [&](size_t off, uint32_t v) {
            wav[off] = v & 0xFF; wav[off + 1] = (v >> 8) & 0xFF;
            wav[off + 2] = (v >> 16) & 0xFF; wav[off + 3] = (v >> 24) & 0xFF;
        };
        auto put16 = [&](size_t off, uint16_t v) {
            wav[off] = v & 0xFF; wav[off + 1] = (v >> 8) & 0xFF;
        };
        memcpy(wav.data(), "RIFF", 4);
        put32(4, 36 + static_cast<uint32_t>(data_bytes));
        memcpy(wav.data() + 8, "WAVEfmt ", 8);
        put32(16, 16);                       // fmt chunk size
        put16(20, 3);                        // WAVE_FORMAT_IEEE_FLOAT
        put16(22, 1);                        // channels
        put32(24, static_cast<uint32_t>(sample_rate));
        put32(28, static_cast<uint32_t>(sample_rate) * 4);  // byte rate
        put16(32, 4);                        // block align
        put16(34, 32);                       // bits per sample
        memcpy(wav.data() + 36, "data", 4);
        put32(40, static_cast<uint32_t>(data_bytes));
        memcpy(wav.data() + 44, samples, data_bytes);

        ma_decoder_config dcfg = ma_decoder_config_init(
            ma_format_f32, 1, static_cast<ma_uint32>(sample_rate));
        ma_decoder decoder;
        if (ma_decoder_init_memory(wav.data(), wav.size(), &dcfg, &decoder) != MA_SUCCESS) {
            std::fprintf(stderr, "tts: playback setup failed\n");
            return;
        }
        ma_sound sound;
        if (ma_sound_init_from_data_source(&engine, &decoder, 0, nullptr,
                                           &sound) != MA_SUCCESS) {
            std::fprintf(stderr, "tts: playback init failed\n");
            ma_decoder_uninit(&decoder);
            return;
        }
        ma_sound_start(&sound);
        while (ma_sound_is_playing(&sound))
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        ma_sound_uninit(&sound);
        ma_decoder_uninit(&decoder);
    }
};

Tts::Tts(VoiceRegistry& voices, const Config& cfg)
    : impl_(std::make_unique<Impl>(voices, cfg))
{
}

Tts::~Tts() = default;

bool Tts::init()
{
    if (impl_->engine_ok)
        return true;
    impl_->engine_ok = (ma_engine_init(nullptr, &impl_->engine) == MA_SUCCESS);
    if (!impl_->engine_ok)
        std::fprintf(stderr,
                     "tts: no audio output device — speech will be silent\n");
    return true;  // synthesis itself still works
}

bool Tts::speak(const std::string& text)
{
    if (text.empty())
        return false;
    if (!impl_->ensure_voice())
        return false;

    const float speed = impl_->cfg.tts.speed > 0.0
                            ? static_cast<float>(impl_->cfg.tts.speed)
                            : 1.0f;
    SherpaOnnxGenerationConfig gen{};
    gen.speed = speed;
    gen.sid = 0;
    gen.silence_scale = 0.2f;

    const auto t0 = std::chrono::steady_clock::now();
    const SherpaOnnxGeneratedAudio* audio = SherpaOnnxOfflineTtsGenerateWithConfig(
        impl_->tts, text.c_str(), &gen, nullptr, nullptr);
    const auto gen_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0)
                            .count();
    if (!audio || audio->n == 0) {
        if (audio)
            SherpaOnnxDestroyOfflineTtsGeneratedAudio(audio);
        std::fprintf(stderr, "tts: synthesis failed\n");
        return false;
    }

    const double secs = audio->n / static_cast<double>(audio->sample_rate);
    const int rate = audio->sample_rate;
    if (impl_->engine_ok)
        impl_->play(audio->samples, audio->n, audio->sample_rate);
    const bool silent = !impl_->engine_ok;
    SherpaOnnxDestroyOfflineTtsGeneratedAudio(audio);

    std::printf("[tts] %s: %.1fs audio @%d Hz, generated in %lld ms%s\n",
                impl_->voices.current()->id.c_str(), secs, rate,
                static_cast<long long>(gen_ms),
                silent ? " (silent: no output device)" : "");
    return true;
}

} // namespace krastor

#else  // ------------------------------------------------------------- non-Windows

namespace krastor {

struct Tts::Impl {};

Tts::Tts(VoiceRegistry&, const Config&) : impl_(std::make_unique<Impl>()) {}
Tts::~Tts() = default;

bool Tts::init()
{
    std::fprintf(stderr, "tts: not available on this platform yet "
                         "(sherpa-onnx prebuilt binaries are Windows-only for now)\n");
    return false;
}

bool Tts::speak(const std::string&) { return false; }

} // namespace krastor

#endif // _WIN32
