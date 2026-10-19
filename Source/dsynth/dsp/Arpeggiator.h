#pragma once

#include "Params.h"

#include "common/dsp/Noise.h"

#include <array>

namespace dsynth
{

// Arpeggiator clocked in steps (fractions of a beat). It either follows the host's musical
// position or free-runs at its own tempo, and produces note on/off events at exact sample
// offsets: the engine asks how far away the next event is, renders up to it, then pops it.
class Arpeggiator
{
public:
    struct Event
    {
        bool on;
        int note;
        float velocity;
    };

    void prepare (double newSampleRate);
    void setParams (ArpMode, int octaves, double stepBeats, float gate, bool latch);

    // Call once per block. hostPlaying: lock to hostPpq (quarter notes); otherwise free-run.
    void setClock (double bpm, bool hostPlaying, double hostPpq);

    void keyDown (int note, float velocity);
    void keyUp (int note);
    void reset(); // forget everything; any sounding note is released on the next pop

    int samplesUntilNextEvent (int maxSamples) const;
    void advance (int samples);
    int popEvents (Event* out); // up to 3 events are due at the current position

    bool isRunning() const { return patternSize > 0; }

private:
    void rebuildSequence();
    void patternChanged (bool wasEmpty);

    static constexpr double eps = 1.0e-9;

    double sampleRate = 48000.0;
    ArpMode mode = ArpMode::Up;
    int octaves = 1;
    double stepBeats = 0.25;
    float gate = 0.5f;
    bool latch = false;
    bool synced = false;
    double bpm = 120.0;

    // Keys physically held, and the pattern being played (the same, unless latched)
    std::array<int, 128> held {};
    int heldCount = 0;
    std::array<int, 128> pattern {};
    std::array<float, 128> velocities {};
    int patternSize = 0;

    std::array<int, 128 * 4 * 2> sequence {}; // up to 4 octaves, doubled by Up & Down
    std::array<float, 128 * 4 * 2> sequenceVelocity {};
    int sequenceSize = 0;

    double position = 0.0;  // in steps
    double nextStep = 0.0;
    double gateOffAt = -1.0;
    int stepIndex = 0;
    int sounding = -1;
    bool pendingOff = false;
    Random rng { 0xa4b9u };
};

} // namespace dsynth
