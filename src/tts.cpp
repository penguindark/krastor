// Krastor — local TTS implementation.
// sherpa-onnx C API (Piper/VITS) + miniaudio playback. The miniaudio
// implementation itself is compiled once in libs/miniaudio_impl.c (C).

#include "tts.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <thread>
#include <vector>

#include "config.h"
#include "voices.h"
#include "audio_fx.h"
#include "tts_text.h"

#ifdef _WIN32

#include <miniaudio.h>
#include <sherpa-onnx/c-api/c-api.h>

#include <algorithm>
#include <mutex>

namespace krastor {

namespace fs = std::filesystem;

struct Tts::Impl {
    VoiceRegistry& voices;
    const Config& cfg;

    const SherpaOnnxOfflineTts* tts = nullptr;
    std::string loaded_folder;  // voice folder currently loaded into `tts`
    std::atomic<bool> cancel{false};  // set by interrupt(): stop playback now

    // Dedicated playback device (instead of ma_engine): its data callback
    // knows the exact samples reaching the speakers, so it can feed the
    // far-end reference to the AEC (AudioFx::push_playback). Opened lazily
    // at the loaded voice's native rate; mono float32.
    ma_device out{};
    bool out_ok = false;
    int out_rate = 0;

    // Current playback buffer (guarded by play_mtx; the audio thread pulls
    // sequential frames, play() waits for the cursor to reach the end).
    std::mutex play_mtx;
    const float* play_buf = nullptr;
    long long play_total = 0;
    long long play_pos = 0;

    Impl(VoiceRegistry& v, const Config& c) : voices(v), cfg(c) {}
    ~Impl()
    {
        if (tts)
            SherpaOnnxDestroyOfflineTts(tts);
        if (out_ok)
            ma_device_uninit(&out);
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

    // Opens (or reopens after a voice sample-rate change) the playback device
    // at the voice's native rate and keeps it running; idle = silence.
    bool ensure_out(int sample_rate)
    {
        if (out_ok && out_rate == sample_rate)
            return true;
        if (out_ok) {
            ma_device_uninit(&out);
            out_ok = false;
        }
        ma_device_config dc = ma_device_config_init(ma_device_type_playback);
        dc.playback.format = ma_format_f32;
        dc.playback.channels = 1;
        dc.sampleRate = static_cast<ma_uint32>(sample_rate);
        dc.dataCallback = out_callback;
        dc.pUserData = this;
        if (ma_device_init(nullptr, &dc, &out) != MA_SUCCESS ||
            ma_device_start(&out) != MA_SUCCESS) {
            std::fprintf(stderr,
                         "tts: no audio output device — speech will be silent\n");
            out_ok = false;
            return false;
        }
        out_ok = true;
        out_rate = sample_rate;
        return true;
    }

    // Pulls the current utterance's PCM; whatever is emitted (including the
    // zero padding) is handed to the AEC as the far-end reference.
    static void out_callback(ma_device* device, void* output, const void*,
                             ma_uint32 frames)
    {
        auto* self = static_cast<Impl*>(device->pUserData);
        auto* out = static_cast<float*>(output);
        {
            std::lock_guard<std::mutex> lock(self->play_mtx);
            long long take = 0;
            if (self->play_buf && !self->cancel.load())
                take = std::min<long long>(
                    self->play_total - self->play_pos,
                    static_cast<long long>(frames));
            if (take > 0)
                std::memcpy(out, self->play_buf + self->play_pos,
                            sizeof(float) * static_cast<size_t>(take));
            if (take < static_cast<long long>(frames))
                std::memset(out + take, 0,
                            sizeof(float) * (frames - take));
            self->play_pos += take;
        }
        AudioFx::instance().push_playback(out, static_cast<int>(frames),
                                          self->out_rate);
    }

    // Blocking mono float playback through the default output device.
    bool play(const float* samples, int n, int sample_rate)
    {
        if (!ensure_out(sample_rate))
            return false;
        {
            std::lock_guard<std::mutex> lock(play_mtx);
            play_buf = samples;
            play_total = n;
            play_pos = 0;
        }
        while (!cancel.load() && play_pos < play_total)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        if (cancel.load()) {
            // Let the callback switch to silence before releasing the buffer.
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
        }
        {
            std::lock_guard<std::mutex> lock(play_mtx);
            play_buf = nullptr;
            play_total = 0;
            play_pos = 0;
        }
        return true;
    }
};

Tts::Tts(VoiceRegistry& voices, const Config& cfg)
    : impl_(std::make_unique<Impl>(voices, cfg))
{
}

Tts::~Tts() = default;

bool Tts::init()
{
    // The playback device opens lazily with the voice's sample rate (see
    // Impl::ensure_out); synthesis itself always works.
    return true;
}

bool Tts::speak(const std::string& text)
{
    // The speakers get the plain prose: Markdown markup, code blocks,
    // tables, links and emoji are filtered out (see tts_text.h).
    const std::string spoken = tts_filter(text);
    if (spoken.empty())
        return false;
    impl_->cancel.store(false);
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
        impl_->tts, spoken.c_str(), &gen, nullptr, nullptr);
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
    const bool played = !impl_->cancel.load() &&
                        impl_->play(audio->samples, audio->n, audio->sample_rate);
    const bool silent = !played;
    SherpaOnnxDestroyOfflineTtsGeneratedAudio(audio);

    std::printf("[tts] %s: %.1fs audio @%d Hz, generated in %lld ms%s\n",
                impl_->voices.current()->id.c_str(), secs, rate,
                static_cast<long long>(gen_ms),
                silent ? " (silent: no output device)" : "");
    return true;
}

void Tts::interrupt()
{
    impl_->cancel.store(true);
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
void Tts::interrupt() {}

} // namespace krastor

#endif // _WIN32
