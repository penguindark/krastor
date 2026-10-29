// Krastor — local STT implementation.
// sherpa-onnx C API (Parakeet transducer + Silero VAD) + miniaudio capture.

#include "stt.h"

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <thread>
#include <vector>

#include "config.h"
#include "audio_fx.h"

#ifdef _WIN32

#include <miniaudio.h>
#include <sherpa-onnx/c-api/c-api.h>

namespace krastor {

namespace fs = std::filesystem;

namespace {
constexpr int kSampleRate = 16000;
constexpr int kVadWindow = 512;  // Silero window size in samples
} // namespace

struct Stt::Impl {
    const Config& cfg;

    const SherpaOnnxOfflineRecognizer* recognizer = nullptr;
    const SherpaOnnxVoiceActivityDetector* vad = nullptr;

    // Microphone capture state.
    ma_context ctx{};
    bool ctx_ok = false;
    ma_device mic{};
    bool mic_ok = false;
    int open_index = -2;  // device index currently opened (-1 = system default)
    std::mutex pcm_mutex;
    std::vector<float> pcm;

    // Live input levels (for the /mic meter).
    std::atomic<float> level{0.0f};  // max |sample| of the most recent block
    std::atomic<float> peak{0.0f};   // max |sample| since the last reset

    // Hands-free stream mode (see Stt::start_stream).
    std::thread feeder;
    std::atomic<bool> stream_run{false};

    // Lock-free ring of recent mic samples for the shader spectrum feed.
    // Single producer (mic callback), single consumer (render thread).
    static constexpr size_t kRing = 16384;  // 1 s @ 16 kHz
    std::vector<float> ring = std::vector<float>(kRing, 0.0f);
    std::atomic<size_t> ring_w{0};  // monotonically increasing write cursor

    struct CapDev {
        ma_device_id id;
        std::string name;
        bool is_default;
    };

    explicit Impl(const Config& c) : cfg(c)
    {
        ctx_ok = (ma_context_init(nullptr, 0, nullptr, &ctx) == MA_SUCCESS);
    }

    ~Impl()
    {
        stream_run.store(false);
        if (feeder.joinable())
            feeder.join();
        if (mic_ok)
            ma_device_uninit(&mic);
        if (vad)
            SherpaOnnxDestroyVoiceActivityDetector(vad);
        if (recognizer)
            SherpaOnnxDestroyOfflineRecognizer(recognizer);
        if (ctx_ok)
            ma_context_uninit(&ctx);
    }

    static ma_bool32 enum_cb(ma_context*, ma_device_type type,
                             const ma_device_info* info, void* ud)
    {
        if (type == ma_device_type_capture) {
            auto* out = static_cast<std::vector<CapDev>*>(ud);
            out->push_back({info->id, info->name, info->isDefault == MA_TRUE});
        }
        return MA_TRUE;
    }

    // All capture devices in enumeration order (index = position in this list).
    static std::vector<CapDev> enumerate()
    {
        std::vector<CapDev> out;
        ma_context c{};
        if (ma_context_init(nullptr, 0, nullptr, &c) != MA_SUCCESS)
            return out;
        ma_context_enumerate_devices(&c, enum_cb, &out);
        ma_context_uninit(&c);
        return out;
    }

    std::string mic_name() const
    {
        const auto devs = enumerate();
        if (open_index >= 0 && open_index < static_cast<int>(devs.size()))
            return devs[open_index].name;
        for (const auto& d : devs)
            if (d.is_default)
                return d.name;
        return "(system default)";
    }

    static void mic_callback(ma_device* device, void* output, const void* input,
                             ma_uint32 frame_count)
    {
        (void)device;
        (void)output;
        auto* self = static_cast<Impl*>(device->pUserData);
        const auto* samples = static_cast<const float*>(input);
        // Echo cancellation + noise suppression (see audio_fx.h). The stage
        // may return fewer samples than fed while its 10 ms frame buffers
        // fill; the rest arrives with the next block (fixed delay, no loss).
        float proc[4096];
        const float* src = samples;
        ma_uint32 left = frame_count;
        float mx = 0.0f;
        while (left > 0) {
            const int chunk = static_cast<int>(std::min<ma_uint32>(left, 4096));
            const int got = AudioFx::instance().process(src, proc, chunk);
            src += chunk;
            left -= static_cast<ma_uint32>(chunk);
            for (int i = 0; i < got; ++i) {
                const float a = std::fabs(proc[i]);
                if (a > mx) mx = a;
            }
            if (got <= 0)
                continue;
            // Copy into the visualization ring, then publish the new cursor
            // so the reader never sees an index ahead of the data.
            const size_t w = self->ring_w.load(std::memory_order_relaxed);
            for (int i = 0; i < got; ++i)
                self->ring[(w + i) % Stt::Impl::kRing] = proc[i];
            self->ring_w.store(w + static_cast<size_t>(got),
                               std::memory_order_release);
            std::lock_guard<std::mutex> lock(self->pcm_mutex);
            self->pcm.insert(self->pcm.end(), proc, proc + got);
        }
        self->level.store(mx);
        float cur = self->peak.load();
        while (cur < mx && !self->peak.compare_exchange_weak(cur, mx)) {}
    }

    // Opens (or switches to) the capture device and starts it.
    // device_index < 0 = system default microphone.
    bool open_mic(int device_index)
    {
        if (mic_ok && open_index != device_index) {
            ma_device_uninit(&mic);
            mic_ok = false;
        }
        if (!mic_ok) {
            ma_device_config dc = ma_device_config_init(ma_device_type_capture);
            dc.capture.format = ma_format_f32;
            dc.capture.channels = 1;
            dc.sampleRate = kSampleRate;
            dc.dataCallback = mic_callback;
            dc.pUserData = this;
            if (device_index >= 0) {
                const auto devs = enumerate();
                if (device_index >= static_cast<int>(devs.size())) {
                    std::fprintf(stderr,
                                 "stt: no capture device with index %d (see /devices)\n",
                                 device_index);
                    return false;
                }
                dc.capture.pDeviceID = &devs[device_index].id;
            }
            const ma_result r = ma_device_init(ctx_ok ? &ctx : nullptr, &dc, &mic);
            if (r != MA_SUCCESS) {
                std::fprintf(stderr,
                             "stt: cannot open microphone (miniaudio error %d) — run /devices\n",
                             static_cast<int>(r));
                return false;
            }
            mic_ok = true;
            open_index = device_index;
            {
                std::lock_guard<std::mutex> lock(pcm_mutex);
                pcm.clear();
            }
        }
        if (ma_device_start(&mic) != MA_SUCCESS) {
            std::fprintf(stderr, "stt: cannot start microphone capture\n");
            return false;
        }
        return true;
    }

    // Runs the recognizer on one PCM block and returns the text.
    std::string decode(const float* samples, int n) const
    {
        const SherpaOnnxOfflineStream* stream =
            SherpaOnnxCreateOfflineStream(recognizer);
        SherpaOnnxAcceptWaveformOffline(stream, kSampleRate, samples, n);
        SherpaOnnxDecodeOfflineStream(recognizer, stream);
        const SherpaOnnxOfflineRecognizerResult* result =
            SherpaOnnxGetOfflineStreamResult(stream);
        std::string text = result && result->text ? result->text : "";
        SherpaOnnxDestroyOfflineRecognizerResult(result);
        SherpaOnnxDestroyOfflineStream(stream);
        // Trim surrounding whitespace.
        const auto b = text.find_first_not_of(" \t\n\r");
        const auto e = text.find_last_not_of(" \t\n\r");
        return b == std::string::npos ? "" : text.substr(b, e - b + 1);
    }

    // Decodes every closed VAD segment; returns the first non-empty text
    // (or an empty string when no segment is ready yet).
    std::string drain_vad()
    {
        std::string out;
        while (!SherpaOnnxVoiceActivityDetectorEmpty(vad)) {
            const SherpaOnnxSpeechSegment* seg =
                SherpaOnnxVoiceActivityDetectorFront(vad);
            std::string text = decode(seg->samples, seg->n);
            SherpaOnnxDestroySpeechSegment(seg);
            SherpaOnnxVoiceActivityDetectorPop(vad);
            if (!out.empty())
                out += " ";
            out += text;
        }
        return out;
    }

    bool open_mic()
    {
        if (mic_ok)
            return true;
        ma_device_config dc = ma_device_config_init(ma_device_type_capture);
        dc.capture.format = ma_format_f32;
        dc.capture.channels = 1;
        dc.sampleRate = kSampleRate;
        dc.dataCallback = mic_callback;
        dc.pUserData = this;
        if (ma_device_init(nullptr, &dc, &mic) != MA_SUCCESS) {
            std::fprintf(stderr, "stt: cannot open the default microphone\n");
            return false;
        }
        mic_ok = true;
        {
            std::lock_guard<std::mutex> lock(pcm_mutex);
            pcm.clear();
        }
        if (ma_device_start(&mic) != MA_SUCCESS) {
            std::fprintf(stderr, "stt: cannot start microphone capture\n");
            ma_device_uninit(&mic);
            mic_ok = false;
            return false;
        }
        return true;
    }
};

Stt::Stt(const Config& cfg) : impl_(std::make_unique<Impl>(cfg)) {}
Stt::~Stt() = default;

bool Stt::ensure_loaded()
{
    if (impl_->recognizer && impl_->vad)
        return true;

    const fs::path models(impl_->cfg.tts.models_dir);
    const fs::path parakeet = models / "stt-parakeet-multilingual";
    const fs::path vad_model = models / "vad-silero" / "silero_vad.onnx";
    std::error_code ec;
    if (!fs::is_regular_file(parakeet / "encoder.int8.onnx", ec) ||
        !fs::is_regular_file(vad_model, ec)) {
        std::fprintf(stderr,
                     "stt: Parakeet or Silero VAD model missing — run "
                     "tools\\download_models.py\n");
        return false;
    }

    if (!impl_->recognizer) {
        std::printf("[stt] loading Parakeet TDT v3 (EN+IT) ...\n");
        const std::string enc = (parakeet / "encoder.int8.onnx").string();
        const std::string dec = (parakeet / "decoder.int8.onnx").string();
        const std::string jn = (parakeet / "joiner.int8.onnx").string();
        const std::string tok = (parakeet / "tokens.txt").string();
        const std::string provider = "cpu";

        SherpaOnnxOfflineRecognizerConfig c{};
        c.model_config.transducer.encoder = enc.c_str();
        c.model_config.transducer.decoder = dec.c_str();
        c.model_config.transducer.joiner = jn.c_str();
        c.model_config.tokens = tok.c_str();
        c.model_config.model_type = "nemo_transducer";
        c.model_config.num_threads = 8;
        c.model_config.provider = provider.c_str();
        c.model_config.debug = 0;
        c.decoding_method = "greedy_search";

        impl_->recognizer = SherpaOnnxCreateOfflineRecognizer(&c);
        if (!impl_->recognizer) {
            std::fprintf(stderr, "stt: failed to load Parakeet from %s\n",
                         parakeet.string().c_str());
            return false;
        }
    }

    if (!impl_->vad) {
        const std::string vad_path = vad_model.string();
        SherpaOnnxVadModelConfig v{};
        v.silero_vad.model = vad_path.c_str();
        v.silero_vad.threshold = 0.5f;
        v.silero_vad.min_silence_duration = 0.5f;  // close ~0.5 s after speech ends
        v.silero_vad.min_speech_duration = 0.25f;
        v.silero_vad.max_speech_duration = 15.0f;
        v.silero_vad.window_size = kVadWindow;
        v.sample_rate = kSampleRate;
        v.num_threads = 1;
        v.debug = 0;

        impl_->vad = SherpaOnnxCreateVoiceActivityDetector(&v, 60.0f);
        if (!impl_->vad) {
            std::fprintf(stderr, "stt: failed to load Silero VAD\n");
            return false;
        }
    }
    return true;
}

std::string Stt::list_input_devices()
{
    const auto devs = Impl::enumerate();
    if (devs.empty())
        return "no capture devices found\n";
    std::string out = "capture devices:\n";
    for (size_t i = 0; i < devs.size(); ++i) {
        out += "  " + std::to_string(i) + "  " + devs[i].name;
        if (devs[i].is_default)
            out += "  [default]";
        out += '\n';
    }
    out += "Select one with: /listen <sec> <index>  or set stt.input_device in config.json\n";
    return out;
}

double Stt::monitor(double seconds, int device_index)
{
    if (!impl_->open_mic(device_index))
        return -1.0;
    std::printf("[mic] input: %s — speak or clap for %.0fs\n",
                impl_->mic_name().c_str(), seconds);
    impl_->level.store(0.0f);
    impl_->peak.store(0.0f);

    constexpr int kBar = 30;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(static_cast<int>(seconds * 1000));
    while (std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        const float lvl = impl_->level.load();
        float scaled = lvl * 4.0f;  // amplify so normal speech fills the bar
        if (scaled > 1.0f)
            scaled = 1.0f;
        const int filled = static_cast<int>(scaled * kBar);
        std::printf("\r[mic] [");
        for (int i = 0; i < kBar; ++i)
            std::printf("%s", i < filled ? "#" : ".");
        std::printf("] %.3f   ", lvl);
        std::fflush(stdout);
    }
    const double peak = impl_->peak.load();
    ma_device_stop(&impl_->mic);
    std::printf("\r[mic] peak level: %.4f%s\n", peak,
                peak < 0.01 ? "  <- NO SIGNAL" : "");
    return peak;
}

std::string Stt::listen(double max_seconds, int device_index)
{
    if (impl_->stream_run.load())  // recognizer busy on the stream feeder
        return "";
    if (!ensure_loaded() || !impl_->open_mic(device_index))
        return "";
    std::printf("[stt] input: %s\n", impl_->mic_name().c_str());

    SherpaOnnxVoiceActivityDetectorClear(impl_->vad);
    {
        std::lock_guard<std::mutex> lock(impl_->pcm_mutex);
        impl_->pcm.clear();
    }

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(
                              static_cast<int>(max_seconds * 1000));
    std::vector<float> local;
    while (std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        {
            std::lock_guard<std::mutex> lock(impl_->pcm_mutex);
            local.swap(impl_->pcm);
        }
        size_t off = 0;
        while (off + kVadWindow <= local.size()) {
            SherpaOnnxVoiceActivityDetectorAcceptWaveform(
                impl_->vad, local.data() + off, kVadWindow);
            off += kVadWindow;
        }
        // Keep the (< window) remainder for the next round.
        if (off < local.size()) {
            std::lock_guard<std::mutex> lock(impl_->pcm_mutex);
            impl_->pcm.insert(impl_->pcm.begin(), local.begin() + off,
                              local.end());
        }

        std::string text = impl_->drain_vad();
        if (!text.empty()) {
            ma_device_stop(&impl_->mic);
            return text;
        }
    }

    // Timeout: force-close any speech in progress and transcribe it.
    SherpaOnnxVoiceActivityDetectorFlush(impl_->vad);
    std::string text = impl_->drain_vad();
    ma_device_stop(&impl_->mic);
    return text;
}

std::string Stt::transcribe_file(const std::string& path)
{
    if (!ensure_loaded())
        return "";

    // Decode any supported container to 16 kHz mono float.
    ma_decoder_config dcfg = ma_decoder_config_init(ma_format_f32, 1, kSampleRate);
    ma_decoder decoder;
    if (ma_decoder_init_file(path.c_str(), &dcfg, &decoder) != MA_SUCCESS) {
        std::fprintf(stderr, "stt: cannot decode %s\n", path.c_str());
        return "";
    }
    std::vector<float> samples;
    float buf[4096];
    ma_uint64 got = 0;
    ma_result res = MA_SUCCESS;
    while ((res = ma_decoder_read_pcm_frames(&decoder, buf, 4096, &got)) == MA_SUCCESS &&
           got > 0)
        samples.insert(samples.end(), buf, buf + got);
    ma_decoder_uninit(&decoder);

    if (samples.size() < static_cast<size_t>(kSampleRate / 10)) {
        std::fprintf(stderr, "stt: %s is too short to transcribe\n", path.c_str());
        return "";
    }
    return impl_->decode(samples.data(), static_cast<int>(samples.size()));
}

std::string Stt::recognize(const float* samples, size_t n)
{
    if (!ensure_loaded() || !samples || n < static_cast<size_t>(kSampleRate / 10))
        return "";
    return impl_->decode(samples, static_cast<int>(n));
}

bool Stt::start_stream(int device_index,
                       std::function<void(std::vector<float>)> on_utterance)
{
    if (impl_->stream_run.load())
        return true;  // already streaming
    if (!ensure_loaded() || !impl_->open_mic(device_index))
        return false;

    SherpaOnnxVoiceActivityDetectorClear(impl_->vad);
    {
        std::lock_guard<std::mutex> lock(impl_->pcm_mutex);
        impl_->pcm.clear();
    }
    impl_->stream_run.store(true);
    Impl* impl = impl_.get();
    impl->feeder = std::thread([impl, on_utterance] {
        std::vector<float> local;
        while (impl->stream_run.load()) {
          try {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            {
                std::lock_guard<std::mutex> lock(impl->pcm_mutex);
                local.swap(impl->pcm);
            }
            // Feed the VAD in fixed 512-sample windows; keep the remainder.
            size_t off = 0;
            while (off + kVadWindow <= local.size()) {
                SherpaOnnxVoiceActivityDetectorAcceptWaveform(
                    impl->vad, local.data() + off, kVadWindow);
                off += kVadWindow;
            }
            if (off < local.size()) {
                std::lock_guard<std::mutex> lock(impl->pcm_mutex);
                impl->pcm.insert(impl->pcm.begin(), local.begin() + off,
                                 local.end());
            }
            local.clear();
            // Hand every closed segment's audio to the pipeline callback.
            while (!SherpaOnnxVoiceActivityDetectorEmpty(impl->vad)) {
                const SherpaOnnxSpeechSegment* seg =
                    SherpaOnnxVoiceActivityDetectorFront(impl->vad);
                std::vector<float> audio;
                if (seg->n >= kSampleRate / 4)  // ignore < 0.25 s blips
                    audio.assign(seg->samples, seg->samples + seg->n);
                SherpaOnnxDestroySpeechSegment(seg);
                SherpaOnnxVoiceActivityDetectorPop(impl->vad);
                if (!audio.empty() && on_utterance)
                    on_utterance(std::move(audio));
            }
          } catch (const std::exception& e) {
            // An escaping exception would terminate() the process; log,
            // back off briefly and keep the stream alive.
            std::fprintf(stderr, "stt stream error: %s\n", e.what());
            local.clear();
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
          }
        }
    });
    return true;
}

void Stt::stop_stream()
{
    if (!impl_->stream_run.exchange(false))
        return;
    if (impl_->feeder.joinable())
        impl_->feeder.join();
    if (impl_->vad)
        SherpaOnnxVoiceActivityDetectorClear(impl_->vad);
    if (impl_->mic_ok)
        ma_device_stop(&impl_->mic);
}

bool Stt::streaming() const
{
    return impl_->stream_run.load();
}

size_t Stt::latest(float* dst, size_t n) const
{
    using ImplT = Stt::Impl;
    n = std::min(n, ImplT::kRing);
    const size_t w = impl_->ring_w.load(std::memory_order_acquire);
    if (w == 0 || n == 0)
        return 0;
    const size_t take = std::min(n, w);
    for (size_t i = 0; i < take; ++i)
        dst[i] = impl_->ring[(w - take + i) % ImplT::kRing];
    return take;
}

} // namespace krastor

#else  // ------------------------------------------------------------- non-Windows

namespace krastor {

struct Stt::Impl {};

Stt::Stt(const Config&) : impl_(std::make_unique<Impl>()) {}
Stt::~Stt() = default;

bool Stt::ensure_loaded()
{
    std::fprintf(stderr, "stt: not available on this platform yet "
                         "(sherpa-onnx prebuilt binaries are Windows-only for now)\n");
    return false;
}

std::string Stt::list_input_devices() { return "stt: not available on this platform\n"; }
double Stt::monitor(double, int) { return -1.0; }
std::string Stt::listen(double, int) { return ""; }
std::string Stt::transcribe_file(const std::string&) { return ""; }
std::string Stt::recognize(const float*, size_t) { return ""; }
bool Stt::start_stream(int, std::function<void(std::vector<float>)>) { return false; }
void Stt::stop_stream() {}
bool Stt::streaming() const { return false; }
size_t Stt::latest(float*, size_t) const { return 0; }

} // namespace krastor

#endif // _WIN32
