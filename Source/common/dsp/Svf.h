#pragma once


#include <algorithm>
#include <cmath>
#include <numbers>

namespace dustify
{

// Linear trapezoidal state-variable filter (Cytomic / Simper form). Used for utility filtering
// (high-pass, BBD anti-aliasing), not for the characterful voice filters.
class Svf
{
public:
    struct Out
    {
        float low, band, high;
    };

    void setup (double sampleRate, double cutoffHz, double q)
    {
        g = std::tan (std::numbers::pi * std::clamp (cutoffHz, 1.0, 0.49 * sampleRate) / sampleRate);
        k = 1.0 / q;
        a1 = 1.0 / (1.0 + g * (g + k));
        a2 = g * a1;
        a3 = g * a2;
    }

    void reset() { ic1 = ic2 = 0.0; }

    Out process (float x)
    {
        const double v3 = x - ic2;
        const double v1 = a1 * ic1 + a2 * v3;
        const double v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2.0 * v1 - ic1;
        ic2 = 2.0 * v2 - ic2;
        return { (float) v2, (float) v1, (float) (x - k * v1 - v2) };
    }

private:
    double g = 0.0, k = 1.4142, a1 = 1.0, a2 = 0.0, a3 = 0.0;
    double ic1 = 0.0, ic2 = 0.0;
};

} // namespace dustify
