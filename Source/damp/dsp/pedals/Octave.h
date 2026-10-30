#pragma once

#include "Drives.h"

namespace damp::pedals
{

// OC-2-style analog octave down, monophonic: a steep low-pass isolates the note's fundamental,
// a flip-flop toggles on its rising zero crossings (with hysteresis, so harmonics left over don't
// retrigger it) and a second one on the first's. The first flips the polarity of the fundamental,
// giving a round sub an octave down, the second that of the rectified fundamental (two octaves
// down); switching at zero crossings keeps both free of clicks, and scaling them by the input's
// envelope makes them follow the playing. Chords confuse the tracker, as on the original.
// In stereo the left's tracker makes the octaves for both channels: tracked apart, the least
// difference between the inputs (a mono guitar on two inputs) would flip them at different
// times, or a half cycle apart for good, and the noise in a weak fundamental would differ.
class Octave final : public DrivePedal
{
public:
    void reset() override;
    void setPatch (const Patch&, double rate, int blockSize) override;
    void process (int channel, float* x, int n) override;

private:
    struct State
    {
        OnePole input;
        std::array<OnePole, 4> track;
        OnePole sub1Tone, sub2Tone, output;
        float level = 0.0f, peak = 0.0f; // the input's envelope, and the tracked fundamental's
        bool armed = false, first = false, second = false;
        Ramp direct, oct1, oct2;
    };
    template <bool lead>
    void track (State&, float* x, int n);

    std::array<State, 2> state;
    static constexpr int maxLed = 512; // Board::maxBlock
    std::array<float, maxLed> ledSubs {}; // the left's octaves through its last block
    int ledLength = -1; // that block's length, while the right may follow it (else -1)
    float directTarget = 1.0f, oct1Target = 1.0f, oct2Target = 0.0f;
    float attack = 1.0f, release = 1.0f, peakRelease = 1.0f;
};

} // namespace damp::pedals
