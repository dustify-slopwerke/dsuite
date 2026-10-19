#pragma once

#include <vector>

namespace damp
{

// Monophonic pitch detection for the tuner: YIN (de Cheveigné & Kawahara, 2002) with parabolic
// refinement of the lag. Works on a window of a decimated input (around 11 kHz is plenty for
// guitar), covering roughly 40 Hz to 1.5 kHz. Allocates in its constructor only.
class PitchDetector
{
public:
    PitchDetector (double sampleRate, int windowSize);

    double sampleRate() const { return rate; }
    int windowSize() const { return window; }

    struct Result
    {
        double hz = 0.0;      // 0 = no clear pitch
        double clarity = 0.0; // 1 - YIN's aperiodicity at the chosen lag
    };

    // `samples` must hold windowSize() samples, oldest first
    Result detect (const float* samples);

private:
    double rate;
    int window, maxLag, minLag;
    std::vector<double> difference, normalised;
};

// Nearest equal-tempered note and how far off it is
struct NoteReading
{
    int midiNote = -1;
    double cents = 0.0;
};
NoteReading nearestNote (double hz, double a4 = 440.0);

} // namespace damp
