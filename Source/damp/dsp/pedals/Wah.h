#pragma once

#include "Drives.h"

namespace damp::pedals
{

// Cry Baby-style wah: the inductor and capacitor form a resonance swept by the pedal from about
// 350 Hz (heel) to 2.2 kHz (toe). Its response is not a pure band-pass: below the peak a shelf
// lets some of the lows through, above it the treble falls away. In Auto mode an envelope
// follower pushes the resonance up from the pedal position as you play harder.
class Wah final : public DrivePedal
{
public:
    void reset() override;
    void setPatch (const Patch&, double rate, int blockSize) override;
    void process (int channel, float* x, int n) override;

private:
    struct State
    {
        OnePole input;
        Ramp position;
        float envelope = 0.0f, sweep = -1.0f; // smoothed sweep position, 0 .. 1 (negative: not yet set)
        float damping = 0.2f;                 // smoothed 1/Q
        double ic1 = 0.0, ic2 = 0.0;          // the state-variable filter's integrators
        double a1 = 1.0, a2 = 0.0, a3 = 0.0;  // and its coefficients
        double gain = 0.0;                    // the output's, at the current position
        int countdown = 0;                    // samples until the coefficients are next updated
    };
    void updateFilter (State&) const;

    std::array<State, 2> state;
    bool autoMode = false;
    float positionTarget = 0.5f, depth = 0.0f, attack = 1.0f, release = 1.0f, smoothing = 1.0f, damping = 0.2f;
    int controlInterval = 1;
};

} // namespace damp::pedals
