#pragma once

#include "FastMath.h"
#include "Noise.h"

#include <cmath>
#include <numbers>

namespace dustify
{

// Only ever append: presets and sessions store the index
enum class LfoWave : int { Triangle, Saw, Ramp, Square, Sine, SampleHold };

// Low-frequency oscillator (the global LFO, and LFO 2 in every voice). Bipolar output in [-1, 1].
class Lfo
{
public:
    void setSampleRate (double newRate) { sampleRate = newRate; }
    void reset (double startPhase = 0.0) { phase = startPhase; held = rng.bipolar(); }
    void seed (uint32_t s) { rng = Random (s); }

    float next (double hz, LfoWave wave)
    {
        phase += hz / sampleRate;
        if (phase >= 1.0)
        {
            phase -= std::floor (phase);
            held = rng.bipolar();
        }

        switch (wave)
        {
            case LfoWave::Triangle: return (float) (phase < 0.5 ? 4.0 * phase - 1.0 : 3.0 - 4.0 * phase);
            case LfoWave::Saw: return (float) (2.0 * phase - 1.0);
            case LfoWave::Ramp: return (float) (1.0 - 2.0 * phase);
            case LfoWave::Square: return phase < 0.5 ? 1.0f : -1.0f;
            case LfoWave::Sine: return (float) fastSin2Pi (phase);
            case LfoWave::SampleHold: return held;
        }
        return 0.0f;
    }

private:
    double sampleRate = 96000.0;
    double phase = 0.0;
    float held = 0.0f;
    Random rng { 0x5eed1f0u };
};

} // namespace dustify
