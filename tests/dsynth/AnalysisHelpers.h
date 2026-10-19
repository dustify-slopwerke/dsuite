#pragma once

#include "common/Analysis.h"
#include "dsp/Engine.h"

#include <vector>

namespace test
{

// A patch with all analog randomness off and a wide open, flat filter
inline dsynth::Patch cleanPatch()
{
    using enum dsynth::PID;
    auto p = dsynth::Patch::defaults();
    p[Drift] = 0.0f;
    p[Tolerance] = 0.0f;
    p[FilterCutoff] = 20000.0f;
    p[FilterEnvAmt] = 0.0f;
    p[FilterKeyTrack] = (float) dsynth::KeyTrack::Off;
    p[AEnvS] = 1.0f;
    p[MasterVolume] = 0.0f;
    return p;
}

struct Stereo
{
    std::vector<float> left, right;
};

inline Stereo renderStereo (dsynth::Engine& engine, double seconds, double rate)
{
    Stereo out { std::vector<float> ((size_t) (seconds * rate)), std::vector<float> ((size_t) (seconds * rate)) };
    engine.process (out.left.data(), out.right.data(), (int) out.left.size());
    return out;
}

// Left channel only: identical to the right unless pan spread or chorus is in use
inline std::vector<float> render (dsynth::Engine& engine, double seconds, double rate)
{
    return renderStereo (engine, seconds, rate).left;
}

} // namespace test
