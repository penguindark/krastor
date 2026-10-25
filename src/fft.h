// Krastor — minimal in-place iterative radix-2 FFT (header-only, no deps).
// Used by the spectrum analyzer to feed audio-reactive shader effects.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace krastor {

// Forward complex FFT of length `n` (must be a power of two), in place:
// `re`/`im` are the real/imaginary sample arrays of length n.
inline void fft_inplace(float* re, float* im, size_t n)
{
    // bit-reversal permutation
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j) {
            std::swap(re[i], re[j]);
            std::swap(im[i], im[j]);
        }
    }
    // butterflies
    for (size_t len = 2; len <= n; len <<= 1) {
        const double ang = -2.0 * 3.14159265358979323846 / static_cast<double>(len);
        const double wvr = std::cos(ang), wvi = std::sin(ang);
        const size_t half = len >> 1;
        for (size_t i = 0; i < n; i += len) {
            double cur_r = 1.0, cur_i = 0.0;
            for (size_t k = 0; k < half; ++k) {
                const double ur = re[i + k], ui = im[i + k];
                const double vr = re[i + k + half] * cur_r - im[i + k + half] * cur_i;
                const double vi = re[i + k + half] * cur_i + im[i + k + half] * cur_r;
                re[i + k] = static_cast<float>(ur + vr);
                im[i + k] = static_cast<float>(ui + vi);
                re[i + k + half] = static_cast<float>(ur - vr);
                im[i + k + half] = static_cast<float>(ui - vi);
                const double nr = cur_r * wvr - cur_i * wvi;
                cur_i = cur_r * wvi + cur_i * wvr;
                cur_r = nr;
            }
        }
    }
}

} // namespace krastor
