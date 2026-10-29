// Krastor — microphone input enhancement implementation.
// Pipeline (all at the mic callback's pace, 16 kHz mono float):
//   mic block -> [SpeexDSP AEC, 10 ms frames, far-end from push_playback()]
//             -> [RNNoise denoiser, resampled to 48 kHz, 10 ms frames]
//             -> processed output ring
// Only this file includes rnnoise.h / speex headers (see audio_fx.h).

#include "audio_fx.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <vector>

#include <rnnoise.h>
#include <speex/speex_echo.h>
#include <speex/speex_resampler.h>

namespace krastor {

namespace {
constexpr int kRate = 16000;    // mic / AEC sample rate
constexpr int kRnRate = 48000;  // RNNoise works at 48 kHz only
constexpr int kAecFrame = 160;  // 10 ms @ 16 kHz (Speex AEC frame)
constexpr int kRnFrame = 480;   // 10 ms @ 48 kHz (RNNoise frame)
constexpr int kAecTail = 4096;  // 256 ms echo tail: covers WASAPI buffering
constexpr int kRefDelay = 1600; // 100 ms nominal playback->mic offset
constexpr uint64_t kRefSize = 48000; // 3 s far-end ring @ 16 kHz

// Streams every pending sample of `src` through a Speex resampler, appending
// the converted output to `dst`; consumed input is erased.
void resample_stream(SpeexResamplerState* rs, std::vector<float>& src,
                     std::vector<float>& dst)
{
    size_t off = 0;
    while (off < src.size()) {
        spx_uint32_t in_len =
            static_cast<spx_uint32_t>(std::min<size_t>(src.size() - off, 256));
        float tmp[1024];
        spx_uint32_t out_len = 1024;
        speex_resampler_process_float(rs, 0, src.data() + off, &in_len, tmp,
                                      &out_len);
        dst.insert(dst.end(), tmp, tmp + out_len);
        off += in_len;
    }
    src.erase(src.begin(), src.begin() + off);
}

// float [-1,1] <-> int16 (the Speex AEC's native format).
spx_int16_t to16(float x)
{
    const float v = x * 32767.0f;
    return static_cast<spx_int16_t>(v > 32767.0f ? 32767
                                     : v < -32768.0f ? -32768 : v);
}
} // namespace

struct AudioFx::Impl {
    std::mutex mtx;  // guards every member below (mic + playback threads)

    bool denoise_on = true;
    bool echo_on = true;

    // Denoise stage.
    DenoiseState* rn = nullptr;
    SpeexResamplerState* rs_up = nullptr;   // 16k -> 48k
    SpeexResamplerState* rs_down = nullptr; // 48k -> 16k
    std::vector<float> in16;           // mic samples awaiting AEC framing
    std::vector<float> buf48;          // 48 kHz staging
    std::vector<float> ready;          // processed output awaiting pickup

    // Echo stage.
    SpeexEchoState* aec = nullptr;
    SpeexResamplerState* rs_ref = nullptr;  // playback rate -> 16k
    int ref_rate = 0;
    std::vector<float> ref_ring = std::vector<float>(kRefSize, 0.0f);
    uint64_t ref_w = 0;  // total far-end samples written
    uint64_t ref_r = 0;  // total far-end samples consumed

    Impl()
    {
        rn = rnnoise_create(nullptr);
        int err = 0;
        rs_up = speex_resampler_init(1, kRate, kRnRate, 5, &err);
        rs_down = speex_resampler_init(1, kRnRate, kRate, 5, &err);
        aec = speex_echo_state_init(kAecFrame, kAecTail);
        const int rate = kRate;
        speex_echo_ctl(aec, SPEEX_ECHO_SET_SAMPLING_RATE, const_cast<int*>(&rate));
    }

    ~Impl()
    {
        if (aec)
            speex_echo_state_destroy(aec);
        if (rs_ref)
            speex_resampler_destroy(rs_ref);
        if (rs_up)
            speex_resampler_destroy(rs_up);
        if (rs_down)
            speex_resampler_destroy(rs_down);
        if (rn)
            rnnoise_destroy(rn);
    }
};

AudioFx& AudioFx::instance()
{
    static AudioFx inst;
    return inst;
}

AudioFx::AudioFx() : impl_(std::make_unique<Impl>()) {}
AudioFx::~AudioFx() = default;

void AudioFx::set_denoise(bool on)
{
    std::lock_guard<std::mutex> lock(impl_->mtx);
    if (on == impl_->denoise_on)
        return;
    impl_->denoise_on = on;
    impl_->in16.clear();
    impl_->buf48.clear();
    impl_->ready.clear();
}

void AudioFx::set_echo_cancel(bool on)
{
    std::lock_guard<std::mutex> lock(impl_->mtx);
    if (on == impl_->echo_on)
        return;
    impl_->echo_on = on;
    speex_echo_state_reset(impl_->aec);
    impl_->ref_r = impl_->ref_w;  // start from "now" when re-enabled
}

bool AudioFx::denoise() const
{
    std::lock_guard<std::mutex> lock(impl_->mtx);
    return impl_->denoise_on;
}

bool AudioFx::echo_cancel() const
{
    std::lock_guard<std::mutex> lock(impl_->mtx);
    return impl_->echo_on;
}

int AudioFx::process(const float* in, float* out, int n)
{
    std::lock_guard<std::mutex> lock(impl_->mtx);
    Impl& I = *impl_;

    if (!I.echo_on && !I.denoise_on) {
        std::memcpy(out, in, sizeof(float) * static_cast<size_t>(n));
        return n;
    }

    // --- Stage 1: acoustic echo cancellation (160-sample frames) ----------
    I.in16.insert(I.in16.end(), in, in + n);
    std::vector<float> mid;
    if (I.echo_on) {
        size_t off = 0;
        float ref[kAecFrame];
        spx_int16_t rec16[kAecFrame], ref16[kAecFrame], out16[kAecFrame];
        while (I.in16.size() - off >= static_cast<size_t>(kAecFrame)) {
            // Far-end window: nominally kRefDelay samples behind the live
            // write cursor; Speex's own delay estimator fine-tunes within
            // the 256 ms tail. Gaps (no playback) feed silence.
            const uint64_t target =
                I.ref_w > static_cast<uint64_t>(kRefDelay)
                    ? I.ref_w - static_cast<uint64_t>(kRefDelay)
                    : 0;
            if (I.ref_r + kAecFrame <= target &&
                I.ref_r + kAecFrame + kRefSize >= I.ref_w) {
                for (int i = 0; i < kAecFrame; ++i)
                    ref[i] = I.ref_ring[(I.ref_r + i) % kRefSize];
                I.ref_r += kAecFrame;
            } else {
                std::memset(ref, 0, sizeof(ref));
                if (I.ref_r < target)
                    I.ref_r = target;  // skip the gap after silence
            }
            // The Speex AEC works on int16 samples: scale in, scale back out.
            for (int i = 0; i < kAecFrame; ++i) {
                rec16[i] = to16(I.in16[off + i]);
                ref16[i] = to16(ref[i]);
            }
            speex_echo_cancellation(I.aec, rec16, ref16, out16);
            for (int i = 0; i < kAecFrame; ++i)
                mid.push_back(static_cast<float>(out16[i]) / 32768.0f);
            off += kAecFrame;
        }
        I.in16.erase(I.in16.begin(), I.in16.begin() + off);
    } else {
        I.ref_r = I.ref_w;  // keep the read cursor fresh while disabled
        mid.swap(I.in16);   // no AEC: everything passes through
    }

    // --- Stage 2: RNNoise denoise (480-sample frames at 48 kHz) -----------
    if (I.denoise_on) {
        resample_stream(I.rs_up, mid, I.buf48);
        size_t p = 0;
        while (I.buf48.size() - p >= static_cast<size_t>(kRnFrame)) {
            // RNNoise's float API works on int16-scaled samples (±32768):
            // scale the frame up, denoise in place, scale back to ±1.
            float* f = I.buf48.data() + p;
            for (int i = 0; i < kRnFrame; ++i)
                f[i] *= 32768.0f;
            rnnoise_process_frame(I.rn, f, f);
            for (int i = 0; i < kRnFrame; ++i)
                f[i] /= 32768.0f;
            p += kRnFrame;
        }
        // The denoised frames leave the 48 kHz staging buffer and go back
        // to 16 kHz; only the (< frame) remainder stays for the next call.
        std::vector<float> done(I.buf48.begin(),
                                I.buf48.begin() + static_cast<long>(p));
        I.buf48.erase(I.buf48.begin(), I.buf48.begin() + static_cast<long>(p));
        resample_stream(I.rs_down, done, I.ready);
    } else {
        I.ready.insert(I.ready.end(), mid.begin(), mid.end());
    }

    const int got = static_cast<int>(
        std::min<size_t>(I.ready.size(), static_cast<size_t>(n)));
    std::memcpy(out, I.ready.data(), sizeof(float) * static_cast<size_t>(got));
    I.ready.erase(I.ready.begin(), I.ready.begin() + got);
    return got;
}

void AudioFx::push_playback(const float* samples, int n, int sample_rate)
{
    std::lock_guard<std::mutex> lock(impl_->mtx);
    Impl& I = *impl_;
    if (!I.echo_on || n <= 0)
        return;

    // Resample the far-end to 16 kHz (the rate can change with the TTS voice).
    std::vector<float> ref16;
    if (sample_rate == kRate) {
        ref16.assign(samples, samples + n);
    } else {
        if (I.rs_ref && I.ref_rate != sample_rate) {
            speex_resampler_destroy(I.rs_ref);
            I.rs_ref = nullptr;
        }
        if (!I.rs_ref) {
            int err = 0;
            I.rs_ref = speex_resampler_init(1, sample_rate, kRate, 3, &err);
            I.ref_rate = sample_rate;
        }
        std::vector<float> raw(samples, samples + n);
        resample_stream(I.rs_ref, raw, ref16);  // distinct src/dst vectors
    }

    // Write into the far-end ring (wrap in at most two chunks).
    size_t pos = I.ref_w % kRefSize;
    size_t left = ref16.size();
    const float* src = ref16.data();
    while (left > 0) {
        const size_t chunk = std::min(left, kRefSize - pos);
        std::memcpy(I.ref_ring.data() + pos, src, sizeof(float) * chunk);
        pos = (pos + chunk) % kRefSize;
        src += chunk;
        left -= chunk;
    }
    I.ref_w += ref16.size();
}

} // namespace krastor
