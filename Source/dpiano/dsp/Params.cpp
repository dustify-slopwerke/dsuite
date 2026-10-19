#include "Params.h"

namespace dpiano
{
namespace
{
using sv = std::string_view;

constexpr sv velocityCurves[] { "Soft", "Normal", "Hard" };
constexpr sv perspectives[] { "Player", "Audience" };

// Must be in PID order (checked below)
constexpr ParamInfo table[] {
    number (PID::HammerHardness, "hammer_hardness", "Hammer Hardness", "Hardness", -1.0f, 1.0f, 0.0f),
    choice (PID::VelocityCurve, "velocity_curve", "Velocity Curve", "Vel Curve", velocityCurves, (int) VelocityCurve::Normal),
    number (PID::StrikePosition, "strike_position", "Strike Position", "Strike Pt", -1.0f, 1.0f, 0.0f),

    number (PID::Inharmonicity, "inharmonicity", "Inharmonicity", "Inharm", 0.0f, 2.0f, 1.0f),
    number (PID::UnisonDetune, "unison_detune", "Unison Detune", "Unison", 0.0f, 6.0f, 0.8f, "cents", 1.5f),
    number (PID::Aftersound, "aftersound", "Aftersound Level", "Aftersound", -40.0f, -6.0f, -18.0f, "dB"),
    number (PID::Decay, "decay", "String Decay", "Decay", 0.25f, 4.0f, 1.0f, {}, 1.0f),
    number (PID::Brilliance, "brilliance", "Brilliance", "Brilliance", -1.0f, 1.0f, 0.0f),
    number (PID::DamperStrength, "damper_strength", "Damper Strength", "Dampers", 0.25f, 4.0f, 1.0f, {}, 1.0f),

    number (PID::StretchTuning, "stretch_tuning", "Stretch Tuning", "Stretch", 0.0f, 2.0f, 1.0f),
    number (PID::MasterTune, "master_tune", "Master Tune", "Tune", -100.0f, 100.0f, 0.0f, "ct"),
    choice (PID::Temperament, "temperament", "Temperament", "Temperament", tuningNames, (int) Tuning::Equal),
    choice (PID::TemperamentRoot, "temperament_root", "Temperament Root", "Root", noteNames, 0),

    number (PID::MasterVolume, "master_volume", "Master Volume", "Volume", -48.0f, 12.0f, 0.0f, "dB"),

    number (PID::Sympathetic, "sympathetic", "Sympathetic Resonance", "Sympathetic", 0.0f, 1.0f, 0.5f),
    number (PID::SoundboardBody, "soundboard_body", "Soundboard Body", "Body", 0.0f, 1.0f, 0.6f),
    number (PID::Lid, "lid", "Lid Opening", "Lid", 0.0f, 1.0f, 1.0f),
    number (PID::MechanicalNoise, "mechanical_noise", "Mechanical Noise", "Noises", 0.0f, 2.0f, 1.0f),

    choice (PID::Perspective, "perspective", "Stereo Perspective", "Perspective", perspectives, (int) Perspective::Player),
    number (PID::StereoWidth, "stereo_width", "Stereo Width", "Width", 0.0f, 1.0f, 0.7f),
    number (PID::MicDistance, "mic_distance", "Microphone Distance", "Distance", 0.0f, 1.0f, 0.3f),
    number (PID::RoomMix, "room_mix", "Room Mix", "Room", 0.0f, 1.0f, 0.15f),
    number (PID::RoomSize, "room_size", "Room Size", "Size", 0.0f, 1.0f, 0.5f),

    number (PID::TrebleHardness, "treble_hardness", "Treble Hammer Hardness", "Treble Felt", -1.0f, 1.0f, 0.0f),
    number (PID::FeltNonlinearity, "felt_nonlinearity", "Felt Nonlinearity", "Felt Curve", -1.0f, 1.0f, 0.0f),
    number (PID::DynamicRange, "dynamic_range", "Dynamic Range", "Dyn Range", 12.0f, 40.0f, 28.0f, "dB"),
    number (PID::Knock, "knock", "Hammer Knock", "Knock", 0.0f, 3.0f, 1.0f),
    number (PID::BoardDrain, "board_drain", "Soundboard Energy Drain", "Board Drain", 0.0f, 2.0f, 1.0f),
    number (PID::BassRadiation, "bass_radiation", "Bass Radiation Cutoff", "Bass Cut", 30.0f, 300.0f, 160.0f, "Hz", 100.0f),
    number (PID::Warmth, "warmth", "Warmth (Board Mobility)", "Warmth", 0.0f, 24.0f, 14.0f, "dB"),
    number (PID::Air, "air", "Air (Treble Roll-off)", "Air", 2000.0f, 20000.0f, 6000.0f, "Hz", 6000.0f),
    number (PID::Bloom, "bloom", "Soundboard Bloom", "Bloom", 0.0f, 1.0f, 0.5f),
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

} // namespace dpiano
