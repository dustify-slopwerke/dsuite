#include "Octave.h"

#include <cmath>

namespace damp::pedals
{
namespace
{
// Level knobs are audio-taper-ish (square law). Trims: the direct path at 7 (the default) passes
// the guitar at unity; a sub at 7 has about the guitar's RMS level, but sitting below most of the
// ear's (and the amp's) sensitivity it adds little loudness, so the default mix (direct and one
// down at 7) stays near the bypassed loudness (integrated, K-weighted, through the amp)
constexpr float directTrim = 2.04f, subTrim = 2.1f;
constexpr float hysteresis = 0.3f; // of the tracked fundamental's peak

float follower (double seconds, double rate) { return (float) (1.0 - std::exp (-1.0 / (seconds * rate))); }
float knob (float v) { return (v / 10.0f) * (v / 10.0f); }
} // namespace

void Octave::reset()
{
    for (auto& s : state)
        s = {};
    fresh = true;
}

void Octave::setPatch (const Patch& p, double sampleRate, int samples)
{
    rate = sampleRate;
    blockSize = samples;
    directTarget = directTrim * knob (p[PID::OctaveDirect]);
    oct1Target = subTrim * knob (p[PID::OctaveOct1]);
    oct2Target = subTrim * knob (p[PID::OctaveOct2]);
    attack = follower (0.002, rate);
    release = follower (0.040, rate);
    peakRelease = follower (0.030, rate);
    for (auto& s : state)
    {
        s.input.setCutoff (40.0, rate);
        for (auto& t : s.track)
            t.setCutoff (100.0, rate);
        s.sub1Tone.setCutoff (1000.0, rate);
        s.sub2Tone.setCutoff (500.0, rate);
        s.output.setCutoff (15.0, rate);
        if (fresh)
        {
            s.direct.snap (directTarget);
            s.oct1.snap (oct1Target);
            s.oct2.snap (oct2Target);
        }
    }
    fresh = false;
}

void Octave::process (int channel, float* x, int n)
{
    auto& s = state[(size_t) channel];
    s.direct.toward (directTarget, n);
    s.oct1.toward (oct1Target, n);
    s.oct2.toward (oct2Target, n);
    for (int i = 0; i < n; ++i)
    {
        const float in = s.input.highpass (x[i]);
        const float a = std::abs (in);
        s.level += (a - s.level) * (a > s.level ? attack : release);

        float f = in;
        for (auto& t : s.track)
            f = t.lowpass (f);
        s.peak = std::max (std::abs (f), s.peak * (1.0f - peakRelease));

        // Arm below the hysteresis level, then toggle where the fundamental next rises through zero
        if (f < -hysteresis * s.peak)
            s.armed = true;
        else if (s.armed && f >= 0.0f)
        {
            s.armed = false;
            s.first = ! s.first;
            if (s.first)
                s.second = ! s.second;
        }

        // The fundamental at unit amplitude, carrying the input's envelope
        const float carrier = s.peak > 1.0e-20f ? f / s.peak * s.level : 0.0f;
        const float sub1 = s.sub1Tone.lowpass (s.first ? carrier : -carrier);
        // Flipping the fundamental's polarity at a quarter of its frequency would leave mostly
        // 3f/4; flipping its rectified version gives a square-ish wave with f/4 at its root
        // (still continuous: it toggles where the fundamental crosses zero)
        const float rectified = std::abs (carrier);
        const float sub2 = s.sub2Tone.lowpass (s.second ? rectified : -rectified);
        const float subs = s.output.highpass (s.oct1.next() * sub1 + s.oct2.next() * sub2);
        x[i] = s.direct.next() * x[i] + subs;
    }
}

} // namespace damp::pedals
