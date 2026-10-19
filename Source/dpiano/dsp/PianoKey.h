#pragma once

#include "KeyData.h"

#include <array>

namespace dpiano
{

// One key of the piano: its unison strings as a bank of modes, struck by a nonlinear felt hammer.
//
// Each partial n of the string sits at f_n = n f0 sqrt (1 + B n^2) and decays at its own rate
// sigma_n = b1 + b3 w_n^2. The unison strings (and the two polarisations) are folded into "normal
// modes" per partial: a prompt component, in phase across the strings, that drives the bridge
// hard and decays fast, and slower, slightly detuned aftersound components. Their beating and
// the handover between them give the piano's two-stage decay.
//
// The hammer is a mass on a nonlinear hysteretic spring (Hunt-Crossley / Stulov felt,
// F = K (x^p + alpha d(x^p)/dt) while compressed). Its force drives every mode in proportion to
// the mode shape at the strike point, and the string's displacement there pushes back on the
// hammer, so contact time, and with it brightness, follows from hammer speed and felt.
// While the hammer is in contact the modes run at 4x the sample rate.
//
// The output is the force the strings put on the bridge.
//
// Separately, a small sympathetic bank (one mode per low partial) is driven by the bridge force of
// every other key while this key's dampers are off: the halo of an undamped piano. It never
// drives anything itself, so the coupling can't run away.
class PianoKey
{
public:
    struct Settings
    {
        double sampleRate = 48000.0;
        double firstPartialHz = 261.63;  // pitch heard: the first partial
        double inharmonicityScale = 1.0;
        double detuneCents = 0.8;        // spread of the unison strings
        double aftersoundGain = 0.125;   // aftersound excitation relative to the prompt sound
        double decayScale = 1.0;         // scales every decay time
        double highLossScale = 1.0;      // scales the frequency-dependent loss (less = brighter)
        double hardness = 0.0;           // -1 .. 1: softer .. harder felt
        double strikeScale = 1.0;        // scales the strike point along the string
        double damperScale = 1.0;        // scales damper decay times (less = stronger dampers)
        double exponentOffset = 0.0;     // felt nonlinearity: added to p (force kept at 0.5 mm)
        double boardDrain = 1.0;         // scales the energy the board draws from mid-range partials
    };

    static constexpr int maxModes = 288;
    static constexpr int maxSympathetic = 24;
    static constexpr int maxBlock = 64;
    static constexpr int substeps = 4; // per sample while the hammer is in contact

    void configure (int key, const Settings&);

    // hammerSpeed in m/s (about 0.1 pianissimo .. 6 fortissimo). Una corda: the action shifts so
    // the hammer misses one string of each unison and strikes with less-worn, softer felt.
    void strike (double hammerSpeed, bool unaCorda = false);

    // 0 = damper lifted, 1 = resting on the strings; in between when half-pedalling
    void setDamper (float amount);

    // Adds this key's bridge force to `out` (numSamples <= maxBlock)
    void process (float* out, int numSamples);

    // Adds the sympathetic response to `out`. `bridge` is the total bridge force of all keys
    // over the same samples (this key's own share is taken out); coupling scales the drive.
    void processSympathetic (const float* bridge, float* out, int numSamples, float coupling);

    // Hammer force (N) over the last process() block, for the knock it sends into the board
    const float* lastHammerForce() const { return hammerForce.data(); }
    bool wasHammering() const { return hammerInLastBlock; }

    bool isActive() const { return active; }
    bool isResonating() const { return sympatheticActive; }
    void reset();

    int numModes() const { return modeCount; }
    double modeFrequency (int mode) const; // Hz, for tests
    const KeyData& data() const { return keyData_; }

private:
    void processHammer (float* out, int numSamples);
    void processFree (float* out, int numSamples);
    void updateDamper();
    float energy() const;
    float sympatheticEnergy() const;

    KeyData keyData_;
    double sampleRate = 48000.0;
    int modeCount = 0;
    bool active = false;

    // Modes as complex one-pole resonators, struct-of-arrays so the loops vectorise:
    // s <- p s + g F; displacement = Im (s)
    alignas (32) std::array<float, maxModes> re {}, im {};
    alignas (32) std::array<float, maxModes> pRe {}, pIm {};       // pole at the sample rate
    alignas (32) std::array<float, maxModes> pReSub {}, pImSub {}; // pole at the substep rate
    alignas (32) std::array<float, maxModes> inputGain {};          // per newton, per substep
    alignas (32) std::array<float, maxModes> hammerGain {};         // inputGain for this strike
    alignas (32) std::array<float, maxModes> promptMode {};         // 1 = prompt, 0 = aftersound
    alignas (32) std::array<float, maxModes> strikeShape {};        // mode shape at the strike point
    alignas (32) std::array<float, maxModes> bridgeGain {};         // displacement -> bridge force
    std::array<double, maxModes> modeHz {};

    // Sympathetic bank
    alignas (32) std::array<float, maxSympathetic> symRe {}, symIm {}, symPRe {}, symPIm {}, symInput {}, symOutput {};
    int symCount = 0;
    bool sympatheticActive = false;
    std::array<float, maxBlock> ownOutput {}; // this key's last block, excluded from its drive
    std::array<float, maxBlock> hammerForce {};
    bool hammerInLastBlock = false;

    // Hammer
    double hammerMass = 3e-3, stiffness = 4e9, strikeStiffness = 4e9, exponent = 2.5, hysteresis = 3e-5;
    double hammerPos = 0.0, hammerVel = 0.0, lastCompressionPow = 0.0;
    double contactTime = 0.0, sinceContact = 0.0;
    bool hammerActive = false;

    // Dampers: an extra decay applied to every mode
    double damperT60 = 0.3;
    float damperAmount = 1.0f;
    float damperGain = 1.0f, damperTarget = 1.0f; // per-sample gain, ramped towards the target
    int quietBlocks = 0;
};

} // namespace dpiano
