#pragma once

#include "common/dsp/ParamTable.h"

#include <span>
#include <string_view>

namespace damp
{
using namespace dustify;

// Single source of truth for every parameter (see common/dsp/ParamTable.h). Ids are persisted in
// sessions and presets: never rename or reuse one, and only append new ones before Count.
enum class PID : int
{
    // Input
    InputGain, GateOn, GateThreshold, GateRelease,
    // Amp
    Gain,
    // Output
    Oversampling, OutputVolume,

    // Amp models
    Model, Bass, Middle, Treble, Presence, Resonance, Master, Bright, Sag,

    // Cabinet
    Cab, Mic, MicPosition, MicDistance,

    // Pedals: drives
    BoostOn, BoostGain, BoostTone,
    MidDriveOn, MidDriveDrive, MidDriveTone, MidDriveLevel,
    BluesDriveOn, BluesDriveGain, BluesDriveTone, BluesDriveLevel,
    DistortionOn, DistortionDist, DistortionTone, DistortionLevel,
    RodentOn, RodentDist, RodentFilter, RodentLevel,
    FaceFuzzOn, FaceFuzzFuzz, FaceFuzzLevel,
    BigFuzzOn, BigFuzzSustain, BigFuzzTone, BigFuzzLevel,

    // Pedals: dynamics, filter, pitch, modulation
    CompressorOn, CompressorSustain, CompressorAttack, CompressorBlend, CompressorLevel,
    WahOn, WahMode, WahPosition, WahSensitivity, WahResponse, WahPeak,
    OctaveOn, OctaveDirect, OctaveOct1, OctaveOct2,
    ChorusOn, ChorusRate, ChorusDepth, ChorusMix, ChorusWidth,
    FlangerOn, FlangerRate, FlangerDepth, FlangerManual, FlangerRegen,
    PhaserOn, PhaserRate, PhaserDepth, PhaserFeedback, PhaserStages,
    TremoloOn, TremoloRate, TremoloDepth, TremoloShape, TremoloPan,
    VibratoOn, VibratoRate, VibratoDepth, VibratoMode,

    // Pedals: delay and reverb
    DelayOn, DelayMode, DelayTime, DelaySync, DelayFeedback, DelayMix, DelayTone, DelayMod, DelayPingPong, DelayTrails,
    ReverbOn, ReverbMode, ReverbDecay, ReverbPredelay, ReverbTone, ReverbMix, ReverbTrails,

    // Pedals: pitch
    PitchOn, PitchMode, PitchShift, PitchPosition, PitchMix,

    // Pedals: graphic EQ
    EqOn, Eq100, Eq200, Eq400, Eq800, Eq1k6, Eq3k2, Eq6k4, EqLevel,

    // Looper (session only: presets neither save nor load these). The transport buttons act when
    // they go from off to on, so a footswitch mapped to one fires once per press.
    LooperOn, LooperTrack, LooperRec, LooperPlayStop, LooperUndo, LooperRedo, LooperClear,
    // Per looper track, in TrackParam order (see trackParam)
    Track1Level, Track1Mute, Track1Reverse, Track1Half, Track1Sync,
    Track2Level, Track2Mute, Track2Reverse, Track2Half, Track2Sync,
    Track3Level, Track3Mute, Track3Reverse, Track3Half, Track3Sync,
    Track4Level, Track4Mute, Track4Reverse, Track4Half, Track4Sync,
    Track5Level, Track5Mute, Track5Reverse, Track5Half, Track5Sync,
    Track6Level, Track6Mute, Track6Reverse, Track6Half, Track6Sync,
    Track7Level, Track7Mute, Track7Reverse, Track7Half, Track7Sync,
    Track8Level, Track8Mute, Track8Reverse, Track8Half, Track8Sync,

    // Session only too: the output after the looper and metronome, the tempo and the metronome
    MasterOutput,
    Tempo, TempoFollowHost, LooperQuantise,
    MetronomeOn, MetronomeLevel, MetronomeAccent, BeatsPerBar, CountIn, MetronomeRecOnly,

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
enum class Oversampling : int { X2, X4, X8 };
enum class AmpModel : int { Clean, Chime, Crunch, HighGain, ModernHighGain };
enum class WahMode : int { Pedal, Auto };
enum class PhaserStages : int { Four, Six, Eight, Twelve };
enum class TremoloShape : int { Sine, Triangle, Square, Opto, Harmonic };
enum class VibratoMode : int { Vibrato, Vibe };
enum class DelayMode : int { Digital, Analog, Tape };
enum class DelaySync : int { Off, Half, DottedQuarter, Quarter, QuarterTriplet, DottedEighth, Eighth, EighthTriplet, Sixteenth };
enum class ReverbMode : int { Spring, Plate, Hall };
enum class PitchMode : int { Steps, Free };
enum class BeatsPerBar : int { Two, Three, Four, Five, Six, Seven }; // index + 2 beats
constexpr int beatsPerBar (int index) { return index + 2; }

// Looper tracks: each has the same parameters, in this order from Track1Level on
constexpr int looperTracks = 8;
enum class TrackParam : int { Level, Mute, Reverse, Half, Sync, Count };
constexpr PID trackParam (int track, TrackParam p)
{
    return (PID) ((int) PID::Track1Level + track * (int) TrackParam::Count + (int) p);
}
static_assert (trackParam (looperTracks - 1, TrackParam::Sync) == PID::Track8Sync);

// Presets hold the sound, not the looper, tempo, metronome or master output: those live only in
// the session
constexpr bool inPresets (PID p) { return (int) p < (int) PID::LooperOn; }

} // namespace damp
