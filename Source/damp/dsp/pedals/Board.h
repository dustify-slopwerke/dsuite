#pragma once

#include "Pedal.h"

#include <array>
#include <memory>

namespace damp
{

// The pedals and their order. Rig runs the part before the amp at the amp's oversampled rate
// (inside Amp, via processBeforeAmp) and the part after it at the host rate. A pedal switched on
// or off crossfades over a few milliseconds (pedals with tails switch themselves, see Pedal);
// one that is fully off, and not ringing, costs nothing.
class Board
{
public:
    static constexpr int maxBlock = 512; // largest block either section is called with

    Board();
    ~Board();

    // hostRate: the rate after the amp; maxRate: the highest rate before it (oversampled)
    void prepare (double hostRate, double maxRate);
    void reset();

    void setPatch (const Patch&);
    void setTempo (double bpm);
    // At a block boundary (Rig fades the output around it)
    void setOrder (const BoardOrder& order) { board = order; }
    const BoardOrder& order() const { return board; }

    // Before the amp, one channel at a time: begin once per block, then process each channel
    void beginBeforeAmp (double rate, int n);
    void processBeforeAmp (int channel, float* x, int n);

    // After the amp, left / right (right may be nullptr)
    void processAfterAmp (float* left, float* right, int n, double rate);

private:
    void runPedal (int pedal, int channel, float* x, int n);
    void runPedalStereo (int pedal, float* left, float* right, int n);
    void begin (int from, int to, double rate, int n);
    bool active (int pedal) const; // on, crossfading or ringing

    std::array<std::unique_ptr<Pedal>, numPedals> pedals;
    BoardOrder board = BoardOrder::defaults();
    Patch patch = Patch::defaults();
    std::array<bool, numPedals> on {};
    std::array<std::array<float, 2>, numPedals> fade {}; // per pedal and channel, 0 (bypassed) .. 1
    float fadeStep = 0.001f;
    std::array<float, maxBlock> dry {}, dryRight {};
};

} // namespace damp
