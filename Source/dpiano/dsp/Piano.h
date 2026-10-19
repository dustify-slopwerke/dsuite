#pragma once

#include "KeyData.h"
#include "MechanicalNoise.h"
#include "Params.h"
#include "PianoKey.h"
#include "Soundboard.h"

#include "common/dsp/Reverb.h"

#include <array>
#include <vector>

namespace dpiano
{

// The whole piano minus the plugin wrapper: 88 keys, each always ready to be struck (a key is
// its own voice: restriking a ringing key hits the same moving strings), dampers, the three
// pedals, sympathetic resonance between the strings, the action's noises and the soundboard.
// Real-time safe after prepare().
class Piano
{
public:
    void prepare (double sampleRate);
    void setPatch (const Patch&);

    void noteOn (int note, float velocity);
    void noteOff (int note);
    void sustainPedal (bool down) { sustainPedalLevel (down ? 1.0f : 0.0f); }
    void sustainPedalLevel (float level); // 0 .. 1 (CC64 / 127): half-pedalling in between
    void sostenutoPedal (bool down);       // holds the dampers of keys down when it's pressed
    void softPedal (bool down);            // una corda
    void allNotesOff();

    // Renders (replaces) numSamples of stereo output
    void process (float* left, float* right, int numSamples);

    int activeKeys() const;
    int resonatingKeys() const;
    const PianoKey& key (int note) const { return keys[index (note)]; }

    // Hammer speed in m/s for a MIDI velocity in 0..1, after the velocity curve
    double hammerSpeed (float velocity) const;
    double firstPartialHz (int note) const;

private:
    static size_t index (int note) { return (size_t) (std::clamp (note, lowestKey, highestKey) - lowestKey); }
    PianoKey::Settings settingsFor (int note) const;
    void configureKey (int note);
    void refreshDamper (int note);
    float damperAmount (int note) const;

    double sampleRate = 48000.0;
    Patch patch = Patch::defaults();
    std::array<double, numKeys> stretch {};
    std::array<PianoKey, numKeys> keys;
    std::array<int, numKeys> keyGeneration {};
    std::array<bool, numKeys> held {}, sostenutoHeld {};
    int generation = 1, nextToRefresh = 0;
    bool stretchValid = false;
    float sustain = 0.0f;
    bool sostenuto = false, unaCorda = false;

    Soundboard soundboard;
    dustify::FdnReverb room;
    std::array<float, numKeys> panLeft {}, panRight {};
    MechanicalNoise noises, pedalStir; // pedalStir: lifting all the dampers sets the strings stirring
    float volume = 1.0f, coupling = 0.0f, noiseScale = 1.0f, knockLevel = 0.0f;
    static constexpr int chunk = PianoKey::maxBlock;
    std::array<float, chunk> bridge {}, drive {}, board {}, directLeft {}, directRight {}, keyOut {}, knock {};
};

} // namespace dpiano
