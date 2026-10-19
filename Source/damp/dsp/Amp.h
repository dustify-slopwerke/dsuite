#pragma once

#include "AmpModels.h"
#include "NoiseGate.h"
#include "pedals/Board.h"
#include "Params.h"

#include "common/dsp/Oversampler.h"
#include "common/dsp/Smoother.h"

#include <array>

namespace damp
{

// Input gain and noise gate at the host rate, then (at 2x/4x/8x) the pedals placed before the amp
// and the amp model itself: preamp stages, gain pot, tone stack, power amp. Mono (right =
// nullptr) or stereo; a stereo input runs two amps with one gate, keyed from both channels.
// Delays by `latencySamples` at every oversampling factor. Rig adds the cab, the pedals after
// the amp and the output volume.
class Amp
{
public:
    static constexpr int latencySamples = dustify::Oversampler::latency;

    Amp();

    void setPatch (const Patch&);
    void prepare (double sampleRate);
    void reset();

    // In place. Call setPatch() (once per block is enough) before this.
    void process (float* left, float* right, int numSamples);

    // The board whose pre-amp pedals run inside the amp's oversampled section (nullptr: none)
    void setBoard (Board* b) { board = b; }
    double oversampledRate() const { return rate * (1 << stages); }

private:
    struct Channel
    {
        dustify::Oversampler oversampler;
        std::array<PreampStage, 4> stages;
        ToneStack toneStack;
        OnePole bright;
        PowerAmp power;
        float dcIn = 0.0f, dcOut = 0.0f;
    };

    void processChunk (float* left, float* right, int n);
    void applyPatch (bool snap);
    void configureModel(); // after a change of model or oversampling factor
    void runModel (Channel&, float* x, int n);

    Patch patch = Patch::defaults();
    double rate = 48000.0;
    bool prepared = false;
    int model = -1, stages = 0;
    float smoothing = 1.0f, dcCoeff = 0.999f;
    NoiseGate gate;
    bool gateOn = true;
    dustify::Smoother inputGain, gainPot, masterPot;
    Board* board = nullptr;
    dustify::Smoother bass, middle, treble; // pot positions, smoothed per chunk
    float toneSet[3] { -1.0f, -1.0f, -1.0f };
    std::array<float, dustify::Oversampler::maxBlock> gainRamp {}, masterRamp {};
    std::array<Channel, 2> channels;
};

} // namespace damp
