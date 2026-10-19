#pragma once

#include "common/dsp/ParamTable.h"
#include "common/dsp/Tuning.h"

#include <span>
#include <string_view>

namespace dpiano
{
using namespace dustify;

// Single source of truth for every parameter (see common/dsp/ParamTable.h). Ids are persisted in
// sessions and presets: never rename or reuse one, and only append new ones before Count.
enum class PID : int
{
    // Hammers
    HammerHardness, VelocityCurve, StrikePosition,
    // Strings
    Inharmonicity, UnisonDetune, Aftersound, Decay, Brilliance, DamperStrength,
    // Tuning
    StretchTuning, MasterTune, Temperament, TemperamentRoot,
    // Output
    MasterVolume,

    // Body and resonance
    Sympathetic, SoundboardBody, Lid, MechanicalNoise,

    // Microphones and room
    Perspective, StereoWidth, MicDistance, RoomMix, RoomSize,

    // Voicing: the model's own physical constants, for tuning by ear
    TrebleHardness, FeltNonlinearity, DynamicRange, Knock, BoardDrain, BassRadiation, Warmth, Air, Bloom,

    Count
};

constexpr int numParams = (int) PID::Count;

using ParamInfo = dustify::ParamInfoT<PID>;
using Patch = dustify::PatchT<PID>;

std::span<const ParamInfo> paramTable (PID); // the whole table (any PID value)
inline std::span<const ParamInfo> allParams() { return paramTable (PID {}); }
inline const ParamInfo& paramInfo (PID p) { return dustify::paramInfo (p); }
inline const ParamInfo* findParam (std::string_view id) { return dustify::findParam<PID> (id); }

// Choice values. Only ever append: presets and sessions store the index.
enum class VelocityCurve : int { Soft, Normal, Hard };
enum class Perspective : int { Player, Audience };

} // namespace dpiano
