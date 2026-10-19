#pragma once

#include "FastMath.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <vector>

namespace dustify
{

// Only ever append: presets and sessions store the index
enum class DelayMode : int { Off, Stereo, PingPong };

// Stereo / ping-pong delay with analog-style repeats: each pass through the feedback loop is
// darkened (tone low-pass), thinned (gentle high-pass) and soft-saturated, so long feedback
// settings decay musically instead of building up. Delay time changes glide like a tape delay.
class StereoDelay
{
public:
    static constexpr double maxSeconds = 2.5;

    void prepare (double newRate)
    {
        sampleRate = newRate;
        const auto size = (size_t) (maxSeconds * sampleRate) + 4;
        for (auto& line : lines)
            line.assign (size, 0.0f);
        write = 0;
        for (int c = 0; c < 2; ++c)
            lowState[c] = highState[c] = 0.0f;
        currentDelay = targetDelay;
        snapNext = true; // the first setting after prepare applies at once, no tape glide
        glide = (float) (1.0 - std::exp (-1.0 / (0.08 * sampleRate)));
        mixSmoothed = 0.0f;
    }

    void setParameters (DelayMode newMode, double delaySeconds, float newFeedback, float toneHz, float newMix)
    {
        if (newMode != mode && mode == DelayMode::Off)
            for (auto& line : lines)
                std::fill (line.begin(), line.end(), 0.0f); // no stale echoes when switching on
        mode = newMode;
        targetDelay = std::clamp (delaySeconds * sampleRate, 1.0, maxSeconds * sampleRate);
        if (snapNext)
        {
            currentDelay = targetDelay;
            snapNext = false;
        }
        feedback = newFeedback;
        lowCoeff = (float) (1.0 - std::exp (-2.0 * std::numbers::pi * std::min ((double) toneHz, 0.45 * sampleRate) / sampleRate));
        highCoeff = (float) (1.0 - std::exp (-2.0 * std::numbers::pi * 60.0 / sampleRate));
        mix = mode == DelayMode::Off ? 0.0f : newMix;
    }

    void process (float* left, float* right, int n)
    {
        if (mode == DelayMode::Off && mixSmoothed < 1.0e-4f)
            return;

        const auto size = lines[0].size();
        for (int i = 0; i < n; ++i)
        {
            currentDelay += (targetDelay - currentDelay) * glide;
            mixSmoothed += (mix - mixSmoothed) * 0.001f;

            float wet[2] { read (0, currentDelay), read (1, currentDelay) };

            // Feedback path: tone, low cut, saturation
            float back[2];
            for (int c = 0; c < 2; ++c)
            {
                lowState[c] += (wet[c] - lowState[c]) * lowCoeff;
                highState[c] += (lowState[c] - highState[c]) * highCoeff;
                back[c] = fastTanh ((lowState[c] - highState[c]) * feedback);
            }

            if (mode == DelayMode::PingPong)
            {
                // Mono input enters the left line; each line feeds the other, so echoes alternate
                lines[0][write] = 0.5f * (left[i] + right[i]) + back[1];
                lines[1][write] = back[0];
            }
            else
            {
                lines[0][write] = left[i] + back[0];
                lines[1][write] = right[i] + back[1];
            }

            left[i] += wet[0] * mixSmoothed;
            right[i] += wet[1] * mixSmoothed;
            write = write + 1 == size ? 0 : write + 1;
        }
    }

private:
    float read (int channel, double delay) const
    {
        const auto& line = lines[(size_t) channel];
        const auto size = (double) line.size();
        double pos = (double) write - delay;
        if (pos < 0.0)
            pos += size;
        const auto i0 = (size_t) pos;
        const auto i1 = i0 + 1 == line.size() ? 0 : i0 + 1;
        const auto t = (float) (pos - (double) i0);
        return line[i0] + (line[i1] - line[i0]) * t;
    }

    double sampleRate = 48000.0;
    DelayMode mode = DelayMode::Off;
    std::vector<float> lines[2];
    size_t write = 0;
    double targetDelay = 0.375 * 48000.0, currentDelay = targetDelay;
    float glide = 0.0f;
    bool snapNext = true;
    float feedback = 0.35f, mix = 0.0f, mixSmoothed = 0.0f;
    float lowCoeff = 0.5f, highCoeff = 0.01f;
    float lowState[2] {}, highState[2] {};
};

} // namespace dustify
