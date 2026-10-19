#pragma once

#include "Engine.h"

#include <array>

namespace dsynth
{

// Two complete engines (layers A and B) played Single (A only), Dual (both on every note,
// CS-80 / Jupiter-8 style) or Split (B below the split point, A from it upwards). The layer
// setup parameters are read from layer A's patch.
class LayeredSynth
{
public:
    void prepare (double sampleRate);
    void setPatches (const Patch& a, const Patch& b);
    void setTransport (double bpm, bool playing, double ppqPosition);

    void noteOn (int note, float velocity, int channel = 0);
    void noteOff (int note, int channel = 0);
    void pitchBend (float normalised, int channel = 0);
    void modWheel (float normalised);
    void aftertouch (float normalised, int channel = 0);
    void polyPressure (int note, float normalised);
    void timbre (float normalised, int channel = 0);
    void sustainPedal (bool down);
    void allNotesOff();

    void process (float* left, float* right, int n);

    const Engine& layer (int index) const { return index == 0 ? a : b; }

private:
    bool layerBNeeded() const { return mode != LayerMode::Single || b.numActiveVoices() > 0; }

    Engine a, b;
    LayerMode mode = LayerMode::Single;
    int splitNote = 60;
    float targetGain[2] { 1.0f, 1.0f }, gain[2] { 1.0f, 1.0f }, gainCoeff = 0.001f;

    // Which layers each sounding note went to, so note-offs follow even if the setup changes
    std::array<uint8_t, 128> routed {};
    static constexpr uint8_t toA = 1, toB = 2;

    static constexpr int chunk = 256;
    std::array<float, chunk> scratchL {}, scratchR {};
};

} // namespace dsynth
