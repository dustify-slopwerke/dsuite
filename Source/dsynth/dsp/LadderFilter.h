#pragma once

#include "common/dsp/FastMath.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace dsynth
{
using namespace dustify;

// Zero-delay-feedback 4-pole transistor ladder (Zavalishin TPT form). The differential-pair
// input stage is modelled with a tanh on (input - k * feedback), solved against a linear
// estimate of the output: cheap, stable, and it self-oscillates at the cutoff when k > 4.
// The 12 dB mode taps the second stage while feedback still comes from the fourth, as in the
// Oberheim Xpander / Matrix-12 multimode ladders.
class LadderFilter
{
public:
    struct Character
    {
        double drive;        // input differential-pair saturation
        double passbandKeep; // built-in resonance gain compensation (0..1)
    };

    static constexpr Character moog { 1.0, 0.0 };
    // ARP 4072/4075: near-copy of the ladder, but cleaner and losing less low end with resonance
    static constexpr Character arp { 0.6, 0.5 };

    static constexpr double maxFeedback = 4.4; // resonance 1.0; self-oscillation starts near 0.91

    void setSampleRate (double newRate) { sampleRate = newRate; }
    void setCharacter (const Character& c) { character = c; }

    void reset() { s[0] = s[1] = s[2] = s[3] = 0.0; }

    // resonance in [0, 1]; compensation in [0, 1] restores the passband gain the ladder
    // loses as resonance rises (0 = authentic Moog bass loss).
    float process (float input, double cutoffHz, float resonance, float compensation, bool twoPole = false)
    {
        cutoffHz = std::clamp (cutoffHz, 5.0, 0.45 * sampleRate);
        const double g = fastTan (std::numbers::pi * cutoffHz / sampleRate);
        const double G = g / (1.0 + g);
        const double a = 1.0 / (1.0 + g);
        const double k = maxFeedback * resonance;

        const double G2 = G * G;
        const double sigma = G2 * G * a * s[0] + G2 * a * s[1] + G * a * s[2] + a * s[3];
        const double y4estimate = (G2 * G2 * input + sigma) / (1.0 + k * G2 * G2);

        double x = fastTanh (character.drive * (input - k * y4estimate)) / character.drive;
        double y2 = 0.0;
        for (int i = 0; i < 4; ++i)
        {
            const double v = (x - s[i]) * G;
            x = v + s[i];
            s[i] = x + v;
            if (i == 1)
                y2 = x;
        }

        const double comp = std::max ((double) compensation, character.passbandKeep);
        return (float) ((twoPole ? y2 : x) * (1.0 + k * comp));
    }

    bool isSelfOscillating (float resonance) const { return maxFeedback * resonance >= 4.0; }

private:
    Character character = moog;
    double sampleRate = 96000.0;
    double s[4] {};
};

} // namespace dsynth
