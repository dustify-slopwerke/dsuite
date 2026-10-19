#include "Drives.h"

#include <cmath>

namespace damp::pedals
{
namespace
{
// Each pedal's output trim: Level at 0 dB with default knobs comes out near the bypassed loudness
// (measured on a plucked-string DI peaking at -12 dBFS; the boost boosts by its Gain knob)
constexpr float boostTrim = 1.0f, midDriveTrim = 0.172f, bluesTrim = 0.081f, distortionTrim = 0.226f, rodentTrim = 0.157f,
                faceFuzzTrim = 0.050f, bigFuzzTrim = 0.218f;
} // namespace

// ---- Boost: clean gain from a high-headroom transistor stage, tilt tone around 800 Hz ----

void Boost::reset()
{
    for (auto& s : state)
        s = {};
    fresh = true;
}

void Boost::setPatch (const Patch& p, double sampleRate, int samples)
{
    rate = sampleRate;
    blockSize = samples;
    gain = dbToGain (p[PID::BoostGain]);
    const float tilt = (p[PID::BoostTone] - 5.0f) / 5.0f * 6.0f; // +-6 dB each side
    for (auto& s : state)
    {
        s.input.setCutoff (20.0, rate);
        s.low.setLowShelf (800.0, -tilt, rate);
        s.high.setHighShelf (800.0, tilt, rate);
    }
    setLevel (boostTrim, fresh);
    fresh = false;
}

void Boost::process (int channel, float* x, int n)
{
    auto& s = state[(size_t) channel];
    auto& level = levelRamp (channel);
    for (int i = 0; i < n; ++i)
    {
        const float v = s.high.process (s.low.process (s.input.highpass (x[i]) * gain));
        x[i] = level.next() * limiter (v, 2.0f, 2.5f); // runs out of headroom only at the very top
    }
}

// ---- Mid Drive (TS-style): the op-amp's gain applies above ~720 Hz only, diodes in its
// feedback clip that gained part softly, the clean signal passes alongside; then a tone filter ----

void MidDrive::reset()
{
    for (auto& s : state)
        s = {};
    fresh = true;
}

void MidDrive::setPatch (const Patch& p, double sampleRate, int samples)
{
    rate = sampleRate;
    blockSize = samples;
    // Feedback resistance: 51k + the 500k drive pot (audio taper); gain leg 4.7k + 47 nF
    const float rf = 51.0e3f + 500.0e3f * logTaper (p[PID::MidDriveDrive] / 10.0f);
    clipGain = rf / 4.7e3f;
    const double feedbackHz = 1.0 / (2.0 * std::numbers::pi * rf * 51.0e-12); // 51 pF across the feedback
    const double toneHz = 600.0 * std::pow (10.0, p[PID::MidDriveTone] / 10.0); // 600 Hz .. 6 kHz
    for (auto& s : state)
    {
        s.input.setCutoff (20.0, rate);
        s.emphasis.setCutoff (720.0, rate);
        s.feedbackCap.setCutoff (feedbackHz, rate);
        s.tone.setCutoff (toneHz, rate);
        s.output.setCutoff (10.0, rate);
    }
    setLevel (midDriveTrim * dbToGain (p[PID::MidDriveLevel]), fresh);
    fresh = false;
}

void MidDrive::process (int channel, float* x, int n)
{
    auto& s = state[(size_t) channel];
    auto& level = levelRamp (channel);
    for (int i = 0; i < n; ++i)
    {
        const float in = s.input.highpass (x[i]);
        const float gained = s.feedbackCap.lowpass (s.emphasis.highpass (in) * clipGain);
        const float out = in + limiter (gained, 0.55f, 2.5f);
        x[i] = level.next() * s.output.highpass (s.tone.lowpass (out));
    }
}

// ---- Blues Drive: two discrete gain stages, the first clipping asymmetrically, a wide
// (not mid-humped) response and a treble shelf for tone ----

void BluesDrive::reset()
{
    for (auto& s : state)
        s = {};
    fresh = true;
}

void BluesDrive::setPatch (const Patch& p, double sampleRate, int samples)
{
    rate = sampleRate;
    blockSize = samples;
    gain = 2.0f * std::pow (50.0f, p[PID::BluesDriveGain] / 10.0f); // 2 .. 100
    const float tone = p[PID::BluesDriveTone];
    for (auto& s : state)
    {
        s.input.setCutoff (90.0, rate);
        s.between.setCutoff (7000.0, rate);
        s.output.setCutoff (15.0, rate);
        if (std::abs (tone - toneSetting) > 1.0e-4f)
            s.shelf.setHighShelf (1500.0, (tone - 5.0f) / 5.0f * 10.0f, rate);
    }
    toneSetting = tone;
    setLevel (bluesTrim * dbToGain (p[PID::BluesDriveLevel]), fresh);
    fresh = false;
}

void BluesDrive::process (int channel, float* x, int n)
{
    auto& s = state[(size_t) channel];
    auto& level = levelRamp (channel);
    constexpr float bias = 0.2f;
    static const float offset = std::tanh (bias);
    for (int i = 0; i < n; ++i)
    {
        const float v = s.input.highpass (x[i]) * gain;
        const float first = s.between.lowpass (std::tanh (v + bias) - offset);
        const float second = limiter (2.0f * first, 0.9f, 2.0f);
        x[i] = level.next() * s.output.highpass (s.shelf.process (second));
    }
}

// ---- Distortion (DS-1-style): a transistor booster into an op-amp with diodes to ground (hard
// clipping), then a tone control that crossfades a low-pass and a high-pass (a scoop at noon) ----

void Distortion::reset()
{
    for (auto& s : state)
        s = {};
    fresh = true;
}

void Distortion::setPatch (const Patch& p, double sampleRate, int samples)
{
    rate = sampleRate;
    blockSize = samples;
    gain = 8.0f * (1.0f + 100.0e3f * logTaper (p[PID::DistortionDist] / 10.0f) / 4.7e3f); // booster x op-amp
    tone = p[PID::DistortionTone] / 10.0f;
    for (auto& s : state)
    {
        s.input.setCutoff (100.0, rate);
        s.opamp.setCutoff (8000.0, rate);
        s.low.setCutoff (234.0, rate);   // 6.8k / 100 nF
        s.high.setCutoff (1500.0, rate); // 22 nF into the tone pot
        s.output.setCutoff (15.0, rate);
    }
    setLevel (distortionTrim * dbToGain (p[PID::DistortionLevel]), fresh);
    fresh = false;
}

void Distortion::process (int channel, float* x, int n)
{
    auto& s = state[(size_t) channel];
    auto& level = levelRamp (channel);
    for (int i = 0; i < n; ++i)
    {
        const float clipped = limiter (s.opamp.lowpass (s.input.highpass (x[i]) * gain), 0.6f, 3.5f);
        const float shaped = (1.0f - tone) * 2.5f * s.low.lowpass (clipped) + tone * s.high.highpass (clipped);
        x[i] = level.next() * s.output.highpass (shaped);
    }
}

// ---- Rodent (RAT-style): an op-amp with up to ~67 dB of gain whose own bandwidth (gain-bandwidth
// product ~1 MHz) darkens it as the gain goes up, diodes to ground, then a variable low-pass ----

void Rodent::reset()
{
    for (auto& s : state)
        s = {};
    fresh = true;
}

void Rodent::setPatch (const Patch& p, double sampleRate, int samples)
{
    rate = sampleRate;
    blockSize = samples;
    pot = 100.0e3f * logTaper (p[PID::RodentDist] / 10.0f);
    const float closedLoop = 1.0f + pot / 47.0f + pot / 560.0f;
    const double bandwidthHz = std::clamp (1.0e6 / closedLoop, 300.0, 0.45 * rate);
    const double filterHz = 1.0 / (2.0 * std::numbers::pi * (1.5e3 + 100.0e3 * p[PID::RodentFilter] / 10.0) * 3.3e-9);
    for (auto& s : state)
    {
        s.input.setCutoff (20.0, rate);
        s.leg47.setCutoff (1539.0, rate);  // 47R + 2.2 uF
        s.leg560.setCutoff (60.5, rate);   // 560R + 4.7 uF
        s.bandwidth.setCutoff (bandwidthHz, rate);
        s.filter.setCutoff (filterHz, rate);
        s.output.setCutoff (15.0, rate);
    }
    setLevel (rodentTrim * dbToGain (p[PID::RodentLevel]), fresh);
    fresh = false;
}

void Rodent::process (int channel, float* x, int n)
{
    auto& s = state[(size_t) channel];
    auto& level = levelRamp (channel);
    for (int i = 0; i < n; ++i)
    {
        const float in = s.input.highpass (x[i]);
        // Non-inverting gain 1 + Rpot (Y47 + Y560): each ground leg conducts above its corner
        const float gained = in + pot * (s.leg47.highpass (in) / 47.0f + s.leg560.highpass (in) / 560.0f);
        const float clipped = limiter (s.bandwidth.lowpass (gained), 0.6f, 3.0f); // silicon diodes' exponential knee
        x[i] = level.next() * s.output.highpass (s.filter.lowpass (clipped));
    }
}

// ---- Face Fuzz: two transistors with a lot of gain, clipping asymmetrically; heavy clipping
// shifts the bias, which makes the decay sputter and gate ----

void FaceFuzz::reset()
{
    for (auto& s : state)
        s = {};
    fresh = true;
}

void FaceFuzz::setPatch (const Patch& p, double sampleRate, int samples)
{
    rate = sampleRate;
    blockSize = samples;
    gain = 20.0f * std::pow (15.0f, p[PID::FaceFuzzFuzz] / 10.0f); // 20 .. 300
    charge = (float) (1.0 - std::exp (-1.0 / (0.004 * rate)));
    bleed = (float) std::exp (-1.0 / (0.08 * rate));
    for (auto& s : state)
    {
        s.input.setCutoff (60.0, rate);
        s.darken.setCutoff (5000.0, rate);
        s.output.setCutoff (30.0, rate);
    }
    setLevel (faceFuzzTrim * dbToGain (p[PID::FaceFuzzLevel]), fresh);
    fresh = false;
}

void FaceFuzz::process (int channel, float* x, int n)
{
    auto& s = state[(size_t) channel];
    auto& level = levelRamp (channel);
    for (int i = 0; i < n; ++i)
    {
        const float v = s.input.highpass (x[i]) * gain - s.shift;
        // Saturates hard one way, softly and later the other
        const float clipped = v > 0.0f ? limiter (v, 0.7f, 6.0f) : limiter (v, 1.4f, 2.0f);
        s.shift = (s.shift + std::max (0.0f, v - clipped) * charge * 0.02f) * bleed;
        x[i] = level.next() * s.output.highpass (s.darken.lowpass (clipped));
    }
}

// ---- Big Fuzz (Muff-style): two cascaded clipping stages (diodes in feedback: soft and
// symmetric), each with its own band limits, then the passive tone control's mid scoop ----

void BigFuzz::reset()
{
    for (auto& s : state)
        s = {};
    fresh = true;
}

void BigFuzz::setPatch (const Patch& p, double sampleRate, int samples)
{
    rate = sampleRate;
    blockSize = samples;
    gain = 5.0f * std::pow (20.0f, p[PID::BigFuzzSustain] / 10.0f); // 5 .. 100
    tone = p[PID::BigFuzzTone] / 10.0f;
    for (auto& s : state)
    {
        s.input.setCutoff (20.0, rate);
        s.couple1.setCutoff (16.0, rate); // 100 nF into 100k
        s.cap1.setCutoff (3400.0, rate);  // 100 pF across 470k
        s.couple2.setCutoff (16.0, rate);
        s.cap2.setCutoff (3400.0, rate);
        s.low.setCutoff (450.0, rate);    // the tone network's low-pass leg
        s.high.setCutoff (1600.0, rate);  // and its high-pass leg
        s.output.setCutoff (20.0, rate);
    }
    setLevel (bigFuzzTrim * dbToGain (p[PID::BigFuzzLevel]), fresh);
    fresh = false;
}

void BigFuzz::process (int channel, float* x, int n)
{
    auto& s = state[(size_t) channel];
    auto& level = levelRamp (channel);
    for (int i = 0; i < n; ++i)
    {
        float v = s.input.highpass (x[i]);
        v = s.cap1.lowpass (limiter (s.couple1.highpass (v) * gain, 0.6f, 2.0f));
        v = s.cap2.lowpass (limiter (s.couple2.highpass (v) * 30.0f, 0.6f, 2.0f));
        const float shaped = (1.0f - tone) * s.low.lowpass (v) + tone * s.high.highpass (v);
        x[i] = level.next() * s.output.highpass (shaped);
    }
}

} // namespace damp::pedals
