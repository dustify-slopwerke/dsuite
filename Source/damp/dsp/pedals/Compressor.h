#pragma once

#include "Drives.h"

namespace damp::pedals
{

// Dyna Comp / Ross-style OTA compressor: the Sustain knob sets how hard the signal drives an
// OTA whose gain an envelope follower turns down (feed forward), so the loud start of a note
// is held near a fixed level while its decay is lifted. A slow attack lets the pick through
// before the gain comes down (the classic "pluck"); the OTA's own clipping bounds that
// overshoot. Blend mixes the dry signal back in for a modern parallel compressor.
class Compressor final : public DrivePedal
{
public:
    void reset() override;
    void setPatch (const Patch&, double rate, int blockSize) override;
    void process (int channel, float* x, int n) override;

private:
    struct State
    {
        OnePole input, treble;
        float envelope = 0.0f;
        Ramp drive, makeup, blend;
    };
    std::array<State, 2> state;
    float driveTarget = 1.0f, makeupTarget = 1.0f, blendTarget = 1.0f, attack = 1.0f, release = 1.0f;
};

} // namespace damp::pedals
