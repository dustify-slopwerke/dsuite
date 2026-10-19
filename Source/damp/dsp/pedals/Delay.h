#pragma once

#include "Modulation.h"

#include "common/dsp/Noise.h"

namespace damp::pedals
{

// Echo in three flavours, after the amp at the host rate:
//   Digital: clean repeats under a gentle one-pole hi-cut (Tone). A time change crossfades
//     between two read positions, so the repeats never bend in pitch.
//   Analog (DM-2 / Carbon Copy): a bucket brigade's band limit, 2.5-4 kHz falling with longer
//     times as its clock slows, on the way in and on every repeat, with soft saturation in the
//     loop and a slow chorus-like wobble (Mod). A time change glides the pitch, as a BBD's clock does.
//   Tape (Echoplex / Space Echo): tape saturation, a low-mid head bump and heavy high loss on
//     every repeat, wow and flutter (Mod); a time change glides like a motor changing speed.
// The dry signal always passes at unity and the repeats are added to it. In stereo, Ping-Pong
// feeds the two sides' mono sum to the left line and cross-couples the lines, so repeats
// alternate left and right.
//
// Delay times, feedback and levels are worked out at control points in setPatch (see
// ModulationPedal), so the two channels of a block see the same modulation however they're run.
// A long delay line read at a fractional delay, like DelayLine, but forgotten in constant time: a
// reset doesn't clear seconds of buffer in one go, only what the reads can reach (ensureClear)
class EchoLine
{
public:
    void allocate (int maxDelaySamples);
    int maxDelay() const { return (int) buffer.size() - 4; }

    // Everything in it is stale from now on
    void forget() { valid = 0; }
    // The last `samples` before the write position hold silence or what was written since forget()
    void ensureClear (int samples);
    // After n pushes
    void advanced (int n) { valid = std::min (valid + n, (int) buffer.size()); }

    void push (float x)
    {
        buffer[(size_t) write] = x;
        write = (write + 1) & mask;
    }
    // As DelayLine::read: 2 <= delay <= maxDelay(), before this sample's push
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
    int mask = 7, write = 0, valid = 8;
};

class Delay final : public ModulationPedal
{
public:
    static constexpr double maxSeconds = 4.0; // synced times at slow tempos are clamped to it

    void prepare (double maxRate) override;
    void reset() override;
    void setPatch (const Patch&, double rate, int blockSize) override;
    void process (int channel, float* x, int n) override;
    void processStereo (float* left, float* right, int n) override;
    void setTempo (double bpm) override { tempo = bpm; }

    bool hasTails() const override { return true; }
    void setEngaged (bool on) override { engaged = on; }
    bool ringing() const override;

    // The delay the patch asks for, seconds (Time, or Sync at the tempo)
    static double targetSeconds (const Patch&, double bpm);

private:
    struct State
    {
        EchoLine line;
        OnePole pre, tone, low; // input band limit (analog), hi-cut (digital), loop low cut
        Biquad band, bump;      // analog / tape band limit; tape head bump
        int quiet = 0;          // samples since this side last held anything audible
    };
    template <DelayMode> float repeat (State&, float tap) const;
    template <DelayMode> float record (State&, float in) const;
    template <DelayMode> float loop (float v) const;
    template <DelayMode> void run (State&, float* x, int n);
    template <DelayMode> void runStereo (float* left, float* right, int n);
    void glideTime (double target, double seconds);
    double modulation (double seconds, float amount); // delay offset, seconds
    void designFilters (const Patch&);

    std::array<State, 2> state;
    DelayMode mode = DelayMode::Analog, filtersFor = DelayMode::Analog;
    double tempo = 120.0;
    bool engaged = true; // a pedal on its own (not on a Board) just runs
    float feedLevel = 0.0f, wetLevel = 0.0f; // switching: the input into the line, and the repeats out
    float wetBefore = 0.0f;                  // wetLevel at the start of the block
    int quietLimit = 0;                      // samples of quiet after which the line is empty

    // Time: two read positions, `weight` of the second (it moves to 0 or 1 for a crossfade)
    std::array<double, 2> seconds {};
    float weight = 0.0f, weightTarget = 0.0f;

    Glide feedback, mix, mod, ping, offset;
    double flutterPhase = 0.0, driftPhase = 0.0;
    float driftFrom = 0.0f, driftTo = 0.0f;
    dustify::Random rng { 0xdea1u };

    Lane tap0 {}, tap1 {}, tapWeight {}, feed {}, gain {}, wet {}, pong {};
};

} // namespace damp::pedals
