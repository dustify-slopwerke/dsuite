#pragma once

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace dustify
{

// Stereo-linked look-ahead peak limiter, a safety net for an output stage. The signal is delayed by
// the look-ahead; the gain each sample needs (ceiling / peak of both channels) is held at its
// minimum over the look-ahead window and then averaged over the same window, so the gain ramps down
// linearly and is all the way there by the time the peak comes out: no overshoot, no clipping.
// It recovers with a one-pole release. While nothing reaches the ceiling the gain is exactly 1, so
// the signal passes bit for bit (just delayed). Real-time safe after prepare().
class PeakLimiter
{
public:
    void prepare (double sampleRate, double lookaheadSeconds = 0.001, double releaseSeconds = 0.08)
    {
        lookahead = std::max (1, (int) std::lround (lookaheadSeconds * sampleRate));
        window = lookahead + 1;
        delayLeft.assign ((size_t) lookahead, 0.0f);
        delayRight.assign ((size_t) lookahead, 0.0f);
        box.assign ((size_t) window, 1.0f);
        minValue.assign ((size_t) window, 1.0f);
        minIndex.assign ((size_t) window, 0);
        release = 1.0 - std::exp (-1.0 / (releaseSeconds * sampleRate));
        reset();
    }

    void reset()
    {
        std::fill (delayLeft.begin(), delayLeft.end(), 0.0f);
        std::fill (delayRight.begin(), delayRight.end(), 0.0f);
        std::fill (box.begin(), box.end(), 1.0f);
        delayPos = boxPos = 0;
        boxSum = (double) window;
        boxLimiting = 0;
        minHead = minCount = 0;
        sampleIndex = 0;
        gain = 1.0;
        lowestGain = 1.0f;
    }

    void setCeiling (float dB) { ceiling = std::pow (10.0f, dB / 20.0f); }
    float ceilingLevel() const { return ceiling; } // linear

    // The delay the limiter adds, in samples
    int latencySamples() const { return lookahead; }
    // The gain applied to the last sample processed (1 = not limiting)
    float currentGain() const { return (float) gain; }
    // The lowest gain applied since the last call, for metering
    float takeLowestGain() { return std::exchange (lowestGain, (float) gain); }

    // In place; right may be null for mono
    void process (float* left, float* right, int numSamples)
    {
        for (int i = 0; i < numSamples; ++i)
        {
            const float inLeft = left[i], inRight = right != nullptr ? right[i] : 0.0f;
            const float peak = std::max (std::abs (inLeft), std::abs (inRight));
            const float needed = peak > ceiling ? ceiling / peak : 1.0f;

            // The minimum over the window (a monotonic queue, oldest first)
            while (minCount > 0 && minValue[back()] >= needed)
                --minCount;
            minValue[slot (minCount)] = needed;
            minIndex[slot (minCount)] = sampleIndex;
            ++minCount;
            if (minIndex[(size_t) minHead] + window <= sampleIndex)
            {
                minHead = (minHead + 1) % window;
                --minCount;
            }
            const float held = minValue[(size_t) minHead];

            // ...averaged over the window: a linear ramp that lands on each peak's gain. The sum
            // is reset exactly whenever the window is all 1s, so it can't drift
            boxLimiting += (held < 1.0f ? 1 : 0) - (box[(size_t) boxPos] < 1.0f ? 1 : 0);
            boxSum += (double) held - (double) box[(size_t) boxPos];
            box[(size_t) boxPos] = held;
            boxPos = boxPos + 1 == window ? 0 : boxPos + 1;
            if (boxLimiting == 0)
                boxSum = (double) window;
            const double target = boxLimiting == 0 ? 1.0 : boxSum / (double) window;

            // Down at once (the ramp already is the attack), back up with the release, snapping the
            // last 0.001 dB (in double: in float the last steps would round away short of 1)
            if (target < gain)
                gain = target;
            else if (gain < 1.0)
                gain = target - gain < 1.0e-4 ? target : gain + (target - gain) * release;

            const float outLeft = delayLeft[(size_t) delayPos], outRight = delayRight[(size_t) delayPos];
            delayLeft[(size_t) delayPos] = inLeft;
            delayRight[(size_t) delayPos] = inRight;
            delayPos = delayPos + 1 == lookahead ? 0 : delayPos + 1;
            ++sampleIndex;
            const auto g = (float) gain;
            lowestGain = std::min (lowestGain, g);

            if (gain >= 1.0) // never above 1: not limiting
            {
                left[i] = outLeft;
                if (right != nullptr)
                    right[i] = outRight;
            }
            else
            {
                // The clamp only catches rounding: the gain already holds the peak at the ceiling
                left[i] = std::clamp (outLeft * g, -ceiling, ceiling);
                if (right != nullptr)
                    right[i] = std::clamp (outRight * g, -ceiling, ceiling);
            }
        }
    }

private:
    size_t slot (int offset) const { return (size_t) ((minHead + offset) % window); }
    size_t back() const { return slot (minCount - 1); }

    int lookahead = 1, window = 2;
    float ceiling = std::pow (10.0f, -1.0f / 20.0f);
    double release = 0.0, gain = 1.0;
    float lowestGain = 1.0f;
    std::vector<float> delayLeft, delayRight, box, minValue;
    std::vector<long long> minIndex;
    int delayPos = 0, boxPos = 0, boxLimiting = 0, minHead = 0, minCount = 0;
    long long sampleIndex = 0;
    double boxSum = 0.0;
};

} // namespace dustify
