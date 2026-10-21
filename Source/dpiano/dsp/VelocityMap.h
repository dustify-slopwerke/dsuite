#pragma once

#include <algorithm>
#include <cmath>
#include <functional>

namespace dpiano
{

// Keyboard calibration: maps the velocity a controller sends (0 .. 1) onto the full range before
// the patch's own velocity curve and dynamic range. It belongs to the keyboard, not the sound, so
// it is not a preset parameter. The defaults are an exact identity.
//
//   v' = clamp ((v - inMin) / (inMax - inMin), 0, 1)       the played range fills 0 .. 1
//   v' = v' ^ 2^(-sensitivity)                              sensitivity -1 .. 1: harder .. easier
//   out = floor + (1 - floor) v'   floor = min (inMin, 0.02)
//
// The floor keeps the softest calibrated note above velocity 0 (a little above MIDI velocity 2)
// rather than at the very bottom of the hammer's range; with inMin = 0 it is 0, so an
// uncalibrated map stays exact.
struct VelocityMap
{
    static constexpr float maxFloor = 0.02f;
    static constexpr float minSpread = 0.15f; // a narrower calibrated range is rejected

    float inMin = 0.0f, inMax = 1.0f;
    float sensitivity = 0.0f; // -1 .. 1 (shown as -100 .. +100 %)

    bool isIdentity() const { return exactly (inMin, 0.0f) && exactly (inMax, 1.0f) && exactly (sensitivity, 0.0f); }
    bool isCalibrated() const { return ! exactly (inMin, 0.0f) || ! exactly (inMax, 1.0f); } // the range, not sensitivity

    // Keeps the values usable whatever was loaded: 0 <= inMin, inMin + minSpread <= inMax <= 1
    VelocityMap sanitised() const
    {
        VelocityMap m;
        if (! std::isfinite (inMin) || ! std::isfinite (inMax) || ! std::isfinite (sensitivity))
            return m;
        m.inMax = std::clamp (inMax, minSpread, 1.0f);
        m.inMin = std::clamp (inMin, 0.0f, m.inMax - minSpread);
        m.sensitivity = std::clamp (sensitivity, -1.0f, 1.0f);
        return m;
    }

    float apply (float velocity) const
    {
        if (isIdentity())
            return velocity;
        float v = std::clamp ((velocity - inMin) / (inMax - inMin), 0.0f, 1.0f);
        if (! exactly (sensitivity, 0.0f))
            v = std::pow (v, std::exp2 (-sensitivity));
        const float floor = std::min (inMin, maxFloor);
        return floor + (1.0f - floor) * v;
    }

    bool operator== (const VelocityMap&) const = default;

private:
    // Without -Wfloat-equal noise: these are deliberate exact comparisons with stored defaults
    static bool exactly (float a, float b) { return std::equal_to<float>() (a, b); }
};

// Tracks the softest and hardest note-on velocities while calibrating. Velocity 0 (a note-off
// in disguise) is ignored.
struct VelocityCapture
{
    float lowest = 1.0f, highest = 0.0f;
    int notes = 0;

    // Returns true if the note widened the range
    bool add (float velocity)
    {
        if (! (velocity > 0.0f))
            return false;
        ++notes;
        const bool wider = velocity < lowest || velocity > highest;
        lowest = std::min (lowest, velocity);
        highest = std::max (highest, velocity);
        return wider;
    }

    bool enoughRange() const { return notes >= 2 && highest - lowest >= VelocityMap::minSpread; }

    // The map for the captured range, keeping a sensitivity setting
    VelocityMap result (float sensitivity) const
    {
        VelocityMap m;
        m.inMin = lowest;
        m.inMax = highest;
        m.sensitivity = sensitivity;
        return m.sanitised();
    }
};

} // namespace dpiano
