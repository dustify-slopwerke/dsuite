#pragma once

#include <array>
#include <cmath>
#include <numbers>

namespace dustify
{

// Kaiser-windowed (beta 8) halfband sinc, 2 * HalfLength + 1 taps, unity DC gain. Every other tap
// is zero apart from the centre one (0.5).
template <int HalfLength>
std::array<double, (size_t) (2 * HalfLength + 1)> halfbandTaps()
{
    auto besselI0 = [] (double x)
    {
        double sum = 1.0, term = 1.0;
        for (int k = 1; k < 50; ++k)
        {
            term *= (x / (2.0 * k)) * (x / (2.0 * k));
            sum += term;
        }
        return sum;
    };

    constexpr double beta = 8.0;
    std::array<double, (size_t) (2 * HalfLength + 1)> taps {};
    for (int i = 0; i < 2 * HalfLength + 1; ++i)
    {
        const int n = i - HalfLength;
        const double sinc = n == 0 ? 0.5 : std::sin (std::numbers::pi * n / 2.0) / (std::numbers::pi * n);
        const double r = (double) n / HalfLength;
        taps[(size_t) i] = sinc * besselI0 (beta * std::sqrt (1.0 - r * r)) / besselI0 (beta);
    }
    return taps;
}

// 2:1 decimator: linear-phase halfband FIR (Kaiser-windowed sinc, 2 * HalfLength + 1 taps).
// HalfLength must be odd. With 31 (63 taps): passband to about 0.42 fs_out, stopband from about
// 0.58 fs_out at -80 dB or better, latency 15.5 output samples. The first stage of a 4x cascade
// only has to protect the band the second stage keeps, so it gets away with far fewer taps.
template <int HalfLength = 31>
class HalfbandDecimatorT
{
public:
    static_assert (HalfLength % 2 == 1, "the outermost halfband taps must be non-zero");
    static constexpr int halfLength = HalfLength;
    static constexpr int numTaps = 2 * halfLength + 1;

    HalfbandDecimatorT()
        : taps (halfbandTaps<HalfLength>())
    {
        reset();
    }

    void reset()
    {
        history.fill (0.0);
        pos = 0;
    }

    // Takes the two newest input samples (in time order), returns one output sample.
    float process (float first, float second)
    {
        push (first);
        push (second);

        // Only the centre tap and odd offsets from it are non-zero in a halfband filter.
        double acc = taps[halfLength] * at (halfLength);
        for (int k = 1; k <= halfLength; k += 2)
            acc += taps[(size_t) (halfLength + k)] * (at (halfLength - k) + at (halfLength + k));
        return (float) acc;
    }

private:
    void push (float x)
    {
        pos = (pos + 1) % numTaps;
        history[(size_t) pos] = x;
        history[(size_t) (pos + numTaps)] = x;
    }

    // Input sample `delay` samples ago
    double at (int delay) const { return history[(size_t) (pos + numTaps - delay)]; }

    std::array<double, (size_t) numTaps> taps {};
    std::array<double, (size_t) (2 * numTaps)> history {};
    int pos = 0;
};

using HalfbandDecimator = HalfbandDecimatorT<31>;

} // namespace dustify
