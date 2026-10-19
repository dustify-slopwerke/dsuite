#pragma once

#include "Decimator.h"

namespace dustify
{

// 1:2 interpolator, the counterpart of HalfbandDecimatorT: the same halfband FIR, run polyphase
// (one branch is a pure delay, the other the odd taps). Each input sample lands on the second of
// the two output samples it produces, so an upsampler followed by a decimator of the same length
// delays by exactly HalfLength input samples (HalfLength output samples each way).
template <int HalfLength = 31>
class HalfbandUpsamplerT
{
public:
    static_assert (HalfLength % 2 == 1, "the outermost halfband taps must be non-zero");
    static constexpr int halfLength = HalfLength;
    static constexpr int historyLength = halfLength + 1; // input delays 0 .. halfLength

    HalfbandUpsamplerT()
    {
        // Gain 2 makes up for the zeros the interpolation inserts
        const auto taps = halfbandTaps<HalfLength>();
        for (int j = 1, k = 0; j <= halfLength; j += 2, ++k)
            oddTaps[(size_t) k] = 2.0 * taps[(size_t) (halfLength + j)];
        reset();
    }

    void reset()
    {
        history.fill (0.0);
        pos = 0;
    }

    // Takes one input sample, writes the two output samples it produces (in time order)
    void process (float x, float& first, float& second)
    {
        pos = (pos + 1) % historyLength;
        history[(size_t) pos] = x;
        history[(size_t) (pos + historyLength)] = x;

        first = (float) at ((halfLength + 1) / 2);

        double acc = 0.0;
        for (int j = 1, k = 0; j <= halfLength; j += 2, ++k)
            acc += oddTaps[(size_t) k] * (at ((halfLength - j) / 2) + at ((halfLength + j) / 2));
        second = (float) acc;
    }

private:
    // Input sample `delay` samples ago
    double at (int delay) const { return history[(size_t) (pos + historyLength - delay)]; }

    std::array<double, (size_t) (halfLength + 1) / 2> oddTaps {};
    std::array<double, (size_t) (2 * historyLength)> history {};
    int pos = 0;
};

using HalfbandUpsampler = HalfbandUpsamplerT<31>;

} // namespace dustify
