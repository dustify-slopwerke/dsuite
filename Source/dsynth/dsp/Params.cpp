#include "Params.h"

#include <algorithm>
#include <cassert>

namespace dsynth
{
namespace
{
using sv = std::string_view;

constexpr sv waves[] { "Sine", "Triangle", "Saw", "Ramp", "Square", "Pulse", "Saw+Pulse", "Saw+Tri", "Tri+Pulse" };
constexpr sv ranges[] { "Lo", "32'", "16'", "8'", "4'", "2'" };
constexpr sv noiseColours[] { "White", "Pink" };
constexpr sv filterModels[] { "Ladder 24dB", "Ladder 12dB", "CEM 24dB", "IR3109 24dB", "IR3109 12dB", "SEM 12dB", "MS-20 12dB", "ARP 24dB" };
constexpr sv keyTracks[] { "Off", "1/3", "2/3", "Full" };
constexpr sv envCurves[] { "Analog RC", "Linear" };
constexpr sv releaseModes[] { "Independent", "Use Decay" };
constexpr sv voiceModes[] { "Mono", "Legato", "Poly", "Unison", "Duo" };
constexpr sv lfoWaves[] { "Triangle", "Saw", "Ramp", "Square", "Sine", "S&H" };
constexpr sv subOctaves[] { "-1 Oct", "-2 Oct" };
constexpr sv oscTypes[] { "VCO", "DCO" };
constexpr sv hpfModes[] { "Off", "Bass Boost", "1", "2", "3" };
constexpr sv vcaModes[] { "Envelope", "Gate" };
constexpr sv arpModes[] { "Up", "Down", "Up & Down", "Random", "As Played" };
constexpr sv arpRates[] { "1/4", "1/8", "1/8 T", "1/16", "1/16 T", "1/32" };
constexpr sv chorusModes[] { "Off", "I", "II", "I + II" };
constexpr sv layerModes[] { "Single", "Dual", "Split" };
constexpr sv delayModes[] { "Off", "Stereo", "Ping-Pong" };
constexpr sv delayNotes[] { "1/32", "1/16 T", "1/16", "1/16 D", "1/8 T", "1/8", "1/8 D", "1/4 T", "1/4", "1/4 D", "1/2", "1/1" };
constexpr sv modSources[] { "Osc 3", "LFO" };
constexpr sv matrixSources[] { "Off", "LFO", "Filter Env", "Amp Env", "Osc 3", "Velocity", "Key", "Mod Wheel", "Aftertouch", "Noise", "Timbre (CC74)", "LFO 2" };
constexpr sv matrixDests[] { "Pitch", "Osc 1 Pitch", "Osc 2 Pitch", "Osc 3 Pitch", "Osc 1 PW", "Osc 2 PW", "Cutoff",
                             "Resonance", "Osc 1 Level", "Osc 2 Level", "Noise Level", "Amp" };
constexpr sv priorities[] { "Last", "Low", "High" };
constexpr sv glideTypes[] { "Exponential", "Linear" };
constexpr sv glideModes[] { "Always", "Legato Only" };
constexpr sv keyAssigns[] { "Poly 1", "Poly 2", "Rotate" };
constexpr sv velocityCurves[] { "Linear", "Soft", "Hard", "S-Curve", "Fixed" };
constexpr sv qualities[] { "Normal (2x)", "HQ (4x)" };

constexpr ParamInfo layerSetup (ParamInfo info)
{
    info.global = true;
    return info;
}

constexpr ParamInfo time (PID p, sv id, sv name, sv label, float def)
{
    return number (p, id, name, label, 0.001f, 10.0f, def, "s", 0.3f);
}

#define DSYNTH_OSC(n, on, level, range)                                                                       \
    boolean (PID::Osc##n##On, "osc" #n "_on", "Osc " #n " On", "On", on),                                     \
    choice (PID::Osc##n##Wave, "osc" #n "_wave", "Osc " #n " Wave", "Wave", waves, (int) Wave::Saw),          \
    choice (PID::Osc##n##Range, "osc" #n "_range", "Osc " #n " Range", "Range", ranges, (int) (range)),       \
    number (PID::Osc##n##Tune, "osc" #n "_tune", "Osc " #n " Tune", "Tune", -7.0f, 7.0f, 0.0f, "st"),         \
    number (PID::Osc##n##PW, "osc" #n "_pw", "Osc " #n " Pulse Width", "PW", 0.05f, 0.5f, 0.25f),             \
    number (PID::Osc##n##Level, "osc" #n "_level", "Osc " #n " Level", "Level", 0.0f, 1.0f, level)

#define DSYNTH_MATRIX(n)                                                                                        \
    choice (PID::Mx##n##Source, "mx" #n "_source", "Matrix " #n " Source", "Source " #n, matrixSources, 0),         \
    choice (PID::Mx##n##Dest, "mx" #n "_dest", "Matrix " #n " Destination", "Dest " #n, matrixDests, (int) MatrixDest::Cutoff), \
    number (PID::Mx##n##Amount, "mx" #n "_amount", "Matrix " #n " Amount", "Amount", -1.0f, 1.0f, 0.0f)

// Must be in PID order (checked at startup in paramInfo()).
constexpr ParamInfo table[] {
    DSYNTH_OSC (1, true, 0.8f, Range::Ft8),
    boolean (PID::Osc1Sync, "osc1_sync", "Osc 1 Sync (to Osc 2)", "Sync", false),
    DSYNTH_OSC (2, false, 0.8f, Range::Ft8),
    boolean (PID::Osc2KeyTrack, "osc2_keytrack", "Osc 2 Key Track", "Key Trk", true),
    DSYNTH_OSC (3, false, 0.8f, Range::Ft16),
    boolean (PID::Osc3KeyTrack, "osc3_keytrack", "Osc 3 Key Track", "Key Trk", true),

    number (PID::NoiseLevel, "noise_level", "Noise Level", "Noise", 0.0f, 1.0f, 0.0f),
    choice (PID::NoiseColour, "noise_colour", "Noise Colour", "Colour", noiseColours, (int) NoiseColour::White),
    number (PID::MixerDrive, "mixer_drive", "Mixer Drive", "Drive", 0.0f, 24.0f, 0.0f, "dB"),
    number (PID::SubLevel, "sub_level", "Sub Oscillator Level", "Sub", 0.0f, 1.0f, 0.0f),
    choice (PID::SubOctave, "sub_octave", "Sub Oscillator Octave", "Sub Oct", subOctaves, (int) SubOctave::Down1),
    choice (PID::OscType, "osc_type", "Oscillator Type", "Osc Type", oscTypes, (int) OscType::Vco),
    boolean (PID::PhaseReset, "phase_reset", "Oscillator Phase Reset", "Ph Reset", false),
    number (PID::CrossMod, "cross_mod", "Cross Mod (Osc 2 > Osc 1)", "X-Mod", 0.0f, 1.0f, 0.0f),
    number (PID::RingMod, "ring_mod", "Ring Mod Level (Osc 1 x Osc 2)", "Ring", 0.0f, 1.0f, 0.0f),

    number (PID::PmFilterEnv, "pm_fenv", "Poly-Mod Filter Env Amount", "Filt Env", -1.0f, 1.0f, 0.0f),
    number (PID::PmOsc2, "pm_osc2", "Poly-Mod Osc 2 Amount", "Osc 2", 0.0f, 1.0f, 0.0f),
    boolean (PID::PmToFreq, "pm_freq", "Poly-Mod To Osc 1 Freq", "> Freq 1", true),
    boolean (PID::PmToPW, "pm_pw", "Poly-Mod To Osc 1 PW", "> PW 1", false),
    boolean (PID::PmToFilter, "pm_filter", "Poly-Mod To Filter", "> Filter", false),

    number (PID::LfoRate, "lfo_rate", "LFO Rate", "Rate", 0.05f, 50.0f, 5.0f, "Hz", 3.0f),
    choice (PID::LfoWave, "lfo_wave", "LFO Wave", "Wave", lfoWaves, (int) LfoWave::Triangle),
    boolean (PID::LfoKeySync, "lfo_keysync", "LFO Key Sync", "Key Sync", false),

    choice (PID::ModSource, "mod_source", "Wheel Mod Source", "Source", modSources, (int) ModSource::Osc3),
    number (PID::ModMix, "mod_mix", "Wheel Mod Mix (Source / Noise)", "Src/Noise", 0.0f, 1.0f, 0.0f),
    number (PID::ModAmount, "mod_amount", "Wheel Mod Amount", "Amount", 0.0f, 1.0f, 0.0f),
    boolean (PID::ModToPitch, "mod_pitch", "Wheel Mod To Pitch", "> Pitch", true),
    boolean (PID::ModToFilter, "mod_filter", "Wheel Mod To Filter", "> Filter", false),
    boolean (PID::ModToPW, "mod_pw", "Wheel Mod To PW", "> PW", false),

    DSYNTH_MATRIX (1), DSYNTH_MATRIX (2), DSYNTH_MATRIX (3), DSYNTH_MATRIX (4),

    choice (PID::FilterModel, "filter_model", "Filter Model", "Model", filterModels, (int) FilterModel::Ladder24),
    number (PID::FilterCutoff, "filter_cutoff", "Filter Cutoff", "Cutoff", 20.0f, 20000.0f, 2000.0f, "Hz", 1000.0f),
    number (PID::FilterReso, "filter_reso", "Filter Resonance", "Reso", 0.0f, 1.0f, 0.0f),
    number (PID::FilterEnvAmt, "filter_env_amt", "Filter Env Amount", "Env Amt", -1.0f, 1.0f, 0.5f),
    choice (PID::FilterKeyTrack, "filter_keytrack", "Filter Key Track", "Key Trk", keyTracks, (int) KeyTrack::TwoThirds),
    number (PID::FilterResComp, "filter_res_comp", "Filter Resonance Compensation", "Res Comp", 0.0f, 1.0f, 0.0f),
    number (PID::FilterVel, "filter_vel", "Filter Velocity", "Velocity", 0.0f, 1.0f, 0.0f),
    choice (PID::HpfMode, "hpf_mode", "High-Pass Filter", "HPF", hpfModes, (int) HpfMode::Off),
    number (PID::FilterShape, "filter_shape", "Filter Shape (SEM: LP / Notch / HP)", "Shape", 0.0f, 1.0f, 0.0f),
    boolean (PID::FilterBandpass, "filter_bp", "Filter Bandpass (SEM)", "BP", false),
    number (PID::VoiceHpfCutoff, "vhpf_cutoff", "Voice HPF Cutoff", "Cutoff", 10.0f, 10000.0f, 10.0f, "Hz", 300.0f),
    number (PID::VoiceHpfReso, "vhpf_reso", "Voice HPF Resonance", "Reso", 0.0f, 1.0f, 0.0f),

    time (PID::FEnvA, "fenv_attack", "Filter Env Attack", "Attack", 0.005f),
    time (PID::FEnvD, "fenv_decay", "Filter Env Decay", "Decay", 0.4f),
    number (PID::FEnvS, "fenv_sustain", "Filter Env Sustain", "Sustain", 0.0f, 1.0f, 0.3f),
    time (PID::FEnvR, "fenv_release", "Filter Env Release", "Release", 0.3f),
    choice (PID::EnvCurve, "env_curve", "Envelope Curve", "Curve", envCurves, (int) EnvCurve::AnalogRC),

    time (PID::AEnvA, "aenv_attack", "Amp Env Attack", "Attack", 0.003f),
    time (PID::AEnvD, "aenv_decay", "Amp Env Decay", "Decay", 0.3f),
    number (PID::AEnvS, "aenv_sustain", "Amp Env Sustain", "Sustain", 0.0f, 1.0f, 1.0f),
    time (PID::AEnvR, "aenv_release", "Amp Env Release", "Release", 0.15f),
    choice (PID::EnvReleaseMode, "env_release_mode", "Envelope Release Mode", "Rel Mode", releaseModes, (int) ReleaseMode::Independent),
    number (PID::AmpVel, "amp_vel", "Amp Velocity", "Velocity", 0.0f, 1.0f, 0.0f),
    choice (PID::VcaMode, "vca_mode", "VCA Mode", "VCA", vcaModes, (int) VcaMode::Envelope),

    choice (PID::VoiceMode, "voice_mode", "Voice Mode", "Mode", voiceModes, (int) VoiceMode::Mono),
    choice (PID::NotePriority, "note_priority", "Note Priority", "Priority", priorities, (int) NotePriority::Low),
    number (PID::GlideTime, "glide_time", "Glide Time", "Glide", 0.0f, 5.0f, 0.0f, "s", 0.3f),
    choice (PID::GlideType, "glide_type", "Glide Type", "Glide Type", glideTypes, (int) GlideType::Exponential),
    choice (PID::GlideMode, "glide_mode", "Glide Mode", "Glide Mode", glideModes, (int) GlideMode::Always),
    number (PID::BendRange, "bend_range", "Pitch Bend Range", "Bend", 0.0f, 12.0f, 2.0f, "st", 0.0f, 1.0f),
    number (PID::UnisonVoices, "unison_voices", "Unison Voices", "Uni Voices", 2.0f, 8.0f, 4.0f, {}, 0.0f, 1.0f),
    number (PID::UnisonDetune, "unison_detune", "Unison Detune", "Uni Detune", 0.0f, 1.0f, 0.3f),

    number (PID::Drift, "drift", "Analog Drift", "Drift", 0.0f, 1.0f, 0.3f),
    number (PID::Tolerance, "tolerance", "Component Tolerance", "Tolerance", 0.0f, 1.0f, 0.3f),
    number (PID::MasterTune, "master_tune", "Master Tune", "Tune", -100.0f, 100.0f, 0.0f, "ct"),
    number (PID::MasterVolume, "master_volume", "Master Volume", "Volume", -48.0f, 6.0f, -6.0f, "dB"),
    number (PID::PanSpread, "pan_spread", "Voice Pan Spread", "Pan Spread", 0.0f, 1.0f, 0.0f),

    boolean (PID::ArpOn, "arp_on", "Arpeggiator On", "On", false),
    choice (PID::ArpMode, "arp_mode", "Arpeggiator Mode", "Mode", arpModes, (int) ArpMode::Up),
    choice (PID::ArpRate, "arp_rate", "Arpeggiator Rate", "Rate", arpRates, (int) ArpRate::Sixteenth),
    number (PID::ArpOctaves, "arp_octaves", "Arpeggiator Octaves", "Octaves", 1.0f, 4.0f, 1.0f, {}, 0.0f, 1.0f),
    number (PID::ArpGate, "arp_gate", "Arpeggiator Gate", "Gate", 0.05f, 1.0f, 0.5f),
    boolean (PID::ArpLatch, "arp_latch", "Arpeggiator Latch", "Latch", false),
    boolean (PID::ArpSync, "arp_sync", "Arpeggiator Host Sync", "Host Sync", true),
    number (PID::ArpTempo, "arp_tempo", "Arpeggiator Tempo (unsynced)", "Tempo", 40.0f, 240.0f, 120.0f, "bpm", 0.0f, 1.0f),

    choice (PID::ChorusMode, "chorus_mode", "Chorus", "Chorus", chorusModes, (int) ChorusMode::Off),

    choice (PID::DelayMode, "delay_mode", "Delay Mode", "Mode", delayModes, (int) DelayMode::Off),
    boolean (PID::DelaySync, "delay_sync", "Delay Tempo Sync", "Sync", true),
    choice (PID::DelayNote, "delay_note", "Delay Time (synced)", "Note", delayNotes, (int) DelayNote::EighthDotted),
    number (PID::DelayTime, "delay_time", "Delay Time (free)", "Time", 1.0f, 2000.0f, 375.0f, "ms", 300.0f),
    number (PID::DelayFeedback, "delay_feedback", "Delay Feedback", "Feedback", 0.0f, 0.95f, 0.35f),
    number (PID::DelayTone, "delay_tone", "Delay Tone", "Tone", 500.0f, 20000.0f, 5000.0f, "Hz", 3000.0f),
    number (PID::DelayMix, "delay_mix", "Delay Mix", "Mix", 0.0f, 1.0f, 0.25f),

    number (PID::ReverbMix, "reverb_mix", "Reverb Mix", "Mix", 0.0f, 1.0f, 0.0f),
    number (PID::ReverbSize, "reverb_size", "Reverb Size", "Size", 0.0f, 1.0f, 0.5f),
    number (PID::ReverbDecay, "reverb_decay", "Reverb Decay", "Decay", 0.2f, 20.0f, 2.5f, "s", 2.0f),
    number (PID::ReverbDamping, "reverb_damping", "Reverb Damping", "Damping", 0.0f, 1.0f, 0.5f),
    number (PID::ReverbPredelay, "reverb_predelay", "Reverb Pre-Delay", "Pre-Delay", 0.0f, 200.0f, 10.0f, "ms"),

    layerSetup (choice (PID::LayerMode, "layer_mode", "Layer Mode", "Mode", layerModes, (int) LayerMode::Single)),
    layerSetup (number (PID::SplitNote, "split_note", "Split Point (B below)", "Split", 0.0f, 127.0f, 60.0f, "note", 0.0f, 1.0f)),
    layerSetup (number (PID::LayerBalance, "layer_balance", "Layer Balance (A / B)", "A / B", -1.0f, 1.0f, 0.0f)),
    layerSetup (number (PID::LayerDetune, "layer_detune", "Layer B Detune", "B Detune", -50.0f, 50.0f, 0.0f, "ct")),
    layerSetup (boolean (PID::MpeMode, "mpe_mode", "MPE Mode", "MPE", false)),
    layerSetup (number (PID::MpeBendRange, "mpe_bend_range", "MPE Note Bend Range", "Note Bend", 1.0f, 96.0f, 48.0f, "st", 0.0f, 1.0f)),

    number (PID::LfoDelay, "lfo_delay", "LFO Delay (per-note fade-in)", "Delay", 0.0f, 5.0f, 0.0f, "s", 0.8f),
    number (PID::Lfo2Rate, "lfo2_rate", "LFO 2 Rate (per voice)", "Rate", 0.05f, 50.0f, 5.0f, "Hz", 3.0f),
    choice (PID::Lfo2Wave, "lfo2_wave", "LFO 2 Wave", "Wave", lfoWaves, (int) LfoWave::Sine),
    number (PID::Lfo2Delay, "lfo2_delay", "LFO 2 Delay (fade-in)", "Delay", 0.0f, 5.0f, 0.0f, "s", 0.8f),
    boolean (PID::Lfo2KeySync, "lfo2_keysync", "LFO 2 Key Sync", "Key Sync", true),
    choice (PID::KeyAssign, "key_assign", "Poly Key Assign", "Assign", keyAssigns, (int) KeyAssign::Poly1),
    layerSetup (choice (PID::VelocityCurve, "velocity_curve", "Velocity Curve", "Vel Curve", velocityCurves, (int) VelocityCurve::Linear)),
    layerSetup (choice (PID::Quality, "quality", "Oversampling Quality", "Quality", qualities, (int) Quality::Normal)),
    layerSetup (choice (PID::Tuning, "tuning", "Tuning", "Tuning", tuningNames, (int) Tuning::Equal)),
    layerSetup (choice (PID::TuningRoot, "tuning_root", "Tuning Root", "Root", noteNames, 0)),
};

#undef DSYNTH_OSC
#undef DSYNTH_MATRIX

static_assert (std::size (table) == (size_t) numParams, "parameter table out of sync with PID");

constexpr bool tableInOrder()
{
    for (int i = 0; i < numParams; ++i)
        if ((int) table[i].pid != i)
            return false;
    return true;
}

static_assert (tableInOrder(), "parameter table must be in PID order");
} // namespace

std::span<const ParamInfo> paramTable (PID) { return table; }

} // namespace dsynth
