// Krastor — microphone input enhancement (noise suppression + echo cancel).
// Shared between the capture path (stt.cpp) and the TTS playback reference
// tap (tts.cpp). This is the ONLY translation unit that includes the RNNoise
// and SpeexDSP headers (isolation rule, same as src/icons.cpp for LunaSVG).

#pragma once

#include <memory>

namespace krastor {

class AudioFx {
public:
    // Process-wide singleton: one microphone pipeline, one playback tap.
    static AudioFx& instance();

    // Runtime toggles (config.json / Settings modal).
    void set_denoise(bool on);
    void set_echo_cancel(bool on);
    bool denoise() const;
    bool echo_cancel() const;

    // Mic path: 16 kHz mono float block of arbitrary length. Writes the
    // processed samples to `out` and returns how many are ready (slightly
    // fewer than n while the internal 10 ms frame buffers fill, then n in
    // steady state — the stream is delayed by ~25 ms, never stretched).
    int process(const float* in, float* out, int n);

    // Far-end reference: the exact samples the speakers are playing (mono,
    // any sample rate). No-op while echo cancellation is off.
    void push_playback(const float* samples, int n, int sample_rate);

    ~AudioFx();
    AudioFx(const AudioFx&) = delete;
    AudioFx& operator=(const AudioFx&) = delete;

private:
    AudioFx();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace krastor
