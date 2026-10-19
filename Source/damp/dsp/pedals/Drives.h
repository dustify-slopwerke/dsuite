#pragma once

#include "Pedal.h"

#include "../Filters.h"

namespace damp
{

// The overdrive, distortion and fuzz pedals. Each follows its classic circuit's character, not
// its exact schematic: where the gain is applied (frequency-dependent gain before the clipping
// sets most of a drive's voice), how it clips (diodes in the feedback loop clip softly, diodes to
// ground hard, transistors asymmetrically), and the tone network after. Levels are set so the
// Level knob at 0 dB sits close to the bypassed signal's loudness at default settings.
namespace pedals
{

// Smooth limiter with a knee set by `hardness` (2 = soft, 8 = nearly hard), saturating at +-limit
inline float limiter (float v, float limit, float hardness)
{
    const float r = std::abs (v / limit);
    return v / std::pow (1.0f + std::pow (r, hardness), 1.0f / hardness);
}

// Audio-taper pot position (log), about a tenth at half way
inline float logTaper (float position)
{
    return (std::exp (4.6f * position) - 1.0f) / (std::exp (4.6f) - 1.0f);
}

inline float dbToGain (float db) { return std::pow (10.0f, db / 20.0f); }

// Shared bits: a per-channel level ramp and the block's rate
class DrivePedal : public Pedal
{
public:
    void prepare (double maxRate) override { (void) maxRate; reset(); }

protected:
    // Call at the top of process(): this channel's output gain, ramping to the block's target
    Ramp& levelRamp (int channel)
    {
        auto& r = levels[(size_t) channel];
        r.toward (levelTarget, blockSize);
        return r;
    }
    void setLevel (float gain, bool snap)
    {
        levelTarget = gain;
        if (snap)
            for (auto& r : levels)
                r.snap (gain);
    }

    double rate = 48000.0;
    int blockSize = 1;
    bool fresh = true; // snap ramps on the first block after reset()

private:
    std::array<Ramp, 2> levels;
    float levelTarget = 1.0f;
};

class Boost final : public DrivePedal
{
public:
    void reset() override;
    void setPatch (const Patch&, double rate, int blockSize) override;
    void process (int channel, float* x, int n) override;

private:
    struct State
    {
        OnePole input;
        Biquad low, high;
    };
    std::array<State, 2> state;
    float gain = 1.0f, tone = 0.5f;
};

class MidDrive final : public DrivePedal
{
public:
    void reset() override;
    void setPatch (const Patch&, double rate, int blockSize) override;
    void process (int channel, float* x, int n) override;

private:
    struct State
    {
        OnePole input, emphasis, feedbackCap, tone, output;
    };
    std::array<State, 2> state;
    float clipGain = 10.0f;
};

class BluesDrive final : public DrivePedal
{
public:
    void reset() override;
    void setPatch (const Patch&, double rate, int blockSize) override;
    void process (int channel, float* x, int n) override;

private:
    struct State
    {
        OnePole input, between, output;
        Biquad shelf;
    };
    std::array<State, 2> state;
    float gain = 10.0f, toneSetting = -1.0f;
};

class Distortion final : public DrivePedal
{
public:
    void reset() override;
    void setPatch (const Patch&, double rate, int blockSize) override;
    void process (int channel, float* x, int n) override;

private:
    struct State
    {
        OnePole input, opamp, low, high, output;
    };
    std::array<State, 2> state;
    float gain = 10.0f, tone = 0.5f;
};

class Rodent final : public DrivePedal
{
public:
    void reset() override;
    void setPatch (const Patch&, double rate, int blockSize) override;
    void process (int channel, float* x, int n) override;

private:
    struct State
    {
        OnePole input, leg47, leg560, bandwidth, filter, output;
    };
    std::array<State, 2> state;
    float pot = 50.0e3f;
};

class FaceFuzz final : public DrivePedal
{
public:
    void reset() override;
    void setPatch (const Patch&, double rate, int blockSize) override;
    void process (int channel, float* x, int n) override;

private:
    struct State
    {
        OnePole input, output, darken;
        float shift = 0.0f;
    };
    std::array<State, 2> state;
    float gain = 100.0f, charge = 0.0f, bleed = 1.0f;
};

class BigFuzz final : public DrivePedal
{
public:
    void reset() override;
    void setPatch (const Patch&, double rate, int blockSize) override;
    void process (int channel, float* x, int n) override;

private:
    struct State
    {
        OnePole input, couple1, cap1, couple2, cap2, low, high, output;
    };
    std::array<State, 2> state;
    float gain = 20.0f, tone = 0.5f;
};

} // namespace pedals
} // namespace damp
