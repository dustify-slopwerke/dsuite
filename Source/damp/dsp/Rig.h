#pragma once

#include "Amp.h"
#include "looper/Looper.h"
#include "pedals/Board.h"

#include "common/dsp/Convolver.h"

#include <array>

namespace damp
{

// Everything between the guitar and the output: input gain and gate, the pedals before the amp,
// the amp, its cab, the pedals after it, the looper, output volume. Mono or stereo in; the pedals after the
// amp can make a mono input stereo.
class Rig
{
public:
    static constexpr int latencySamples = Amp::latencySamples;

    Rig();

    void setPatch (const Patch&);
    // The host's tempo in BPM, for tempo-synced pedals (before process)
    void setTempo (double bpm)
    {
        pedals.setTempo (bpm);
        loops.setTempo (bpm);
    }
    // Any time on the audio thread: a change dips the output for a few milliseconds around it
    void setBoard (const BoardOrder&);
    const BoardOrder& board() const { return pedals.order(); }

    // maxImpulseLength: the longest cab impulse response that will be used
    void prepare (double sampleRate, int maxImpulseLength);
    void reset();

    // Cab impulse response (see Convolver): crossfaded in, or at once before processing starts
    void setCabKernel (const dustify::ConvolutionKernel* k);
    void setCabKernelNow (const dustify::ConvolutionKernel* k);

    // In place. right == nullptr: mono in and out. monoInput with right != nullptr: the input is
    // in left only, and right receives the stereo output.
    void process (float* left, float* right, int n, bool monoInput);

    // At the end of the chain, before output volume (see Looper for its threads)
    Looper& looper() { return loops; }

private:
    void processChunk (float* left, float* right, int n, bool monoInput);

    Amp amp;
    Board pedals;
    Looper loops;
    std::array<dustify::Convolver, 2> cab;
    double rate = 48000.0;
    float smoothing = 1.0f;
    dustify::Smoother outputGain;

    // Board order changes: fade out, swap at silence, fade in
    BoardOrder pendingBoard = BoardOrder::defaults();
    bool boardPending = false;
    float dip = 1.0f, dipStep = 0.01f;
};

} // namespace damp
