#pragma once

#include <cmath>

namespace damp
{

// Noise gate gain computer: a peak detector opens the gate above the threshold and lets it close
// once the level has stayed 6 dB below it for the hold time (the hysteresis stops chatter on a
// decaying note). The gain opens in well under a millisecond and fades out to -60 dB over the
// release time.
class NoiseGate
{
public:
    void prepare (double sampleRate)
    {
        rate = sampleRate;
        detectorRelease = std::exp (-1.0f / (float) (0.020 * rate));
        openCoeff = 1.0f - std::exp (-1.0f / (float) (0.0003 * rate));
        holdSamples = (int) (0.040 * rate);
        setRelease (releaseMs);
        reset();
    }

    void reset()
    {
        envelope = 0.0f;
        gain = 0.0f;
        open = false;
        hold = 0;
    }

    void setThreshold (float db)
    {
        openLevel = std::pow (10.0f, db / 20.0f);
        closeLevel = 0.5f * openLevel;
    }

    void setRelease (float ms)
    {
        releaseMs = ms;
        releaseCoeff = std::exp (-6.9f / (ms * 0.001f * (float) rate)); // e^-6.9 = -60 dB
    }

    // Gain for the next sample, given the level to detect (e.g. the larger of |left|, |right|)
    float next (float level)
    {
        envelope = level > envelope ? level : envelope * detectorRelease;

        if (envelope > openLevel)
            open = true;
        if (envelope > closeLevel)
            hold = holdSamples;
        else if (hold > 0)
            --hold;
        else
            open = false;

        if (open)
            gain += (1.0f - gain) * openCoeff;
        else
            gain = gain < 1.0e-5f ? 0.0f : gain * releaseCoeff;
        return gain;
    }

    bool isOpen() const { return open; }

private:
    double rate = 48000.0;
    float openLevel = 0.0f, closeLevel = 0.0f;
    float releaseMs = 80.0f, releaseCoeff = 0.0f;
    float detectorRelease = 0.0f, openCoeff = 1.0f;
    int holdSamples = 0, hold = 0;
    float envelope = 0.0f, gain = 0.0f;
    bool open = false;
};

} // namespace damp
