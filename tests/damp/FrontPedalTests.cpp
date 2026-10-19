#include "common/Analysis.h"
#include "damp/PedalHelpers.h"

#include "common/dsp/Noise.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

using namespace damp;
using namespace damp::pedaltest;

namespace
{
// A plucked string stand-in: decaying harmonics, the upper ones faster. `second` sets the 2nd
// harmonic (which can be stronger than the fundamental on a real string)
std::vector<float> pluck (double f, double seconds, double rate, double peak = 0.25, double second = 0.5)
{
    std::vector<float> x ((size_t) (seconds * rate));
    for (size_t i = 0; i < x.size(); ++i)
    {
        const double t = (double) i / rate;
        double v = 0.0;
        for (int k = 1; k <= 12; ++k)
            v += (k == 2 ? second : 1.0 / k) * std::sin (2.0 * std::numbers::pi * f * k * t + 0.7 * k * k)
                 * std::exp (-t * (1.5 + 0.4 * k));
        x[i] = (float) (0.5 * peak * v);
    }
    return x;
}

// RMS level of x over [from, to) seconds, dB
double windowDb (const std::vector<float>& x, double rate, double from, double to)
{
    return test::db (test::rms (x, (size_t) (from * rate), (size_t) (to * rate)));
}

// Level of a quiet tone through the wah, dB
double wahGain (double hz, double rate, std::initializer_list<std::pair<PID, float>> settings, double amplitude = 1.0e-3)
{
    const auto y = runPedal (PedalId::Wah, sine (hz, 0.1, amplitude, rate), settings, rate);
    return test::toneLevelDb (y, rate, hz, y.size() / 2, y.size() / 2 - 1) - test::db (amplitude);
}

// The wah's resonance: the loudest of a grid of tones
double wahPeakHz (double rate, float position, float peak = 5.0f)
{
    double best = -1.0e9, bestHz = 0.0;
    for (double hz = 200.0; hz < 4000.0; hz *= 1.03)
        if (const double g = wahGain (hz, rate, { { PID::WahPosition, position }, { PID::WahPeak, peak } }); g > best)
        {
            best = g;
            bestHz = hz;
        }
    return bestHz;
}

double largestStep (const std::vector<float>& y, size_t from, size_t to)
{
    double m = 0.0;
    for (size_t i = from + 1; i < to; ++i)
        m = std::max (m, (double) std::abs (y[i] - y[i - 1]));
    return m;
}
} // namespace

// ---- Compressor ----

TEST_CASE ("compressor evens out a plucked note, more with more sustain", "[pedals][compressor]")
{
    const auto x = pluck (110.0, 2.5, highRate);
    // How far the note falls from its start to two seconds later
    auto range = [&] (const std::vector<float>& y) { return windowDb (y, highRate, 0.02, 0.07) - windowDb (y, highRate, 2.2, 2.25); };
    const double dry = range (x);
    const double low = range (runPedal (PedalId::Compressor, x, { { PID::CompressorSustain, 0.0f } }));
    const double mid = range (runPedal (PedalId::Compressor, x));
    const double high = range (runPedal (PedalId::Compressor, x, { { PID::CompressorSustain, 10.0f } }));
    INFO ("dry " << dry << " sustain 0 " << low << " 5 " << mid << " 10 " << high);
    CHECK (dry > 25.0);
    CHECK (low < dry - 2.0);
    CHECK (mid < low - 5.0);
    CHECK (high < mid - 5.0);
    CHECK (high < 10.0);
}

TEST_CASE ("compressor narrows the gap between loud and quiet playing", "[pedals][compressor]")
{
    auto gap = [] (float sustain)
    {
        auto level = [&] (double amplitude)
        {
            const auto y = runPedal (PedalId::Compressor, sine (220.0, 0.5, amplitude, highRate), { { PID::CompressorSustain, sustain } });
            return test::toneLevelDb (y, highRate, 220.0, y.size() / 2, 32768);
        };
        return level (0.3) - level (0.03); // 20 dB apart going in
    };
    const double low = gap (0.0f), high = gap (10.0f);
    CHECK (low < 15.0);
    CHECK (high < 3.0);
    CHECK (high < low - 8.0);
}

TEST_CASE ("compressor attack: a slow one lets the pluck through", "[pedals][compressor]")
{
    const auto x = pluck (110.0, 0.5, highRate);
    // The pick's peak over the level a little later
    auto overshoot = [&] (float attack)
    {
        const auto y = runPedal (PedalId::Compressor, x, { { PID::CompressorAttack, attack } });
        return windowDb (y, highRate, 0.0, 0.01) - windowDb (y, highRate, 0.2, 0.25);
    };
    CHECK (overshoot (10.0f) > overshoot (0.0f) + 3.0);
}

TEST_CASE ("compressor blend at 0 passes the dry signal, scaled by Level", "[pedals][compressor]")
{
    const auto x = pluck (196.0, 0.3, highRate);
    const auto y = runPedal (PedalId::Compressor, x, { { PID::CompressorBlend, 0.0f }, { PID::CompressorLevel, 6.0f } });
    const float gain = std::pow (10.0f, 6.0f / 20.0f);
    for (size_t i = 0; i < x.size(); ++i)
        REQUIRE (std::abs (y[i] - gain * x[i]) < 1.0e-5f);
    // Half way, late in the note: between the dry decay and the fully compressed one
    const auto note = pluck (196.0, 1.5, highRate);
    const auto half = runPedal (PedalId::Compressor, note, { { PID::CompressorBlend, 5.0f }, { PID::CompressorSustain, 10.0f } });
    const auto full = runPedal (PedalId::Compressor, note, { { PID::CompressorSustain, 10.0f } });
    const double late = windowDb (half, highRate, 1.2, 1.25);
    CHECK (late > windowDb (note, highRate, 1.2, 1.25) + 3.0);
    CHECK (late < windowDb (full, highRate, 1.2, 1.25) - 3.0);
}

// ---- Wah ----

TEST_CASE ("wah resonance sweeps up from heel to toe", "[pedals][wah]")
{
    for (double rate : { 48000.0, highRate })
    {
        INFO ("rate " << rate);
        const double heel = wahPeakHz (rate, 0.0f), mid = wahPeakHz (rate, 5.0f), toe = wahPeakHz (rate, 10.0f);
        CHECK (heel > 300.0);
        CHECK (heel < 420.0);
        CHECK (mid > 700.0);
        CHECK (mid < 1100.0);
        CHECK (toe > 1900.0);
        CHECK (toe < 2600.0);
    }
    // A resonant peak with a low shelf below it and the treble falling away above, not a band-pass
    const std::initializer_list<std::pair<PID, float>> noon { { PID::WahPosition, 5.0f } };
    const double peak = wahGain (870.0, highRate, noon);
    CHECK (wahGain (100.0, highRate, noon) < peak - 12.0);
    CHECK (wahGain (100.0, highRate, noon) > peak - 25.0);
    CHECK (wahGain (5000.0, highRate, noon) < peak - 20.0);
    // The Peak knob sharpens it
    CHECK (wahGain (100.0, highRate, { { PID::WahPosition, 5.0f }, { PID::WahPeak, 10.0f } })
           < wahGain (100.0, highRate, { { PID::WahPosition, 5.0f }, { PID::WahPeak, 0.0f } }) - 10.0);
}

TEST_CASE ("wah auto mode sweeps up with the playing level", "[pedals][wah]")
{
    // Gain at 1.5 kHz, above the heel's resonance: a loud signal opens the filter towards it
    auto gain = [] (WahMode mode, double amplitude)
    {
        return wahGain (1500.0, highRate,
                        { { PID::WahMode, (float) mode }, { PID::WahPosition, 0.0f }, { PID::WahSensitivity, 7.0f } }, amplitude);
    };
    CHECK (gain (WahMode::Auto, 0.3) > gain (WahMode::Auto, 0.001) + 10.0);
    CHECK (std::abs (gain (WahMode::Pedal, 0.3) - gain (WahMode::Pedal, 0.001)) < 0.1); // linear in Pedal mode
    // No sensitivity, no sweep
    const auto still = [] (double amplitude)
    {
        return wahGain (1500.0, highRate,
                        { { PID::WahMode, (float) WahMode::Auto }, { PID::WahPosition, 0.0f }, { PID::WahSensitivity, 0.0f } }, amplitude);
    };
    CHECK (std::abs (still (0.3) - still (0.001)) < 0.1);
}

TEST_CASE ("wah pedal moves without clicks", "[pedals][wah]")
{
    for (double rate : { 48000.0, highRate })
    {
        INFO ("rate " << rate);
        const auto x = sine (330.0, 0.6, 0.25, rate);
        auto run = [&] (auto position)
        {
            auto patch = Patch::defaults();
            pedals::Wah wah;
            wah.prepare (rate);
            auto y = x;
            for (size_t pos = 0; pos < y.size(); pos += 64)
            {
                const int n = (int) std::min<size_t> (64, y.size() - pos);
                patch[PID::WahPosition] = position ((double) pos / rate);
                wah.setPatch (patch, rate, n);
                wah.process (0, y.data() + pos, n);
            }
            return y;
        };
        // Rocked heel to toe and back in 30 ms each way, and slammed in a single block
        const auto rocked = run ([] (double t) { return (float) std::clamp (t < 0.3 ? (t - 0.2) / 0.03 : (0.33 - t) / 0.03, 0.0, 1.0) * 10.0f; });
        const auto slammed = run ([] (double t) { return t < 0.3 ? 0.0f : 10.0f; });
        const auto start = (size_t) (0.1 * rate), end = x.size();
        const double steady = std::max (largestStep (run ([] (double) { return 0.0f; }), start, end),
                                        largestStep (run ([] (double) { return 10.0f; }), start, end));
        CHECK (largestStep (rocked, start, end) < 1.3 * steady);
        CHECK (largestStep (slammed, start, end) < 1.3 * steady);
    }
}

// ---- Octave ----

TEST_CASE ("octave tracks single notes across the guitar's range", "[pedals][octave]")
{
    auto check = [] (double f, double rate, double second)
    {
        INFO ("f " << f << " rate " << rate << " 2nd harmonic " << second);
        const auto x = pluck (f, 0.6, rate, 0.25, second);
        const auto one = runPedal (PedalId::Octave, x, { { PID::OctaveDirect, 0.0f }, { PID::OctaveOct1, 7.0f } }, rate);
        const auto two = runPedal (PedalId::Octave, x, { { PID::OctaveDirect, 0.0f }, { PID::OctaveOct1, 0.0f }, { PID::OctaveOct2, 7.0f } }, rate);
        const auto from = (size_t) (0.1 * rate), n = (size_t) (0.4 * rate);
        auto level = [&] (const std::vector<float>& y, double hz) { return test::toneLevelDb (y, rate, hz, from, n); };
        const double input = level (x, f);
        // One down: f/2 near the input's level, nothing at the subharmonics a mistracking would leave
        CHECK (level (one, f / 2) > input - 6.0);
        CHECK (level (one, f / 2) > level (one, f / 4) + 30.0);
        CHECK (level (one, f / 2) > level (one, 0.75 * f) + 30.0);
        CHECK (level (one, f / 2) > level (one, f) + 30.0);
        // Two down: f/4 at the root
        CHECK (level (two, f / 4) > input - 6.0);
        CHECK (level (two, f / 4) > level (two, 0.75 * f) + 4.0);
        CHECK (level (two, f / 4) > level (two, f / 8) + 30.0);
        CHECK (level (two, f / 4) > level (two, f / 2) + 30.0);
    };
    for (double f : { 82.41, 110.0, 146.83, 196.0, 246.94, 329.63, 440.0, 659.26 })
        check (f, highRate, 0.5);
    check (82.41, highRate, 1.5); // a 2nd harmonic stronger than the fundamental
    check (196.0, highRate, 1.5);
    check (82.41, 1536000.0, 0.5); // 192 kHz x 8
    check (659.26, 1536000.0, 0.5);
}

TEST_CASE ("octave: the sub follows the playing, direct alone is the dry guitar", "[pedals][octave]")
{
    const auto x = pluck (110.0, 1.5, highRate);
    const auto sub = runPedal (PedalId::Octave, x, { { PID::OctaveDirect, 0.0f } });
    auto level = [&] (const std::vector<float>& y, double hz, double t) { return test::toneLevelDb (y, highRate, hz, (size_t) (t * highRate), 32768); };
    const double inputFall = level (x, 110.0, 0.1) - level (x, 110.0, 1.2);
    const double subFall = level (sub, 55.0, 0.1) - level (sub, 55.0, 1.2);
    CHECK (std::abs (subFall - inputFall) < 6.0);

    // Silence in, silence out
    const auto quiet = runPedal (PedalId::Octave, std::vector<float> (20000, 0.0f), { { PID::OctaveOct2, 10.0f } });
    CHECK (std::all_of (quiet.begin(), quiet.end(), [] (float v) { return v == 0.0f; }));

    // Direct only: the input, scaled
    const auto direct = runPedal (PedalId::Octave, x, { { PID::OctaveDirect, 7.0f }, { PID::OctaveOct1, 0.0f } });
    const double gain = test::rms (direct) / test::rms (x);
    CHECK (std::abs (test::db (gain)) < 4.0);
    for (size_t i = 0; i < x.size(); ++i)
        REQUIRE (std::abs (direct[i] - (float) gain * x[i]) < 1.0e-5f);
}

// ---- All three ----

TEST_CASE ("compressor and octave sit near the bypassed loudness", "[pedals][compressor][octave]")
{
    // A few plucked notes in a row. Loudness is K-weighted (BS.1770's filters, ungated), as the
    // trims were set by integrated loudness through the amp. (The wah's trim isn't checked here:
    // its loudness depends on how the guitar's spectrum meets its peak and then the amp's voicing,
    // which a synthetic pluck straight into a meter doesn't reproduce.)
    constexpr double rate = 48000.0;
    std::vector<float> x;
    for (double f : { 82.41, 196.0, 329.63, 146.83 })
    {
        const auto note = pluck (f, 0.8, rate);
        x.insert (x.end(), note.begin(), note.end());
    }
    auto loudness = [] (std::vector<float> y)
    {
        Biquad shelf, highpass;
        shelf.setHighShelf (1681.97, 4.0, rate);
        highpass.setHighpass (38.13, 0.5, rate);
        for (auto& v : y)
            v = highpass.process (shelf.process (v));
        return test::db (test::rms (y));
    };
    auto relative = [&] (PedalId id, std::initializer_list<std::pair<PID, float>> settings = {})
    {
        return loudness (runPedal (id, x, settings, rate)) - loudness (x);
    };
    CHECK (std::abs (relative (PedalId::Compressor)) < 2.0);
    CHECK (std::abs (relative (PedalId::Octave, { { PID::OctaveOct1, 0.0f } })) < 0.5); // direct at 7: unity
    CHECK (relative (PedalId::Octave) > 0.0); // the sub adds a little
    CHECK (relative (PedalId::Octave) < 4.0);
    // The compressor's makeup keeps it there across the Sustain knob
    for (float sustain : { 0.0f, 10.0f })
    {
        INFO ("sustain " << sustain);
        CHECK (std::abs (relative (PedalId::Compressor, { { PID::CompressorSustain, sustain } })) < 2.0);
    }
}

TEST_CASE ("front pedals stay finite and DC-free under abuse at any rate", "[pedals][compressor][wah][octave]")
{
    dustify::Random rng (7);
    auto unit = [&] { return 0.5f * (rng.bipolar() + 1.0f); };
    for (auto id : { PedalId::Compressor, PedalId::Wah, PedalId::Octave })
        for (double rate : { 44100.0, highRate, 1536000.0 })
        {
            INFO (pedalInfo (id).name << " at " << rate);
            auto pedal = makePedal (id);
            pedal->prepare (rate);
            auto patch = Patch::defaults();
            patch[pedalInfo (id).onParam] = 1.0f;
            std::vector<float> x (512);
            for (int block = 0; block < 300; ++block)
            {
                if (block % 10 == 0)
                    for (auto pid : pedalInfo (id).params)
                    {
                        const auto& info = paramInfo (pid);
                        patch[pid] = info.min + unit() * (info.max - info.min);
                    }
                for (size_t i = 0; i < x.size(); ++i)
                    x[i] = block % 3 == 0 ? 2.0f * rng.bipolar() : block % 3 == 1 ? (float) std::sin (0.05 * (double) i) : 0.0f;
                pedal->setPatch (patch, rate, (int) x.size());
                pedal->process (block % 2, x.data(), (int) x.size());
                for (float s : x)
                {
                    REQUIRE (std::isfinite (s));
                    REQUIRE (std::abs (s) < 50.0f);
                }
            }
            // Every knob up on a steady low note: no DC (averaged over whole periods of f/4)
            auto settings = Patch::defaults();
            for (auto pid : pedalInfo (id).params)
                settings[pid] = paramInfo (pid).max;
            auto y = sine (82.0, 3.0, 0.5, rate);
            pedal->reset();
            for (size_t pos = 0; pos < y.size(); pos += 4096)
            {
                const int n = (int) std::min<size_t> (4096, y.size() - pos);
                pedal->setPatch (settings, rate, n);
                pedal->process (0, y.data() + pos, n);
            }
            double sum = 0.0;
            const auto from = (size_t) rate;
            for (size_t i = from; i < y.size(); ++i)
                sum += y[i];
            CHECK (std::abs (sum / (double) (y.size() - from)) < 2.0e-3);
        }
}
