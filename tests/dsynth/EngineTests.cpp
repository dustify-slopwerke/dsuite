#include "AnalysisHelpers.h"

#include "dsp/LayeredSynth.h"
#include "dsp/Preset.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <filesystem>
#include <fstream>

using namespace dsynth;
using Catch::Matchers::WithinAbs;

namespace
{
constexpr double rate = 48000.0;

Engine makeEngine (const Patch& patch)
{
    Engine engine;
    engine.setPatch (patch);
    engine.prepare (rate);
    return engine;
}

Patch sinePatch()
{
    auto p = test::cleanPatch();
    p[PID::Osc1Wave] = (float) Wave::Sine;
    return p;
}

std::string readFile (const std::filesystem::path& path)
{
    std::ifstream f (path);
    return { std::istreambuf_iterator<char> (f), {} };
}
} // namespace

TEST_CASE ("Notes play at concert pitch through the whole engine")
{
    struct Case { Range range; float tune; float masterCents; int note; double expectedHz; };
    const auto c = GENERATE (Case { Range::Ft8, 0.0f, 0.0f, 69, 440.0 },
                             Case { Range::Ft16, 0.0f, 0.0f, 69, 220.0 },
                             Case { Range::Ft2, 0.0f, 0.0f, 57, 880.0 },
                             Case { Range::Ft8, 7.0f, 0.0f, 69, 659.2551 },
                             Case { Range::Ft8, 0.0f, 50.0f, 60, 269.2918 });

    auto p = sinePatch();
    p[PID::Osc1Range] = (float) c.range;
    p[PID::Osc1Tune] = c.tune;
    p[PID::MasterTune] = c.masterCents;
    auto engine = makeEngine (p);

    engine.noteOn (c.note, 1.0f);
    const auto out = test::render (engine, 1.5, rate);
    const double f = test::estimateFrequency (out, rate, (size_t) (0.5 * rate));
    CHECK_THAT (test::cents (f, c.expectedHz), WithinAbs (0.0, 1.0));
}

TEST_CASE ("Mono note priority")
{
    auto p = test::cleanPatch();
    p[PID::VoiceMode] = (float) VoiceMode::Mono;

    SECTION ("Low: a higher key doesn't take over, releasing the low key does")
    {
        p[PID::NotePriority] = (float) NotePriority::Low;
        auto engine = makeEngine (p);
        engine.noteOn (60, 1.0f);
        engine.noteOn (64, 1.0f);
        CHECK (engine.getMonoNote() == 60);
        engine.noteOn (55, 1.0f);
        CHECK (engine.getMonoNote() == 55);
        engine.noteOff (55);
        engine.noteOff (60);
        CHECK (engine.getMonoNote() == 64);
    }

    SECTION ("High")
    {
        p[PID::NotePriority] = (float) NotePriority::High;
        auto engine = makeEngine (p);
        engine.noteOn (60, 1.0f);
        engine.noteOn (55, 1.0f);
        CHECK (engine.getMonoNote() == 60);
        engine.noteOn (67, 1.0f);
        CHECK (engine.getMonoNote() == 67);
    }

    SECTION ("Last, returning to held keys")
    {
        p[PID::NotePriority] = (float) NotePriority::Last;
        auto engine = makeEngine (p);
        engine.noteOn (60, 1.0f);
        engine.noteOn (64, 1.0f);
        engine.noteOn (67, 1.0f);
        CHECK (engine.getMonoNote() == 67);
        engine.noteOff (67);
        CHECK (engine.getMonoNote() == 64);
        engine.noteOff (60);
        CHECK (engine.getMonoNote() == 64);
        engine.noteOff (64);
        CHECK (engine.getMonoNote() == -1);
        CHECK_FALSE (engine.getVoice (0).isGateOn());
    }
}

TEST_CASE ("Mono retriggers on new keys, legato doesn't")
{
    auto p = test::cleanPatch();
    p[PID::NotePriority] = (float) NotePriority::Last;

    p[PID::VoiceMode] = (float) VoiceMode::Mono;
    auto mono = makeEngine (p);
    mono.noteOn (60, 1.0f);
    mono.noteOn (64, 1.0f);
    CHECK (mono.getVoice (0).getTriggerCount() == 2);
    mono.noteOff (64); // back to the held 60: no retrigger
    CHECK (mono.getVoice (0).getTriggerCount() == 2);
    CHECK (mono.getMonoNote() == 60);

    p[PID::VoiceMode] = (float) VoiceMode::Legato;
    auto legato = makeEngine (p);
    legato.noteOn (60, 1.0f);
    legato.noteOn (64, 1.0f);
    legato.noteOff (64);
    CHECK (legato.getVoice (0).getTriggerCount() == 1);
    legato.noteOff (60);
    legato.noteOn (62, 1.0f); // detached note retriggers
    CHECK (legato.getVoice (0).getTriggerCount() == 2);
}

TEST_CASE ("Glide reaches its target in the set time")
{
    auto p = test::cleanPatch();
    p[PID::VoiceMode] = (float) VoiceMode::Legato;
    p[PID::NotePriority] = (float) NotePriority::Last;
    p[PID::GlideTime] = 0.1f;

    SECTION ("Exponential: 99% of the way after the glide time")
    {
        p[PID::GlideType] = (float) GlideType::Exponential;
        auto engine = makeEngine (p);
        engine.noteOn (48, 1.0f);
        test::render (engine, 0.05, rate);
        CHECK_THAT (engine.getVoice (0).getPitch(), WithinAbs (48.0, 1.0e-6)); // first note doesn't glide
        engine.noteOn (60, 1.0f);
        test::render (engine, 0.1, rate);
        CHECK_THAT (engine.getVoice (0).getPitch(), WithinAbs (60.0, 0.121));
        test::render (engine, 0.1, rate);
        CHECK_THAT (engine.getVoice (0).getPitch(), WithinAbs (60.0, 0.002));
    }

    SECTION ("Linear: constant time, straight line")
    {
        p[PID::GlideType] = (float) GlideType::Linear;
        auto engine = makeEngine (p);
        engine.noteOn (48, 1.0f);
        engine.noteOn (60, 1.0f);
        test::render (engine, 0.05, rate);
        CHECK_THAT (engine.getVoice (0).getPitch(), WithinAbs (54.0, 0.01));
        test::render (engine, 0.051, rate);
        CHECK_THAT (engine.getVoice (0).getPitch(), WithinAbs (60.0, 1.0e-6));
    }

    SECTION ("Legato-only glide doesn't slide between detached notes")
    {
        p[PID::GlideMode] = (float) GlideMode::LegatoOnly;
        auto engine = makeEngine (p);
        engine.noteOn (48, 1.0f);
        engine.noteOff (48);
        engine.noteOn (60, 1.0f);
        CHECK_THAT (engine.getVoice (0).getPitch(), WithinAbs (60.0, 1.0e-6));
    }
}

TEST_CASE ("Poly mode allocates, reuses and steals voices")
{
    auto p = test::cleanPatch();
    p[PID::VoiceMode] = (float) VoiceMode::Poly;
    auto engine = makeEngine (p);

    for (int n = 60; n < 70; ++n)
        engine.noteOn (n, 1.0f);
    CHECK (engine.numActiveVoices() == Engine::maxVoices);

    // The two oldest notes were stolen
    std::vector<int> notes;
    for (int i = 0; i < Engine::maxVoices; ++i)
        notes.push_back (engine.getVoice (i).getNote());
    std::sort (notes.begin(), notes.end());
    CHECK (notes == std::vector<int> { 62, 63, 64, 65, 66, 67, 68, 69 });

    // Re-pressing a sounding note reuses its voice
    engine.noteOn (65, 1.0f);
    CHECK (engine.numActiveVoices() == Engine::maxVoices);
    int count65 = 0;
    for (int i = 0; i < Engine::maxVoices; ++i)
        count65 += engine.getVoice (i).getNote() == 65;
    CHECK (count65 == 1);
}

TEST_CASE ("Sustain pedal holds released notes")
{
    auto p = test::cleanPatch();
    p[PID::VoiceMode] = (float) VoiceMode::Poly;
    auto engine = makeEngine (p);

    engine.noteOn (60, 1.0f);
    engine.sustainPedal (true);
    engine.noteOff (60);
    CHECK (engine.getVoice (0).isGateOn());
    engine.sustainPedal (false);
    CHECK_FALSE (engine.getVoice (0).isGateOn());
}

TEST_CASE ("Released notes decay to silence and free their voice")
{
    auto engine = makeEngine (Patch::defaults());
    engine.noteOn (60, 1.0f);
    test::render (engine, 0.2, rate);
    engine.noteOff (60);
    const auto out = test::render (engine, 1.5, rate);
    CHECK (test::rms (out, out.size() - 4800) < 1.0e-4);
    CHECK (engine.numActiveVoices() == 0);
}

TEST_CASE ("Ladder Whistle preset plays the keyboard pitch with no oscillators")
{
    const auto preset = parsePreset (readFile (std::filesystem::path (DSYNTH_PRESET_DIR) / "80_04_ladder_whistle.dsynth"));
    REQUIRE (preset.warnings.empty());

    auto engine = makeEngine (preset.patch);
    engine.noteOn (69, 1.0f);
    const auto out = test::render (engine, 1.0, rate);

    // Must build up quickly enough to be playable: clearly audible (not noise floor) within 0.2 s
    const double early = test::rms (out, (size_t) (0.1 * rate), (size_t) (0.2 * rate));
    const double settled = test::rms (out, (size_t) (0.6 * rate));
    INFO ("early " << test::db (early) << " dB, settled " << test::db (settled) << " dB");
    CHECK (test::db (early / settled) > -30.0);

    const double f = test::estimateFrequency (out, rate, out.size() / 2);
    INFO ("whistle at " << f << " Hz");
    CHECK (std::abs (test::cents (f, 440.0)) < 15.0);
}

TEST_CASE ("Random patches and gestures never produce NaN or blow up")
{
    Random rng (1234);
    for (int trial = 0; trial < 100; ++trial)
    {
        Patch p;
        for (const auto& info : allParams())
        {
            const float u = (float) rng.unipolar();
            p[info.pid] = info.type == ParamType::Float ? info.min + u * (info.max - info.min)
                                                        : std::round (u * info.max);
        }
        p[PID::MasterVolume] = 0.0f;

        auto engine = makeEngine (p);
        engine.noteOn (36 + (int) (rng.unipolar() * 60), 1.0f);
        engine.noteOn (36 + (int) (rng.unipolar() * 60), 0.5f);
        engine.pitchBend (rng.bipolar());
        engine.modWheel ((float) rng.unipolar());
        auto out = test::render (engine, 0.15, rate);
        engine.noteOff (36);
        engine.allNotesOff();
        const auto tail = test::render (engine, 0.1, rate);
        out.insert (out.end(), tail.begin(), tail.end());

        float peak = 0.0f;
        for (float s : out)
        {
            REQUIRE (std::isfinite (s));
            peak = std::max (peak, std::abs (s));
        }
        INFO ("trial " << trial << "\n" << serialisePreset (p, "random"));
        // Hot settings legitimately stack up (drive + resonance compensation + bass boost, then
        // delay and reverb added on top at high mix); this catches instability, which shows up
        // as inf/NaN or runaway growth far beyond this
        CHECK (peak < 100.0f);
    }
}

TEST_CASE ("Osc 1 hard-syncs to osc 2")
{
    auto p = test::cleanPatch();
    p[PID::Osc1Tune] = 5.3f;     // slave pitch is no longer a harmonic of the master...
    p[PID::Osc1Sync] = 1.0f;     // ...but sync forces it to repeat at osc 2's period
    p[PID::Osc2On] = 0.0f;       // master runs silently, as on a Prophet sync lead
    auto engine = makeEngine (p);
    engine.noteOn (57, 1.0f);    // osc 2 at 220 Hz
    const auto out = test::render (engine, 1.0, rate);

    const double alias = test::worstAliasDb (out, rate, 220.0, 15000.0);
    INFO ("worst non-harmonic of 220 Hz: " << alias << " dB");
    CHECK (alias < -40.0);
}

TEST_CASE ("Poly-Mod filter envelope raises osc 1 pitch")
{
    auto p = sinePatch();
    p[PID::PmFilterEnv] = 0.5f;  // 0.5^2 * 48 = +12 st at full envelope
    p[PID::PmToFreq] = 1.0f;
    p[PID::FEnvA] = 0.001f;
    p[PID::FEnvS] = 1.0f;
    auto engine = makeEngine (p);
    engine.noteOn (69, 1.0f);
    const auto out = test::render (engine, 1.0, rate);
    CHECK_THAT (test::cents (test::estimateFrequency (out, rate, out.size() / 2), 880.0), WithinAbs (0.0, 1.0));
}

TEST_CASE ("Mod matrix: mod wheel to osc 1 pitch")
{
    auto p = sinePatch();
    p[PID::Mx1Source] = (float) MatrixSource::ModWheel;
    p[PID::Mx1Dest] = (float) MatrixDest::Osc1Pitch;
    p[PID::Mx1Amount] = 0.5f;    // 0.5^2 * 24 = +6 st at full wheel
    p[PID::ModToPitch] = 0.0f;   // the wheel also drives the wheel-mod section
    auto engine = makeEngine (p);
    engine.modWheel (1.0f);
    engine.setPatch (p);
    engine.noteOn (69, 1.0f);
    const auto out = test::render (engine, 1.0, rate);
    CHECK_THAT (test::cents (test::estimateFrequency (out, rate, out.size() / 2), 440.0 * std::sqrt (2.0)), WithinAbs (0.0, 1.0));
}

TEST_CASE ("Mod matrix: velocity to amp")
{
    auto p = sinePatch();
    p[PID::Mx1Source] = (float) MatrixSource::Velocity;
    p[PID::Mx1Dest] = (float) MatrixDest::Amp;
    p[PID::Mx1Amount] = -1.0f; // full velocity silences, zero velocity is full level

    auto levelFor = [&] (float velocity)
    {
        auto engine = makeEngine (p);
        engine.noteOn (69, velocity);
        const auto out = test::render (engine, 0.5, rate);
        return test::rms (out, out.size() / 2);
    };
    CHECK_THAT (levelFor (0.5f) / levelFor (0.0f), WithinAbs (0.5, 0.01));
    CHECK (levelFor (1.0f) < 1.0e-4);
}

TEST_CASE ("Unison stacks detuned voices on one note")
{
    auto p = test::cleanPatch();
    p[PID::VoiceMode] = (float) VoiceMode::Unison;
    p[PID::NotePriority] = (float) NotePriority::Last;
    p[PID::UnisonVoices] = 4.0f;
    p[PID::UnisonDetune] = 1.0f;
    auto engine = makeEngine (p);

    engine.noteOn (60, 1.0f);
    REQUIRE (engine.numActiveVoices() == 4);
    std::vector<float> detunes;
    for (int i = 0; i < 4; ++i)
    {
        CHECK (engine.getVoice (i).getNote() == 60);
        detunes.push_back (engine.getVoice (i).getDetune());
    }
    CHECK_THAT (detunes.front(), WithinAbs (-0.3, 1.0e-6));
    CHECK_THAT (detunes.back(), WithinAbs (0.3, 1.0e-6));

    engine.noteOn (64, 1.0f);
    for (int i = 0; i < 4; ++i)
        CHECK (engine.getVoice (i).getNote() == 64);

    engine.noteOff (64);
    engine.noteOff (60);
    for (int i = 0; i < 4; ++i)
        CHECK_FALSE (engine.getVoice (i).isGateOn());
}

TEST_CASE ("Wheel mod LFO vibrato runs at the LFO rate")
{
    auto p = sinePatch();
    p[PID::ModSource] = (float) ModSource::Lfo;
    p[PID::ModAmount] = 0.2f;
    p[PID::LfoRate] = 5.0f;
    p[PID::LfoWave] = (float) LfoWave::Sine;
    auto engine = makeEngine (p);
    engine.noteOn (69, 1.0f);
    const auto out = test::render (engine, 2.0, rate);

    // Instantaneous frequency from successive zero crossings, then the rate of its wobble
    std::vector<float> freq;
    double lastCrossing = -1.0;
    for (size_t i = 1; i < out.size(); ++i)
        if (out[i - 1] < 0.0f && out[i] >= 0.0f)
        {
            const double t = (double) (i - 1) + out[i - 1] / (double) (out[i - 1] - out[i]);
            if (lastCrossing >= 0.0)
                freq.push_back ((float) (rate / (t - lastCrossing) - 440.0));
            lastCrossing = t;
        }
    const double periodsPerSecond = (double) freq.size() / 2.0;
    CHECK_THAT (test::estimateFrequency (freq, periodsPerSecond), WithinAbs (5.0, 0.1));
}

TEST_CASE ("Phase reset restarts the waveform on every note from silence")
{
    auto attackOf = [] (bool reset)
    {
        auto p = test::cleanPatch();
        p[PID::PhaseReset] = reset ? 1.0f : 0.0f;
        p[PID::AEnvA] = 0.001f;
        p[PID::AEnvR] = 0.005f;
        auto engine = makeEngine (p);

        std::vector<std::vector<float>> attacks;
        for (int i = 0; i < 2; ++i)
        {
            engine.noteOn (57, 1.0f);
            auto out = test::render (engine, 0.05, rate);
            engine.noteOff (57);
            test::render (engine, 0.3 + 0.0123 * i, rate); // different gaps: free-running VCOs drift apart
            attacks.push_back (std::vector<float> (out.begin(), out.begin() + 600));
        }
        double diff = 0.0;
        for (size_t i = 0; i < 600; ++i)
            diff = std::max (diff, (double) std::abs (attacks[0][i] - attacks[1][i]));
        return diff;
    };

    CHECK (attackOf (true) < 1.0e-3);
    CHECK (attackOf (false) > 0.05);
}

TEST_CASE ("DCO pitch ignores drift and tolerance")
{
    auto p = sinePatch();
    p[PID::Drift] = 1.0f;
    p[PID::Tolerance] = 1.0f;
    p[PID::FilterCutoff] = 20000.0f;

    auto pitchFor = [&] (OscType type)
    {
        p[PID::OscType] = (float) type;
        auto engine = makeEngine (p);
        engine.noteOn (69, 1.0f);
        const auto out = test::render (engine, 2.0, rate);
        return test::cents (test::estimateFrequency (out, rate, out.size() / 2), 440.0);
    };
    CHECK_THAT (pitchFor (OscType::Dco), WithinAbs (0.0, 0.5));
    CHECK (std::abs (pitchFor (OscType::Vco)) > 0.5);
}

TEST_CASE ("Sub-oscillator sits one or two octaves below osc 1")
{
    const auto octave = GENERATE (SubOctave::Down1, SubOctave::Down2);

    auto p = test::cleanPatch();
    p[PID::Osc1Level] = 0.0f;
    p[PID::SubLevel] = 0.5f;
    p[PID::SubOctave] = (float) octave;
    auto engine = makeEngine (p);
    engine.noteOn (69, 1.0f);
    const auto out = test::render (engine, 1.0, rate);

    const double expected = octave == SubOctave::Down1 ? 220.0 : 110.0;
    CHECK_THAT (test::cents (test::estimateFrequency (out, rate, out.size() / 2), expected), WithinAbs (0.0, 1.0));
}

TEST_CASE ("Cross-mod adds sidebands at osc 2 offsets")
{
    auto p = sinePatch();
    p[PID::Osc2Wave] = (float) Wave::Sine;
    p[PID::Osc2Tune] = 7.0f;   // inharmonic-ish ratio 1.498
    p[PID::CrossMod] = 0.3f;
    auto engine = makeEngine (p);
    engine.noteOn (57, 1.0f);
    const auto out = test::render (engine, 1.0, rate);

    // With FM, energy appears away from the harmonics of osc 1
    const double sideband = test::worstAliasDb (out, rate, 220.0, 5000.0);
    INFO ("strongest component off osc 1's harmonics: " << sideband << " dB");
    CHECK (sideband > -20.0);
    for (float s : out)
        REQUIRE (std::isfinite (s));
}

TEST_CASE ("Gate VCA opens and closes with the key, ignoring the amp envelope")
{
    auto p = sinePatch();
    p[PID::VcaMode] = (float) VcaMode::Gate;
    p[PID::AEnvA] = 5.0f;  // would take seconds in envelope mode
    p[PID::AEnvR] = 5.0f;
    auto engine = makeEngine (p);

    engine.noteOn (69, 1.0f);
    auto out = test::render (engine, 0.05, rate);
    CHECK (test::rms (out, out.size() / 2) > 0.5);

    engine.noteOff (69);
    out = test::render (engine, 0.05, rate);
    CHECK (test::rms (out, out.size() / 2) < 1.0e-3);
    CHECK (engine.numActiveVoices() == 0);
}

TEST_CASE ("Pan spread places poly voices across the stereo field")
{
    auto p = sinePatch();
    p[PID::VoiceMode] = (float) VoiceMode::Poly;
    p[PID::PanSpread] = 1.0f;
    auto engine = makeEngine (p);
    engine.noteOn (69, 1.0f); // first voice: hard left
    const auto out = test::renderStereo (engine, 0.2, rate);
    CHECK (test::rms (out.left, out.left.size() / 2) > 0.5);
    CHECK (test::rms (out.right, out.right.size() / 2) < 1.0e-3);
}

TEST_CASE ("Arpeggiator plays held notes in order through the engine")
{
    auto p = sinePatch();
    p[PID::VoiceMode] = (float) VoiceMode::Poly;
    p[PID::ArpOn] = 1.0f;
    p[PID::ArpMode] = (float) ArpMode::Up;
    p[PID::ArpRate] = (float) ArpRate::Sixteenth;
    p[PID::ArpTempo] = 120.0f; // 1/16 = 125 ms
    p[PID::ArpGate] = 0.5f;
    p[PID::AEnvA] = 0.001f;
    p[PID::AEnvR] = 0.002f;
    auto engine = makeEngine (p);

    engine.noteOn (69, 1.0f);
    engine.noteOn (57, 1.0f);
    engine.noteOn (64, 1.0f);
    const auto out = test::render (engine, 0.75, rate);

    // Each 125 ms step: measure the pitch in the middle of its gate
    const double expected[] { 220.0, 329.63, 440.0, 220.0, 329.63, 440.0 };
    for (int step = 0; step < 6; ++step)
    {
        const auto from = (size_t) ((step * 0.125 + 0.01) * rate);
        const std::vector<float> slice (out.begin() + (long) from, out.begin() + (long) from + 2400);
        INFO ("step " << step);
        CHECK_THAT (test::cents (test::estimateFrequency (slice, rate), expected[step]), WithinAbs (0.0, 5.0));

        // Gate 50%: (near) silent in the second half of each step. Not exactly zero: the DC
        // blocker leaves a faint tail after each short burst.
        const auto gap = (size_t) ((step * 0.125 + 0.1) * rate);
        CHECK (test::rms (out, gap, gap + 480) < 0.01 * test::rms (slice));
    }
}

TEST_CASE ("Ring modulator outputs sum and difference frequencies")
{
    auto p = test::cleanPatch();
    p[PID::Osc1Wave] = (float) Wave::Sine;
    p[PID::Osc1Level] = 0.0f;
    p[PID::Osc2Wave] = (float) Wave::Sine;
    p[PID::Osc2Tune] = 7.3f;      // osc 2 at ~671 Hz against osc 1 at 440 Hz (harmonics of the
                                  // difference tone, from the filter's input stage, stay clear of it)
    p[PID::RingMod] = 1.0f;
    auto engine = makeEngine (p);
    engine.noteOn (69, 1.0f);
    const auto out = test::render (engine, 1.0, rate);

    constexpr size_t n = 1 << 15;
    const auto mag = test::magnitudeSpectrum (out, out.size() - n, n);
    auto level = [&] (double hz)
    {
        double peak = 0.0;
        for (long i = std::lround (hz / (rate / n)) - 2; i <= std::lround (hz / (rate / n)) + 2; ++i)
            peak = std::max (peak, mag[(size_t) i]);
        return test::db (peak);
    };
    const double f2 = 440.0 * std::pow (2.0, 7.3 / 12.0);
    const double sum = level (440.0 + f2), difference = level (f2 - 440.0);
    INFO ("sum " << sum << " dB, difference " << difference << " dB, 440 Hz " << level (440.0) << " dB");
    CHECK (std::abs (sum - difference) < 1.0);
    CHECK (level (440.0) < sum - 40.0);
    CHECK (level (f2) < sum - 40.0);
}

TEST_CASE ("Voice high-pass removes a low note's fundamental")
{
    auto p = sinePatch();
    auto levelWith = [&] (float cutoff)
    {
        p[PID::VoiceHpfCutoff] = cutoff;
        auto engine = makeEngine (p);
        engine.noteOn (45, 1.0f); // 110 Hz
        const auto out = test::render (engine, 0.5, rate);
        return test::rms (out, out.size() / 2);
    };
    CHECK (test::db (levelWith (1000.0f) / levelWith (10.0f)) < -35.0);
}

TEST_CASE ("Duo mode: osc 1 takes the lowest key, osc 2 the highest")
{
    auto p = test::cleanPatch();
    p[PID::VoiceMode] = (float) VoiceMode::Duo;
    auto engine = makeEngine (p);

    engine.noteOn (67, 1.0f);
    CHECK (engine.getMonoNote() == 67);
    CHECK (engine.getVoice (0).getDuoOffset() == 0.0f);

    engine.noteOn (60, 1.0f);
    CHECK (engine.getMonoNote() == 60);
    CHECK (engine.getVoice (0).getDuoOffset() == 7.0f);

    engine.noteOn (72, 1.0f);
    CHECK (engine.getVoice (0).getDuoOffset() == 12.0f);

    engine.noteOff (72);
    engine.noteOff (67);
    CHECK (engine.getVoice (0).getDuoOffset() == 0.0f);
    CHECK (engine.getMonoNote() == 60);
}

namespace
{
LayeredSynth makeLayers (Patch a, const Patch& b, LayerMode mode)
{
    a[PID::LayerMode] = (float) mode;
    LayeredSynth synth;
    synth.setPatches (a, b);
    synth.prepare (rate);
    return synth;
}
} // namespace

TEST_CASE ("Layers: single, dual and split routing")
{
    auto poly = test::cleanPatch();
    poly[PID::VoiceMode] = (float) VoiceMode::Poly;

    SECTION ("Single plays only A")
    {
        auto synth = makeLayers (poly, poly, LayerMode::Single);
        synth.noteOn (60, 1.0f);
        CHECK (synth.layer (0).numActiveVoices() == 1);
        CHECK (synth.layer (1).numActiveVoices() == 0);
    }

    SECTION ("Dual plays both")
    {
        auto synth = makeLayers (poly, poly, LayerMode::Dual);
        synth.noteOn (60, 1.0f);
        CHECK (synth.layer (0).numActiveVoices() == 1);
        CHECK (synth.layer (1).numActiveVoices() == 1);
    }

    SECTION ("Split: B below the split point, A from it upwards; note-offs follow")
    {
        auto a = poly;
        a[PID::SplitNote] = 60.0f;
        auto synth = makeLayers (a, poly, LayerMode::Split);
        synth.noteOn (48, 1.0f);
        synth.noteOn (60, 1.0f);
        CHECK (synth.layer (0).getVoice (0).getNote() == 60);
        CHECK (synth.layer (1).getVoice (0).getNote() == 48);
        CHECK (synth.layer (0).numActiveVoices() == 1);
        CHECK (synth.layer (1).numActiveVoices() == 1);

        // Move the split while 48 is held: its note-off must still reach layer B
        a[PID::SplitNote] = 40.0f;
        synth.setPatches (a, poly);
        synth.noteOff (48);
        CHECK_FALSE (synth.layer (1).getVoice (0).isGateOn());
    }
}

TEST_CASE ("Layer B detune and balance")
{
    auto a = sinePatch();
    a[PID::LayerBalance] = 1.0f;   // B only
    a[PID::LayerDetune] = 50.0f;   // +50 cents on B
    auto synth = makeLayers (a, sinePatch(), LayerMode::Dual);
    synth.noteOn (69, 1.0f);

    std::vector<float> left (48000), right (48000);
    synth.process (left.data(), right.data(), 48000);
    CHECK_THAT (test::cents (test::estimateFrequency (left, rate, 24000), 440.0), WithinAbs (50.0, 1.0));
}

namespace
{
// Level (dB) of the strongest spectrum bin near hz
double levelNear (const std::vector<float>& x, double hz)
{
    constexpr size_t n = 1 << 15; // 1.5 Hz bins: a semitone at 220 Hz is ~9 bins, clear of the window's main lobe
    const auto mag = test::magnitudeSpectrum (x, x.size() - n, n);
    const double bin = rate / n;
    double peak = 0.0;
    for (long i = std::lround (hz / bin) - 2; i <= std::lround (hz / bin) + 2; ++i)
        peak = std::max (peak, mag[(size_t) i]);
    return test::db (peak);
}

Patch expressivePoly (MatrixSource source)
{
    auto p = sinePatch();
    p[PID::VoiceMode] = (float) VoiceMode::Poly;
    p[PID::Mx1Source] = (float) source;
    p[PID::Mx1Dest] = (float) MatrixDest::Osc1Pitch;
    p[PID::Mx1Amount] = 0.5f; // +6 st at full
    return p;
}
} // namespace

TEST_CASE ("Poly aftertouch bends only the pressed note")
{
    auto engine = makeEngine (expressivePoly (MatrixSource::Aftertouch));
    engine.noteOn (57, 1.0f);           // 220 Hz
    engine.noteOn (69, 1.0f);           // 440 Hz
    engine.polyPressure (69, 1.0f);     // -> +6 st = 622 Hz
    const auto out = test::render (engine, 1.0, rate);

    const double moved = 440.0 * std::sqrt (2.0);
    CHECK (levelNear (out, 220.0) > levelNear (out, 440.0) + 30.0); // 220 untouched
    CHECK (levelNear (out, moved) > levelNear (out, 440.0) + 30.0); // 440 moved to 622
    CHECK (levelNear (out, 220.0 * std::sqrt (2.0)) < levelNear (out, 220.0) - 30.0);
}

TEST_CASE ("MPE: member channels bend their own note; channel 1 stays global")
{
    auto p = sinePatch();
    p[PID::VoiceMode] = (float) VoiceMode::Poly;
    p[PID::MpeMode] = 1.0f;
    p[PID::MpeBendRange] = 12.0f;
    auto engine = makeEngine (p);

    engine.noteOn (57, 1.0f, 1);         // MIDI channel 2
    engine.noteOn (69, 1.0f, 2);         // MIDI channel 3
    engine.pitchBend (0.5f, 2);          // +6 st on channel 3 only
    const auto out = test::render (engine, 1.0, rate);

    CHECK (levelNear (out, 220.0) > levelNear (out, 440.0) + 30.0);
    CHECK (levelNear (out, 440.0 * std::sqrt (2.0)) > levelNear (out, 440.0) + 30.0);

    // Without MPE, the same channels share one global bend (2 st default range)
    p[PID::MpeMode] = 0.0f;
    auto plain = makeEngine (p);
    plain.noteOn (57, 1.0f, 1);
    plain.noteOn (69, 1.0f, 2);
    plain.pitchBend (0.5f, 2);           // +1 st on everything
    const auto both = test::render (plain, 1.0, rate);
    INFO ("220: " << levelNear (both, 220.0) << " 233: " << levelNear (both, 233.08) << " 440: " << levelNear (both, 440.0) << " 466: " << levelNear (both, 466.16));
    CHECK (levelNear (both, 220.0 * std::pow (2.0, 1.0 / 12.0)) > levelNear (both, 220.0) + 30.0);
    CHECK (levelNear (both, 440.0 * std::pow (2.0, 1.0 / 12.0)) > levelNear (both, 440.0) + 30.0);
}

TEST_CASE ("Timbre (CC74) is a matrix source, per note under MPE")
{
    auto p = expressivePoly (MatrixSource::Timbre);
    p[PID::MpeMode] = 1.0f;
    auto engine = makeEngine (p);
    engine.noteOn (57, 1.0f, 1);
    engine.noteOn (69, 1.0f, 2);
    engine.timbre (1.0f, 2);
    const auto out = test::render (engine, 1.0, rate);
    CHECK (levelNear (out, 220.0) > levelNear (out, 220.0 * std::sqrt (2.0)) + 30.0);
    CHECK (levelNear (out, 440.0 * std::sqrt (2.0)) > levelNear (out, 440.0) + 30.0);
}

namespace
{
// Pitch (cents from `reference`) over consecutive windows of a rendered mono signal
std::vector<double> pitchTrack (const std::vector<float>& x, double reference, double window, double from = 0.0, double to = 0.0)
{
    std::vector<double> track;
    const auto len = (size_t) (window * rate);
    const auto end = to > 0.0 ? (size_t) (to * rate) : x.size();
    for (auto start = (size_t) (from * rate); start + len <= end; start += len)
    {
        const std::vector<float> part (x.begin() + (long) start, x.begin() + (long) (start + len));
        track.push_back (test::cents (test::estimateFrequency (part, rate), reference));
    }
    return track;
}

double spread (const std::vector<double>& v)
{
    const auto [lo, hi] = std::minmax_element (v.begin(), v.end());
    return *hi - *lo;
}
} // namespace

TEST_CASE ("LFO delay fades vibrato in after each note")
{
    const bool perVoice = GENERATE (false, true);
    auto p = sinePatch();
    p[PID::Mx1Source] = (float) (perVoice ? MatrixSource::Lfo2 : MatrixSource::Lfo);
    p[PID::Mx1Dest] = (float) MatrixDest::Osc1Pitch;
    p[PID::Mx1Amount] = 0.2f; // 0.2^2 * 24 = +/- 0.96 st
    p[perVoice ? PID::Lfo2Rate : PID::LfoRate] = 5.0f;
    p[perVoice ? PID::Lfo2Wave : PID::LfoWave] = (float) LfoWave::Sine;
    p[perVoice ? PID::Lfo2Delay : PID::LfoDelay] = 1.0f;
    auto engine = makeEngine (p);

    for (int repeat = 0; repeat < 2; ++repeat)
    {
        INFO ((perVoice ? "LFO 2" : "LFO") << ", note " << repeat);
        engine.noteOn (69, 1.0f);
        const auto out = test::render (engine, 2.0, rate);
        engine.noteOff (69);
        test::render (engine, 0.5, rate);

        CHECK (spread (pitchTrack (out, 440.0, 0.025, 0.025, 0.1)) < 10.0); // barely moving yet (after the attack)
        CHECK (spread (pitchTrack (out, 440.0, 0.025, 1.2, 2.0)) > 150.0); // full +/- 96 cents
    }
}

TEST_CASE ("LFO 2 key sync restarts each voice's LFO on its note")
{
    const bool keySync = GENERATE (true, false);
    auto p = sinePatch();
    p[PID::VoiceMode] = (float) VoiceMode::Poly;
    p[PID::Mx1Source] = (float) MatrixSource::Lfo2;
    p[PID::Mx1Dest] = (float) MatrixDest::Osc1Pitch;
    p[PID::Mx1Amount] = 0.2f;
    p[PID::Lfo2Rate] = 3.0f;
    p[PID::Lfo2Wave] = (float) LfoWave::Square;
    p[PID::Lfo2KeySync] = keySync ? 1.0f : 0.0f;
    p[PID::KeyAssign] = (float) KeyAssign::Rotate; // a different voice each time
    p[PID::AEnvR] = 0.001f;

    std::vector<double> firstWindow;
    auto engine = makeEngine (p);
    for (int i = 0; i < 6; ++i)
    {
        engine.noteOn (69, 1.0f);
        const auto out = test::render (engine, 0.05, rate);
        engine.noteOff (69);
        test::render (engine, 0.137, rate); // not a whole number of LFO cycles
        firstWindow.push_back (pitchTrack (out, 440.0, 0.04, 0.005)[0]);
    }
    INFO ("key sync " << keySync);
    if (keySync)
        CHECK (spread (firstWindow) < 2.0);   // square starts high every time
    else
        CHECK (spread (firstWindow) > 100.0); // free-running voices: some notes start low
}

TEST_CASE ("Key assign modes pick voices like the Jupiter-8 and Juno")
{
    auto p = test::cleanPatch();
    p[PID::VoiceMode] = (float) VoiceMode::Poly;
    p[PID::AEnvR] = 2.0f;

    auto voiceOf = [] (const Engine& engine, int note)
    {
        for (int i = 0; i < Engine::maxVoices; ++i)
            if (engine.getVoice (i).isGateOn() && engine.getVoice (i).getNote() == note)
                return i;
        return -1;
    };

    SECTION ("Poly 1 keeps release tails: a new note goes to an untouched voice")
    {
        p[PID::KeyAssign] = (float) KeyAssign::Poly1;
        auto engine = makeEngine (p);
        engine.noteOn (60, 1.0f);
        engine.noteOff (60);
        engine.noteOn (62, 1.0f);
        CHECK (voiceOf (engine, 62) != 0);
        CHECK_FALSE (engine.getVoice (0).isFree()); // still releasing
    }

    SECTION ("Poly 2 always takes the lowest released voice")
    {
        p[PID::KeyAssign] = (float) KeyAssign::Poly2;
        auto engine = makeEngine (p);
        for (int n : { 60, 62, 64 })
        {
            engine.noteOn (n, 1.0f);
            CHECK (voiceOf (engine, n) == 0);
            engine.noteOff (n);
        }
        engine.noteOn (60, 1.0f);
        engine.noteOn (67, 1.0f);
        CHECK (voiceOf (engine, 67) == 1);
    }

    SECTION ("Rotate cycles through the voices even when earlier ones are free")
    {
        p[PID::KeyAssign] = (float) KeyAssign::Rotate;
        p[PID::AEnvR] = 0.001f;
        auto engine = makeEngine (p);
        for (int i = 0; i < Engine::maxVoices + 2; ++i)
        {
            engine.noteOn (60, 1.0f);
            CHECK (voiceOf (engine, 60) == i % Engine::maxVoices);
            engine.noteOff (60);
            test::render (engine, 0.05, rate);
            REQUIRE (engine.numActiveVoices() == 0);
        }

        // Held voices are skipped
        engine.noteOn (60, 1.0f);  // voice 2
        engine.noteOn (62, 1.0f);  // voice 3
        engine.noteOff (60);
        engine.noteOn (64, 1.0f);
        CHECK (voiceOf (engine, 64) == 4);
    }
}

TEST_CASE ("Velocity curves shape the played velocity")
{
    auto p = sinePatch();
    p[PID::AmpVel] = 1.0f; // amp follows velocity directly

    auto levelFor = [&] (VelocityCurve curve, float velocity)
    {
        p[PID::VelocityCurve] = (float) curve;
        auto engine = makeEngine (p);
        engine.noteOn (69, velocity);
        const auto out = test::render (engine, 0.3, rate);
        return test::rms (out, out.size() / 2);
    };

    const double full = levelFor (VelocityCurve::Linear, 1.0f);
    CHECK_THAT (levelFor (VelocityCurve::Linear, 0.5f) / full, WithinAbs (0.5, 0.01));
    CHECK_THAT (levelFor (VelocityCurve::Hard, 0.5f) / full, WithinAbs (0.25, 0.01));
    CHECK_THAT (levelFor (VelocityCurve::Soft, 0.25f) / full, WithinAbs (0.5, 0.01));
    CHECK_THAT (levelFor (VelocityCurve::SCurve, 0.5f) / full, WithinAbs (0.5, 0.01));
    CHECK (levelFor (VelocityCurve::SCurve, 0.25f) / full < 0.2);
    CHECK_THAT (levelFor (VelocityCurve::Fixed, 0.1f) / full, WithinAbs (1.0, 0.01));
}

TEST_CASE ("Tunings retune notes relative to the root")
{
    struct Case { Tuning tuning; int root; int note; double expectedHz; };
    const double c4 = 440.0 * std::exp2 (-9.0 / 12.0), d4 = 440.0 * std::exp2 (-7.0 / 12.0);
    const auto c = GENERATE_COPY (Case { Tuning::Equal, 0, 64, 440.0 * std::exp2 (-5.0 / 12.0) },
                                  Case { Tuning::Just, 0, 64, c4 * 5.0 / 4.0 },
                                  Case { Tuning::Just, 0, 67, c4 * 3.0 / 2.0 },
                                  Case { Tuning::Just, 2, 64, d4 * 9.0 / 8.0 },  // root D: E is a whole tone
                                  Case { Tuning::Just, 2, 62, d4 },              // the root itself stays put
                                  Case { Tuning::Pythagorean, 0, 76, 2.0 * c4 * 81.0 / 64.0 },
                                  Case { Tuning::Meantone, 0, 64, c4 * 1.25 });  // pure major third

    auto p = sinePatch();
    p[PID::Tuning] = (float) c.tuning;
    p[PID::TuningRoot] = (float) c.root;
    auto engine = makeEngine (p);
    engine.noteOn (c.note, 1.0f);
    const auto out = test::render (engine, 1.0, rate);
    INFO ("tuning " << (int) c.tuning << " root " << c.root << " note " << c.note);
    CHECK_THAT (test::cents (test::estimateFrequency (out, rate, out.size() / 2), c.expectedHz), WithinAbs (0.0, 0.5));
}

TEST_CASE ("HQ (4x) plays in tune with lower aliasing than 2x")
{
    auto p = test::cleanPatch();
    p[PID::FilterModel] = (float) FilterModel::Sem12; // flattest passband

    auto render = [&] (Quality quality, int note)
    {
        p[PID::Quality] = (float) quality;
        auto engine = makeEngine (p);
        CHECK (engine.getOversampling() == (quality == Quality::High ? 4 : 2));
        engine.noteOn (note, 1.0f);
        return test::render (engine, 1.5, rate);
    };

    const auto a4 = render (Quality::High, 69);
    CHECK_THAT (test::cents (test::estimateFrequency (a4, rate, a4.size() / 2), 440.0), WithinAbs (0.0, 1.0));

    const double hz = 440.0 * std::exp2 ((100 - 69) / 12.0); // E7
    const double normal = test::worstAliasDb (render (Quality::Normal, 100), rate, hz, 20000.0);
    const double high = test::worstAliasDb (render (Quality::High, 100), rate, hz, 20000.0);
    INFO ("worst alias 2x " << normal << " dB, 4x " << high << " dB");
    CHECK (high < normal - 15.0); // about 18 dB better for a saw
    CHECK (high < -75.0);
}

TEST_CASE ("Switching quality while playing restarts cleanly")
{
    auto p = test::cleanPatch();
    p[PID::VoiceMode] = (float) VoiceMode::Poly;
    auto engine = makeEngine (p);
    engine.noteOn (60, 1.0f);
    engine.noteOn (64, 1.0f);
    test::render (engine, 0.1, rate);

    p[PID::Quality] = (float) Quality::High;
    engine.setPatch (p);
    CHECK (engine.numActiveVoices() == 0);
    engine.noteOn (67, 1.0f);
    const auto out = test::render (engine, 0.5, rate);
    CHECK (test::rms (out, out.size() / 2) > 0.05);
    for (float x : out)
        REQUIRE (std::isfinite (x));
}

TEST_CASE ("Every factory preset sounds, stays finite and doesn't clip")
{
    for (const auto& entry : std::filesystem::directory_iterator (DSYNTH_PRESET_DIR))
    {
        if (entry.path().extension() != ".dsynth")
            continue;
        const auto preset = parsePreset (readFile (entry.path()));
        INFO (entry.path().filename().string());

        LayeredSynth synth;
        synth.setPatches (preset.patch, preset.layerB);
        synth.prepare (rate);
        for (int note : { 48, 55, 60, 64 })
            synth.noteOn (note, 0.8f);

        std::vector<float> left ((size_t) (0.6 * rate)), right (left.size());
        synth.process (left.data(), right.data(), (int) left.size());

        bool finite = true;
        float peak = 0.0f;
        for (size_t i = 0; i < left.size(); ++i)
        {
            finite = finite && std::isfinite (left[i]) && std::isfinite (right[i]);
            peak = std::max ({ peak, std::abs (left[i]), std::abs (right[i]) });
        }
        CHECK (finite);
        CHECK (peak < 1.0f);
        CHECK (test::db (test::rms (left, left.size() / 2) + test::rms (right, right.size() / 2)) > -50.0);
    }
}
