#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <numbers>

namespace dustify
{

// [7/6] Pade approximant of tanh. It reaches exactly 1 near |x| = 4.97, where it is clamped; the
// error stays below 1e-4 everywhere (checked in the tests). Several times cheaper than std::tanh,
// and the filters call it on every stage of every voice at the oversampled rate.
inline double fastTanh (double x)
{
    x = std::clamp (x, -4.97, 4.97);
    const double x2 = x * x;
    return x * (135135.0 + x2 * (17325.0 + x2 * (378.0 + x2)))
           / (135135.0 + x2 * (62370.0 + x2 * (3150.0 + 28.0 * x2)));
}

// tanh(x) / x: the small-signal gain of a tanh at x, used to linearise nonlinear ZDF stages.
// Same approximant with the x factored out, so there's no division by x and no special case at 0.
inline double tanhSlope (double x)
{
    if (std::abs (x) > 4.97)
        return 1.0 / std::abs (x); // tanh is 1 there (to 1e-4), so the slope is 1/|x|
    const double x2 = x * x;
    return (135135.0 + x2 * (17325.0 + x2 * (378.0 + x2)))
           / (135135.0 + x2 * (62370.0 + x2 * (3150.0 + 28.0 * x2)));
}

inline float fastTanh (float x) { return (float) fastTanh ((double) x); }

// 2^x, relative error below 7e-9 (1e-5 cents as a pitch), for |x| < 1000. The voices convert
// pitch and cutoff to frequency with it on every sample, where std::exp2 was a real cost.
inline double fastExp2 (double x)
{
    x = std::clamp (x, -1000.0, 1000.0);
    auto n = (int64_t) x;
    if ((double) n > x)
        --n; // floor
    const double f = x - (double) n; // [0, 1)
    const double p = 1.0000000061582002 + f * (0.6931468377007717 + f * (0.24023109676078122 + f * (0.05547891246761408
                     + f * (0.009686186278635945 + f * (0.0012382415662949501 + f * 0.00021871261843335756)))));
    const auto bits = (uint64_t) (n + 1023) << 52;
    double scale;
    std::memcpy (&scale, &bits, sizeof (scale));
    return p * scale;
}

// sin and cos for x in [0, pi/2], absolute error below 2e-8
inline double sinQuadrant (double x)
{
    const double x2 = x * x;
    return x * (0.9999999827780323 + x2 * (-0.16666651517228176 + x2 * (0.008332963963587292
                + x2 * (-0.0001980475171638324 + x2 * 2.598102773365696e-06))));
}

inline double cosQuadrant (double x)
{
    const double x2 = x * x;
    return 0.9999999998456964 + x2 * (-0.4999999951156761 + x2 * (0.04166664188189741 + x2 * (-0.0013888432404327855
           + x2 * (2.4763770585051192e-05 + x2 * -2.611502391003384e-07))));
}

// tan(x) for x in [0, 0.48 pi]: the bilinear prewarp g = tan(pi fc / fs) every filter computes per sample
inline double fastTan (double x)
{
    return sinQuadrant (x) / cosQuadrant (x);
}

// sin(2 pi phase) for any phase, absolute error below 2e-8
inline double fastSin2Pi (double phase)
{
    phase -= std::floor (phase);
    const double q = phase * 4.0; // quadrant position
    constexpr double halfPi = std::numbers::pi / 2.0;
    if (q < 1.0) return sinQuadrant (q * halfPi);
    if (q < 2.0) return sinQuadrant ((2.0 - q) * halfPi);
    if (q < 3.0) return -sinQuadrant ((q - 2.0) * halfPi);
    return -sinQuadrant ((4.0 - q) * halfPi);
}

} // namespace dustify
