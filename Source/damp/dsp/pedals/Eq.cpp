#include "Eq.h"

#include <cmath>

namespace damp::pedals
{
namespace
{
constexpr PID bandParams[Eq::numBands] { PID::Eq100, PID::Eq200, PID::Eq400, PID::Eq800, PID::Eq1k6, PID::Eq3k2, PID::Eq6k4 };
constexpr double bandHz[Eq::numBands] { 100.0, 200.0, 400.0, 800.0, 1600.0, 3200.0, 6400.0 };
// An octave wide (sqrt 2): neighbours both at +12 dB give +14.5 dB at their centres and +12 dB
// half way between them; a single band at +12 dB gives +2.5 dB an octave away, +0.5 dB at two
constexpr double q = 1.41;
constexpr double controlRate = 50000.0; // coefficient updates per second (at most once a sample)
constexpr double smoothingSeconds = 0.01;
} // namespace

void Eq::reset()
{
    for (auto& s : state)
        s = {};
    fresh = true;
}

void Eq::setPatch (const Patch& p, double sampleRate, int samples)
{
    rate = sampleRate;
    blockSize = samples;
    controlInterval = std::max (1, (int) std::lround (rate / controlRate));
    smoothing = 1.0 - std::exp (-controlInterval / (smoothingSeconds * rate));
    for (int b = 0; b < numBands; ++b)
    {
        g[(size_t) b] = std::tan (std::numbers::pi * std::min (bandHz[b], 0.45 * rate) / rate);
        target[(size_t) b] = std::pow (10.0, p[bandParams[b]] / 40.0);
    }
    for (auto& s : state)
        for (int b = 0; b < numBands; ++b)
        {
            auto& band = s.bands[(size_t) b];
            if (fresh)
                band.gain = target[(size_t) b];
            updateBand (band, b); // the rate may have changed
        }
    setLevel (dbToGain (p[PID::EqLevel]), fresh);
    fresh = false;
}

void Eq::updateBand (Band& band, int index) const
{
    // Bell: low + band * (1 + k (A^2 - 1)) + high, with k = 1 / (Q A); m1 = 0 at 0 dB
    const double a = band.gain, k = 1.0 / (q * a), gb = g[(size_t) index];
    band.a1 = 1.0 / (1.0 + gb * (gb + k));
    band.a2 = gb * band.a1;
    band.a3 = gb * band.a2;
    band.m1 = k * (a * a - 1.0);
}

void Eq::process (int channel, float* x, int n)
{
    auto& s = state[(size_t) channel];
    auto& level = levelRamp (channel);
    for (int i = 0; i < n; ++i)
    {
        if (--s.countdown <= 0)
        {
            s.countdown = controlInterval;
            for (int b = 0; b < numBands; ++b)
            {
                auto& band = s.bands[(size_t) b];
                const double t = target[(size_t) b];
                if (band.gain == t)
                    continue;
                band.gain = std::abs (t - band.gain) < 1e-6 * t ? t : band.gain + (t - band.gain) * smoothing;
                updateBand (band, b);
            }
        }
        double v = x[i];
        for (auto& band : s.bands)
        {
            const double v3 = v - band.ic2;
            const double v1 = band.a1 * band.ic1 + band.a2 * v3;
            const double v2 = band.ic2 + band.a2 * band.ic1 + band.a3 * v3;
            band.ic1 = 2.0 * v1 - band.ic1;
            band.ic2 = 2.0 * v2 - band.ic2;
            v += band.m1 * v1;
        }
        x[i] = level.next() * (float) v;
    }
}

} // namespace damp::pedals
