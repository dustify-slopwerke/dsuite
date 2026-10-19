#pragma once

#include "common/dsp/Noise.h"

#include <array>
#include <cmath>
#include <numbers>

namespace dpiano
{

// The action's own sounds, synthesised: short filtered noise bursts with exponential decay.
// Levels put the keybed thump 20-30 dB under the tone it comes with (Askenfelt 1993).
// The key bottoming on the keybed (a dull thump), dampers landing on the strings (a soft felt
// "tss"), and the sustain pedal (a thump from the trapwork as all the dampers lift or fall).
class MechanicalNoise
{
public:
    void prepare (double newRate)
    {
        sampleRate = newRate;
        for (auto& b : bursts)
            b.level = 0.0f;
    }

    void keyThump (float velocity, float scale) { start (0.285f * velocity * velocity * scale, 0.012, 260.0); }
    void damperNoise (float scale) { start (0.0285f * scale, 0.035, 1800.0); }
    void pedalThump (float scale) { start (0.43f * scale, 0.025, 180.0); }

    // Adds the noise into `out`
    void process (float* out, int numSamples)
    {
        for (auto& b : bursts)
        {
            if (b.level < 1.0e-7f)
                continue;
            for (int i = 0; i < numSamples; ++i)
            {
                // Two cascaded one-poles: a soft low-pass on white noise
                b.lp1 += b.coeff * (rng.bipolar() - b.lp1);
                b.lp2 += b.coeff * (b.lp1 - b.lp2);
                out[i] += b.level * b.lp2 * b.norm;
                b.level *= b.decay;
            }
        }
    }

private:
    struct Burst
    {
        float level = 0.0f, decay = 0.0f, coeff = 0.0f, norm = 1.0f, lp1 = 0.0f, lp2 = 0.0f;
    };

    void start (float level, double seconds, double cutoffHz)
    {
        // Reuse the quietest slot
        auto* slot = &bursts[0];
        for (auto& b : bursts)
            if (b.level < slot->level)
                slot = &b;
        slot->level = level;
        slot->decay = (float) std::exp (-6.9 / (seconds * sampleRate));
        slot->coeff = (float) (1.0 - std::exp (-2.0 * std::numbers::pi * cutoffHz / sampleRate));
        slot->norm = (float) (1.0 / std::sqrt (slot->coeff)); // roughly level-independent of the cutoff
    }

    double sampleRate = 48000.0;
    dustify::Random rng { 0x7a11u };
    std::array<Burst, 12> bursts {};
};

} // namespace dpiano
