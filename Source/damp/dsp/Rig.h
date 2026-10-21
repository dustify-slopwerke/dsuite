#pragma once

#include "Amp.h"
#include "Metronome.h"
#include "looper/Looper.h"
#include "pedals/Board.h"

#include "common/dsp/Convolver.h"

#include <array>

namespace damp
{

// Everything between the guitar and the output: input gain and gate, the pedals before the amp,
// the amp, its cab, the pedals after it, the preset's level, the looper, the metronome, the master
// output. Mono or stereo in; the pedals after the amp can make a mono input stereo. The level is
// recorded with the loops (a preset change leaves them as they are); the master output scales
// everything, loops and clicks included.
class Rig
{
public:
    static constexpr int latencySamples = Amp::latencySamples;

    Rig();

    void setPatch (const Patch&);
    // Before process: the tempo in effect (BPM) for tempo-synced pedals and the looper's beats,
    // and whether those follow a playing host's, at position ppq (quarter notes) on the block's
    // first frame
    void setTransport (double bpm, bool hostRunning, double ppq)
    {
        pedals.setTempo (bpm);
        bpmNow = bpm;
        hostTransport = hostRunning;
        hostPpq = ppq;
    }
    void setTempo (double bpm) { setTransport (bpm, false, 0.0); }
    // Any time on the audio thread: a change dips the output for a few milliseconds around it
    void setBoard (const BoardOrder&);
    const BoardOrder& board() const { return pedals.order(); }

    // maxImpulseLength: the longest cab impulse response that will be used
    void prepare (double sampleRate, int maxImpulseLength);
    void reset();

    // Cab impulse response (see Convolver): crossfaded in, or at once before processing starts.
    // While cabSwitching(), the kernel before the last one handed over may still be read: hand
    // over a newer one only once it's false, so that only the last two need to stay alive.
    void setCabKernel (const dustify::ConvolutionKernel* k);
    void setCabKernelNow (const dustify::ConvolutionKernel* k);
    bool cabSwitching() const;

    // In place. right == nullptr: mono in and out. monoInput with right != nullptr: the input is
    // in left only, and right receives the stereo output.
    void process (float* left, float* right, int n, bool monoInput);

    // After the level, before the metronome and master output (see Looper for its threads)
    Looper& looper() { return loops; }

private:
    void processChunk (float* left, float* right, int n, bool monoInput);

    Amp amp;
    Board pedals;
    Looper loops;
    Metronome metronome;
    std::array<dustify::Convolver, 2> cab;
    bool rightCabIdle = false; // the right cab sat out a mono block: it catches up when next used
    double rate = 48000.0;
    float smoothing = 1.0f;
    dustify::Smoother outputGain, masterGain;
    double bpmNow = 120.0, hostPpq = 0.0;
    bool hostTransport = false;

    // Board order changes: fade out, swap at silence, fade in
    BoardOrder pendingBoard = BoardOrder::defaults();
    bool boardPending = false;
    float dip = 1.0f, dipStep = 0.01f;
};

} // namespace damp
