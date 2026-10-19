#include "common/Analysis.h"
#include "damp/PedalHelpers.h"
#include "dsp/pedals/Board.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <random>
#include <utility>

using namespace damp;
using namespace damp::pedaltest;

namespace
{
using Settings = std::initializer_list<std::pair<PID, float>>;
constexpr double hostRate = 48000.0;
constexpr DelayMode modes[] { DelayMode::Digital, DelayMode::Analog, DelayMode::Tape };

const char* modeName (DelayMode m) { return m == DelayMode::Digital ? "digital" : m == DelayMode::Analog ? "analog" : "tape"; }

// Clean, unmodulated repeats at full level unless the test says otherwise
Patch delayPatch (DelayMode mode, Settings settings)
{
    auto patch = Patch::defaults();
    patch[PID::DelayOn] = 1.0f;
    patch[PID::DelayMode] = (float) mode;
    patch[PID::DelayMix] = 10.0f;
    patch[PID::DelayMod] = 0.0f;
    patch[PID::DelayTone] = 10.0f;
    for (auto [pid, v] : settings)
        patch[pid] = v;
    return patch;
}

// The pedal on its own, mono, in blocks of 256
std::vector<float> runDelay (const Patch& patch, std::vector<float> x, double rate = hostRate, double bpm = 120.0)
{
    pedals::Delay delay;
    delay.prepare (rate);
    for (size_t pos = 0; pos < x.size(); pos += 256)
    {
        const int n = (int) std::min<size_t> (256, x.size() - pos);
        delay.setTempo (bpm);
        delay.setPatch (patch, rate, n);
        delay.process (0, x.data() + pos, n);
    }
    return x;
}

std::vector<float> impulse (double seconds, size_t at = 4800)
{
    std::vector<float> x ((size_t) (seconds * hostRate), 0.0f);
    x[at] = 1.0f;
    return x;
}

// The loudest sample within `radius` of `around`
size_t peakNear (const std::vector<float>& y, size_t around, size_t radius)
{
    size_t best = around - radius;
    for (size_t i = around - radius; i <= around + radius; ++i)
        if (std::abs (y[i]) > std::abs (y[best]))
            best = i;
    return best;
}

double energy (const std::vector<float>& y, size_t from, size_t to)
{
    double e = 0.0;
    for (size_t i = from; i < to; ++i)
        e += (double) y[i] * y[i];
    return e;
}

// Where a repeat's spectrum (a 4096-sample window centred on it) falls 6 dB below its 300-800 Hz level
double cornerHz (const std::vector<float>& y, size_t at)
{
    constexpr size_t n = 4096;
    const auto mag = test::magnitudeSpectrum (y, at - n / 2, n);
    const double bin = hostRate / (double) n;
    double ref = 0.0;
    for (auto i = (size_t) (300.0 / bin); i < (size_t) (800.0 / bin); ++i)
        ref = std::max (ref, mag[i]);
    for (auto i = (size_t) (800.0 / bin); i < mag.size(); ++i)
        if (mag[i] < 0.5 * ref)
            return (double) i * bin;
    return hostRate / 2.0;
}

double largestStep (const std::vector<float>& y, size_t from, size_t to)
{
    double worst = 0.0;
    for (size_t i = std::max<size_t> (from, 1); i < to; ++i)
        worst = std::max (worst, (double) std::abs (y[i] - y[i - 1]));
    return worst;
}

double largestDifference (const std::vector<float>& a, const std::vector<float>& b, size_t from, size_t to)
{
    double worst = 0.0;
    for (size_t i = from; i < to; ++i)
        worst = std::max (worst, (double) std::abs (a[i] - b[i]));
    return worst;
}

// The delay on a Board after the amp, mono, switched by its patch as the plugin would
struct TestBoard
{
    Board board;
    Patch patch;

    explicit TestBoard (Patch p) : patch (p) { board.prepare (hostRate, 4.0 * hostRate); }

    void run (std::vector<float>& x, size_t from, size_t to, int block = 128)
    {
        for (size_t pos = from; pos < to; pos += (size_t) block)
        {
            const int n = (int) std::min<size_t> ((size_t) block, to - pos);
            board.setTempo (120.0);
            board.setPatch (patch);
            board.processAfterAmp (x.data() + pos, nullptr, n, hostRate);
        }
    }
};
} // namespace

TEST_CASE ("delay repeats land at the set time and at synced note values", "[pedals][delay]")
{
    for (auto mode : modes)
    {
        INFO (modeName (mode));
        // Analog and tape band-limit every repeat: allow for their filters' group delay
        const size_t slack = mode == DelayMode::Digital ? 1 : 24;
        const auto y = runDelay (delayPatch (mode, { { PID::DelayTime, 250.0f }, { PID::DelayFeedback, 5.0f } }), impulse (1.0));
        for (size_t k = 1; k <= 3; ++k)
        {
            const size_t expected = 4800 + k * 12000;
            const size_t found = peakNear (y, expected, 1200);
            INFO ("repeat " << k << " at " << found << ", expected " << expected);
            CHECK (found + slack >= expected);
            CHECK (found <= expected + slack);
        }
    }

    // Note values at a few tempos (in quarter notes)
    struct Case { DelaySync sync; double bpm, quarters; };
    for (auto c : { Case { DelaySync::Quarter, 120.0, 1.0 }, Case { DelaySync::DottedEighth, 100.0, 0.75 },
                    Case { DelaySync::EighthTriplet, 90.0, 1.0 / 3.0 }, Case { DelaySync::Sixteenth, 140.0, 0.25 },
                    Case { DelaySync::Half, 75.0, 2.0 } })
    {
        const double seconds = c.quarters * 60.0 / c.bpm;
        INFO ("sync " << (int) c.sync << " at " << c.bpm << " bpm: " << seconds << " s");
        const auto patch = delayPatch (DelayMode::Digital, { { PID::DelaySync, (float) c.sync }, { PID::DelayTime, 900.0f } });
        const auto y = runDelay (patch, impulse (2.0 * seconds + 0.2), hostRate, c.bpm);
        const auto expected = 4800 + (size_t) std::lround (seconds * hostRate);
        const size_t found = peakNear (y, expected, (size_t) (0.1 * seconds * hostRate));
        CHECK (found + 1 >= expected);
        CHECK (found <= expected + 1);
    }

    // Slow tempos are clamped to the longest time the line holds
    CHECK (pedals::Delay::targetSeconds (delayPatch (DelayMode::Digital, { { PID::DelaySync, (float) DelaySync::Half } }), 20.0)
           == pedals::Delay::maxSeconds);
}

TEST_CASE ("delay feedback sets the decay per repeat", "[pedals][delay]")
{
    // A 1 kHz burst, so the hi-cut (at its brightest) doesn't come into it
    auto burst = sine (1000.0, 1.0, 0.5, hostRate);
    for (size_t i = 0; i < burst.size(); ++i)
        if (i < 4800 || i >= 4800 + 1920)
            burst[i] = 0.0f;
    double last = 0.0;
    for (float knob : { 2.0f, 5.0f, 8.0f })
    {
        const auto y = runDelay (delayPatch (DelayMode::Digital, { { PID::DelayTime, 200.0f }, { PID::DelayFeedback, knob } }), burst);
        // Repeats every 9600 samples: compare the middle of the second with that of the third
        const auto level = [&y] (size_t k) { return test::rms (y, 4800 + k * 9600 + 200, 4800 + k * 9600 + 1700); };
        const double ratio = level (3) / level (2);
        const double k = knob / 10.0, expected = 0.95 * k * (1.15 - 0.15 * k);
        INFO ("feedback " << knob << ": " << ratio << " per repeat, expected " << expected);
        CHECK (std::abs (ratio - expected) < 0.02);
        CHECK (ratio > last);
        last = ratio;
    }
    // No feedback: a single repeat
    const auto y = runDelay (delayPatch (DelayMode::Analog, { { PID::DelayTime, 200.0f }, { PID::DelayFeedback, 0.0f } }), impulse (1.0));
    CHECK (energy (y, 4800 + 2 * 9600 - 400, y.size()) < 1.0e-10);
}

TEST_CASE ("analog and tape repeats darken, digital ones stay clear", "[pedals][delay]")
{
    for (auto mode : modes)
    {
        INFO (modeName (mode));
        // Digital at its brightest; analog and tape at noon
        const float tone = mode == DelayMode::Digital ? 10.0f : 5.0f;
        const auto y = runDelay (delayPatch (mode, { { PID::DelayTime, 300.0f }, { PID::DelayFeedback, 7.0f }, { PID::DelayTone, tone } }),
                                 impulse (1.4));
        const double first = cornerHz (y, 4800 + 14400), third = cornerHz (y, 4800 + 3 * 14400);
        INFO ("corner " << first << " Hz, then " << third << " Hz by the third repeat");
        if (mode == DelayMode::Digital)
        {
            CHECK (third > 0.6 * first);
        }
        else
        {
            CHECK (first < 6000.0);
            CHECK (first > 1500.0);
            CHECK (third < 0.8 * first);
        }
    }

    // A BBD's band limit falls as its clock slows for a longer delay
    const auto bandAt = [] (float ms)
    {
        const auto y = runDelay (delayPatch (DelayMode::Analog, { { PID::DelayTime, ms }, { PID::DelayTone, 5.0f } }), impulse (1.2));
        return cornerHz (y, 4800 + (size_t) (ms * 48.0));
    };
    const double shortTime = bandAt (60.0f), longTime = bandAt (1000.0f);
    INFO ("analog band: " << shortTime << " Hz at 60 ms, " << longTime << " Hz at 1 s");
    CHECK (longTime < 0.85 * shortTime);
}

TEST_CASE ("delay stays bounded at full feedback with a loud input", "[pedals][delay]")
{
    for (auto mode : modes)
    {
        INFO (modeName (mode));
        std::mt19937 rng (7);
        std::uniform_real_distribution<float> u (-2.0f, 2.0f);
        std::vector<float> x ((size_t) (4.0 * hostRate), 0.0f);
        for (size_t i = 0; i < (size_t) hostRate; ++i)
            x[i] = u (rng);
        const auto y = runDelay (delayPatch (mode, { { PID::DelayTime, 120.0f }, { PID::DelayFeedback, 10.0f }, { PID::DelayMod, 10.0f } }), x);
        double peakTail = 0.0;
        for (size_t i = (size_t) hostRate; i < y.size(); ++i)
        {
            REQUIRE (std::isfinite (y[i]));
            peakTail = std::max (peakTail, (double) std::abs (y[i]));
        }
        INFO ("peak after the input stops " << peakTail);
        CHECK (peakTail < 3.0); // from an input peaking at +6 dBFS
        const double lastSecond = test::rms (y, y.size() - (size_t) hostRate);
        INFO ("last second " << test::db (lastSecond) << " dB RMS");
        if (mode == DelayMode::Digital)
            CHECK (lastSecond < 0.7 * test::rms (y, (size_t) hostRate, 2 * (size_t) hostRate)); // dying away
        else
            CHECK (lastSecond > 0.01); // still going: at the edge of self-oscillation
    }
}

TEST_CASE ("delay ping-pong alternates sides, and Mix 0 is dry", "[pedals][delay]")
{
    for (auto mode : modes)
    {
        INFO (modeName (mode));
        auto patch = delayPatch (mode, { { PID::DelayTime, 200.0f }, { PID::DelayFeedback, 6.0f }, { PID::DelayPingPong, 1.0f } });
        pedals::Delay delay;
        delay.prepare (hostRate);
        auto left = impulse (1.0), right = impulse (1.0);
        for (size_t pos = 0; pos < left.size(); pos += 256)
        {
            const int n = (int) std::min<size_t> (256, left.size() - pos);
            delay.setPatch (patch, hostRate, n);
            delay.processStereo (left.data() + pos, right.data() + pos, n);
        }
        for (size_t k = 1; k <= 4; ++k)
        {
            const size_t from = 4800 + k * 9600 - 600, to = 4800 + k * 9600 + 600;
            const double l = energy (left, from, to), r = energy (right, from, to);
            INFO ("repeat " << k << ": left " << test::db (std::sqrt (l)) << " dB, right " << test::db (std::sqrt (r)) << " dB");
            CHECK ((k % 2 == 1 ? l : r) > 1.0e-4);
            CHECK ((k % 2 == 1 ? r : l) < 1.0e-6 * (k % 2 == 1 ? l : r));
        }
    }

    // Off, a mono input repeats on both sides alike
    {
        auto patch = delayPatch (DelayMode::Analog, { { PID::DelayMod, 5.0f } });
        pedals::Delay delay;
        delay.prepare (hostRate);
        auto left = sine (330.0, 1.0, 0.3, hostRate), right = left;
        delay.setPatch (patch, hostRate, (int) left.size());
        delay.processStereo (left.data(), right.data(), (int) left.size());
        CHECK (left == right);
    }

    // Mix 0: exactly the dry signal, whatever else is set
    for (auto mode : modes)
    {
        const auto x = sine (196.0, 1.0, 0.5, hostRate);
        const auto y = runDelay (delayPatch (mode, { { PID::DelayMix, 0.0f }, { PID::DelayFeedback, 10.0f }, { PID::DelayMod, 10.0f } }), x);
        CHECK (largestDifference (x, y, 0, x.size()) == 0.0);
    }
}

TEST_CASE ("sweeping the delay time doesn't click", "[pedals][delay]")
{
    for (auto mode : modes)
    {
        INFO (modeName (mode));
        const auto patch = delayPatch (mode, { { PID::DelayTime, 150.0f }, { PID::DelayFeedback, 4.0f }, { PID::DelayMix, 5.0f } });
        auto x = sine (110.0, 4.0, 0.2, hostRate);
        pedals::Delay delay;
        delay.prepare (hostRate);
        auto p = patch;
        for (size_t pos = 0; pos < x.size(); pos += 128)
        {
            // Steady for 1.5 s, then swept 150 -> 700 ms over a second, then a jump back to 80 ms
            const double t = (double) pos / hostRate;
            p[PID::DelayTime] = t < 1.5 ? 150.0f : t < 2.5 ? (float) (150.0 + 550.0 * (t - 1.5)) : 80.0f;
            delay.setPatch (p, hostRate, 128);
            delay.process (0, x.data() + pos, 128);
        }
        // The dry tone's own steps, and those of the repeats at most: their sum in phase, each
        // pass through the line bending it up by as much as half again as the time shortens
        const double dry = 2.0 * std::numbers::pi * 110.0 * 0.2 / hostRate;
        const double sweep = largestStep (x, 72000, x.size());
        INFO ("dry " << dry << ", while the time moves " << sweep);
        CHECK (sweep < 4.0 * dry);
    }
}

TEST_CASE ("delay stays finite and DC-free under parameter abuse", "[pedals][delay]")
{
    for (double rate : { 44100.0, 192000.0 })
    {
        INFO (rate << " Hz");
        dustify::Random rng (91);
        auto unit = [&] { return 0.5f * (rng.bipolar() + 1.0f); };
        pedals::Delay delay;
        delay.prepare (rate);
        auto patch = Patch::defaults();
        std::vector<float> left (512), right (512);
        for (int block = 0; block < 400; ++block)
        {
            if (block % 10 == 0)
            {
                for (PID pid : pedalInfo (PedalId::Delay).params)
                {
                    const auto& info = paramInfo (pid);
                    patch[pid] = info.min + unit() * (info.max - info.min);
                }
                delay.setTempo (20.0 + 280.0 * unit());
                delay.setEngaged (unit() > 0.3f);
            }
            const int n = 1 + (int) (unit() * 511.0f);
            for (int i = 0; i < n; ++i)
            {
                left[(size_t) i] = block % 3 == 0 ? 2.0f * rng.bipolar() : block % 3 == 1 ? (float) std::sin (0.03 * i) : 0.0f;
                right[(size_t) i] = block % 4 == 0 ? left[(size_t) i] : 0.5f * rng.bipolar();
            }
            delay.setPatch (patch, rate, n);
            if (block % 5 == 0)
                delay.process (0, left.data(), n);
            else
                delay.processStereo (left.data(), right.data(), n);
            for (int i = 0; i < n; ++i)
            {
                REQUIRE (std::isfinite (left[(size_t) i]));
                REQUIRE (std::isfinite (right[(size_t) i]));
                REQUIRE (std::abs (left[(size_t) i]) < 20.0f);
                REQUIRE (std::abs (right[(size_t) i]) < 20.0f);
            }
        }
    }

    // A loud low tone at full feedback leaves no DC
    for (auto mode : modes)
    {
        INFO (modeName (mode));
        const auto y = runDelay (delayPatch (mode, { { PID::DelayFeedback, 10.0f }, { PID::DelayMod, 10.0f } }),
                                 sine (82.0, 2.0, 0.8, hostRate));
        // (relative to the level: the repeats' swelling envelopes leave a little mean on their own)
        double sum = 0.0;
        for (size_t i = y.size() / 2; i < y.size(); ++i)
            sum += y[i];
        const double mean = sum / (double) (y.size() / 2), level = test::rms (y, y.size() / 2);
        INFO ("mean " << mean << ", RMS " << level);
        CHECK (std::abs (mean) < 3.0e-3 * level);
    }
}

TEST_CASE ("switched off, the delay's repeats ring out or fade, the dry signal untouched", "[pedals][delay][rig]")
{
    const auto base = delayPatch (DelayMode::Digital, { { PID::DelayTime, 100.0f }, { PID::DelayFeedback, 5.0f }, { PID::DelayMix, 6.0f } });
    const auto burst = []
    {
        // A short note, then silence
        auto x = sine (440.0, 3.0, 0.3, hostRate);
        std::fill (x.begin() + 2400, x.end(), 0.0f);
        return x;
    };
    const size_t off = 12032; // 250 ms (on a block boundary): the second repeat has just passed

    // Left on: the reference
    auto reference = burst();
    {
        TestBoard t (base);
        t.run (reference, 0, reference.size());
    }

    SECTION ("trails on: the repeats carry on as if it were still on")
    {
        auto y = burst();
        TestBoard t (base);
        t.run (y, 0, off);
        t.patch[PID::DelayOn] = 0.0f;
        t.run (y, off, y.size());
        // (until the tail is under -90 dBFS, when the board stops running it)
        CHECK (largestDifference (y, reference, 0, y.size()) < 3.2e-5);
        CHECK (largestDifference (y, reference, 0, y.size() / 2) < 1.0e-6);
        CHECK (energy (y, off + 4800, off + 9600) > 1.0e-3); // the next repeat is there
    }

    SECTION ("and switching back on mid-tail carries on just the same")
    {
        auto y = burst();
        TestBoard t (base);
        t.run (y, 0, off);
        t.patch[PID::DelayOn] = 0.0f;
        t.run (y, off, 2 * off);
        t.patch[PID::DelayOn] = 1.0f;
        t.run (y, 2 * off, y.size());
        CHECK (largestDifference (y, reference, 0, y.size()) < 1.0e-6);
    }

    SECTION ("trails off: the repeats fade within 20 ms")
    {
        auto y = burst();
        TestBoard t (base);
        t.patch[PID::DelayTrails] = 0.0f;
        t.run (y, 0, off);
        t.patch[PID::DelayOn] = 0.0f;
        t.run (y, off, y.size());
        CHECK (largestDifference (y, reference, 0, off) < 1.0e-6);
        CHECK (energy (y, off + 960, y.size()) == 0.0);
    }
}

TEST_CASE ("switching the delay on and off under a steady tone doesn't click", "[pedals][delay][rig]")
{
    for (bool trails : { true, false })
    {
        INFO ((trails ? "trails" : "no trails"));
        // 14 cycles of the tone a repeat: the repeats build up in phase, never past their steady sum
        const auto base = delayPatch (DelayMode::Digital, { { PID::DelayTime, 14000.0f / 110.0f }, { PID::DelayMix, 6.0f },
                                                            { PID::DelayTrails, trails ? 1.0f : 0.0f } });
        const auto x = sine (110.0, 3.0, 0.2, hostRate);
        auto y = x;
        TestBoard t (base);
        t.patch[PID::DelayOn] = 0.0f;
        t.run (y, 0, 24000);
        t.patch[PID::DelayOn] = 1.0f; // on at 0.5 s
        t.run (y, 24000, 72000);
        t.patch[PID::DelayOn] = 0.0f; // off at 1.5 s
        t.run (y, 72000, 120000);
        t.patch[PID::DelayOn] = 1.0f; // on again at 2.5 s, mid-tail with trails
        t.run (y, 120000, y.size());

        // Off at first: exactly dry
        CHECK (largestDifference (x, y, 0, 24000) == 0.0);
        const double dry = largestStep (x, 0, 24000), steady = largestStep (y, 48000, 72000);
        INFO ("dry " << dry << ", on " << steady);
        CHECK (largestStep (y, 23000, 36000) < 1.05 * steady); // on, then the first repeat arriving
        CHECK (largestStep (y, 71000, 84000) < 1.05 * steady); // off
        CHECK (largestStep (y, 119000, 132000) < 1.05 * steady); // on again
        if (! trails)
            CHECK (largestDifference (x, y, 72000 + 960, 120000) == 0.0); // faded out: dry at unity
    }
}

TEST_CASE ("an off delay stops ringing once its tail has died away", "[pedals][delay]")
{
    for (bool trails : { true, false })
    {
        INFO ((trails ? "trails" : "no trails"));
        auto patch = delayPatch (DelayMode::Tape, { { PID::DelayTime, 100.0f }, { PID::DelayFeedback, 5.0f },
                                                    { PID::DelayTrails, trails ? 1.0f : 0.0f } });
        pedals::Delay delay;
        delay.prepare (hostRate);
        std::vector<float> x (128);
        double loudestAfter = 0.0;
        int blocks = 0, silentFrom = -1;
        for (; blocks < 2000; ++blocks)
        {
            std::fill (x.begin(), x.end(), 0.0f);
            if (blocks == 2)
                x[0] = 1.0f;
            const bool on = blocks < 40; // off after about 100 ms
            delay.setEngaged (on);
            if (! on && ! delay.ringing())
            {
                silentFrom = blocks;
                break;
            }
            delay.setPatch (patch, hostRate, 128);
            delay.process (0, x.data(), 128);
            if (! on)
                for (float s : x)
                    loudestAfter = std::max (loudestAfter, (double) std::abs (s));
        }
        const double seconds = silentFrom * 128.0 / hostRate;
        INFO ("stopped ringing after " << seconds << " s; loudest after switching off " << test::db (loudestAfter) << " dB");
        REQUIRE (silentFrom > 0);
        if (trails)
        {
            CHECK (seconds > 0.8);
            CHECK (seconds < 3.0);
            CHECK (loudestAfter > 0.05);
        }
        else
        {
            CHECK (silentFrom <= 40 + 6); // the 10 ms fade, give or take a block
        }

        // What it still holds is inaudible: running it on would add nothing above -90 dBFS
        double leftover = 0.0;
        for (int b = 0; b < 200; ++b)
        {
            std::fill (x.begin(), x.end(), 0.0f);
            delay.setPatch (patch, hostRate, 128);
            delay.process (0, x.data(), 128);
            for (float s : x)
                leftover = std::max (leftover, (double) std::abs (s));
        }
        INFO ("leftover " << test::db (leftover) << " dB");
        CHECK (leftover < 3.2e-5);
    }
}

TEST_CASE ("after a reset the delay repeats nothing it held before", "[pedals][delay]")
{
    // The line is forgotten rather than cleared, so what the reads reach is cleared as they reach it
    for (auto mode : modes)
    {
        INFO (modeName (mode));
        auto patch = delayPatch (mode, { { PID::DelayTime, 2000.0f }, { PID::DelayFeedback, 3.0f } });
        pedals::Delay delay;
        delay.prepare (hostRate);
        std::vector<float> x (256);
        const auto block = [&] (bool loud)
        {
            for (auto& v : x)
                v = loud ? 0.5f : 0.0f;
            delay.setPatch (patch, hostRate, (int) x.size());
            delay.process (0, x.data(), (int) x.size());
        };
        for (int b = 0; b < 600; ++b) // 3.2 s of a loud input
            block (true);
        delay.reset();
        patch[PID::DelayTime] = 300.0f;
        double worst = 0.0;
        for (int b = 0; b < 1000; ++b) // silence, the time jumping back to 2 s after 0.5 s
        {
            if (b == 94)
                patch[PID::DelayTime] = 2000.0f;
            block (false);
            for (float v : x)
                worst = std::max (worst, (double) std::abs (v));
        }
        CHECK (worst == 0.0);
    }
}
