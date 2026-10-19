#pragma once

#include <algorithm>
#include <cmath>

namespace dustify
{

// ADSR with either analog RC (exponential) or linear segments. A retrigger continues from the
// current level, like a capacitor-based envelope, so voice stealing never clicks.
class Envelope
{
public:
    enum class Stage { Idle, Attack, Decay, Release };

    void setSampleRate (double newRate) { sampleRate = newRate; }

    // Times in seconds. Analog: attack charges toward 1.2 and ends on reaching 1.0 (the classic
    // overshoot curve); decay and release times are to 99% of the way to their target.
    void setParameters (float attack, float decay, float sustainLevel, float release, bool linear)
    {
        isLinear = linear;
        sustain = sustainLevel;

        if (linear)
        {
            attackRate = rate (attack);
            decayRate = rate (decay);
            releaseRate = rate (release);
        }
        else
        {
            attackRate = coefficient (attack / std::log (6.0));
            decayRate = coefficient (decay / std::log (100.0));
            releaseRate = coefficient (release / std::log (100.0));
        }
    }

    void gateOn() { stage = Stage::Attack; }

    void gateOff()
    {
        if (stage != Stage::Idle)
            stage = Stage::Release;
    }

    void reset()
    {
        stage = Stage::Idle;
        level = 0.0;
    }

    float next()
    {
        switch (stage)
        {
            case Stage::Idle: break;

            case Stage::Attack:
                level += isLinear ? attackRate : (attackOvershoot - level) * attackRate;
                if (level >= 1.0)
                {
                    level = 1.0;
                    stage = Stage::Decay;
                }
                break;

            case Stage::Decay:
                if (isLinear)
                    level = level > sustain ? std::max ((double) sustain, level - decayRate)
                                            : std::min ((double) sustain, level + decayRate);
                else
                    level += (sustain - level) * decayRate;
                break;

            case Stage::Release:
                level = isLinear ? level - releaseRate : level - level * releaseRate;
                if (level <= 1.0e-5)
                {
                    level = 0.0;
                    stage = Stage::Idle;
                }
                break;
        }

        return (float) level;
    }

    bool isActive() const { return stage != Stage::Idle; }
    Stage getStage() const { return stage; }
    float getLevel() const { return (float) level; }

private:
    double coefficient (double tau) const { return 1.0 - std::exp (-1.0 / (std::max (tau, 1.0e-5) * sampleRate)); }
    double rate (double seconds) const { return 1.0 / (std::max (seconds, 1.0e-5) * sampleRate); }

    static constexpr double attackOvershoot = 1.2;

    double sampleRate = 96000.0;
    Stage stage = Stage::Idle;
    double level = 0.0;
    double attackRate = 0.0, decayRate = 0.0, releaseRate = 0.0;
    float sustain = 1.0f;
    bool isLinear = false;
};

} // namespace dustify
