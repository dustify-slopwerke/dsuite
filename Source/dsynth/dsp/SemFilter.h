#pragma once

#include "common/dsp/FastMath.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace dsynth
{
using namespace dustify;

// Oberheim SEM-style 12 dB state-variable filter with the continuous LP -> notch -> HP shape
// control and a bandpass switch. The OTA integrators' saturation is modelled as damping that
// grows with the band signal's level: it rounds off loud resonance and, like the SEM, keeps the
// filter from fully self-oscillating.
class SemFilter
{
public:
    void setSampleRate (double newRate) { sampleRate = newRate; }
    void reset() { s1 = s2 = 0.0; }

    // shape: 0 = low-pass, 0.5 = notch, 1 = high-pass (ignored when bandpass)
    float process (float input, double cutoffHz, float resonance, float shape, bool bandpass)
    {
        cutoffHz = std::clamp (cutoffHz, 5.0, 0.45 * sampleRate);
        const double g = fastTan (std::numbers::pi * cutoffHz / sampleRate);

        // Q from 0.5 (no resonance) to 20, exponentially along the knob
        const double R = 1.0 / (2.0 * 0.5 * fastExp2 (5.321928094887363 * resonance)); // 40^resonance
        const double damping = R + 0.06 * s1 * s1; // OTA saturation: louder band, more damping

        const double x = fastTanh ((double) input * 0.5) * 2.0; // gentle input stage
        const double hp = (x - (2.0 * damping + g) * s1 - s2) / (1.0 + 2.0 * damping * g + g * g);
        const double bp = g * hp + s1;
        const double lp = g * bp + s2;
        s1 = g * hp + bp;
        s2 = g * bp + lp;

        if (bandpass)
            return (float) (bp * 2.0 * R); // unity at the peak

        const double sh = std::clamp ((double) shape, 0.0, 1.0);
        return (float) ((lp * (1.0 - sh) + hp * sh) / std::max (sh, 1.0 - sh));
    }

private:
    double sampleRate = 96000.0;
    double s1 = 0.0, s2 = 0.0;
};

// One-pole TPT section used by the Korg35 structure, with the "feedback output" taps it needs
struct OnePole
{
    double G = 0.0, beta = 0.0, s = 0.0;

    double lowpass (double x)
    {
        const double v = (x - s) * G;
        const double y = v + s;
        s = y + v;
        return y;
    }

    double highpass (double x) { return x - lowpass (x); }
    double feedback() const { return beta * s; }
};

// Korg MS-20 (Korg35 Sallen-Key) 12 dB low-pass, after Pirkle's ZDF formulation. The saturating
// feedback amplifier is what makes the MS-20 scream; it self-oscillates at full resonance.
class Ms20Filter
{
public:
    void setSampleRate (double newRate) { sampleRate = newRate; }
    void reset() { lpf1.s = lpf2.s = hpf1.s = 0.0; }

    float process (float input, double cutoffHz, float resonance)
    {
        cutoffHz = std::clamp (cutoffHz, 5.0, 0.45 * sampleRate);
        const double g = fastTan (std::numbers::pi * cutoffHz / sampleRate);
        const double G = g / (1.0 + g);
        const double K = 0.01 + 2.04 * resonance; // self-oscillation from K = 2

        lpf1.G = lpf2.G = hpf1.G = G;
        lpf2.beta = (K - K * G) / (1.0 + g);
        hpf1.beta = -1.0 / (1.0 + g);
        const double alpha0 = 1.0 / (1.0 - K * G + K * G * G);

        const double y1 = lpf1.lowpass (input);
        const double u = fastTanh (alpha0 * (y1 + hpf1.feedback() + lpf2.feedback()));
        const double y = K * lpf2.lowpass (u);
        hpf1.highpass (y);
        return (float) (y / K);
    }

    static bool isSelfOscillating (float resonance) { return 0.01 + 2.04 * resonance >= 2.0; }

private:
    double sampleRate = 96000.0;
    OnePole lpf1, lpf2, hpf1;
};

// Resonant 12 dB high-pass in front of the main filter (MS-20's HPF -> LPF, CS-80's HPF + LPF).
// Saturation on the band path keeps strong resonance musical rather than piercing.
class VoiceHighpass
{
public:
    void setSampleRate (double newRate) { sampleRate = newRate; }
    void reset() { s1 = s2 = 0.0; }

    void setup (double cutoffHz, float resonance)
    {
        bypass = cutoffHz <= 10.5 && resonance <= 0.0f;
        g = std::tan (std::numbers::pi * std::clamp (cutoffHz, 5.0, 0.45 * sampleRate) / sampleRate);
        R = 1.0 / (2.0 * 0.7071 * std::pow (12.0, (double) resonance)); // Q 0.71 .. 8.5
    }

    float process (float x)
    {
        if (bypass)
            return x;
        const double damping = R + 0.05 * s1 * s1;
        const double hp = (x - (2.0 * damping + g) * s1 - s2) / (1.0 + 2.0 * damping * g + g * g);
        const double bp = g * hp + s1;
        const double lp = g * bp + s2;
        s1 = g * hp + bp;
        s2 = g * bp + lp;
        return (float) hp;
    }

private:
    double sampleRate = 96000.0;
    double g = 0.0, R = 0.7071;
    double s1 = 0.0, s2 = 0.0;
    bool bypass = true;
};

} // namespace dsynth
