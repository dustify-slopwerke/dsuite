#pragma once

#include "looper/TempoGrid.h"

#include "common/dsp/Smoother.h"

namespace damp
{

// The looper's click: a short sine tick per beat (1 kHz; the bar's first beat 1.5 kHz and 4 dB
// louder when accented), 0.5 ms linear attack, then an exponential decay (7 ms time constant) cut
// at 40 ms. One voice, restarted from phase 0 by each click, so the same clicks always render the
// same samples. Added to the signal after the looper (it's never recorded). No allocation.
class Metronome
{
public:
    void prepare (double sampleRate);
    void reset();
    // level in dB (-48: silent)
    void setPatch (float levelDb, bool accent);
    // Adds the clicks (in offset order) to the block; right == nullptr: mono
    void render (float* left, float* right, int n, const tempo::Click* clicks, int count);

private:
    void start (bool accent);

    double rate = 48000.0;
    dustify::Smoother level;
    float smoothing = 1.0f;
    bool accentOn = true;

    // The voice
    bool active = false;
    int age = 0, attack = 24, length = 1920;
    float env = 0.0f, decay = 0.99f, gain = 1.0f;
    float c = 1.0f, s = 0.0f, rc = 1.0f, rs = 0.0f; // the sine, by rotation
};

} // namespace damp
