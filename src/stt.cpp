// Krastor — local STT implementation.
// sherpa-onnx C API (Parakeet transducer + Silero VAD) + miniaudio capture.

#include "stt.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <thread>
#include <vector>

#include "config.h"

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
    ma_device mic{};
    bool mic_ok = false;
    std::mutex pcm_mutex;
    std::vector<float> pcm;

    explicit Impl(const Config& c) : cfg(c) {}
    ~Impl()
    {
        if (mic_ok)
            ma_device_uninit(&mic);
        if (vad)
            SherpaOnnxDestroyVoiceActivityDetector(vad);
        if (recognizer)
            SherpaOnnxDestroyOfflineRecognizer(recognizer);
    }

    static void mic_callback(ma_device* device, void* output, const void* input,
                             ma_uint32 frame_count)
    {
        (void)device;
        (void)output;
        auto* self = static_cast<Impl*>(device->pUserData);
        const auto* samples = static_cast<const float*>(input);
        std::lock_guard<std::mutex> lock(self->pcm_mutex);
        self->pcm.insert(self->pcm.end(), samples, samples + frame_count);
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

std::string Stt::listen(double max_seconds)
{
    if (!ensure_loaded() || !impl_->open_mic())
        return "";

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

std::string Stt::listen(double) { return ""; }
std::string Stt::transcribe_file(const std::string&) { return ""; }

} // namespace krastor

#endif // _WIN32
