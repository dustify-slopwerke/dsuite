#pragma once

#include "common/dsp/FastMath.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace dsynth
{
using namespace dustify;

// 4-pole OTA cascade with global feedback, the topology of the CEM3320 / SSM2040 (Prophet-5) and
// Roland IR3109 (Juno, Jupiter) filters. Unlike the Moog ladder, every stage saturates on
// (input - output) of its own OTA, and the resonance path is soft-limited. A Character sets how
// hard each part saturates, which is most of what tells these chips apart.
//
// Zero-delay feedback with the nonlinearities linearised around the previous sample (each
// tanh(x) becomes x * tanh(x0)/x0), so every sample is a linear solve: stable at any setting.
class OtaFilter
{
public:
    struct Character
    {
        double stageDrive;    // per-stage OTA saturation
        double feedbackDrive; // resonance limiting; high values keep self-oscillation in tune
        double inputDrive;    // input stage soft clip; 0 = clean
        double passbandKeep;  // built-in resonance gain compensation (0..1)
    };

    // Smooth, vocal; resonance thins the sound like the original; input clips late and softly
    static constexpr Character cem { 0.25, 2.5, 0.3, 0.0 };
    // Cleaner self-oscillation, keeps more low end with resonance, input saturates sooner
    static constexpr Character ir3109 { 0.35, 3.0, 0.6, 0.35 };

    static constexpr double maxFeedback = 4.4;

    void setSampleRate (double newRate) { sampleRate = newRate; }
    void setCharacter (const Character& c) { character = c; }

    void reset()
    {
        for (int i = 0; i < 4; ++i)
            s[i] = d[i] = 0.0;
        y4Prev = 0.0;
    }

    // twoPole taps the second stage (12 dB/oct) while feedback still comes from the fourth
    float process (float input, double cutoffHz, float resonance, float compensation, bool twoPole = false)
    {
        cutoffHz = std::clamp (cutoffHz, 5.0, 0.45 * sampleRate);
        const double g = fastTan (std::numbers::pi * cutoffHz / sampleRate);
        const double k = maxFeedback * resonance;

        double x = input;
        if (character.inputDrive > 0.0)
            x = fastTanh (x * character.inputDrive) / character.inputDrive;

        // Per-stage gains with the OTA's tanh linearised at the last difference voltage
        double G[4], S[4];
        for (int i = 0; i < 4; ++i)
        {
            const double gi = g * slope (d[i] * character.stageDrive);
            const double r = 1.0 / (1.0 + gi);
            G[i] = gi * r;
            S[i] = s[i] * r;
        }

        const double feedbackGain = k * slope (y4Prev * character.feedbackDrive);
        const double P = G[0] * G[1] * G[2] * G[3];
        const double sigma = G[3] * (G[2] * (G[1] * S[0] + S[1]) + S[2]) + S[3];
        const double y4 = (P * x + sigma) / (1.0 + feedbackGain * P);

        x -= feedbackGain * y4;
        double y2 = 0.0;
        for (int i = 0; i < 4; ++i)
        {
            const double v = (x - s[i]) * G[i];
            const double y = v + s[i];
            s[i] = y + v;
            d[i] = x - y;
            x = y;
            if (i == 1)
                y2 = y;
        }
        y4Prev = x;

        const double comp = std::max (compensation, (float) character.passbandKeep);
        return (float) ((twoPole ? y2 : x) * (1.0 + k * comp));
    }

    bool isSelfOscillating (float resonance) const { return maxFeedback * resonance >= 4.0; }

private:
    static double slope (double x) { return tanhSlope (x); }

    Character character = cem;
    double sampleRate = 96000.0;
    double s[4] {}, d[4] {};
    double y4Prev = 0.0;
};

} // namespace dsynth
