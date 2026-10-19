#pragma once

#include <array>
#include <vector>

namespace dpiano
{

// The soundboard and case: turns the force on the bridge into the sound heard at two
// microphones.
//
// - Below about 1.5 kHz the board has distinct modes. They are a bank of resonators, sparse at
//   the bottom (the first mode is near 50 Hz, below which the board radiates little) and denser
//   higher up. Each mode has its own gain at each microphone, which is where the stereo image
//   and much of the "body" come from.
// - Above that the modes overlap into a dense response, modelled by a feedback delay network of
//   16 millisecond lines (about 60 modes per kHz, like a real board) with frequency-dependent
//   decay. Sound that comes out through these resonances takes tens of milliseconds to build up:
//   the "bloom" of a real attack. Bloom moves sound from the direct path into them.
// - A direct path stands for the bridge region radiating straight out (panned by key along the
//   bridge). Its mobility falls above a few hundred Hz (a shelf, by default -14 dB by 2 kHz). The
//   lid shapes the highs (an open lid reflects the treble towards the listener).
// - Above a few kHz the board is mass-controlled and radiates less: a gentle roll-off. Below a
//   couple of hundred Hz it radiates poorly too (by default a high-pass at 160 Hz), which
//   is why a grand's lowest fundamentals are so weak.
// - Microphone distance trades the direct sound for the board's body and some treble.
class Soundboard
{
public:
    void prepare (double sampleRate);

    // body: 0 .. 1, how much the board's own modes and diffuse ring colour the sound
    // lid: 0 (closed) .. 1 (fully open); distance: 0 (close) .. 1 (far)
    // bloom: 0 .. 1, how much of the sound comes out through the board's slow-building resonances
    void setParameters (float body, float lid, float distance, float bloom);

    // bassHz: radiation high-pass corner; warmthDb: how far the mobility shelf drops by 2 kHz;
    // airHz: the treble roll-off corner
    void setRadiation (float bassHz, float warmthDb, float airHz);

    // Adds the board's response into left/right: `force` (mono) drives the board, `directLeft`/
    // `directRight` are the same force panned along the bridge
    void process (const float* force, const float* directLeft, const float* directRight, float* left, float* right,
                  int numSamples);

    void reset();

    static constexpr int numModes = 64;

private:
    void updateRadiationFilters();

    double sampleRate = 48000.0;
    float radiationBassHz = 160.0f, radiationWarmthDb = 14.0f, radiationAirHz = 6000.0f;
    double mobilityHighGain = 0.2;

    alignas (32) std::array<float, numModes> re {}, im {}, pRe {}, pIm {}, gain {}, toLeft {}, toRight {};

    // Diffuse part: short lines through a Hadamard matrix, one-pole damping in each line
    static constexpr int numLines = 16;
    std::array<std::vector<float>, numLines> lines;
    std::array<int, numLines> lengths {}, writePos {};
    std::array<float, numLines> lineGain {}, damp {};
    float dampCoeff = 0.5f, diffuseT60 = 0.07f;
    void updateLineGains();

    // Lid shelf, mix gains
    double lidState[2] {}, lidCoeff = 0.0;
    double airState[2] {}, airCoeff = 0.0; // the board radiates less and less above a few kHz
    double mobilityState[2] {}, mobilityCoeff = 0.0; // and its mobility falls above a few hundred Hz
    struct Biquad
    {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
        double process (double x)
        {
            const double y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }
    };
    Biquad bassLeft, bassRight; // low-frequency radiation loss
    float lidHighGain = 1.0f;
    float directGain = 1.0f, modalGain = 1.0f, diffuseGain = 1.0f;
};

} // namespace dpiano
