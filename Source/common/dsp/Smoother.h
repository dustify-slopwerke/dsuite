#pragma once

#include <cmath>

namespace dustify
{

// One-pole parameter smoother: chases `target` a fixed fraction of the way each sample
struct Smoother
{
    float value = 0.0f, target = 0.0f;
    void snap() { value = target; }
    float next (float coeff) { return value += (target - value) * coeff; }

    // Per-sample coefficient for a time constant of `seconds` at `sampleRate`
    static float coefficient (double seconds, double sampleRate)
    {
        return (float) (1.0 - std::exp (-1.0 / (seconds * sampleRate)));
    }
};

} // namespace dustify
