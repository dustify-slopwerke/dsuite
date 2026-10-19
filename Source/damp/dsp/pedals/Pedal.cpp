#include "Pedal.h"

#include <algorithm>
#include <cctype>
#include <vector>

namespace damp
{
namespace
{
constexpr PID boostKnobs[] { PID::BoostGain, PID::BoostTone };
constexpr PID midDriveKnobs[] { PID::MidDriveDrive, PID::MidDriveTone, PID::MidDriveLevel };
constexpr PID bluesDriveKnobs[] { PID::BluesDriveGain, PID::BluesDriveTone, PID::BluesDriveLevel };
constexpr PID distortionKnobs[] { PID::DistortionDist, PID::DistortionTone, PID::DistortionLevel };
constexpr PID rodentKnobs[] { PID::RodentDist, PID::RodentFilter, PID::RodentLevel };
constexpr PID faceFuzzKnobs[] { PID::FaceFuzzFuzz, PID::FaceFuzzLevel };
constexpr PID bigFuzzKnobs[] { PID::BigFuzzSustain, PID::BigFuzzTone, PID::BigFuzzLevel };
constexpr PID compressorKnobs[] { PID::CompressorSustain, PID::CompressorAttack, PID::CompressorBlend, PID::CompressorLevel };
constexpr PID wahKnobs[] { PID::WahMode, PID::WahPosition, PID::WahSensitivity, PID::WahResponse, PID::WahPeak };
constexpr PID octaveKnobs[] { PID::OctaveDirect, PID::OctaveOct1, PID::OctaveOct2 };
constexpr PID chorusKnobs[] { PID::ChorusRate, PID::ChorusDepth, PID::ChorusMix, PID::ChorusWidth };
constexpr PID flangerKnobs[] { PID::FlangerRate, PID::FlangerDepth, PID::FlangerManual, PID::FlangerRegen };
constexpr PID phaserKnobs[] { PID::PhaserRate, PID::PhaserDepth, PID::PhaserFeedback, PID::PhaserStages };
constexpr PID tremoloKnobs[] { PID::TremoloRate, PID::TremoloDepth, PID::TremoloShape, PID::TremoloPan };
constexpr PID vibratoKnobs[] { PID::VibratoRate, PID::VibratoDepth, PID::VibratoMode };
constexpr PID delayKnobs[] { PID::DelayMode, PID::DelayTime, PID::DelaySync, PID::DelayFeedback, PID::DelayMix,
                             PID::DelayTone, PID::DelayMod, PID::DelayPingPong, PID::DelayTrails };
constexpr PID pitchKnobs[] { PID::PitchMode, PID::PitchShift, PID::PitchPosition, PID::PitchMix };
constexpr PID eqKnobs[] { PID::Eq100, PID::Eq200, PID::Eq400, PID::Eq800, PID::Eq1k6, PID::Eq3k2, PID::Eq6k4, PID::EqLevel };
constexpr PID reverbKnobs[] { PID::ReverbMode, PID::ReverbDecay, PID::ReverbPredelay, PID::ReverbTone, PID::ReverbMix,
                              PID::ReverbTrails };

constexpr PedalInfo pedals[] {
    { PedalId::Boost, "boost", "Boost", PID::BoostOn, boostKnobs, Placement::BeforeAmp },
    { PedalId::MidDrive, "mid_drive", "Mid Drive", PID::MidDriveOn, midDriveKnobs, Placement::BeforeAmp },
    { PedalId::BluesDrive, "blues_drive", "Blues Drive", PID::BluesDriveOn, bluesDriveKnobs, Placement::BeforeAmp },
    { PedalId::Distortion, "distortion", "Distortion", PID::DistortionOn, distortionKnobs, Placement::BeforeAmp },
    { PedalId::Rodent, "rodent", "Rodent", PID::RodentOn, rodentKnobs, Placement::BeforeAmp },
    { PedalId::FaceFuzz, "face_fuzz", "Face Fuzz", PID::FaceFuzzOn, faceFuzzKnobs, Placement::BeforeAmp },
    { PedalId::BigFuzz, "big_fuzz", "Big Fuzz", PID::BigFuzzOn, bigFuzzKnobs, Placement::BeforeAmp },
    { PedalId::Compressor, "compressor", "Compressor", PID::CompressorOn, compressorKnobs, Placement::Either },
    { PedalId::Wah, "wah", "Wah", PID::WahOn, wahKnobs, Placement::Either },
    { PedalId::Octave, "octave", "Octave", PID::OctaveOn, octaveKnobs, Placement::BeforeAmp }, // tracks the dry guitar only
    { PedalId::Chorus, "chorus", "Chorus", PID::ChorusOn, chorusKnobs, Placement::Either },
    { PedalId::Flanger, "flanger", "Flanger", PID::FlangerOn, flangerKnobs, Placement::Either },
    { PedalId::Phaser, "phaser", "Phaser", PID::PhaserOn, phaserKnobs, Placement::Either },
    { PedalId::Tremolo, "tremolo", "Tremolo", PID::TremoloOn, tremoloKnobs, Placement::Either },
    { PedalId::Vibrato, "vibrato", "Vibrato", PID::VibratoOn, vibratoKnobs, Placement::Either },
    { PedalId::Delay, "delay", "Delay", PID::DelayOn, delayKnobs, Placement::AfterAmp },
    { PedalId::Reverb, "reverb", "Reverb", PID::ReverbOn, reverbKnobs, Placement::AfterAmp },
    { PedalId::Pitch, "pitch", "Pitch Shifter", PID::PitchOn, pitchKnobs, Placement::Either },
    { PedalId::Eq, "eq", "Graphic EQ", PID::EqOn, eqKnobs, Placement::Either },
};
static_assert (std::size (pedals) == (size_t) numPedals);

// Wah, compressor, octave and pitch shifter first, then fuzzes (they want the guitar's own impedance),
// distortions, drives, boost, amp; the EQ (in the amp's loop), modulation, then delay and reverb
constexpr int defaultOrder[] { (int) PedalId::Wah, (int) PedalId::Compressor, (int) PedalId::Octave, (int) PedalId::Pitch,
                               (int) PedalId::FaceFuzz, (int) PedalId::BigFuzz, (int) PedalId::Rodent, (int) PedalId::Distortion,
                               (int) PedalId::BluesDrive, (int) PedalId::MidDrive, (int) PedalId::Boost, BoardOrder::amp,
                               (int) PedalId::Eq, (int) PedalId::Phaser, (int) PedalId::Flanger, (int) PedalId::Chorus, (int) PedalId::Vibrato,
                               (int) PedalId::Tremolo, (int) PedalId::Delay, (int) PedalId::Reverb };
static_assert (std::size (defaultOrder) == (size_t) BoardOrder::size);
} // namespace

const PedalInfo& pedalInfo (PedalId id)
{
    return pedals[(size_t) id];
}

BoardOrder BoardOrder::defaults()
{
    BoardOrder b;
    std::copy (std::begin (defaultOrder), std::end (defaultOrder), b.slots.begin());
    return b;
}

int BoardOrder::ampPosition() const
{
    return (int) (std::find (slots.begin(), slots.end(), amp) - slots.begin());
}

BoardOrder BoardOrder::parse (std::string_view text)
{
    std::vector<int> order;
    auto seen = [&] (int v) { return std::find (order.begin(), order.end(), v) != order.end(); };

    size_t at = 0;
    while (at <= text.size())
    {
        const auto end = std::min (text.find (',', at), text.size());
        auto word = text.substr (at, end - at);
        while (! word.empty() && std::isspace ((unsigned char) word.front()))
            word.remove_prefix (1);
        while (! word.empty() && std::isspace ((unsigned char) word.back()))
            word.remove_suffix (1);
        at = end + 1;

        int v = word == "amp" ? amp : -2;
        for (const auto& p : pedals)
            if (p.key == word)
                v = (int) p.id;
        if (v != -2 && ! seen (v))
            order.push_back (v);
    }
    if (! seen (amp))
        order.push_back (amp);

    // Clipping pedals stay before the amp, delays and reverbs after it
    std::vector<int> before, after;
    for (auto it = order.begin(); it != order.end();)
    {
        const bool beforeAmp = it < std::find (order.begin(), order.end(), amp);
        if (*it != amp && beforeAmp && ! pedalInfo ((PedalId) *it).beforeAmpAllowed())
        {
            after.push_back (*it);
            it = order.erase (it);
        }
        else if (*it != amp && ! beforeAmp && ! pedalInfo ((PedalId) *it).afterAmpAllowed())
        {
            before.push_back (*it);
            it = order.erase (it);
        }
        else
        {
            ++it;
        }
    }
    order.insert (std::find (order.begin(), order.end(), amp), before.begin(), before.end());
    order.insert (std::find (order.begin(), order.end(), amp) + 1, after.begin(), after.end());

    // Missing pedals: before the first pedal (or the amp) that follows them in the default order
    // (after the clean-up, so a pedal meant for the front can't follow a misplaced one past the amp)
    const auto& d = defaultOrder;
    for (auto v = std::begin (d); v != std::end (d); ++v)
    {
        if (seen (*v))
            continue;
        auto place = order.end();
        for (auto next = v + 1; next != std::end (d) && place == order.end(); ++next)
            place = std::find (order.begin(), order.end(), *next);
        order.insert (place, *v);
    }

    BoardOrder b;
    std::copy (order.begin(), order.end(), b.slots.begin());
    return b;
}

std::string BoardOrder::format() const
{
    std::string s;
    for (int v : slots)
    {
        if (! s.empty())
            s += ", ";
        s += v == amp ? std::string ("amp") : std::string (pedalInfo ((PedalId) v).key);
    }
    return s;
}

} // namespace damp
