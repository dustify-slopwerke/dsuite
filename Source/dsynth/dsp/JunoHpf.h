#pragma once

#include "Params.h"

#include "common/dsp/Svf.h"

namespace dsynth
{

// Juno-style non-resonant high-pass after the voices: off, a bass-boost shelf, or three
// increasingly thin settings (12 dB/oct Butterworth).
class JunoHpf
{
public:
    void prepare (double newRate)
    {
        sampleRate = newRate;
        mode = HpfMode::Off;
        hp.reset();
        boost.reset();
        boost.setup (sampleRate, 90.0, 0.7071);
    }

    void setMode (HpfMode newMode)
    {
        if (newMode == mode)
            return;
        mode = newMode;
        constexpr double cutoffs[] { 0.0, 0.0, 225.0, 500.0, 1100.0 };
        if (cutoffs[(int) mode] > 0.0)
            hp.setup (sampleRate, cutoffs[(int) mode], 0.7071);
    }

    float process (float x)
    {
        switch (mode)
        {
            case HpfMode::Off: return x;
            case HpfMode::BassBoost: return x + boost.process (x).low; // about +6 dB below 90 Hz
            case HpfMode::One:
            case HpfMode::Two:
            case HpfMode::Three: return hp.process (x).high;
        }
        return x;
    }

private:
    double sampleRate = 96000.0;
    HpfMode mode = HpfMode::Off;
    Svf hp, boost;
};

} // namespace dsynth
