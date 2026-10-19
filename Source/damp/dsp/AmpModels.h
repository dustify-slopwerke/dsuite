#pragma once

#include "Params.h"
#include "Stages.h"
#include "ToneStack.h"

#include <array>
#include <string_view>

namespace damp
{

// One amp voicing: its preamp stages, where the gain pot and tone stack sit between them, and its
// power section. Signals are in volts up to the power amp's grid drive.
struct ModelSpec
{
    std::string_view name;
    int numStages = 1;
    std::array<StageSpec, 4> stages {};
    int gainPotAfter = 0;   // the gain pot follows this stage...
    int toneStackAfter = 0; // ...and the tone stack this one (before the pot if they share a stage)
    double inputVolts = 1.0;  // a full-scale input sample at the first grid (an interface set so
                              // hard picking peaks near -6 dBFS: about 0.5 V, a hot pickup)
    double brightHz = 0.0;    // bright cap on the gain pot: where its treble bypass starts
    ToneStackParts toneStack {};
    PowerSpec power {};
    double outputTrim = 1.0;
};

constexpr int numModels = 5; // AmpModel values

const ModelSpec& modelSpec (AmpModel);

// The tube curves for each of a model's stages (built on first use; call from a non-realtime thread
// first, e.g. when preparing)
struct ModelCurves
{
    std::array<TriodeCurve, 4> stage, unbypassed;
};
const ModelCurves& modelCurves (AmpModel);

} // namespace damp
