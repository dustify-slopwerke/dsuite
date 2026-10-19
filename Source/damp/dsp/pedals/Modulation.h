#pragma once

#include "Pedal.h"

#include "../Filters.h"

#include <array>
#include <vector>

namespace damp
{

// The modulation pedals: chorus, flanger, phaser, tremolo and vibrato. Each follows its classic
// box's character: a short delay swept by a slow LFO (chorus, flanger, vibrato), a chain of
// swept allpass stages (phaser, Uni-Vibe), or a modulated gain (tremolo).
//
// They all work the same way underneath. The LFO and everything derived from it (delay times,
// filter coefficients, gains) is worked out in setPatch at a control rate of about 2 kHz, into
// "lanes" of values at evenly spaced points of the block, and process() interpolates linearly
// between those points. So the control work costs the same at the oversampled rate (up to
// ~1.5 MHz) as at the host rate, and since nothing in process() moves the LFO, the two channels
// of a block see the same LFO time however they are called. Knob settings glide to their
// targets at the control rate, so turning a knob doesn't zipper. (The tremolo's control points
// come four times as often, for the quick edges of its square and its photocell.)
namespace pedals
{

// A delay line read at a fractional delay
class DelayLine
{
public:
    void allocate (int maxDelaySamples);
    void clear();
    int maxDelay() const { return (int) buffer.size() - 4; }

    void push (float x)
    {
        buffer[(size_t) write] = x;
        write = (write + 1) & mask;
    }
    // The input `delay` samples ago (2 <= delay <= maxDelay()), called before this sample's push.
    // Cubic (Hermite) interpolation, or linear at oversampled rates where it's plenty.
    float read (float delay, bool cubic) const
    {
        const int whole = (int) delay;
        const float f = delay - (float) whole;
        const auto at = [this, whole] (int k) { return buffer[(size_t) ((write - whole - k) & mask)]; };
        const float y0 = at (0), y1 = at (1);
        if (! cubic)
            return y0 + f * (y1 - y0);
        const float ym1 = at (-1), y2 = at (2);
        const float c1 = 0.5f * (y1 - ym1);
        const float c2 = ym1 - 2.5f * y0 + 2.0f * y1 - 0.5f * y2;
        const float c3 = 0.5f * (y2 - ym1) + 1.5f * (y0 - y1);
        return ((c3 * f + c2) * f + c1) * f + y0;
    }

private:
    std::vector<float> buffer = std::vector<float> (8, 0.0f);
    int mask = 7, write = 0;
};

// A knob setting gliding to its target, one control point at a time
struct Glide
{
    float value = 0.0f;
    float follow (float target, float k) { return value += k * (target - value); }
};

// Shared machinery: the block's control points, the LFO phase and the gliding settings
class ModulationPedal : public Pedal
{
protected:
    static constexpr int maxSegments = 128;
    // Values at the block's control points 0 .. segments (one spare, read with zero weight when
    // a block runs past its schedule)
    using Lane = std::array<float, maxSegments + 2>;

    // At the top of setPatch: lays out the control points, about controlRate a second
    void beginBlock (double sampleRate, int samples, double controlRate = 2000.0);
    // Seconds from control point j - 1 to j (0 for the first point)
    double segmentSeconds (int j) const;
    // Glide coefficient over segment j for a time constant in seconds (snaps on a fresh start)
    float glide (int j, double seconds) const;
    // The first point continues where the last block's lanes ended
    void carry (Lane& lane) const { lane[0] = lane[(size_t) previousSegments]; }
    // Moves the LFO on to point j at hz, returning its phase in cycles
    double advance (int j, double hz);
    // Calls f (j, from, to, 1 / segment length) for each segment of the block's first n samples:
    // a lane's value across it is lane[j] + (lane[j + 1] - lane[j]) * (i - from) / length
    template <typename F> void forSegments (int n, F&& f) const
    {
        const int m = std::min (n, blockSamples);
        for (int j = 0; j < segments; ++j)
        {
            const int a = j * step, b = std::min (a + step, blockSamples);
            if (a >= m)
                break;
            f (j, a, std::min (b, m), 1.0f / (float) (b - a));
        }
        if (n > m)
            f (segments, m, n, 0.0f); // not in this block's schedule: hold the last value
    }

    double rate = 48000.0;
    int blockSamples = 0, step = 1, segments = 0, previousSegments = 0;
    double phase = 0.0; // LFO phase at the last control point, cycles
    bool fresh = true;  // snap the settings and lanes on the first block after reset()
    bool cubic = true;  // interpolation of delay reads
};

// Boss CE-2 / Small Clone-style BBD chorus: a 4-12 ms delay swept by a triangle-ish LFO, its
// output darkened and thinned like a bucket brigade's, mixed with the dry signal. In stereo the
// right side's LFO runs up to half a cycle apart (Width).
class Chorus final : public ModulationPedal
{
public:
    void prepare (double maxRate) override;
    void reset() override;
    void setPatch (const Patch&, double rate, int blockSize) override;
    void process (int channel, float* x, int n) override;
    void processStereo (float* left, float* right, int n) override;

private:
    struct State
    {
        DelayLine line;
        OnePole thin, dark1, dark2;
    };
    void run (State&, const Lane& delay, float* x, int n);

    std::array<State, 2> state;
    Glide rateHz, depth, mix, width;
    Lane delayLeft {}, delayRight {}, dry {}, wet {};
};

// Electric Mistress / MXR-style flanger: a delay of up to 15 ms swept on a log curve around Manual,
// with regeneration. In stereo the right side's sweep runs a quarter cycle apart.
class Flanger final : public ModulationPedal
{
public:
    void prepare (double maxRate) override;
    void reset() override;
    void setPatch (const Patch&, double rate, int blockSize) override;
    void process (int channel, float* x, int n) override;
    void processStereo (float* left, float* right, int n) override;

private:
    struct State
    {
        DelayLine line;
        OnePole dark, block;
    };
    void run (State&, const Lane& delay, float* x, int n);

    std::array<State, 2> state;
    Glide rateHz, depth, centre, regen;
    Lane delayLeft {}, delayRight {}, feedback {}, gain {};
};

// Phase 90 / Small Stone-style phaser: 4-12 identical first-order allpass stages swept together
// on a log curve, with feedback for resonance, mixed 50/50 with the dry signal. In stereo the
// right side's LFO runs a quarter cycle apart.
class Phaser final : public ModulationPedal
{
public:
    static constexpr int maxStages = 12;

    void prepare (double maxRate) override;
    void reset() override;
    void setPatch (const Patch&, double rate, int blockSize) override;
    void process (int channel, float* x, int n) override;
    void processStereo (float* left, float* right, int n) override;

private:
    struct State
    {
        std::array<float, maxStages> s {}; // allpass stage states
        float last = 0.0f;                 // the chain's last output, fed back
    };
    void run (State&, const Lane& coefficient, float* x, int n);
    template <int stages> void runStages (State&, const Lane& coefficient, float* x, int n);

    std::array<State, 2> state;
    Glide rateHz, depth, resonance, blend;
    int stagesFrom = 4, stagesTo = 4; // a change of stage count crossfades from one tap to the other
    Lane coefficientLeft {}, coefficientRight {}, feedback {}, gain {}, mixTo {};
};

// Tremolo with five shapes: sine, triangle, square (slewed), opto (a lamp lighting a photocell,
// the dips sharp and the recoveries slower) and harmonic (Brownface: the lows and highs either
// side of ~1 kHz pulse in opposite phase). Pan swaps the right side's phase for an auto-pan.
class Tremolo final : public ModulationPedal
{
public:
    void prepare (double maxRate) override;
    void reset() override;
    void setPatch (const Patch&, double rate, int blockSize) override;
    void process (int channel, float* x, int n) override;
    void processStereo (float* left, float* right, int n) override;

private:
    struct State
    {
        OnePole split1, split2;
    };
    void run (State&, const Lane& low, const Lane& high, float* x, int n);
    float shapeAt (TremoloShape, double phase, float hz) const;
    void measureShape (TremoloShape, float hz);

    std::array<State, 2> state;
    Glide rateHz, depth;
    std::array<float, 2> photocell {}; // opto: left and right (panned) lamp states
    std::array<float, 4> smoothed {}, lastTarget {}; // final gains: low left, high left, low right, high right
    TremoloShape measuredShape = TremoloShape::Sine;
    float measuredHz = -1.0f, shapeMean = 0.5f, shapeSquare = 0.375f;
    Lane lowLeft {}, highLeft {}, lowRight {}, highRight {};
};

// Vibrato in two modes, wet only. Vibrato (BV-2 / CE-style): pure pitch modulation from a delay
// swept by a sine. Vibe (Uni-Vibe's vibrato mode): four allpass stages with staggered caps, their
// photocell lit by a lamp that brightens faster than it dims, which gives its lopsided throb.
class Vibrato final : public ModulationPedal
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
        std::array<float, 4> s {}; // Vibe's allpass stage states
    };

    std::array<State, 2> state;
    Glide rateHz, depth, vibe;
    float lamp = 0.0f;
    bool vibeRuns = true, delayRuns = true;
    Lane delaySamples {}, mode {}, level {};
    std::array<Lane, 4> coefficient {};
};

} // namespace pedals
} // namespace damp
