// Krastor — microphone spectrum analysis implementation.

#include "audio_analysis.h"

#include <algorithm>
#include <cmath>

#include "fft.h"

namespace krastor {

namespace {
constexpr double kPi = 3.14159265358979323846;
// Tiny hash for the synthetic feed.
float hash11(float x)
{
    const double s = std::sin(x * 12.9898) * 43758.5453;
    return static_cast<float>(s - std::floor(s));
}
} // namespace

SpectrumAnalyzer::SpectrumAnalyzer(int sample_rate) : sample_rate_(sample_rate)
{
    history_.assign(kHistory * kBands, 0);

    // Log-spaced band edges from ~60 Hz to just below Nyquist.
    const double fmin = 60.0;
    const double fmax = std::min(7800.0, sample_rate_ * 0.5 * 0.98);
    const double ratio = fmax / fmin;
    for (int i = 0; i <= kBands; ++i)
        edge_hz_[i] = static_cast<float>(fmin * std::pow(ratio, double(i) / kBands));
}

void SpectrumAnalyzer::smooth_from(const float* raw)
{
    // Fast attack, slow release: speech bands should pop, then decay smoothly.
    for (int i = 0; i < kBands; ++i) {
        const float v = std::clamp(raw[i], 0.0f, 1.0f);
        const float cur = bands_[i];
        const float k = v > cur ? 0.55f : 0.08f;
        bands_[i] = cur + k * (v - cur);
    }
}

void SpectrumAnalyzer::feed(const float* window)
{
    // Hann window into the FFT buffers.
    float sum_sq = 0.0f;
    for (int i = 0; i < kWindow; ++i) {
        const float w = 0.5f - 0.5f * std::cos(2.0f * float(kPi) * float(i) / float(kWindow - 1));
        re_[i] = window[i] * w;
        im_[i] = 0.0f;
        sum_sq += window[i] * window[i];
    }
    fft_inplace(re_, im_, kWindow);

    // Band magnitudes: average |X| over the bins of each log band.
    const int half = kWindow / 2;
    const float bin_hz = float(sample_rate_) / float(kWindow);
    float raw[kBands];
    for (int b = 0; b < kBands; ++b) {
        int i0 = std::max(1, int(edge_hz_[b] / bin_hz));
        int i1 = std::min(half - 1, int(edge_hz_[b + 1] / bin_hz));
        if (i1 <= i0)
            i1 = i0 + 1;
        double acc = 0.0;
        for (int i = i0; i <= i1; ++i)
            acc += std::sqrt(double(re_[i]) * re_[i] + double(im_[i]) * im_[i]);
        const double mag = acc / double(i1 - i0 + 1);
        // Compress: speech fundamentals sit low, so boost with a log curve.
        raw[b] = static_cast<float>(std::log1p(mag * 24.0) / std::log(25.0));
    }
    smooth_from(raw);

    const float rms = std::sqrt(sum_sq / float(kWindow));
    level_ += (rms > level_ ? 0.5f : 0.1f) * (std::min(1.0f, rms * 4.0f) - level_);
}

void SpectrumAnalyzer::synthetic(double t)
{
    // Slow drifting waves + per-band shimmer: keeps the effects alive without
    // the microphone (much quieter than real speech).
    float raw[kBands];
    for (int b = 0; b < kBands; ++b) {
        const float f = float(b) / float(kBands);
        float v = 0.10f + 0.07f * std::sin(float(t) * 0.6f + f * 5.0f) *
                  std::sin(float(t) * 0.23f + f * 11.0f);
        v += 0.05f * hash11(float(b) * 7.13f + std::floor(t * 8.0) * 0.31f);
        v *= 1.0f - 0.5f * f;  // tilt toward the low end
        raw[b] = std::max(0.0f, v);
    }
    smooth_from(raw);
    level_ += 0.1f * (0.18f - level_);
}

void SpectrumAnalyzer::advance()
{
    // Ring-write the current bands as the newest column.
    for (int b = 0; b < kBands; ++b)
        history_[size_t(head_) * kBands + b] =
            static_cast<unsigned char>(std::clamp(bands_[b], 0.0f, 1.0f) * 255.0f);
    head_ = (head_ + 1) % kHistory;
}

} // namespace krastor
