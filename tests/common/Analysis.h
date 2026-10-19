#pragma once

// Signal analysis for DSP tests: pitch, level, spectrum

#include <algorithm>
#include <cmath>
#include <complex>
#include <numbers>
#include <vector>

namespace test
{

// Frequency from interpolated upward zero crossings
inline double estimateFrequency (const std::vector<float>& x, double sampleRate, size_t from = 0)
{
    double first = -1.0, last = -1.0;
    int crossings = 0;
    for (size_t i = std::max<size_t> (from, 1); i < x.size(); ++i)
    {
        if (x[i - 1] < 0.0f && x[i] >= 0.0f)
        {
            const double t = (double) (i - 1) + x[i - 1] / (double) (x[i - 1] - x[i]);
            if (first < 0.0)
                first = t;
            last = t;
            ++crossings;
        }
    }
    return crossings < 2 ? 0.0 : (crossings - 1) * sampleRate / (last - first);
}

inline double cents (double f, double reference) { return 1200.0 * std::log2 (f / reference); }

inline double rms (const std::vector<float>& x, size_t from = 0, size_t to = 0)
{
    to = to == 0 ? x.size() : to;
    double sum = 0.0;
    for (size_t i = from; i < to; ++i)
        sum += (double) x[i] * x[i];
    return std::sqrt (sum / (double) (to - from));
}

inline double db (double x) { return 20.0 * std::log10 (std::max (x, 1.0e-12)); }

// Magnitude spectrum of x[from .. from + n) with a 4-term Blackman-Harris window (n = power of 2)
inline std::vector<double> magnitudeSpectrum (const std::vector<float>& x, size_t from, size_t n)
{
    std::vector<std::complex<double>> a (n);
    for (size_t i = 0; i < n; ++i)
    {
        const double p = 2.0 * std::numbers::pi * (double) i / (double) (n - 1);
        const double w = 0.35875 - 0.48829 * std::cos (p) + 0.14128 * std::cos (2 * p) - 0.01168 * std::cos (3 * p);
        a[i] = x[from + i] * w;
    }

    for (size_t i = 1, j = 0; i < n; ++i)
    {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j)
            std::swap (a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1)
    {
        const auto w = std::polar (1.0, -2.0 * std::numbers::pi / (double) len);
        for (size_t i = 0; i < n; i += len)
        {
            std::complex<double> wk = 1.0;
            for (size_t k = 0; k < len / 2; ++k, wk *= w)
            {
                const auto u = a[i + k], v = a[i + k + len / 2] * wk;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
            }
        }
    }

    std::vector<double> mag (n / 2);
    for (size_t i = 0; i < n / 2; ++i)
        mag[i] = std::abs (a[i]);
    return mag;
}

// Worst spectral peak below maxHz that isn't near a harmonic of f0, relative to the
// fundamental, in dB. Optionally reports where it is.
inline double worstAliasDb (const std::vector<float>& x, double sampleRate, double f0, double maxHz, double* worstHz = nullptr)
{
    constexpr size_t n = 1 << 15;
    const auto mag = magnitudeSpectrum (x, x.size() - n, n);
    const double binHz = sampleRate / (double) n;

    const double fundamental = mag[(size_t) std::lround (f0 / binHz)];
    double worst = 0.0;
    for (size_t i = 4; i < mag.size() && (double) i * binHz < maxHz; ++i)
    {
        const double f = (double) i * binHz;
        const double harmonic = std::round (f / f0) * f0;
        if (std::abs (f - harmonic) > 6.0 * binHz && mag[i] > worst)
        {
            worst = mag[i];
            if (worstHz != nullptr)
                *worstHz = f;
        }
    }
    return db (worst / fundamental);
}

// Level (dB) of the component at f in x[from .. from + n), Hann-windowed Goertzel. Steady
// sinusoid of amplitude A reads 20 log10 (A).
inline double toneLevelDb (const std::vector<float>& x, double sampleRate, double f, size_t from, size_t n)
{
    const double w = 2.0 * std::numbers::pi * f / sampleRate;
    std::complex<double> acc = 0.0;
    double windowSum = 0.0;
    for (size_t i = 0; i < n && from + i < x.size(); ++i)
    {
        const double win = 0.5 - 0.5 * std::cos (2.0 * std::numbers::pi * (double) i / (double) (n - 1));
        acc += (double) x[from + i] * win * std::polar (1.0, -w * (double) i);
        windowSum += win;
    }
    return db (2.0 * std::abs (acc) / windowSum);
}

// Frequency of the strongest peak between lo and hi Hz, from a long FFT with parabolic
// interpolation on the log magnitude
inline double peakFrequency (const std::vector<float>& x, double sampleRate, double lo, double hi, size_t from = 0)
{
    size_t n = 1;
    while (n * 2 <= x.size() - from)
        n *= 2;
    const auto mag = magnitudeSpectrum (x, from, n);
    const double binHz = sampleRate / (double) n;
    size_t best = (size_t) (lo / binHz);
    for (size_t i = best; i < (size_t) (hi / binHz) && i + 1 < mag.size(); ++i)
        if (mag[i] > mag[best])
            best = i;
    const double a = std::log (mag[best - 1] + 1e-30), b = std::log (mag[best] + 1e-30), c = std::log (mag[best + 1] + 1e-30);
    return ((double) best + 0.5 * (a - c) / (a - 2.0 * b + c)) * binHz;
}

// Amplitude-weighted mean frequency of x[from .. from + n) (n = power of 2)
inline double spectralCentroid (const std::vector<float>& x, double sampleRate, size_t from, size_t n)
{
    const auto mag = magnitudeSpectrum (x, from, n);
    double sum = 0.0, weighted = 0.0;
    for (size_t i = 1; i < mag.size(); ++i)
    {
        sum += mag[i];
        weighted += mag[i] * (double) i * sampleRate / (double) n;
    }
    return weighted / sum;
}

} // namespace test
