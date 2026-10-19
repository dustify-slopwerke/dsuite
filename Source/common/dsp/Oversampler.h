#pragma once

#include "Decimator.h"
#include "Upsampler.h"

#include <algorithm>
#include <array>
#include <cassert>

namespace dustify
{

namespace oversampling
{
constexpr int outerLength = 31, innerLength = 11; // halfband lengths: base-rate stage, the stages above it

// Delay of the bare cascade in high-rate samples: each stage's up + down filters delay by
// 2 * HalfLength samples at that stage's rate
constexpr int cascadeDelay (int stages)
{
    int delay = 2 * outerLength << (stages - 1);
    for (int s = 2; s <= stages; ++s)
        delay += 2 * innerLength << (stages - s);
    return delay;
}

// High-rate samples of extra delay that make the total `latency` base-rate samples
constexpr int padSamples (int stages, int latency) { return (latency << stages) - cascadeDelay (stages); }
} // namespace oversampling

// 2x / 4x / 8x oversampling for one channel of a nonlinear process: cascaded halfband stages, the
// first (sharpest) one at the base rate, shorter ones above it, which only have to protect the
// band the first keeps. Blocks of up to maxBlock base-rate samples:
//
//     float* x = os.upsample (in, n);   // n << stages() samples at the high rate
//     ... process x in place ...
//     os.downsample (out, n);
//
// The delay is `latency` base-rate samples whatever the factor (shorter cascades are padded), so
// a host never sees the latency change when the factor does.
class Oversampler
{
public:
    static constexpr int maxStages = 3;
    static constexpr int maxBlock = 64;
    static constexpr int latency = 40;

    explicit Oversampler (int initialStages = 2) { setStages (initialStages); }

    int stages() const { return numStages; }
    int factor() const { return 1 << numStages; }

    // 1 = 2x, 2 = 4x, 3 = 8x. Changing it clears the filters.
    void setStages (int s)
    {
        assert (s >= 1 && s <= maxStages);
        s = std::clamp (s, 1, maxStages);
        if (s != numStages)
        {
            numStages = s;
            reset();
        }
    }

    void reset()
    {
        upOuter.reset();
        downOuter.reset();
        for (auto& u : upInner)
            u.reset();
        for (auto& d : downInner)
            d.reset();
        pad.fill (0.0f);
        padPos = 0;
    }

    float* upsample (const float* in, int n)
    {
        assert (n <= maxBlock);
        for (int i = 0; i < n; ++i)
            upOuter.process (in[i], bufA[(size_t) (2 * i)], bufA[(size_t) (2 * i + 1)]);

        float* src = bufA.data();
        float* dst = bufB.data();
        for (int s = 1; s < numStages; ++s)
        {
            auto& up = upInner[(size_t) (s - 1)];
            for (int i = 0; i < (n << s); ++i)
                up.process (src[i], dst[2 * i], dst[2 * i + 1]);
            std::swap (src, dst);
        }
        top = src;
        return top;
    }

    // Decimates what upsample() returned (after processing in place) back into out[0 .. n)
    void downsample (float* out, int n)
    {
        const int topLength = n << numStages;
        const int padLength = oversampling::padSamples (numStages, latency);
        for (int i = 0; i < topLength; ++i)
        {
            pad[(size_t) padPos] = top[i];
            const int from = padPos - padLength;
            top[i] = pad[(size_t) (from < 0 ? from + padCapacity : from)];
            padPos = (padPos + 1) % padCapacity;
        }

        float* src = top;
        float* dst = src == bufA.data() ? bufB.data() : bufA.data();
        for (int s = numStages - 1; s >= 1; --s)
        {
            auto& down = downInner[(size_t) (s - 1)];
            for (int i = 0; i < (n << s); ++i)
                dst[i] = down.process (src[2 * i], src[2 * i + 1]);
            std::swap (src, dst);
        }
        for (int i = 0; i < n; ++i)
            out[i] = downOuter.process (src[2 * i], src[2 * i + 1]);
    }

private:
    static constexpr int outerLength = oversampling::outerLength, innerLength = oversampling::innerLength;
    static_assert (oversampling::padSamples (1, latency) >= 0 && oversampling::padSamples (2, latency) >= 0
                       && oversampling::padSamples (3, latency) >= 0, "latency too short for the cascade");
    static constexpr int padCapacity = std::max ({ oversampling::padSamples (1, latency), oversampling::padSamples (2, latency),
                                                   oversampling::padSamples (3, latency) }) + 1;

    HalfbandUpsamplerT<outerLength> upOuter;
    HalfbandDecimatorT<outerLength> downOuter;
    std::array<HalfbandUpsamplerT<innerLength>, maxStages - 1> upInner;
    std::array<HalfbandDecimatorT<innerLength>, maxStages - 1> downInner;

    std::array<float, (size_t) (maxBlock << maxStages)> bufA {}, bufB {};
    std::array<float, (size_t) padCapacity> pad {};
    float* top = bufA.data();
    int padPos = 0;
    int numStages = 0;
};

} // namespace dustify
