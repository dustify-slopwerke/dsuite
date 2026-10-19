#include "common/Analysis.h"
#include "damp/PedalHelpers.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <functional>
#include <numbers>
#include <random>
#include <utility>

using namespace damp;
using namespace damp::pedaltest;

namespace
{
using Settings = std::initializer_list<std::pair<PID, float>>;
constexpr double hostRate = 48000.0;
constexpr double oversampledRate = 1536000.0; // 192 kHz x 8, the highest the amp runs at

// The pedal makes up the loudness a shift loses through an amp: this much louder than the input
double makeUpDb (double semis) { return semis > 0.0 ? 2.5 * semis / 12.0 : -3.0 * semis / 12.0; }

Patch pitchPatch (Settings settings)
{
    auto patch = Patch::defaults();
    patch[PID::PitchOn] = 1.0f;
    for (auto [pid, v] : settings)
        patch[pid] = v;
    return patch;
}

// The pedal on its own, mono, in host-sized blocks (more samples a block when oversampled);
// `change` may move the patch before each block, given the block's start in seconds
std::vector<float> runPitch (Patch patch, std::vector<float> x, double rate = hostRate,
                             const std::function<void (Patch&, double)>& change = {})
{
    pedals::Pitch pitch;
    pitch.prepare (rate);
    const size_t block = (size_t) std::clamp (256.0 * rate / hostRate, 64.0, 8192.0);
    for (size_t pos = 0; pos < x.size(); pos += block)
    {
        const int n = (int) std::min (block, x.size() - pos);
        if (change)
            change (patch, (double) pos / rate);
        pitch.setPatch (patch, rate, n);
        pitch.process (0, x.data() + pos, n);
    }
    return x;
}

// A plucked string: harmonics rolling off, each decaying a bit faster than the one below
std::vector<float> pluck (double f, double seconds, double rate, double amplitude = 0.3, double decay = 0.8)
{
    std::vector<float> x ((size_t) (seconds * rate));
    for (int h = 1; h <= 8; ++h)
    {
        if (f * h > 0.4 * rate)
            break;
        const double a = amplitude / h, tau = decay / (1.0 + 0.3 * (h - 1));
        const double w = 2.0 * std::numbers::pi * f * h / rate;
        for (size_t i = 0; i < x.size(); ++i)
            x[i] += (float) (a * std::exp (-(double) i / (tau * rate)) * std::sin (w * (double) i + 0.7 * h));
    }
    return x;
}

std::vector<float> slice (const std::vector<float>& x, double from, double to, double rate)
{
    return { x.begin() + (long) (from * rate), x.begin() + (long) std::min ((double) x.size(), to * rate) };
}

// Pitch of the output from `from` seconds on, in cents relative to f
double centsOff (const std::vector<float>& y, double f, double rate, double from = 0.2)
{
    return test::cents (test::estimateFrequency (y, rate, (size_t) (from * rate)), f);
}

// Normalised correlation of y against x delayed by lag, over [from, to)
double correlation (const std::vector<float>& x, const std::vector<float>& y, size_t lag, size_t from, size_t to)
{
    double xy = 0.0, xx = 0.0, yy = 0.0;
    for (size_t i = from; i < to; ++i)
    {
        xy += (double) x[i - lag] * y[i];
        xx += (double) x[i - lag] * x[i - lag];
        yy += (double) y[i] * y[i];
    }
    return xy / std::sqrt (xx * yy);
}

// The smallest lag at which y is x delayed (a periodic x matches at whole periods on from it too)
size_t delayOf (const std::vector<float>& x, const std::vector<float>& y, size_t maxLag, size_t from, size_t to,
                double threshold = 0.9999)
{
    for (size_t lag = 0; lag <= maxLag; ++lag)
        if (correlation (x, y, lag, from, to) > threshold)
            return lag;
    return maxLag + 1;
}

// RMS level (dB) of each `seconds`-long frame of x[from .. to)
std::vector<double> frameLevels (const std::vector<float>& x, double from, double to, double seconds, double rate)
{
    std::vector<double> out;
    const auto length = (size_t) (seconds * rate);
    for (auto i = (size_t) (from * rate); i + length <= (size_t) (to * rate); i += length)
        out.push_back (test::db (test::rms (x, i, i + length)));
    return out;
}

double largestStep (const std::vector<float>& y, size_t from)
{
    double worst = 0.0;
    for (size_t i = std::max<size_t> (from, 1); i < y.size(); ++i)
        worst = std::max (worst, (double) std::abs (y[i] - y[i - 1]));
    return worst;
}
} // namespace

TEST_CASE ("pitch shifter: the output's pitch is the input's moved by the shift", "[pedals][pitch]")
{
    for (const double rate : { hostRate, oversampledRate })
    {
        const auto freqs = rate == hostRate ? std::vector<double> { 82.4, 196.0, 440.0, 1000.0 } : std::vector<double> { 110.0, 660.0 };
        for (const double f : freqs)
        {
            const auto x = sine (f, rate == hostRate ? 1.0 : 0.6, 0.3, rate);
            for (const float shift : { -24.0f, -12.0f, -5.0f, 7.0f, 12.0f, 24.0f })
            {
                const auto y = runPitch (pitchPatch ({ { PID::PitchShift, shift } }), x, rate);
                INFO ("rate " << rate << ", " << f << " Hz, shift " << shift);
                CHECK (std::abs (centsOff (y, f, rate) - 100.0 * shift) < 0.5);
                // At full level: the splices neither dip nor bump
                const double level = test::db (test::rms (y, (size_t) (0.2 * rate)) / test::rms (x, (size_t) (0.2 * rate)));
                // (less the gentle high-cut on up-shifts: a one-pole at 16 kHz an octave up)
                const double ratio = std::exp2 (shift / 12.0), out = f * ratio;
                const double highCutDb = ratio > 1.0 ? -10.0 * std::log10 (1.0 + std::pow (out * std::sqrt (ratio - 1.0) / 16000.0, 2.0)) : 0.0;
                CHECK (std::abs (level - makeUpDb (shift) - highCutDb) < 0.3);
            }
        }
    }
}

TEST_CASE ("pitch shifter: Steps locks the shift to half steps", "[pedals][pitch]")
{
    const auto x = sine (220.0, 1.0, 0.3, hostRate);
    const auto steps = (float) PitchMode::Steps;
    const auto knob = runPitch (pitchPatch ({ { PID::PitchMode, steps }, { PID::PitchShift, 7.4f } }), x);
    CHECK (std::abs (centsOff (knob, 220.0, hostRate) - 700.0) < 1.0);
    const auto pedal = runPitch (pitchPatch ({ { PID::PitchMode, steps }, { PID::PitchShift, 12.0f }, { PID::PitchPosition, 5.0f } }), x);
    CHECK (std::abs (centsOff (pedal, 220.0, hostRate) - 600.0) < 1.0);
    const auto between = runPitch (pitchPatch ({ { PID::PitchMode, steps }, { PID::PitchShift, -12.0f }, { PID::PitchPosition, 3.0f } }), x);
    CHECK (std::abs (centsOff (between, 220.0, hostRate) + 400.0) < 1.0); // -3.6 st
}

TEST_CASE ("pitch shifter: Free is continuous, to the cent", "[pedals][pitch]")
{
    const auto free = (float) PitchMode::Free;
    for (const double rate : { hostRate, oversampledRate })
    {
        const auto x = sine (196.0, rate == hostRate ? 1.0 : 0.6, 0.3, rate);
        for (const auto [shift, position, cents] : { std::tuple { 0.5f, 10.0f, 50.0 }, std::tuple { -0.3f, 10.0f, -30.0 },
                                                     std::tuple { 7.4f, 10.0f, 740.0 }, std::tuple { 12.0f, 4.3f, 516.0 } })
        {
            const auto patch = pitchPatch ({ { PID::PitchMode, free }, { PID::PitchShift, shift }, { PID::PitchPosition, position } });
            const auto y = runPitch (patch, x, rate);
            INFO ("rate " << rate << ", shift " << shift << ", position " << position);
            CHECK (std::abs (centsOff (y, 196.0, rate) - cents) < 2.0);
        }
    }
}

TEST_CASE ("pitch shifter: unison is the dry guitar, a few samples late", "[pedals][pitch]")
{
    for (const double rate : { hostRate, oversampledRate })
    {
        const auto x = pluck (110.0, 0.5, rate);
        for (const auto mode : { PitchMode::Steps, PitchMode::Free })
        {
            INFO ((mode == PitchMode::Steps ? "steps" : "free") << ", rate " << rate);
            const auto y = runPitch (pitchPatch ({ { PID::PitchMode, (float) mode }, { PID::PitchPosition, 0.0f } }), x, rate);
            // A plain delay (no comb, no warble) of a few samples
            CHECK (delayOf (x, y, 8, (size_t) (0.1 * rate), (size_t) (0.4 * rate)) <= 4);
        }
    }

    // After a shift it closes the delay back up on a steady note, as far as a whole period
    // allows; either way it's a plain delay again
    for (const double f : { 110.0, 330.0 })
    {
        const auto note = pluck (f, 1.0, hostRate);
        const auto y = runPitch (pitchPatch ({}), note, hostRate, [] (Patch& p, double t) { p[PID::PitchPosition] = t < 0.15 ? 10.0f : 0.0f; });
        const auto lag = delayOf (note, y, 2000, (size_t) (0.6 * hostRate), (size_t) (0.95 * hostRate));
        INFO (f << " Hz: " << lag << " samples late");
        CHECK ((double) lag < std::max (8.0, hostRate / f));
    }
}

TEST_CASE ("pitch shifter: mix 0 is the dry signal, 5 a harmony", "[pedals][pitch]")
{
    const auto x = pluck (147.0, 0.5, hostRate);
    CHECK (runPitch (pitchPatch ({ { PID::PitchMix, 0.0f } }), x) == x);

    // An octave up at 5: the dry note and the shifted one, each 3 dB down
    const auto s = sine (220.0, 0.6, 0.3, hostRate);
    const auto y = runPitch (pitchPatch ({ { PID::PitchMix, 5.0f } }), s);
    const auto from = (size_t) (0.2 * hostRate);
    const double input = test::toneLevelDb (s, hostRate, 220.0, from, 16384);
    CHECK (std::abs (test::toneLevelDb (y, hostRate, 220.0, from, 16384) - input + 3.0) < 0.3);
    CHECK (std::abs (test::toneLevelDb (y, hostRate, 440.0, from, 16384) - input + 3.0 - makeUpDb (12.0)) < 0.5);
}

TEST_CASE ("pitch shifter: rocking the pedal doesn't click", "[pedals][pitch]")
{
    // Heel to toe and back in 150 ms, five times, at +12 and -12
    for (const auto mode : { PitchMode::Steps, PitchMode::Free })
        for (const float shift : { 12.0f, -12.0f, 24.0f })
            for (const double rate : { hostRate, oversampledRate })
            {
                const double f = 220.0, amplitude = 0.3;
                const auto x = sine (f, 0.8, amplitude, rate);
                const auto y = runPitch (pitchPatch ({ { PID::PitchMode, (float) mode }, { PID::PitchShift, shift } }), x, rate,
                                         [] (Patch& p, double t) {
                                             const double phase = std::fmod (t / 0.15, 1.0);
                                             p[PID::PitchPosition] = (float) (10.0 * (1.0 - std::abs (2.0 * phase - 1.0)));
                                         });
                // The steepest a sine at the highest pitch reached can step, with some room for
                // the splices
                const double steepest = amplitude * std::pow (10.0, makeUpDb (shift) / 20.0) * 2.0 * std::numbers::pi * f
                                        * std::exp2 (std::max (shift, 0.0f) / 12.0) / rate;
                INFO ((mode == PitchMode::Steps ? "steps" : "free") << ", shift " << shift << ", rate " << rate);
                CHECK (largestStep (y, (size_t) (0.05 * rate)) < 1.25 * steepest);
            }
}

TEST_CASE ("pitch shifter: a plucked note keeps its envelope", "[pedals][pitch]")
{
    for (const double f : { 82.4, 196.0, 330.0 })
        for (const float shift : { -12.0f, -5.0f, 7.0f, 12.0f })
        {
            const auto x = pluck (f, 1.2, hostRate);
            const auto y = runPitch (pitchPatch ({ { PID::PitchShift, shift } }), x);
            // Frames of a few periods of the lower of the two pitches
            const double frame = std::max (0.04, 4.0 / (f * std::exp2 (std::min (shift, 0.0f) / 12.0)));
            const auto in = frameLevels (x, 0.1, 1.1, frame, hostRate), out = frameLevels (y, 0.1, 1.1, frame, hostRate);
            // The level against the input's, frame by frame: smooth, with no dips at the splices
            double lo = 100.0, hi = -100.0;
            for (size_t i = 0; i < in.size(); ++i)
            {
                lo = std::min (lo, out[i] - in[i]);
                hi = std::max (hi, out[i] - in[i]);
            }
            INFO (f << " Hz, shift " << shift << ": " << lo << " .. " << hi << " dB");
            CHECK (hi - lo < 0.75);
            CHECK (std::abs (0.5 * (hi + lo) - makeUpDb (shift)) < 1.0);
        }
}

TEST_CASE ("pitch shifter: chords come through at about their level", "[pedals][pitch]")
{
    // A fifth and a major third
    for (const auto [a, b] : { std::pair { 110.0, 164.8 }, std::pair { 196.0, 246.9 } })
    {
        auto x = pluck (a, 1.0, hostRate, 0.2);
        const auto other = pluck (b, 1.0, hostRate, 0.2);
        for (size_t i = 0; i < x.size(); ++i)
            x[i] += other[i];
        for (const float shift : { -12.0f, 7.0f, 12.0f, 24.0f })
        {
            const auto y = runPitch (pitchPatch ({ { PID::PitchShift, shift } }), x);
            INFO (a << " + " << b << " Hz, shift " << shift);
            REQUIRE (std::all_of (y.begin(), y.end(), [] (float v) { return std::isfinite (v); }));
            const double level = test::db (test::rms (y, 4800, 43200) / test::rms (x, 4800, 43200));
            CHECK (std::abs (level - makeUpDb (shift)) < 1.5);
            // Warbling a little where no splice point fits both notes, but not breaking up
            const auto in = frameLevels (x, 0.1, 0.9, 0.05, hostRate), out = frameLevels (y, 0.1, 0.9, 0.05, hostRate);
            double lo = 100.0, hi = -100.0;
            for (size_t i = 0; i < in.size(); ++i)
            {
                lo = std::min (lo, out[i] - in[i]);
                hi = std::max (hi, out[i] - in[i]);
            }
            CHECK (hi - lo < 3.0);
        }
    }
}

TEST_CASE ("pitch shifter: random settings stay finite and DC-free", "[pedals][pitch]")
{
    for (const double rate : { hostRate, oversampledRate })
    {
        std::mt19937 rng (7);
        std::uniform_real_distribution<float> u (0.0f, 1.0f);
        // A pluck every 0.2 s over some noise
        const double seconds = rate == hostRate ? 3.0 : 0.8;
        std::vector<float> x ((size_t) ((seconds + 0.3) * rate), 0.0f); // then 0.3 s of silence
        for (size_t i = 0; i < (size_t) (seconds * rate); ++i)
            x[i] = 0.02f * (u (rng) - 0.5f);
        for (double t = 0.0; t + 0.4 < seconds; t += 0.2)
        {
            const auto p = pluck (70.0 + 900.0 * u (rng), 0.4, rate, 0.4, 0.2);
            double mean = 0.0; // each pluck with no DC of its own
            for (const float v : p)
                mean += v / (double) p.size();
            for (size_t i = 0; i < p.size(); ++i)
                x[(size_t) (t * rate) + i] += p[i] - (float) mean;
        }
        const auto y = runPitch (pitchPatch ({}), x, rate, [&] (Patch& p, double) {
            if (u (rng) < 0.3f)
                p[PID::PitchMode] = u (rng) < 0.5f ? 0.0f : 1.0f;
            if (u (rng) < 0.3f)
                p[PID::PitchShift] = -24.0f + 48.0f * u (rng);
            if (u (rng) < 0.5f)
                p[PID::PitchPosition] = 10.0f * u (rng);
            if (u (rng) < 0.2f)
                p[PID::PitchMix] = 10.0f * u (rng);
        });
        INFO ("rate " << rate);
        REQUIRE (std::all_of (y.begin(), y.end(), [] (float v) { return std::isfinite (v) && std::abs (v) < 2.0f; }));
        // No offset: the mean is what shifting the plucks down towards the subsonics leaves
        // (it wanders by about 0.01 over 0.1 s), and once the input stops so does the output
        const auto end = y.size() - (size_t) (0.3 * rate);
        double dc = 0.0;
        for (size_t i = 0; i < end; ++i)
            dc += y[i];
        CHECK (std::abs (dc / (double) end) < 3.0e-3);
        CHECK (test::rms (y, y.size() - (size_t) (0.1 * rate)) < 1.0e-7);
    }
}
