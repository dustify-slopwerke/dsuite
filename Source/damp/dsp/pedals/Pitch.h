#pragma once

#include "Modulation.h"

#include <array>
#include <utility>
#include <vector>

namespace damp::pedals
{

// Whammy-style pitch shifter: the shift (in semitones) times the pedal position, either locked to
// half steps (Steps: knob and pedal both move in whole semitones, each step gliding over ~15 ms)
// or continuous (Free: detune, bends and dive bombs), mixed with the dry signal.
//
// It's a time-domain splicing shifter. One tap reads the input from a delay line, its delay moving
// at (1 - ratio) samples a sample, which plays the input back faster or slower: an exact pitch
// change, clean but for the interpolation. When the tap runs out of room (up: it closes on the
// write head; down: it falls too far behind) it jumps back or forward by a "grain", crossfading
// from the old tap to the new one. The jump is picked by a normalised cross-correlation search over
// ~14 ms of possible jumps (a period of the lowest notes), on a copy of the input decimated to
// ~12 kHz, so on a single note it lands on a whole number of periods (pitch-synchronous, the PSOLA
// idea) and the splice is all but seamless; a chord or noise has no good match and gets a longer
// grain, its fade gains set for constant power instead of constant amplitude. Up-shifts get a
// gentle high-cut, and the level is made up for what a shift loses through an amp.
// At unison the tap stands still (a few samples behind the input) and so is the dry guitar.
// Up to 2 octaves either way. The delay wanders between the splices: 6-9 ms on average for a
// single note (0.5-15 ms), up to ~35 ms for a chord shifted down.
class Pitch final : public Pedal
{
public:
    void prepare (double maxRate) override;
    void reset() override;
    void setPatch (const Patch&, double rate, int blockSize) override;
    void process (int channel, float* x, int n) override;

private:
    struct State
    {
        DelayLine line;
        std::vector<float> coarse; // the input decimated to the control rate, a ring
        int coarseWrite = 0;
        OnePole antiAlias1, antiAlias2;
        float sum = 0.0f;
        int count = 0; // input samples into the current coarse one

        double semis = 0.0, ratio = 1.0, ratioStep = 0.0;
        double delay = 0.0;   // the tap (fading in while splicing)
        double fading = 0.0;  // while splicing, the tap fading out
        float fade = 1.0f, fadeStep = 0.0f, rho = 1.0f; // fade >= 1: not splicing; rho: the match
        int wait = 0; // control ticks until the next try at a splice that can wait
        OnePole highCut;
        Ramp dry, wet;
    };

    void configure (double rate);
    void control (State&);
    void splice (State&, double jump, float match, double fadeSamples);
    // The best jump between jumpLo and jumpHi (samples) away from a tap at `tapDelay`: back into
    // the past (dir 1) or towards the present (dir -1); preferLong breaks near-ties towards the
    // longer jump. Returns the jump and the match there (normalised correlation, 0..1).
    std::pair<double, float> findSplice (const State&, double tapDelay, int dir, double jumpLo, double jumpHi,
                                         bool preferLong);

    std::array<State, 2> state;
    double rate = 0.0;
    int decimation = 1;
    int coarseMask = 0;
    bool cubic = true;
    bool fresh = true;
    double floorDelay = 3.0;          // the closest the taps come to the input, samples
    double minJump = 0.0, shortestJump = 0.0, jumpSpan = 0.0, maxFade = 0.0, maxDelay = 0.0; // samples
    double longFade = 0.0, longGrain = 0.0;
    int window = 0, recentreTicks = 0, retryTicks = 0; // coarse samples, control ticks
    float glide = 1.0f;               // the shift's glide per control tick
    double semisTarget = 0.0;
    float dryTarget = 0.0f, wetTarget = 1.0f;
    std::array<float, 512> scores {};
    std::array<float, 512 + 2 * 256 + 64> run {}; // findSplice's scratch
};

} // namespace damp::pedals
