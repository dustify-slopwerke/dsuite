#include "dsp/Cabinet.h"
#include "dsp/Rig.h"
#include "damp/PedalHelpers.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <random>

using namespace damp;
using namespace damp::pedaltest;

// A mono guitar on both inputs of a stereo interface: the same signal, each with its own ADC
// noise. Pedals that decide something (where to splice, when to flip) must decide it once for
// both channels, or the least difference between them pulls the two apart.
namespace
{
using Settings = std::initializer_list<std::pair<PID, float>>;
constexpr double rate = 192000.0; // 48 kHz x 4 before the amp

// Notes of a few harmonics, one after another, some quiet, some high, one a chord, with a gap of
// silence or two
std::vector<float> phrase()
{
    struct Note
    {
        double hz, level, length;
        double chord = 0.0; // a second note
    };
    const Note notes[] { { 82.4, 0.2, 0.6 }, { 110.0, 0.02, 0.6 }, { 146.8, 0.2, 0.35 }, { 196.0, 0.2, 0.6, 246.9 },
                         { 329.6, 0.01, 0.6 }, { 659.3, 0.05, 0.4 }, { 98.0, 0.2, 0.6, 146.8 } };
    std::vector<float> x ((size_t) (4.4 * rate));
    for (int k = 0; k < 7; ++k)
    {
        const auto& note = notes[k];
        const auto start = (size_t) (k * 0.6 * rate);
        for (size_t i = 0; i < (size_t) (note.length * rate) && start + i < x.size(); ++i)
        {
            const double t = (double) i / rate;
            double v = 0.0;
            for (const double f : { note.hz, note.chord })
                for (int h = 1; h <= 6 && f > 0.0; ++h)
                    v += std::sin (2.0 * std::numbers::pi * f * h * t + 0.7 * h) / h * std::exp (-t * (2.0 + 0.5 * h));
            x[start + i] = (float) (note.level * v);
        }
    }
    return x;
}

std::vector<float> withNoise (std::vector<float> x, double sigma, unsigned seed)
{
    std::mt19937 rng (seed);
    std::normal_distribution<double> noise (0.0, sigma);
    for (auto& v : x)
        v += (float) noise (rng);
    return x;
}

// One pedal on both channels, the way the board runs it: left, then right, block by block
std::pair<std::vector<float>, std::vector<float>> runStereo (PedalId id, std::vector<float> l, std::vector<float> r,
                                                             Settings settings, int block = 512)
{
    auto patch = Patch::defaults();
    for (auto [pid, v] : settings)
        patch[pid] = v;
    auto pedal = makePedal (id);
    pedal->prepare (rate);
    for (size_t pos = 0; pos < l.size(); pos += (size_t) block)
    {
        const int n = (int) std::min<size_t> ((size_t) block, l.size() - pos);
        pedal->setPatch (patch, rate, n);
        pedal->process (0, l.data() + pos, n);
        pedal->process (1, r.data() + pos, n);
    }
    return { l, r };
}

double correlation (const std::vector<float>& a, const std::vector<float>& b)
{
    double ab = 0.0, aa = 0.0, bb = 0.0;
    for (size_t i = 0; i < a.size(); ++i)
    {
        ab += (double) a[i] * b[i];
        aa += (double) a[i] * a[i];
        bb += (double) b[i] * b[i];
    }
    return ab / std::sqrt (aa * bb);
}
} // namespace

TEST_CASE ("stereo link: a mono guitar on two noisy inputs stays one signal", "[pedals][pitch][octave][stereo]")
{
    const auto dry = phrase();
    const auto left = withNoise (dry, 1.0e-4, 1), right = withNoise (dry, 1.0e-4, 2);
    const auto check = [&] (PedalId id, Settings settings)
    {
        const auto [l, r] = runStereo (id, left, right, settings);
        CHECK (correlation (l, r) > 0.99);
    };
    SECTION ("pitch down") { check (PedalId::Pitch, { { PID::PitchShift, -5.0f } }); }
    SECTION ("pitch an octave down") { check (PedalId::Pitch, { { PID::PitchShift, -12.0f } }); }
    SECTION ("pitch up") { check (PedalId::Pitch, { { PID::PitchShift, 7.0f } }); }
    SECTION ("pitch an octave up, free") { check (PedalId::Pitch, { { PID::PitchShift, 12.0f }, { PID::PitchMode, 1.0f } }); }
    SECTION ("octave") { check (PedalId::Octave, { { PID::OctaveOct2, 7.0f } }); }
}

TEST_CASE ("stereo link: a lone right channel still works, and mono is the left's own", "[pedals][pitch][octave][stereo]")
{
    const auto dry = phrase();
    for (const auto id : { PedalId::Pitch, PedalId::Octave })
    {
        INFO ((int) id);
        const Settings settings { { PID::PitchShift, -5.0f }, { PID::OctaveOct2, 7.0f } };
        auto patch = Patch::defaults();
        for (auto [pid, v] : settings)
            patch[pid] = v;
        // Mono: the left only, the same whether or not a right channel ever ran before
        auto a = makePedal (id), b = makePedal (id);
        a->prepare (rate);
        b->prepare (rate);
        auto x = dry, y = dry, z = dry;
        for (size_t pos = 0; pos < x.size(); pos += 512)
        {
            const int n = (int) std::min<size_t> (512, x.size() - pos);
            a->setPatch (patch, rate, n);
            b->setPatch (patch, rate, n);
            a->process (0, x.data() + pos, n);
            b->process (0, y.data() + pos, n);
            if (pos < x.size() / 2)
                b->process (1, z.data() + pos, n); // the right runs for a while, then stops
        }
        CHECK (x == y);

        // The right alone (no left before it this block), in blocks longer than any the board runs
        auto c = makePedal (id);
        c->prepare (rate);
        auto r = dry;
        for (size_t pos = 0; pos < r.size(); pos += 2048)
        {
            const int n = (int) std::min<size_t> (2048, r.size() - pos);
            c->setPatch (patch, rate, n);
            c->process (1, r.data() + pos, n);
        }
        CHECK (std::all_of (r.begin(), r.end(), [] (float v) { return std::isfinite (v); }));
        const auto [l2, r2] = runStereo (id, dry, withNoise (dry, 1.0e-4, 3), settings, 2048);
        CHECK (std::isfinite (correlation (l2, r2)));
    }
}

// ---- The input selector ----

namespace
{
// A whole rig, clean amp, no cab, a stereo chorus after the amp
struct StereoRig
{
    Rig rig;
    Patch patch = Patch::defaults();
    dustify::ConvolutionKernel passThrough { std::vector<float> { 1.0f }.data(), 1 };

    explicit StereoRig (InputMode mode, Settings settings = {})
    {
        patch[PID::Model] = (float) AmpModel::Clean;
        patch[PID::Gain] = 3.0f;
        patch[PID::GateOn] = 0.0f;
        patch[PID::Cab] = (float) CabType::Off;
        patch[PID::ChorusOn] = 1.0f;
        patch[PID::InputMode] = (float) mode;
        for (auto [pid, v] : settings)
            patch[pid] = v;
        rig.setPatch (patch);
        rig.prepare (48000.0, 1);
        rig.setCabKernelNow (&passThrough);
    }

    // right empty: a mono input bus
    std::pair<std::vector<float>, std::vector<float>> run (std::vector<float> l, std::vector<float> r, int block = 256)
    {
        const bool mono = r.empty();
        if (mono)
            r.assign (l.size(), 0.0f);
        for (size_t pos = 0; pos < l.size(); pos += (size_t) block)
        {
            const int n = (int) std::min<size_t> ((size_t) block, l.size() - pos);
            rig.setPatch (patch);
            rig.process (l.data() + pos, r.data() + pos, n, mono);
        }
        return { l, r };
    }
};

std::vector<float> tone (double hz, double amplitude)
{
    return sine (hz, 0.5, amplitude, 48000.0);
}
} // namespace

TEST_CASE ("input: Left, Right and Sum play a stereo input as a mono one", "[rig][input]")
{
    const auto left = tone (220.0, 0.1), right = tone (330.0, 0.05);
    std::vector<float> sum (left.size());
    for (size_t i = 0; i < sum.size(); ++i)
        sum[i] = 0.5f * (left[i] + right[i]);

    const auto mono = [] (const std::vector<float>& x) { return StereoRig (InputMode::Left).run (x, {}); };
    SECTION ("Left") { CHECK (StereoRig (InputMode::Left).run (left, right) == mono (left)); }
    SECTION ("Right") { CHECK (StereoRig (InputMode::Right).run (left, right) == mono (right)); }
    SECTION ("Sum") { CHECK (StereoRig (InputMode::Sum).run (left, right) == mono (sum)); }
    SECTION ("a mono input ignores it")
    {
        CHECK (StereoRig (InputMode::Right).run (left, {}) == mono (left));
        CHECK (StereoRig (InputMode::Stereo).run (left, {}) == mono (left));
    }
    SECTION ("the effects after the amp stay stereo")
    {
        const auto [l, r] = StereoRig (InputMode::Left).run (left, right);
        CHECK (l != r);
        CHECK (correlation (l, r) > 0.5);
    }
}

TEST_CASE ("input: Stereo runs each channel through its own amp", "[rig][input]")
{
    const auto left = tone (220.0, 0.1);
    const std::vector<float> silence (left.size(), 0.0f);
    StereoRig dry (InputMode::Stereo, { { PID::ChorusOn, 0.0f } });
    const auto [l, r] = dry.run (left, silence);
    double rightPeak = 0.0, leftPeak = 0.0;
    for (size_t i = 0; i < l.size(); ++i)
    {
        leftPeak = std::max (leftPeak, (double) std::abs (l[i]));
        rightPeak = std::max (rightPeak, (double) std::abs (r[i]));
    }
    CHECK (leftPeak > 0.01);
    CHECK (rightPeak < 1.0e-6); // nothing in, nothing out

    // A mono guitar on both inputs, each with its own noise, through the pitch shifter and
    // octave: the two sides stay one sound
    const auto guitar = sine (110.0, 1.0, 0.1, 48000.0);
    for (const auto shift : { -12.0f, -5.0f, 7.0f })
    {
        INFO ("shift " << shift);
        StereoRig rig (InputMode::Stereo, { { PID::ChorusOn, 0.0f }, { PID::PitchOn, 1.0f }, { PID::PitchShift, shift },
                                            { PID::OctaveOn, 1.0f } });
        const auto [a, b] = rig.run (withNoise (guitar, 1.0e-4, 4), withNoise (guitar, 1.0e-4, 5));
        CHECK (correlation (a, b) > 0.99);
    }
}

namespace
{
float biggestStep (const std::vector<float>& x, size_t from, size_t to)
{
    float m = 0.0f;
    for (size_t i = std::max<size_t> (from, 1); i < to; ++i)
        m = std::max (m, std::abs (x[i] - x[i - 1]));
    return m;
}

std::vector<float> slice (const std::vector<float>& x, size_t from, size_t to)
{
    return { x.begin() + (std::ptrdiff_t) from, x.begin() + (std::ptrdiff_t) to };
}
} // namespace

TEST_CASE ("input: switching in and out of Stereo, or between channels, while playing doesn't click", "[rig][input]")
{
    // Different sounds on the two inputs, so every switch changes what's heard
    const auto left = tone (220.0, 0.1), right = tone (330.0, 0.08);
    struct Switch
    {
        InputMode from, to;
    };
    for (const auto [from, to] : { Switch { InputMode::Left, InputMode::Stereo }, Switch { InputMode::Sum, InputMode::Stereo },
                                   Switch { InputMode::Right, InputMode::Stereo }, Switch { InputMode::Stereo, InputMode::Left },
                                   Switch { InputMode::Stereo, InputMode::Right }, Switch { InputMode::Left, InputMode::Right },
                                   Switch { InputMode::Right, InputMode::Sum } })
    {
        INFO ((int) from << " -> " << (int) to);
        // Stereo first, so the right amp keeps state from then, then a while in `from`, then `to`
        StereoRig rig (InputMode::Stereo, { { PID::ChorusOn, 0.0f }, { PID::Gain, 6.0f } });
        const size_t third = left.size() / 3;
        std::vector<float> l, r;
        const auto part = [&] (InputMode mode, size_t a, size_t b)
        {
            rig.patch[PID::InputMode] = (float) mode;
            const auto [pl, pr] = rig.run (slice (left, a, b), slice (right, a, b));
            l.insert (l.end(), pl.begin(), pl.end());
            r.insert (r.end(), pr.begin(), pr.end());
        };
        part (InputMode::Stereo, 0, third);
        part (from, third, 2 * third);
        part (to, 2 * third, left.size());

        // The steps around the switch are no bigger than the signal's own, before and after it, but
        // for the slope of the 20 ms crossfade between two different sounds (a click: 100 times more)
        const size_t at = 2 * third, settle = 3600; // 75 ms
        for (const auto* x : { &l, &r })
        {
            const float before = biggestStep (*x, at - settle, at), after = biggestStep (*x, at + settle, at + 2 * settle);
            const float around = biggestStep (*x, at - 64, at + settle);
            INFO ((x == &l ? "left" : "right") << ": before " << before << ", around " << around << ", after " << after);
            CHECK (around <= 1.5f * std::max (before, after));
        }
    }
}

TEST_CASE ("input: after switching to Stereo the right side is its own amp's", "[rig][input]")
{
    const auto left = tone (220.0, 0.1), right = tone (330.0, 0.08);
    StereoRig fresh (InputMode::Stereo, { { PID::ChorusOn, 0.0f } });
    const auto [fl, fr] = fresh.run (left, right);

    StereoRig rig (InputMode::Left, { { PID::ChorusOn, 0.0f } });
    const size_t half = left.size() / 2, end = left.size();
    rig.run (slice (left, 0, half), slice (right, 0, half));
    rig.patch[PID::InputMode] = (float) InputMode::Stereo;
    const auto [l, r] = rig.run (slice (left, half, end), slice (right, half, end));
    // 100 ms on (warmed up and faded in), both sides are a rig's that was in Stereo throughout
    const size_t skip = 4800;
    CHECK (correlation (slice (r, skip, r.size()), slice (fr, half + skip, end)) > 0.999);
    CHECK (correlation (slice (l, skip, l.size()), slice (fl, half + skip, end)) > 0.999);
}
