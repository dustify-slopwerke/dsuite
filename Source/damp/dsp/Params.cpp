#include "Params.h"

namespace damp
{
namespace
{
using sv = std::string_view;

constexpr sv oversamplingFactors[] { "2x", "4x", "8x" };
constexpr sv ampModels[] { "Clean", "Chime", "Crunch", "High Gain", "Modern High Gain" };
constexpr sv cabinets[] { "Off", "1x12 Open", "2x12 Open", "4x12 Closed", "User IR" }; // CabType order
constexpr sv mics[] { "Dynamic", "Ribbon", "Condenser" };
constexpr sv wahModes[] { "Pedal", "Auto" };
constexpr sv phaserStages[] { "4", "6", "8", "12" };
constexpr sv tremoloShapes[] { "Sine", "Triangle", "Square", "Opto", "Harmonic" };
constexpr sv vibratoModes[] { "Vibrato", "Vibe" };
constexpr sv delayModes[] { "Digital", "Analog", "Tape" };
constexpr sv delaySyncs[] { "Off", "1/2", "1/4.", "1/4", "1/4T", "1/8.", "1/8", "1/8T", "1/16" };
constexpr sv reverbModes[] { "Spring", "Plate", "Hall" };
constexpr sv pitchModes[] { "Steps", "Free" };
constexpr sv looperTrackNames[] { "1", "2", "3", "4", "5", "6", "7", "8" };
constexpr sv beatsPerBarNames[] { "2", "3", "4", "5", "6", "7" };

// Must be in PID order (checked below)
constexpr ParamInfo table[] {
    number (PID::InputGain, "input_gain", "Input Gain", "Input", -24.0f, 24.0f, 0.0f, "dB"),
    boolean (PID::GateOn, "gate_on", "Noise Gate", "Gate", true),
    number (PID::GateThreshold, "gate_threshold", "Gate Threshold", "Threshold", -90.0f, -20.0f, -65.0f, "dBFS"),
    number (PID::GateRelease, "gate_release", "Gate Release", "Release", 5.0f, 500.0f, 80.0f, "ms", 80.0f),

    number (PID::Gain, "gain", "Preamp Gain", "Gain", 0.0f, 10.0f, 5.0f),

    choice (PID::Oversampling, "oversampling", "Oversampling", "Quality", oversamplingFactors, (int) Oversampling::X4),
    number (PID::OutputVolume, "output_volume", "Preset Level", "Level", -48.0f, 12.0f, 0.0f, "dB"),

    choice (PID::Model, "model", "Amp Model", "Model", ampModels, (int) AmpModel::Crunch),
    number (PID::Bass, "bass", "Bass", "Bass", 0.0f, 10.0f, 5.0f),
    number (PID::Middle, "middle", "Middle", "Middle", 0.0f, 10.0f, 5.0f),
    number (PID::Treble, "treble", "Treble", "Treble", 0.0f, 10.0f, 5.0f),
    number (PID::Presence, "presence", "Presence", "Presence", 0.0f, 10.0f, 5.0f),
    number (PID::Resonance, "resonance", "Resonance", "Resonance", 0.0f, 10.0f, 3.0f),
    number (PID::Master, "master", "Amp Master Volume", "Master", 0.0f, 10.0f, 5.0f),
    boolean (PID::Bright, "bright", "Bright Switch", "Bright", false),
    number (PID::Sag, "sag", "Power Supply Sag", "Sag", 0.0f, 2.0f, 1.0f),

    choice (PID::Cab, "cab", "Cabinet", "Cabinet", cabinets, 3),
    choice (PID::Mic, "mic", "Microphone", "Mic", mics, 0),
    number (PID::MicPosition, "mic_position", "Mic Position (Centre to Edge)", "Position", 0.0f, 1.0f, 0.3f),
    number (PID::MicDistance, "mic_distance", "Mic Distance", "Distance", 0.0f, 1.0f, 0.2f),

    boolean (PID::BoostOn, "boost_on", "Boost: On", "On", false),
    number (PID::BoostGain, "boost_gain", "Boost: Gain", "Gain", 0.0f, 20.0f, 10.0f, "dB"),
    number (PID::BoostTone, "boost_tone", "Boost: Tone", "Tone", 0.0f, 10.0f, 5.0f),

    boolean (PID::MidDriveOn, "mid_drive_on", "Mid Drive: On", "On", false),
    number (PID::MidDriveDrive, "mid_drive_drive", "Mid Drive: Drive", "Drive", 0.0f, 10.0f, 5.0f),
    number (PID::MidDriveTone, "mid_drive_tone", "Mid Drive: Tone", "Tone", 0.0f, 10.0f, 5.0f),
    number (PID::MidDriveLevel, "mid_drive_level", "Mid Drive: Level", "Level", -24.0f, 12.0f, 0.0f, "dB"),

    boolean (PID::BluesDriveOn, "blues_drive_on", "Blues Drive: On", "On", false),
    number (PID::BluesDriveGain, "blues_drive_gain", "Blues Drive: Gain", "Gain", 0.0f, 10.0f, 5.0f),
    number (PID::BluesDriveTone, "blues_drive_tone", "Blues Drive: Tone", "Tone", 0.0f, 10.0f, 5.0f),
    number (PID::BluesDriveLevel, "blues_drive_level", "Blues Drive: Level", "Level", -24.0f, 12.0f, 0.0f, "dB"),

    boolean (PID::DistortionOn, "distortion_on", "Distortion: On", "On", false),
    number (PID::DistortionDist, "distortion_dist", "Distortion: Dist", "Dist", 0.0f, 10.0f, 5.0f),
    number (PID::DistortionTone, "distortion_tone", "Distortion: Tone", "Tone", 0.0f, 10.0f, 5.0f),
    number (PID::DistortionLevel, "distortion_level", "Distortion: Level", "Level", -24.0f, 12.0f, 0.0f, "dB"),

    boolean (PID::RodentOn, "rodent_on", "Rodent: On", "On", false),
    number (PID::RodentDist, "rodent_dist", "Rodent: Distortion", "Distortion", 0.0f, 10.0f, 5.0f),
    number (PID::RodentFilter, "rodent_filter", "Rodent: Filter", "Filter", 0.0f, 10.0f, 4.0f),
    number (PID::RodentLevel, "rodent_level", "Rodent: Level", "Level", -24.0f, 12.0f, 0.0f, "dB"),

    boolean (PID::FaceFuzzOn, "face_fuzz_on", "Face Fuzz: On", "On", false),
    number (PID::FaceFuzzFuzz, "face_fuzz_fuzz", "Face Fuzz: Fuzz", "Fuzz", 0.0f, 10.0f, 7.0f),
    number (PID::FaceFuzzLevel, "face_fuzz_level", "Face Fuzz: Level", "Level", -24.0f, 12.0f, 0.0f, "dB"),

    boolean (PID::BigFuzzOn, "big_fuzz_on", "Big Fuzz: On", "On", false),
    number (PID::BigFuzzSustain, "big_fuzz_sustain", "Big Fuzz: Sustain", "Sustain", 0.0f, 10.0f, 6.0f),
    number (PID::BigFuzzTone, "big_fuzz_tone", "Big Fuzz: Tone", "Tone", 0.0f, 10.0f, 5.0f),
    number (PID::BigFuzzLevel, "big_fuzz_level", "Big Fuzz: Level", "Level", -24.0f, 12.0f, 0.0f, "dB"),

    boolean (PID::CompressorOn, "compressor_on", "Compressor: On", "On", false),
    number (PID::CompressorSustain, "compressor_sustain", "Compressor: Sustain", "Sustain", 0.0f, 10.0f, 5.0f),
    number (PID::CompressorAttack, "compressor_attack", "Compressor: Attack", "Attack", 0.0f, 10.0f, 5.0f),
    number (PID::CompressorBlend, "compressor_blend", "Compressor: Blend", "Blend", 0.0f, 10.0f, 10.0f),
    number (PID::CompressorLevel, "compressor_level", "Compressor: Level", "Level", -24.0f, 12.0f, 0.0f, "dB"),

    boolean (PID::WahOn, "wah_on", "Wah: On", "On", false),
    choice (PID::WahMode, "wah_mode", "Wah: Mode", "Mode", wahModes, (int) WahMode::Pedal),
    number (PID::WahPosition, "wah_position", "Wah: Pedal Position", "Pedal", 0.0f, 10.0f, 5.0f),
    number (PID::WahSensitivity, "wah_sensitivity", "Wah: Auto Sensitivity", "Sensitivity", 0.0f, 10.0f, 5.0f),
    number (PID::WahResponse, "wah_response", "Wah: Auto Response", "Response", 0.0f, 10.0f, 5.0f),
    number (PID::WahPeak, "wah_peak", "Wah: Peak", "Peak", 0.0f, 10.0f, 5.0f),

    boolean (PID::OctaveOn, "octave_on", "Octave: On", "On", false),
    number (PID::OctaveDirect, "octave_direct", "Octave: Direct Level", "Direct", 0.0f, 10.0f, 7.0f),
    number (PID::OctaveOct1, "octave_oct1", "Octave: One Down", "Oct 1", 0.0f, 10.0f, 7.0f),
    number (PID::OctaveOct2, "octave_oct2", "Octave: Two Down", "Oct 2", 0.0f, 10.0f, 0.0f),

    boolean (PID::ChorusOn, "chorus_on", "Chorus: On", "On", false),
    number (PID::ChorusRate, "chorus_rate", "Chorus: Rate", "Rate", 0.05f, 10.0f, 0.8f, "Hz", 1.0f),
    number (PID::ChorusDepth, "chorus_depth", "Chorus: Depth", "Depth", 0.0f, 10.0f, 5.0f),
    number (PID::ChorusMix, "chorus_mix", "Chorus: Mix", "Mix", 0.0f, 10.0f, 5.0f),
    number (PID::ChorusWidth, "chorus_width", "Chorus: Stereo Width", "Width", 0.0f, 10.0f, 7.0f),

    boolean (PID::FlangerOn, "flanger_on", "Flanger: On", "On", false),
    number (PID::FlangerRate, "flanger_rate", "Flanger: Rate", "Rate", 0.02f, 5.0f, 0.3f, "Hz", 0.5f),
    number (PID::FlangerDepth, "flanger_depth", "Flanger: Depth", "Depth", 0.0f, 10.0f, 6.0f),
    number (PID::FlangerManual, "flanger_manual", "Flanger: Manual", "Manual", 0.0f, 10.0f, 5.0f),
    number (PID::FlangerRegen, "flanger_regen", "Flanger: Regeneration", "Regen", 0.0f, 10.0f, 5.0f),

    boolean (PID::PhaserOn, "phaser_on", "Phaser: On", "On", false),
    number (PID::PhaserRate, "phaser_rate", "Phaser: Rate", "Rate", 0.05f, 10.0f, 0.5f, "Hz", 1.0f),
    number (PID::PhaserDepth, "phaser_depth", "Phaser: Depth", "Depth", 0.0f, 10.0f, 10.0f),
    number (PID::PhaserFeedback, "phaser_feedback", "Phaser: Feedback", "Feedback", 0.0f, 10.0f, 3.0f),
    choice (PID::PhaserStages, "phaser_stages", "Phaser: Stages", "Stages", phaserStages, (int) PhaserStages::Four),

    boolean (PID::TremoloOn, "tremolo_on", "Tremolo: On", "On", false),
    number (PID::TremoloRate, "tremolo_rate", "Tremolo: Rate", "Rate", 0.5f, 15.0f, 5.0f, "Hz", 4.0f),
    number (PID::TremoloDepth, "tremolo_depth", "Tremolo: Depth", "Depth", 0.0f, 10.0f, 6.0f),
    choice (PID::TremoloShape, "tremolo_shape", "Tremolo: Shape", "Shape", tremoloShapes, (int) TremoloShape::Opto),
    boolean (PID::TremoloPan, "tremolo_pan", "Tremolo: Stereo Pan", "Pan", false),

    boolean (PID::VibratoOn, "vibrato_on", "Vibrato: On", "On", false),
    number (PID::VibratoRate, "vibrato_rate", "Vibrato: Rate", "Rate", 0.2f, 12.0f, 5.0f, "Hz", 3.0f),
    number (PID::VibratoDepth, "vibrato_depth", "Vibrato: Depth", "Depth", 0.0f, 10.0f, 4.0f),
    choice (PID::VibratoMode, "vibrato_mode", "Vibrato: Mode", "Mode", vibratoModes, (int) VibratoMode::Vibrato),

    boolean (PID::DelayOn, "delay_on", "Delay: On", "On", false),
    choice (PID::DelayMode, "delay_mode", "Delay: Mode", "Mode", delayModes, (int) DelayMode::Analog),
    number (PID::DelayTime, "delay_time", "Delay: Time", "Time", 20.0f, 2000.0f, 380.0f, "ms", 300.0f),
    choice (PID::DelaySync, "delay_sync", "Delay: Tempo Sync", "Sync", delaySyncs, (int) DelaySync::Off),
    number (PID::DelayFeedback, "delay_feedback", "Delay: Feedback", "Feedback", 0.0f, 10.0f, 4.0f),
    number (PID::DelayMix, "delay_mix", "Delay: Mix", "Mix", 0.0f, 10.0f, 3.0f),
    number (PID::DelayTone, "delay_tone", "Delay: Tone", "Tone", 0.0f, 10.0f, 5.0f),
    number (PID::DelayMod, "delay_mod", "Delay: Modulation", "Mod", 0.0f, 10.0f, 3.0f),
    boolean (PID::DelayPingPong, "delay_ping_pong", "Delay: Ping-Pong", "Ping-Pong", false),
    boolean (PID::DelayTrails, "delay_trails", "Delay: Trails", "Trails", true),

    boolean (PID::ReverbOn, "reverb_on", "Reverb: On", "On", false),
    choice (PID::ReverbMode, "reverb_mode", "Reverb: Mode", "Mode", reverbModes, (int) ReverbMode::Spring),
    number (PID::ReverbDecay, "reverb_decay", "Reverb: Decay", "Decay", 0.0f, 10.0f, 5.0f),
    number (PID::ReverbPredelay, "reverb_predelay", "Reverb: Pre-Delay", "Pre-Delay", 0.0f, 200.0f, 10.0f, "ms", 40.0f),
    number (PID::ReverbTone, "reverb_tone", "Reverb: Tone", "Tone", 0.0f, 10.0f, 5.0f),
    number (PID::ReverbMix, "reverb_mix", "Reverb: Mix", "Mix", 0.0f, 10.0f, 3.0f),
    boolean (PID::ReverbTrails, "reverb_trails", "Reverb: Trails", "Trails", true),

    boolean (PID::PitchOn, "pitch_on", "Pitch Shifter: On", "On", false),
    choice (PID::PitchMode, "pitch_mode", "Pitch Shifter: Mode", "Mode", pitchModes, (int) PitchMode::Steps),
    number (PID::PitchShift, "pitch_shift", "Pitch Shifter: Shift", "Shift", -24.0f, 24.0f, 12.0f, "st"),
    number (PID::PitchPosition, "pitch_position", "Pitch Shifter: Pedal Position", "Pedal", 0.0f, 10.0f, 10.0f),
    number (PID::PitchMix, "pitch_mix", "Pitch Shifter: Mix", "Mix", 0.0f, 10.0f, 10.0f),

    boolean (PID::EqOn, "eq_on", "Graphic EQ: On", "On", false),
    number (PID::Eq100, "eq_100", "Graphic EQ: 100 Hz", "100", -15.0f, 15.0f, 0.0f, "dB"),
    number (PID::Eq200, "eq_200", "Graphic EQ: 200 Hz", "200", -15.0f, 15.0f, 0.0f, "dB"),
    number (PID::Eq400, "eq_400", "Graphic EQ: 400 Hz", "400", -15.0f, 15.0f, 0.0f, "dB"),
    number (PID::Eq800, "eq_800", "Graphic EQ: 800 Hz", "800", -15.0f, 15.0f, 0.0f, "dB"),
    number (PID::Eq1k6, "eq_1k6", "Graphic EQ: 1.6 kHz", "1.6k", -15.0f, 15.0f, 0.0f, "dB"),
    number (PID::Eq3k2, "eq_3k2", "Graphic EQ: 3.2 kHz", "3.2k", -15.0f, 15.0f, 0.0f, "dB"),
    number (PID::Eq6k4, "eq_6k4", "Graphic EQ: 6.4 kHz", "6.4k", -15.0f, 15.0f, 0.0f, "dB"),
    number (PID::EqLevel, "eq_level", "Graphic EQ: Level", "Level", -15.0f, 15.0f, 0.0f, "dB"),

    boolean (PID::LooperOn, "looper_on", "Looper: On", "On", false),
    choice (PID::LooperTrack, "looper_track", "Looper: Selected Track", "Track", looperTrackNames, 0),
    boolean (PID::LooperRec, "looper_rec", "Looper: Rec / Overdub", "Rec", false),
    boolean (PID::LooperPlayStop, "looper_play_stop", "Looper: Play / Stop All", "Play", false),
    boolean (PID::LooperUndo, "looper_undo", "Looper: Undo", "Undo", false),
    boolean (PID::LooperRedo, "looper_redo", "Looper: Redo", "Redo", false),
    boolean (PID::LooperClear, "looper_clear", "Looper: Clear Track", "Clear", false),

    number (PID::Track1Level, "track1_level", "Looper Track 1: Level", "Level", -48.0f, 6.0f, 0.0f, "dB"),
    boolean (PID::Track1Mute, "track1_mute", "Looper Track 1: Mute", "Mute", false),
    boolean (PID::Track1Reverse, "track1_reverse", "Looper Track 1: Reverse", "Reverse", false),
    boolean (PID::Track1Half, "track1_half", "Looper Track 1: Half Speed", "Half", false),
    boolean (PID::Track1Sync, "track1_sync", "Looper Track 1: Sync", "Sync", true),

    number (PID::Track2Level, "track2_level", "Looper Track 2: Level", "Level", -48.0f, 6.0f, 0.0f, "dB"),
    boolean (PID::Track2Mute, "track2_mute", "Looper Track 2: Mute", "Mute", false),
    boolean (PID::Track2Reverse, "track2_reverse", "Looper Track 2: Reverse", "Reverse", false),
    boolean (PID::Track2Half, "track2_half", "Looper Track 2: Half Speed", "Half", false),
    boolean (PID::Track2Sync, "track2_sync", "Looper Track 2: Sync", "Sync", true),

    number (PID::Track3Level, "track3_level", "Looper Track 3: Level", "Level", -48.0f, 6.0f, 0.0f, "dB"),
    boolean (PID::Track3Mute, "track3_mute", "Looper Track 3: Mute", "Mute", false),
    boolean (PID::Track3Reverse, "track3_reverse", "Looper Track 3: Reverse", "Reverse", false),
    boolean (PID::Track3Half, "track3_half", "Looper Track 3: Half Speed", "Half", false),
    boolean (PID::Track3Sync, "track3_sync", "Looper Track 3: Sync", "Sync", true),

    number (PID::Track4Level, "track4_level", "Looper Track 4: Level", "Level", -48.0f, 6.0f, 0.0f, "dB"),
    boolean (PID::Track4Mute, "track4_mute", "Looper Track 4: Mute", "Mute", false),
    boolean (PID::Track4Reverse, "track4_reverse", "Looper Track 4: Reverse", "Reverse", false),
    boolean (PID::Track4Half, "track4_half", "Looper Track 4: Half Speed", "Half", false),
    boolean (PID::Track4Sync, "track4_sync", "Looper Track 4: Sync", "Sync", true),

    number (PID::Track5Level, "track5_level", "Looper Track 5: Level", "Level", -48.0f, 6.0f, 0.0f, "dB"),
    boolean (PID::Track5Mute, "track5_mute", "Looper Track 5: Mute", "Mute", false),
    boolean (PID::Track5Reverse, "track5_reverse", "Looper Track 5: Reverse", "Reverse", false),
    boolean (PID::Track5Half, "track5_half", "Looper Track 5: Half Speed", "Half", false),
    boolean (PID::Track5Sync, "track5_sync", "Looper Track 5: Sync", "Sync", true),

    number (PID::Track6Level, "track6_level", "Looper Track 6: Level", "Level", -48.0f, 6.0f, 0.0f, "dB"),
    boolean (PID::Track6Mute, "track6_mute", "Looper Track 6: Mute", "Mute", false),
    boolean (PID::Track6Reverse, "track6_reverse", "Looper Track 6: Reverse", "Reverse", false),
    boolean (PID::Track6Half, "track6_half", "Looper Track 6: Half Speed", "Half", false),
    boolean (PID::Track6Sync, "track6_sync", "Looper Track 6: Sync", "Sync", true),

    number (PID::Track7Level, "track7_level", "Looper Track 7: Level", "Level", -48.0f, 6.0f, 0.0f, "dB"),
    boolean (PID::Track7Mute, "track7_mute", "Looper Track 7: Mute", "Mute", false),
    boolean (PID::Track7Reverse, "track7_reverse", "Looper Track 7: Reverse", "Reverse", false),
    boolean (PID::Track7Half, "track7_half", "Looper Track 7: Half Speed", "Half", false),
    boolean (PID::Track7Sync, "track7_sync", "Looper Track 7: Sync", "Sync", true),

    number (PID::Track8Level, "track8_level", "Looper Track 8: Level", "Level", -48.0f, 6.0f, 0.0f, "dB"),
    boolean (PID::Track8Mute, "track8_mute", "Looper Track 8: Mute", "Mute", false),
    boolean (PID::Track8Reverse, "track8_reverse", "Looper Track 8: Reverse", "Reverse", false),
    boolean (PID::Track8Half, "track8_half", "Looper Track 8: Half Speed", "Half", false),
    boolean (PID::Track8Sync, "track8_sync", "Looper Track 8: Sync", "Sync", true),

    number (PID::MasterOutput, "master_out", "Master Output", "Master", -48.0f, 18.0f, 0.0f, "dB"),

    number (PID::Tempo, "tempo", "Tempo", "BPM", 40.0f, 240.0f, 120.0f, "BPM", 0.0f, 0.01f),
    boolean (PID::TempoFollowHost, "tempo_follow_host", "Tempo: Follow Host", "Follow", true),
    boolean (PID::LooperQuantise, "looper_quantise", "Looper: Quantise to Tempo", "Quantise", false),

    boolean (PID::MetronomeOn, "metronome_on", "Metronome: On", "Click", false),
    number (PID::MetronomeLevel, "metronome_level", "Metronome: Level", "Level", -48.0f, 0.0f, -12.0f, "dB"),
    boolean (PID::MetronomeAccent, "metronome_accent", "Metronome: Accent", "Accent", true),
    choice (PID::BeatsPerBar, "beats_per_bar", "Beats per Bar", "Beats", beatsPerBarNames, (int) BeatsPerBar::Four),
    boolean (PID::CountIn, "count_in", "Looper: Count-in", "Count-in", false),
    boolean (PID::MetronomeRecOnly, "metronome_rec_only", "Metronome: Only While Recording", "Rec only", false),
};

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

} // namespace damp
