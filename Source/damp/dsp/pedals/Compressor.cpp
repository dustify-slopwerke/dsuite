#include "Compressor.h"

#include <cmath>

namespace damp::pedals
{
namespace
{
// The compressed path's trim: at default settings it comes out near the bypassed loudness
// (integrated, K-weighted, through the amp, from a plucked-string DI peaking at -12 dBFS)
constexpr float compressorTrim = 1.18f;
// Makeup gain, dB, for the Sustain knob (0 .. 10): more sustain squashes the peaks but lifts the
// decay, so the loudness stays about constant across the knob (0 dB at noon)
float makeupDb (float sustain) { return 6.49f * std::exp (-sustain / 3.56f) - 1.59f; }
constexpr float threshold = 0.1f;          // envelope level where the OTA's gain has halved
constexpr float ceiling = 3.0f * threshold; // the OTA saturates: a pluck overshoots by ~10 dB at most
constexpr double releaseSeconds = 0.3;

float follower (double seconds, double rate) { return (float) (1.0 - std::exp (-1.0 / (seconds * rate))); }
} // namespace

void Compressor::reset()
{
    for (auto& s : state)
        s = {};
    fresh = true;
}

void Compressor::setPatch (const Patch& p, double sampleRate, int samples)
{
    rate = sampleRate;
    blockSize = samples;
    driveTarget = dbToGain (3.0f * p[PID::CompressorSustain]);            // 0 .. 30 dB into the OTA
    makeupTarget = compressorTrim * dbToGain (makeupDb (p[PID::CompressorSustain]));
    attack = follower (0.2e-3 * std::pow (100.0, p[PID::CompressorAttack] / 10.0), rate); // 0.2 .. 20 ms
    release = follower (releaseSeconds, rate);
    blendTarget = p[PID::CompressorBlend] / 10.0f;
    for (auto& s : state)
    {
        s.input.setCutoff (20.0, rate);
        s.treble.setCutoff (9000.0, rate); // the classic's slight dulling
        if (fresh)
        {
            s.drive.snap (driveTarget);
            s.makeup.snap (makeupTarget);
            s.blend.snap (blendTarget);
        }
    }
    setLevel (dbToGain (p[PID::CompressorLevel]), fresh);
    fresh = false;
}

void Compressor::process (int channel, float* x, int n)
{
    auto& s = state[(size_t) channel];
    auto& level = levelRamp (channel);
    // Ramps restart from where this channel's last block left them
    s.drive.toward (driveTarget, n);
    s.makeup.toward (makeupTarget, n);
    s.blend.toward (blendTarget, n);
    float envelope = s.envelope; // in a local: x may alias the state as far as the compiler knows
    for (int i = 0; i < n; ++i)
    {
        const float v = s.input.highpass (x[i]) * s.drive.next();
        const float a = std::abs (v);
        envelope += (a - envelope) * (a > envelope ? attack : release);
        // The OTA's gain falls as the envelope rises: loud passages level off near the threshold
        const float squashed = v / (1.0f + envelope * (1.0f / threshold));
        // Its saturation (limiter() at hardness 2)
        const float saturated = squashed / std::sqrt (1.0f + squashed * squashed * (1.0f / (ceiling * ceiling)));
        const float compressed = s.makeup.next() * s.treble.lowpass (saturated);
        const float b = s.blend.next();
        x[i] = level.next() * (x[i] + b * (compressed - x[i]));
    }
    s.envelope = envelope;
}

} // namespace damp::pedals
