// Krastor — microphone spectrum analysis for audio-reactive shader effects.
// Turns 512-sample windows (16 kHz) into 64 log-spaced, smoothed amplitude
// bands plus a scrolling time-history ring that shader plugins sample as a
// texture. Also provides a synthetic feed so effects animate without the mic.

#pragma once

#include <vector>

namespace krastor {

class SpectrumAnalyzer {
public:
    static constexpr int kWindow = 512;   // FFT size (samples @ 16 kHz = 32 ms)
    static constexpr int kBands = 64;     // log-spaced output bands
    static constexpr int kHistory = 128;  // time columns kept for the texture

    explicit SpectrumAnalyzer(int sample_rate = 16000);

    // Analyze one window of kWindow mic samples (updates bands + level).
    void feed(const float* window);
    // Fake feed for when the microphone is closed (keeps effects alive).
    void synthetic(double t_seconds);
    // Shift the current bands into the history ring (call once per frame).
    void advance();

    const float* bands() const { return bands_; }        // kBands values, 0..1
    float level() const { return level_; }              // smoothed RMS, 0..1
    const unsigned char* history() const { return history_.data(); }
    int head() const { return head_; }                  // newest column index

private:
    void smooth_from(const float* raw_bands);

    int sample_rate_;
    float bands_[kBands] = {0};
    float level_ = 0.0f;
    std::vector<unsigned char> history_;
    int head_ = 0;
    float re_[kWindow], im_[kWindow];
    float edge_hz_[kBands + 1];  // log-spaced band edges
};

} // namespace krastor
