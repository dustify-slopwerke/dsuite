#pragma once

#include "common/dsp/BbdChorus.h"
#include "common/dsp/Delay.h"
#include "common/dsp/Lfo.h"
#include "common/dsp/ParamTable.h"
#include "common/dsp/Tuning.h"

#include <array>
#include <span>
#include <string_view>

namespace dsynth
{
using namespace dustify;

// Single source of truth for every parameter (see common/dsp/ParamTable.h). Ids are persisted in
// sessions and presets: never rename or reuse one.

enum class PID : int
{
    Osc1On, Osc1Wave, Osc1Range, Osc1Tune, Osc1PW, Osc1Level, Osc1Sync,
    Osc2On, Osc2Wave, Osc2Range, Osc2Tune, Osc2PW, Osc2Level, Osc2KeyTrack,
    Osc3On, Osc3Wave, Osc3Range, Osc3Tune, Osc3PW, Osc3Level, Osc3KeyTrack,

    NoiseLevel, NoiseColour, MixerDrive, SubLevel, SubOctave, OscType, PhaseReset, CrossMod, RingMod,

    PmFilterEnv, PmOsc2, PmToFreq, PmToPW, PmToFilter,

    LfoRate, LfoWave, LfoKeySync,
    ModSource, ModMix, ModAmount, ModToPitch, ModToFilter, ModToPW,

    Mx1Source, Mx1Dest, Mx1Amount,
    Mx2Source, Mx2Dest, Mx2Amount,
    Mx3Source, Mx3Dest, Mx3Amount,
    Mx4Source, Mx4Dest, Mx4Amount,

    FilterModel, FilterCutoff, FilterReso, FilterEnvAmt, FilterKeyTrack, FilterResComp, FilterVel, HpfMode,
    FilterShape, FilterBandpass, VoiceHpfCutoff, VoiceHpfReso,

    FEnvA, FEnvD, FEnvS, FEnvR, EnvCurve,
    AEnvA, AEnvD, AEnvS, AEnvR, EnvReleaseMode, AmpVel, VcaMode,

    VoiceMode, NotePriority, GlideTime, GlideType, GlideMode, BendRange, UnisonVoices, UnisonDetune,

    Drift, Tolerance, MasterTune, MasterVolume, PanSpread,

    ArpOn, ArpMode, ArpRate, ArpOctaves, ArpGate, ArpLatch, ArpSync, ArpTempo,
    ChorusMode,
    DelayMode, DelaySync, DelayNote, DelayTime, DelayFeedback, DelayTone, DelayMix,
    ReverbMix, ReverbSize, ReverbDecay, ReverbDamping, ReverbPredelay,

    LayerMode, SplitNote, LayerBalance, LayerDetune,
    MpeMode, MpeBendRange,

    // Later additions (new ids go at the end so existing indices never move)
    LfoDelay, Lfo2Rate, Lfo2Wave, Lfo2Delay, Lfo2KeySync,
    KeyAssign, VelocityCurve, Quality, Tuning, TuningRoot,

    Count
};

constexpr int numParams = (int) PID::Count;

using ParamInfo = dustify::ParamInfoT<PID>;
using Patch = dustify::PatchT<PID>;

std::span<const ParamInfo> paramTable (PID); // the whole table (any PID value)
inline std::span<const ParamInfo> allParams() { return paramTable (PID {}); }
inline const ParamInfo& paramInfo (PID p) { return dustify::paramInfo (p); }
inline const ParamInfo* findParam (std::string_view id) { return dustify::findParam<PID> (id); }

// Choice values, kept in sync with the choice string lists in Params.cpp.
// Only ever append to these: presets and sessions store the index.
enum class Wave : int { Sine, Triangle, Saw, Ramp, Square, Pulse, SawPulse, SawTri, TriPulse };
enum class Range : int { Lo, Ft32, Ft16, Ft8, Ft4, Ft2 };
enum class NoiseColour : int { White, Pink };
enum class FilterModel : int { Ladder24, Ladder12, Cem24, Ir3109_24, Ir3109_12, Sem12, Ms20, Arp24 };
enum class KeyTrack : int { Off, Third, TwoThirds, Full };
enum class EnvCurve : int { AnalogRC, Linear };
enum class ReleaseMode : int { Independent, UseDecay };
enum class VoiceMode : int { Mono, Legato, Poly, Unison, Duo };
enum class ModSource : int { Osc3, Lfo };
enum class MatrixSource : int { Off, Lfo, FilterEnv, AmpEnv, Osc3, Velocity, Key, ModWheel, Aftertouch, Noise, Timbre, Lfo2 };
enum class MatrixDest : int { Pitch, Osc1Pitch, Osc2Pitch, Osc3Pitch, Osc1PW, Osc2PW, Cutoff, Resonance,
                              Osc1Level, Osc2Level, NoiseLevel, Amp };
constexpr int numMatrixSlots = 4;
enum class SubOctave : int { Down1, Down2 };
enum class OscType : int { Vco, Dco };
enum class HpfMode : int { Off, BassBoost, One, Two, Three };
enum class VcaMode : int { Envelope, Gate };
enum class ArpMode : int { Up, Down, UpDown, Random, AsPlayed };
enum class ArpRate : int { Quarter, Eighth, EighthTriplet, Sixteenth, SixteenthTriplet, ThirtySecond };
enum class LayerMode : int { Single, Dual, Split };
enum class DelayNote : int { ThirtySecond, SixteenthTriplet, Sixteenth, SixteenthDotted, EighthTriplet, Eighth,
                             EighthDotted, QuarterTriplet, Quarter, QuarterDotted, Half, Whole };
enum class NotePriority : int { Last, Low, High };
enum class GlideType : int { Exponential, Linear };
enum class GlideMode : int { Always, LegatoOnly };
enum class KeyAssign : int { Poly1, Poly2, Rotate };
enum class VelocityCurve : int { Linear, Soft, Hard, SCurve, Fixed };
enum class Quality : int { Normal, High };

} // namespace dsynth
