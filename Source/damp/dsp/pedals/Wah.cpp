#include "Wah.h"

#include <cmath>

namespace damp::pedals
{
namespace
{
// Output trim: at default settings (noon) the wah comes out near the bypassed loudness, measured
// as integrated (K-weighted) loudness of a plucked-string DI through the clean amp preset
constexpr float wahTrim = 12.3f;
constexpr double heelHz = 350.0, toeHz = 2200.0;
// The peak's gain along the sweep, relative to noon: up at the heel, well down at the toe, so the
// ear's (and the amp's) treble sensitivity doesn't make the toe jump out. The sweep still gets
// louder heel to toe (about -5 .. +3 dB vs bypass through a clean amp)
constexpr double heelDb = 4.5, toeDb = -12.4;
constexpr double shelf = 0.75; // the low-pass part of the response, relative to the band-pass part
constexpr double controlRate = 50000.0; // coefficient updates per second (at most once a sample)

float follower (double seconds, double rate) { return (float) (1.0 - std::exp (-1.0 / (seconds * rate))); }
} // namespace

void Wah::reset()
{
    for (auto& s : state)
        s = {};
    fresh = true;
}

void Wah::setPatch (const Patch& p, double sampleRate, int samples)
{
    rate = sampleRate;
    blockSize = samples;
    autoMode = p.choice<WahMode> (PID::WahMode) == WahMode::Auto;
    positionTarget = p[PID::WahPosition] / 10.0f;
    const float sensitivity = p[PID::WahSensitivity] / 10.0f;
    depth = 20.0f * sensitivity * sensitivity;
    const double response = p[PID::WahResponse] / 10.0;
    controlInterval = std::max (1, (int) std::lround (rate / controlRate));
    attack = follower (0.030 * std::pow (1.0 / 15.0, response), rate);  // 30 .. 2 ms
    release = follower (0.500 * std::pow (1.0 / 10.0, response), rate); // 500 .. 50 ms
    smoothing = follower (0.008, rate / controlInterval); // a quick rock of the pedal still glides
    damping = (float) (1.0 / (2.0 * std::pow (6.0, p[PID::WahPeak] / 10.0))); // 1/Q: Q 2 .. 12, ~5 at noon
    for (auto& s : state)
    {
        s.input.setCutoff (30.0, rate);
        if (fresh)
            s.position.snap (positionTarget);
    }
    setLevel (wahTrim, fresh);
    fresh = false;
}

void Wah::updateFilter (State& s) const
{
    // Cytomic's trapezoidal state-variable filter: stays well behaved when swept every sample
    const double hz = std::min (heelHz * std::pow (toeHz / heelHz, (double) s.sweep), 0.45 * rate);
    const double g = std::tan (std::numbers::pi * hz / rate), k = s.damping;
    s.a1 = 1.0 / (1.0 + g * (g + k));
    s.a2 = g * s.a1;
    s.a3 = g * s.a2;
    const double p = s.sweep; // a parabola through heel, noon (0 dB) and toe
    s.gain = k * std::pow (10.0, (heelDb * (2.0 * p - 1.0) * (p - 1.0) + toeDb * p * (2.0 * p - 1.0)) / 20.0);
}

void Wah::process (int channel, float* x, int n)
{
    auto& s = state[(size_t) channel];
    auto& level = levelRamp (channel);
    s.position.toward (positionTarget, n);
    for (int i = 0; i < n; ++i)
    {
        const float in = s.input.highpass (x[i]);
        const float pedal = s.position.next();
        if (autoMode)
        {
            const float a = std::abs (in);
            s.envelope += (a - s.envelope) * (a > s.envelope ? attack : release);
        }
        if (--s.countdown <= 0)
        {
            s.countdown = controlInterval;
            float target = pedal;
            if (autoMode)
            {
                const float e = depth * s.envelope;
                target += (1.0f - pedal) * e / (1.0f + e);
            }
            const bool first = s.sweep < 0.0f;
            s.sweep = first ? target : s.sweep + (target - s.sweep) * smoothing;
            s.damping = first ? damping : s.damping + (damping - s.damping) * smoothing;
            updateFilter (s);
        }
        const double v3 = (double) in - s.ic2;
        const double band = s.a1 * s.ic1 + s.a2 * v3;
        const double low = s.ic2 + s.a2 * s.ic1 + s.a3 * v3;
        s.ic1 = 2.0 * band - s.ic1;
        s.ic2 = 2.0 * low - s.ic2;
        x[i] = level.next() * (float) (s.gain * (band + shelf * low));
    }
}

} // namespace damp::pedals
