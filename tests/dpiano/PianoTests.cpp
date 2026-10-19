#include "common/Analysis.h"
#include "dsp/KeyData.h"
#include "dsp/Piano.h"
#include "dsp/Preset.h"

#include "common/dsp/Noise.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>

using namespace dpiano;

namespace
{
constexpr double rate = 48000.0;

std::vector<float> render (Piano& piano, double seconds)
{
    std::vector<float> l ((size_t) (seconds * rate)), r (l.size());
    piano.process (l.data(), r.data(), (int) l.size());
    return l;
}

// Fresh piano with a patch tweak applied
Piano makePiano (std::initializer_list<std::pair<PID, float>> settings = {})
{
    auto patch = Patch::defaults();
    for (auto [pid, value] : settings)
        patch[pid] = value;
    Piano piano;
    piano.setPatch (patch);
    piano.prepare (rate);
    return piano;
}

std::vector<float> playNote (int note, float velocity, double seconds, std::initializer_list<std::pair<PID, float>> settings = {})
{
    auto piano = makePiano (settings);
    piano.noteOn (note, velocity);
    return render (piano, seconds);
}

float unit (dustify::Random& rng) { return 0.5f * (rng.bipolar() + 1.0f); }

// Decay (dB/s) of the component at f between two times, from 100 ms windows
double decayRate (const std::vector<float>& x, double f, double from, double to)
{
    const auto n = (size_t) (0.1 * rate);
    return (test::toneLevelDb (x, rate, f, (size_t) (from * rate), n) - test::toneLevelDb (x, rate, f, (size_t) (to * rate), n)) / (to - from);
}
} // namespace

TEST_CASE ("parameter table is consistent", "[piano][params]")
{
    REQUIRE (allParams().size() == (size_t) numParams);
    for (const auto& info : allParams())
    {
        CHECK (findParam (info.id) == &info);
        CHECK (info.def >= info.min);
        CHECK (info.def <= info.max);
    }
}

TEST_CASE ("presets round trip through text", "[piano][preset]")
{
    auto patch = Patch::defaults();
    patch[PID::HammerHardness] = 0.4f;
    patch[PID::Temperament] = (float) Tuning::Werckmeister3;
    patch[PID::UnisonDetune] = 2.5f;
    const auto parsed = parsePreset (serialisePreset (patch, "Test"));
    CHECK (parsed.warnings.empty());
    CHECK (parsed.name == "Test");
    CHECK (parsed.patch.v == patch.v);
}

TEST_CASE ("key data covers the keyboard sensibly", "[piano][keys]")
{
    for (int key = lowestKey; key <= highestKey; ++key)
    {
        const auto d = keyData (key);
        CHECK (d.strings >= 1);
        CHECK (d.strings <= 3);
        CHECK (d.inharmonicity > 5e-5);
        CHECK (d.inharmonicity < 0.05);
        CHECK (d.strikePosition > 0.03);
        CHECK (d.strikePosition < 0.2);
        CHECK ((d.damperT60 == 0.0) == (key > 88));
    }
    // Inharmonicity rises steeply into the treble; strings get lighter
    CHECK (keyData (108).inharmonicity > 20.0 * keyData (60).inharmonicity);
    CHECK (keyData (21).stringMass > keyData (60).stringMass);
    CHECK (keyData (60).stringMass > keyData (108).stringMass);
}

TEST_CASE ("stretched tuning: flat bass, sharp treble, A4 untouched", "[piano][tuning]")
{
    const auto cents = stretchCents (1.0);
    auto at = [&] (int key) { return cents[(size_t) (key - lowestKey)]; };
    CHECK (at (69) == 0.0);
    CHECK (at (21) < -10.0);
    CHECK (at (21) > -60.0);
    CHECK (at (108) > 10.0);
    CHECK (at (108) < 60.0);
    for (int key = lowestKey + 1; key <= highestKey; ++key)
        CHECK (at (key) >= at (key - 1) - 1.0e-9); // never compresses

    // Without inharmonicity there's nothing to stretch for
    for (double c : stretchCents (0.0))
        CHECK (std::abs (c) < 1.0e-9);
}

TEST_CASE ("A4 is 440 Hz; temperaments keep A4", "[piano][tuning]")
{
    for (int t = 0; t < (int) std::size (tuningNames); ++t)
    {
        const auto piano = makePiano ({ { PID::Temperament, (float) t }, { PID::TemperamentRoot, 3.0f } });
        CHECK (std::abs (piano.firstPartialHz (69) - 440.0) < 1.0e-9);
    }
    const auto piano = makePiano ({ { PID::MasterTune, 100.0f } });
    CHECK (std::abs (piano.firstPartialHz (69) - 440.0 * std::exp2 (1.0 / 12.0)) < 1.0e-6);
}

TEST_CASE ("partials follow the stiff-string series f_n = n f0 sqrt (1 + B n^2)", "[piano][partials]")
{
    const int note = GENERATE (36, 48, 60, 72);
    // One string component per partial so each peak is clean
    const auto x = playNote (note, 0.7f, 6.0, { { PID::UnisonDetune, 0.0f }, { PID::Decay, 4.0f } });
    const auto piano = makePiano();
    const double f1 = piano.firstPartialHz (note);
    const double b = keyData (note).inharmonicity;
    const double f0 = f1 / std::sqrt (1.0 + b);

    for (int n = 1; n <= 8; ++n)
    {
        const double expected = n * f0 * std::sqrt (1.0 + b * n * n);
        if (n * keyData (note).strikePosition > 0.9 && n * keyData (note).strikePosition < 1.1)
            continue; // at the strike point's node: barely excited
        const double measured = test::peakFrequency (x, rate, expected * 0.985, expected * 1.015, (size_t) (0.05 * rate));
        INFO ("note " << note << " partial " << n << ": expected " << expected << " Hz, measured " << measured);
        CHECK (std::abs (test::cents (measured, expected)) < 1.0);
    }
}

TEST_CASE ("prompt decay follows the measured T60 per register", "[piano][decay]")
{
    const int note = GENERATE (36, 60, 84);
    // Aftersound and the board's drain off: only the prompt component's own losses
    const auto x = playNote (note, 0.7f, 3.0, { { PID::Aftersound, -40.0f }, { PID::BoardDrain, 0.0f } });
    const double f1 = makePiano().firstPartialHz (note);
    const double expected = 60.0 / keyData (note).t60;
    const double measured = decayRate (x, f1, 0.3, 1.3);
    INFO ("note " << note << ": expected " << expected << " dB/s, measured " << measured);
    CHECK (measured > 0.75 * expected);
    CHECK (measured < 1.35 * expected);
}

TEST_CASE ("two-stage decay: prompt sound then a slower aftersound", "[piano][decay]")
{
    const auto x = playNote (60, 0.7f, 10.0);
    const double f1 = makePiano().firstPartialHz (60);
    const double early = decayRate (x, f1, 0.2, 1.2);
    // The aftersound beats, so average its decay over a long span
    const double late = decayRate (x, f1, 4.0, 9.0);
    INFO ("early " << early << " dB/s, late " << late << " dB/s");
    CHECK (early > 2.0 * late);
    CHECK (late > 0.0);
}

TEST_CASE ("harder strikes are brighter (hammer felt nonlinearity)", "[piano][hammer]")
{
    const int note = GENERATE (36, 60, 84, 96);
    double previous = 0.0;
    for (float velocity : { 0.15f, 0.4f, 0.7f, 1.0f })
    {
        const auto x = playNote (note, velocity, 0.5);
        const double centroid = test::spectralCentroid (x, rate, 0, 8192);
        INFO ("note " << note << " velocity " << velocity << " centroid " << centroid);
        CHECK (centroid > previous);
        previous = centroid;
    }
}

TEST_CASE ("louder with velocity, over a wide dynamic range", "[piano][hammer]")
{
    const auto soft = playNote (60, 0.1f, 0.5);
    const auto loud = playNote (60, 1.0f, 0.5);
    const double range = test::db (test::rms (loud) / test::rms (soft));
    INFO ("pp .. ff range " << range << " dB");
    CHECK (range > 20.0);
    CHECK (range < 45.0);
}

TEST_CASE ("the hammer felt setting changes brightness", "[piano][hammer]")
{
    const auto soft = playNote (60, 0.6f, 0.5, { { PID::HammerHardness, -1.0f } });
    const auto hard = playNote (60, 0.6f, 0.5, { { PID::HammerHardness, 1.0f } });
    CHECK (test::spectralCentroid (hard, rate, 0, 8192) > 1.3 * test::spectralCentroid (soft, rate, 0, 8192));
}

TEST_CASE ("dampers stop a released key; the sustain pedal holds it", "[piano][dampers]")
{
    const auto n = (size_t) (0.1 * rate);
    {
        auto piano = makePiano();
        piano.noteOn (60, 0.7f);
        const auto held = render (piano, 1.0);
        piano.noteOff (60);
        const auto released = render (piano, 1.0);
        CHECK (test::db (test::rms (released, released.size() - n)) < test::db (test::rms (held, held.size() - n)) - 35.0);
    }
    {
        auto piano = makePiano();
        piano.sustainPedal (true);
        piano.noteOn (60, 0.7f);
        const auto held = render (piano, 1.0);
        piano.noteOff (60);
        const auto sustained = render (piano, 1.0);
        CHECK (test::db (test::rms (sustained, sustained.size() - n)) > test::db (test::rms (held, held.size() - n)) - 12.0);

        // Lifting the pedal damps it
        piano.sustainPedal (false);
        const auto after = render (piano, 1.0);
        CHECK (test::db (test::rms (after, after.size() - n)) < test::db (test::rms (sustained, sustained.size() - n)) - 30.0);
    }
}

TEST_CASE ("the top keys have no dampers", "[piano][dampers]")
{
    auto piano = makePiano();
    piano.noteOn (96, 0.7f);
    const auto held = render (piano, 0.3);
    piano.noteOff (96);
    const auto released = render (piano, 0.3);
    const auto n = (size_t) (0.05 * rate);
    // Just its natural decay over 0.3 s
    CHECK (test::db (test::rms (released, released.size() - n)) > test::db (test::rms (held, held.size() - n)) - 25.0);
}

TEST_CASE ("silent keys stop being processed", "[piano][cpu]")
{
    auto piano = makePiano();
    piano.noteOn (60, 0.7f);
    piano.noteOn (64, 0.7f);
    render (piano, 0.2);
    CHECK (piano.activeKeys() == 2);
    piano.noteOff (60);
    piano.noteOff (64);
    render (piano, 3.0);
    CHECK (piano.activeKeys() == 0);
}

TEST_CASE ("extreme voicing stays finite: hardest felt, fortissimo, top keys", "[piano][fuzz]")
{
    for (float curve : { -1.0f, 1.0f })
    {
        auto piano = makePiano ({ { PID::HammerHardness, 1.0f }, { PID::TrebleHardness, 1.0f }, { PID::FeltNonlinearity, curve },
                                  { PID::DynamicRange, 40.0f } });
        for (int note = 84; note <= highestKey; ++note)
            piano.noteOn (note, 1.0f);
        for (float s : render (piano, 0.3))
            REQUIRE (std::isfinite (s));
    }
}

TEST_CASE ("restriking a ringing key, and random playing, stay finite and bounded", "[piano][fuzz]")
{
    dustify::Random rng (1234);
    for (int trial = 0; trial < 6; ++trial)
    {
        auto patch = Patch::defaults();
        for (const auto& info : allParams())
            if (info.pid != PID::MasterVolume)
                patch[info.pid] = info.type == ParamType::Float ? info.min + (info.max - info.min) * unit (rng)
                                                                : std::round (info.max * unit (rng));
        Piano piano;
        piano.setPatch (patch);
        piano.prepare (rate);

        float peak = 0.0f;
        for (int block = 0; block < 200; ++block)
        {
            const int note = lowestKey + (int) (unit (rng) * numKeys);
            if (unit (rng) < 0.3f)
                piano.noteOn (note, unit (rng));
            if (unit (rng) < 0.2f)
                piano.noteOff (note);
            if (unit (rng) < 0.05f)
                piano.sustainPedal (unit (rng) < 0.5f);
            for (float s : render (piano, 0.01))
            {
                REQUIRE (std::isfinite (s));
                peak = std::max (peak, std::abs (s));
            }
        }
        CHECK (peak < 32.0f); // random extremes, dozens of keys: just nothing running away
    }
}

TEST_CASE ("sympathetic resonance: undamped strings ring along, dampers stop them", "[piano][sympathetic]")
{
    // Difference between the piano with and without sympathetic strings, after the played notes
    auto bloom = [] (bool pedal)
    {
        std::array<std::vector<float>, 2> outs;
        for (int on = 0; on < 2; ++on)
        {
            auto piano = makePiano ({ { PID::Sympathetic, (float) on }, { PID::MechanicalNoise, 0.0f } });
            piano.sustainPedal (pedal);
            for (int note : { 48, 52, 55 })
                piano.noteOn (note, 0.8f);
            outs[(size_t) on] = render (piano, 3.0);
        }
        std::vector<float> diff (outs[0].size());
        for (size_t i = 0; i < diff.size(); ++i)
            diff[i] = outs[1][i] - outs[0][i];
        const auto from = (size_t) (2.0 * rate);
        return test::db (test::rms (diff, from) / test::rms (outs[0], from));
    };

    const double withPedal = bloom (true), withoutPedal = bloom (false);
    INFO ("sympathetic level with pedal " << withPedal << " dB, without " << withoutPedal << " dB");
    CHECK (withPedal > -35.0);
    CHECK (withPedal < -3.0);
    CHECK (withoutPedal < withPedal - 6.0);
}

TEST_CASE ("una corda is softer and darker", "[piano][pedals]")
{
    auto play = [] (bool soft)
    {
        auto piano = makePiano();
        piano.softPedal (soft);
        piano.noteOn (60, 0.7f);
        return render (piano, 0.5);
    };
    const auto normal = play (false), soft = play (true);
    CHECK (test::rms (soft) < test::rms (normal));
    CHECK (test::spectralCentroid (soft, rate, 0, 8192) < test::spectralCentroid (normal, rate, 0, 8192));
}

TEST_CASE ("half pedal damps more slowly than no pedal", "[piano][pedals]")
{
    auto afterRelease = [] (float pedal)
    {
        auto piano = makePiano ({ { PID::MechanicalNoise, 0.0f } });
        piano.sustainPedalLevel (pedal);
        piano.noteOn (48, 0.7f);
        render (piano, 0.5);
        piano.noteOff (48);
        const auto x = render (piano, 0.6);
        return test::db (test::rms (x, x.size() - (size_t) (0.1 * rate)));
    };
    const double none = afterRelease (0.0f), half = afterRelease (0.5f), full = afterRelease (1.0f);
    INFO ("level 0.5 s after release: no pedal " << none << ", half " << half << ", full " << full);
    CHECK (half > none + 6.0);
    CHECK (full > half + 3.0);
}

TEST_CASE ("sostenuto holds only the keys down when it's pressed", "[piano][pedals]")
{
    auto piano = makePiano ({ { PID::MechanicalNoise, 0.0f }, { PID::Sympathetic, 0.0f } });
    piano.noteOn (48, 0.7f);
    render (piano, 0.1);
    piano.sostenutoPedal (true);
    piano.noteOff (48);
    piano.noteOn (72, 0.7f); // played after: not held
    render (piano, 0.2);
    piano.noteOff (72);
    render (piano, 1.5);
    CHECK (piano.key (48).isActive());
    CHECK (! piano.key (72).isActive());

    piano.sostenutoPedal (false);
    render (piano, 3.0);
    CHECK (! piano.key (48).isActive());
}

TEST_CASE ("the lid shapes the treble; mechanical noise is there and can be turned off", "[piano][body]")
{
    // Closing the lid takes the treble down: energy above 1.8 kHz in a C5
    auto trebleEnergyDb = [] (const std::vector<float>& x)
    {
        const auto mag = test::magnitudeSpectrum (x, 0, 16384);
        double sum = 0.0;
        for (size_t i = (size_t) (1800.0 / (rate / 16384.0)); i < mag.size(); ++i)
            sum += mag[i] * mag[i];
        return 10.0 * std::log10 (sum + 1e-30);
    };
    const auto open = playNote (72, 0.7f, 0.5, { { PID::Lid, 1.0f } });
    const auto closed = playNote (72, 0.7f, 0.5, { { PID::Lid, 0.0f } });
    CHECK (trebleEnergyDb (closed) < trebleEnergyDb (open) - 4.0);

    // The keybed thump: the noise setting adds a short burst of energy at the start of a note
    const auto quiet = playNote (96, 0.9f, 0.1, { { PID::MechanicalNoise, 0.0f } });
    const auto noisy = playNote (96, 0.9f, 0.1, { { PID::MechanicalNoise, 2.0f } });
    std::vector<float> diff (quiet.size());
    for (size_t i = 0; i < diff.size(); ++i)
        diff[i] = noisy[i] - quiet[i];
    const auto n = (size_t) (0.03 * rate);
    CHECK (test::db (test::rms (diff, 0, n) / test::rms (quiet, 0, n)) > -30.0);
}

TEST_CASE ("stereo: bass on the player's left, on the audience's right", "[piano][stereo]")
{
    auto balance = [] (int note, Perspective perspective, float bloom)
    {
        auto piano = makePiano ({ { PID::Perspective, (float) perspective }, { PID::StereoWidth, 1.0f }, { PID::RoomMix, 0.0f },
                                  { PID::Bloom, bloom } });
        piano.noteOn (note, 0.7f);
        std::vector<float> l ((size_t) (0.5 * rate)), r (l.size());
        piano.process (l.data(), r.data(), (int) l.size());
        return test::db (test::rms (l) / test::rms (r)); // > 0: left louder
    };
    // The panning itself (the board's resonances add a little image of their own)
    CHECK (balance (33, Perspective::Player, 0.0f) > 3.0);
    CHECK (balance (96, Perspective::Player, 0.0f) < -3.0);
    CHECK (balance (33, Perspective::Audience, 0.0f) < -3.0);
    CHECK (balance (96, Perspective::Audience, 0.0f) > 3.0);
    // With the default bloom, switching sides still flips the image
    CHECK (balance (33, Perspective::Player, 0.5f) - balance (33, Perspective::Audience, 0.5f) > 4.0);
    CHECK (balance (96, Perspective::Audience, 0.5f) - balance (96, Perspective::Player, 0.5f) > 4.0);
}

TEST_CASE ("factory presets parse cleanly and play sensibly", "[piano][preset]")
{
    int count = 0;
    for (const auto& entry : std::filesystem::directory_iterator (DPIANO_PRESET_DIR))
    {
        std::ifstream file (entry.path());
        const std::string text { std::istreambuf_iterator<char> (file), {} };
        const auto parsed = parsePreset (text);
        INFO (entry.path().filename().string());
        CHECK (parsed.warnings.empty());
        CHECK (! parsed.name.empty());
        CHECK (! parsed.category.empty());

        Piano piano;
        piano.setPatch (parsed.patch);
        piano.prepare (rate);
        piano.sustainPedal (true);
        for (int note : { 36, 48, 55, 60, 64, 67, 72 })
            piano.noteOn (note, 0.9f);
        float peak = 0.0f;
        for (float s : render (piano, 1.0))
        {
            REQUIRE (std::isfinite (s));
            peak = std::max (peak, std::abs (s));
        }
        CHECK (peak < 1.5f); // a 7-note fortissimo chord on the pedal: loud, but nothing running away
        ++count;
    }
    CHECK (count >= 10);
}

TEST_CASE ("voicing: board drain, bloom and knock do what they say", "[piano][voicing]")
{
    const double f1 = makePiano().firstPartialHz (60);

    // The board drains a middle C's fundamental quickly at first
    const auto drained = playNote (60, 0.6f, 1.5, { { PID::BoardDrain, 2.0f }, { PID::Aftersound, -40.0f } });
    const auto undrained = playNote (60, 0.6f, 1.5, { { PID::BoardDrain, 0.0f }, { PID::Aftersound, -40.0f } });
    CHECK (decayRate (drained, f1, 0.2, 1.2) > decayRate (undrained, f1, 0.2, 1.2) + 10.0);

    // Bloom: the sound builds up, so less of it arrives in the first few ms relative to later
    auto onsetShare = [] (const std::vector<float>& x)
    {
        return test::db (test::rms (x, 0, (size_t) (0.004 * rate)) / test::rms (x, (size_t) (0.02 * rate), (size_t) (0.06 * rate)));
    };
    const auto dry = playNote (60, 0.6f, 0.2, { { PID::Bloom, 0.0f }, { PID::RoomMix, 0.0f } });
    const auto bloomy = playNote (60, 0.6f, 0.2, { { PID::Bloom, 1.0f }, { PID::RoomMix, 0.0f } });
    CHECK (onsetShare (bloomy) < onsetShare (dry) - 2.0);

    // Knock: more energy in the first few ms of a treble note, outside its partials
    const auto plain = playNote (96, 0.7f, 0.05, { { PID::Knock, 0.0f }, { PID::MechanicalNoise, 0.0f } });
    const auto knocked = playNote (96, 0.7f, 0.05, { { PID::Knock, 3.0f }, { PID::MechanicalNoise, 0.0f } });
    const auto n = (size_t) (0.01 * rate);
    CHECK (test::toneLevelDb (knocked, rate, 700.0, 0, n) > test::toneLevelDb (plain, rate, 700.0, 0, n) + 6.0);
}

TEST_CASE ("dynamic range sets the pp .. ff span", "[piano][voicing]")
{
    auto span = [] (float range)
    {
        const auto soft = playNote (60, 0.15f, 0.5, { { PID::DynamicRange, range } });
        const auto loud = playNote (60, 1.0f, 0.5, { { PID::DynamicRange, range } });
        return test::db (test::rms (loud) / test::rms (soft));
    };
    CHECK (span (40.0f) > span (14.0f) + 10.0);
}
