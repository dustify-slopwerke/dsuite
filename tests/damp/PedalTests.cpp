#include "common/Analysis.h"
#include "dsp/Cabinet.h"
#include "dsp/Preset.h"
#include "dsp/Rig.h"
#include "dsp/pedals/Drives.h"
#include "damp/PedalHelpers.h"

#include "common/dsp/Noise.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cmath>
#include <tuple>
#include <memory>
#include <numbers>

using namespace damp;
using namespace damp::pedaltest;

namespace
{
// Level of a quiet (linear) tone through the pedal, dB
double smallSignalGain (PedalId id, double hz, std::initializer_list<std::pair<PID, float>> settings = {})
{
    const auto y = runPedal (id, sine (hz, 0.3, 1.0e-4, highRate), settings);
    return test::toneLevelDb (y, highRate, hz, y.size() / 2, 16384) - test::db (1.0e-4);
}

// Harmonic level relative to the fundamental
double harmonicDb (const std::vector<float>& y, double f, int h)
{
    return test::toneLevelDb (y, highRate, f * h, y.size() / 2, 32768) - test::toneLevelDb (y, highRate, f, y.size() / 2, 32768);
}
} // namespace

TEST_CASE ("board order parses, cleans up and round-trips", "[pedals][board]")
{
    const auto defaults = BoardOrder::defaults();
    CHECK (BoardOrder::parse (defaults.format()) == defaults);

    // Every pedal appears exactly once, and the amp once
    auto complete = [] (const BoardOrder& b)
    {
        std::array<int, numPedals + 1> count {};
        for (int v : b.slots)
            ++count[(size_t) (v + 1)];
        return std::all_of (count.begin(), count.end(), [] (int c) { return c == 1; });
    };
    CHECK (complete (defaults));

    // Unknown names and duplicates are dropped, missing pedals restored, clipping pedals moved
    // back in front of the amp
    const auto b = BoardOrder::parse ("amp, rodent, flux_capacitor, boost, boost");
    CHECK (complete (b));
    CHECK (b.ampPosition() == defaults.ampPosition());
    CHECK (BoardOrder::parse ("") == defaults);

    // A board saved before the pedals after the drives existed: they join at their default places
    const auto old = BoardOrder::parse ("boost, face_fuzz, big_fuzz, rodent, distortion, blues_drive, mid_drive, amp");
    CHECK (complete (old));
    CHECK (old.slots[0] == (int) PedalId::Boost);
    CHECK (old.slots[1] == (int) PedalId::Wah); // before the first pedal that follows it by default
    CHECK (old.slots[(size_t) old.ampPosition() + 1] == (int) PedalId::Eq);
    CHECK (old.slots[(size_t) old.ampPosition() + 2] == (int) PedalId::Phaser);
    CHECK (old.slots[BoardOrder::size - 1] == (int) PedalId::Reverb);

    // Delay and reverb only go after the amp: put before it, they move to just after it
    const auto tails = BoardOrder::parse ("reverb, boost, delay, amp, chorus");
    CHECK (complete (tails));
    const auto at = [&] (PedalId p) { return (int) (std::find (tails.slots.begin(), tails.slots.end(), (int) p) - tails.slots.begin()); };
    CHECK (at (PedalId::Reverb) == tails.ampPosition() + 1);
    CHECK (at (PedalId::Delay) > at (PedalId::Reverb));
    CHECK (at (PedalId::Chorus) > at (PedalId::Delay));
    CHECK (at (PedalId::Boost) < tails.ampPosition());

    // Modulation may go either side of the amp; the octave stays before it
    const auto mod = BoardOrder::parse ("phaser, amp, octave, chorus");
    CHECK (std::find (mod.slots.begin(), mod.slots.end(), (int) PedalId::Phaser) - mod.slots.begin() < mod.ampPosition());
    CHECK (mod.ampPosition() < (int) (std::find (mod.slots.begin(), mod.slots.end(), (int) PedalId::Chorus) - mod.slots.begin()));
    CHECK (std::find (mod.slots.begin(), mod.slots.end(), (int) PedalId::Octave) - mod.slots.begin() < mod.ampPosition());

    const auto custom = BoardOrder::parse ("boost, mid_drive, face_fuzz, big_fuzz, rodent, distortion, blues_drive, amp");
    CHECK (custom.slots[0] == (int) PedalId::Boost);
    CHECK (BoardOrder::parse (custom.format()) == custom);
}

TEST_CASE ("presets carry the board order", "[pedals][board]")
{
    auto patch = Patch::defaults();
    patch[PID::MidDriveOn] = 1.0f;
    const auto board = BoardOrder::parse ("mid_drive, boost, amp");
    const auto parsed = parsePreset (serialisePreset (patch, "Test", board));
    CHECK (parsed.warnings.empty());
    CHECK (parsed.board == board);
    CHECK (parsed.patch.v == patch.v);
    // Presets without a board line get the default order
    CHECK (parsePreset ("name = Old\nmodel = Clean\n").board == BoardOrder::defaults());
}

TEST_CASE ("drive pedals have their circuits' character", "[pedals][drives]")
{
    SECTION ("boost: clean gain by its knob, tilt tone")
    {
        CHECK (std::abs (smallSignalGain (PedalId::Boost, 1000.0, { { PID::BoostGain, 12.0f } }) - 12.0) < 1.0);
        CHECK (smallSignalGain (PedalId::Boost, 4000.0, { { PID::BoostTone, 10.0f } }) > smallSignalGain (PedalId::Boost, 4000.0, { { PID::BoostTone, 0.0f } }) + 8.0);
    }

    SECTION ("mid drive: the gain lives above ~720 Hz (the mid hump), more drive, more of it")
    {
        const std::initializer_list<std::pair<PID, float>> hot { { PID::MidDriveDrive, 10.0f }, { PID::MidDriveTone, 10.0f } };
        CHECK (smallSignalGain (PedalId::MidDrive, 1500.0, hot) > smallSignalGain (PedalId::MidDrive, 100.0, hot) + 12.0);
        CHECK (smallSignalGain (PedalId::MidDrive, 1500.0, hot) > smallSignalGain (PedalId::MidDrive, 1500.0, { { PID::MidDriveDrive, 0.0f }, { PID::MidDriveTone, 10.0f } }) + 12.0);
    }

    SECTION ("rodent: more distortion, more gain but less bandwidth; the filter knob darkens")
    {
        const double low = smallSignalGain (PedalId::Rodent, 1000.0, { { PID::RodentDist, 3.0f }, { PID::RodentFilter, 0.0f } });
        const double high = smallSignalGain (PedalId::Rodent, 1000.0, { { PID::RodentDist, 10.0f }, { PID::RodentFilter, 0.0f } });
        CHECK (high > low + 20.0);
        // Flat out, the op-amp's bandwidth (about 1 MHz / gain) rolls off the top
        const std::initializer_list<std::pair<PID, float>> max { { PID::RodentDist, 10.0f }, { PID::RodentFilter, 0.0f } };
        CHECK (smallSignalGain (PedalId::Rodent, 6000.0, max) < smallSignalGain (PedalId::Rodent, 1000.0, max) - 10.0);
        CHECK (smallSignalGain (PedalId::Rodent, 4000.0, { { PID::RodentFilter, 10.0f } }) < smallSignalGain (PedalId::Rodent, 4000.0, { { PID::RodentFilter, 0.0f } }) - 12.0);
    }

    SECTION ("distortion and big fuzz: their tone controls scoop the mids at noon")
    {
        // Quiet enough that the clipping stages are linear, so the tone network shows
        auto gainAt = [] (PedalId id, std::initializer_list<std::pair<PID, float>> settings, double hz)
        {
            const auto y = runPedal (id, sine (hz, 0.3, 1.0e-6, highRate), settings);
            return test::toneLevelDb (y, highRate, hz, y.size() / 2, 16384);
        };
        for (auto [id, tone, drive] : { std::tuple { PedalId::Distortion, PID::DistortionTone, PID::DistortionDist },
                                        std::tuple { PedalId::BigFuzz, PID::BigFuzzTone, PID::BigFuzzSustain } })
        {
            INFO (pedalInfo (id).name);
            const std::initializer_list<std::pair<PID, float>> noon { { tone, 5.0f }, { drive, 0.0f } };
            const double lows = gainAt (id, noon, 150.0), mids = gainAt (id, noon, 1000.0), highs = gainAt (id, noon, 4000.0);
            INFO ("150 Hz " << lows << ", 1 kHz " << mids << ", 4 kHz " << highs);
            // Below the straight line (on a log-frequency axis) between the two ends: a dip, whatever
            // the overall tilt (the fuzz's own feedback caps already darken its top)
            const double line = lows + (highs - lows) * std::log (1000.0 / 150.0) / std::log (4000.0 / 150.0);
            CHECK (mids < line - 2.0);
        }
    }

    SECTION ("face fuzz clips asymmetrically (strong even harmonics); big fuzz symmetrically")
    {
        const auto loud = sine (220.0, 0.5, 0.2, highRate);
        const auto face = runPedal (PedalId::FaceFuzz, loud);
        const auto big = runPedal (PedalId::BigFuzz, loud);
        CHECK (harmonicDb (face, 220.0, 2) > -25.0);
        CHECK (harmonicDb (big, 220.0, 2) < harmonicDb (face, 220.0, 2) - 10.0);
    }

    SECTION ("blues drive: tone is a treble shelf")
    {
        CHECK (smallSignalGain (PedalId::BluesDrive, 5000.0, { { PID::BluesDriveTone, 10.0f } })
               > smallSignalGain (PedalId::BluesDrive, 5000.0, { { PID::BluesDriveTone, 0.0f } }) + 12.0);
    }
}

TEST_CASE ("drive pedals stay finite and DC-free under abuse", "[pedals][fuzz]")
{
    dustify::Random rng (31);
    auto unit = [&] { return 0.5f * (rng.bipolar() + 1.0f); };
    for (int p = 0; p < numPedals; ++p)
    {
        INFO (pedalInfo ((PedalId) p).name);
        auto pedal = makePedal ((PedalId) p);
        pedal->prepare (highRate);
        auto patch = Patch::defaults();
        std::vector<float> x (512);
        double sum = 0.0;
        size_t count = 0;
        for (int block = 0; block < 600; ++block)
        {
            for (const auto& info : allParams())
                if (block % 50 == 0)
                    patch[info.pid] = info.min + unit() * (info.max - info.min);
            for (size_t i = 0; i < x.size(); ++i)
                x[i] = block % 3 == 0 ? 3.0f * rng.bipolar() : block % 3 == 1 ? (float) std::sin (0.05 * (double) i) : 0.0f;
            pedal->setPatch (patch, block % 2 ? highRate : 48000.0, (int) x.size());
            pedal->process (block % 2, x.data(), (int) x.size());
            for (float s : x)
            {
                REQUIRE (std::isfinite (s));
                REQUIRE (std::abs (s) < 100.0f);
            }
        }
        // A steady loud tone leaves no DC
        auto y = runPedal ((PedalId) p, sine (82.0, 2.0, 0.5, highRate), { { pedalInfo ((PedalId) p).params[0], 10.0f } });
        for (size_t i = y.size() / 2; i < y.size(); ++i, ++count)
            sum += y[i];
        CHECK (std::abs (sum / (double) count) < 2.0e-3);
    }
}

namespace
{
// A rig with a clean amp and no cab, so pedal effects are easy to see
struct TestRig
{
    Rig rig;
    Patch patch = Patch::defaults();
    dustify::ConvolutionKernel passThrough { std::vector<float> { 1.0f }.data(), 1 };

    TestRig()
    {
        patch[PID::Model] = (float) AmpModel::Clean;
        patch[PID::Gain] = 3.0f;
        patch[PID::GateOn] = 0.0f;
        patch[PID::Cab] = (float) CabType::Off;
        rig.setPatch (patch);
        rig.prepare (48000.0, 1);
        rig.setCabKernelNow (&passThrough);
    }

    void run (std::vector<float>& x, size_t from, size_t to, int block = 128)
    {
        for (size_t pos = from; pos < to; pos += (size_t) block)
        {
            const int n = (int) std::min<size_t> ((size_t) block, to - pos);
            rig.setPatch (patch);
            rig.process (x.data() + pos, nullptr, n, true);
        }
    }
};

double largestStep (const std::vector<float>& y, size_t from, size_t to)
{
    double worst = 0.0;
    for (size_t i = std::max<size_t> (from, 1); i < to; ++i)
        worst = std::max (worst, (double) std::abs (y[i] - y[i - 1]));
    return worst;
}
} // namespace

TEST_CASE ("switching pedals and reordering the board don't click", "[pedals][rig]")
{
    // A low tone: its own sample-to-sample steps are small, so a click stands out
    TestRig t;
    auto x = sine (110.0, 3.0, 0.1, 48000.0);
    t.run (x, 0, 48000);
    const double steady = largestStep (x, 24000, 48000);

    t.patch[PID::DistortionOn] = 1.0f; // in
    t.run (x, 48000, 72000);
    t.patch[PID::DistortionOn] = 0.0f; // out
    t.run (x, 72000, 96000);
    t.rig.setBoard (BoardOrder::parse ("boost, mid_drive, blues_drive, distortion, rodent, face_fuzz, big_fuzz, amp")); // reorder
    t.patch[PID::BoostOn] = 1.0f;
    t.run (x, 96000, 144000);

    for (float s : x)
        REQUIRE (std::isfinite (s));
    INFO ("steady " << steady);
    CHECK (largestStep (x, 47000, 50000) < 4.0 * steady);   // switching in (the distortion's own edges are larger: allow some)
    CHECK (largestStep (x, 71500, 74000) < 4.0 * steady);   // switching out
    CHECK (largestStep (x, 95500, 98500) < 4.0 * steady);   // reorder (and the boost coming in)
    CHECK (t.rig.board() == BoardOrder::parse ("boost, mid_drive, blues_drive, distortion, rodent, face_fuzz, big_fuzz, amp"));
}

TEST_CASE ("pedals before the amp run oversampled: their clipping doesn't alias", "[pedals][aliasing]")
{
    const double f = 2489.0;
    double worst[2] {};
    for (int factor : { 0, 2 })
    {
        TestRig t;
        t.patch[PID::RodentOn] = 1.0f;
        t.patch[PID::RodentDist] = 10.0f;
        t.patch[PID::RodentFilter] = 0.0f;
        t.patch[PID::Oversampling] = (float) factor;
        auto x = sine (f, 1.0, 0.25, 48000.0);
        t.run (x, 0, x.size());
        worst[factor / 2] = test::worstAliasDb (x, 48000.0, f, 20000.0);
    }
    INFO ("2x " << worst[0] << " dB, 8x " << worst[1] << " dB");
    CHECK (worst[1] < worst[0] - 10.0);
    CHECK (worst[1] < -40.0); // a full-scale 2.5 kHz sine, distortion and brightness flat out
}

TEST_CASE ("a mono input comes out on both sides", "[pedals][rig]")
{
    TestRig t;
    std::vector<float> l = sine (220.0, 0.2, 0.1, 48000.0), r (l.size(), 0.0f);
    t.rig.process (l.data(), r.data(), (int) l.size(), true);
    CHECK (l == r);
}
