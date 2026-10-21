#include "dsp/Cabinet.h"
#include "dsp/Metronome.h"
#include "dsp/Preset.h"
#include "dsp/Rig.h"
#include "dsp/looper/TempoGrid.h"
#include "ui/LoopEditMath.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <vector>

using namespace damp;
using Catch::Approx;

namespace
{
constexpr double rate = 48000.0;

float peak (const std::vector<float>& x, size_t from = 0, size_t to = SIZE_MAX)
{
    float p = 0.0f;
    for (size_t i = from; i < std::min (to, x.size()); ++i)
        p = std::max (p, std::abs (x[i]));
    return p;
}

double rms (const std::vector<float>& x, size_t from, size_t to)
{
    double sum = 0.0;
    for (size_t i = from; i < to; ++i)
        sum += (double) x[i] * x[i];
    return std::sqrt (sum / (double) (to - from));
}

float db (double x) { return (float) (20.0 * std::log10 (x)); }
} // namespace

// ======================================================================================
// TempoGrid

TEST_CASE ("tempo: bar lengths and where a quantised loop closes", "[tempo]")
{
    CHECK (tempo::barFrames (120.0, rate, 4) == 96000.0);
    CHECK (tempo::barFrames (100.0, rate, 3) == Approx (86400.0));
    const double bar = tempo::barFrames (100.0, rate, 4); // 115200
    CHECK (tempo::quantisedClose (149760, bar, 2400) == 230400); // 1.3 bars: bar 2
    CHECK (tempo::quantisedClose (115200 + 1440, bar, 2400) == 115200); // just past bar 1
    CHECK (tempo::quantisedClose (115200 + 2401, bar, 2400) == 230400);
    CHECK (tempo::quantisedClose (23040, bar, 2400) == 115200);  // under a bar: bar 1
    CHECK (tempo::quantisedClose (100, bar, 2400) == 115200);
    CHECK (tempo::quantisedClose (230400, bar, 2400) == 230400);
    // A bar of a non-integer length: bar k on round (k * bar)
    const double odd = tempo::barFrames (137.0, rate, 4);
    CHECK (tempo::quantisedClose (100000, odd, 0) == std::llround (2 * odd));
    CHECK (tempo::wholeBars (161280, bar) == 115200);
    CHECK (tempo::wholeBars (195840, bar) == 230400);
    CHECK (tempo::wholeBars (10, bar) == 115200);
}

TEST_CASE ("tempo: the tempo a loop implies", "[tempo]")
{
    // 4 s of 4/4: 60 BPM a bar (out of 70-180), 120 for two, 180 for three
    CHECK (tempo::tempoFromLoop (192000, rate, 4, 110.0) == Approx (120.0));
    CHECK (tempo::tempoFromLoop (192000, rate, 4, 75.0) == Approx (120.0));
    CHECK (tempo::tempoFromLoop (192000, rate, 4, 170.0) == Approx (180.0));
    CHECK (tempo::tempoFromLoop (192000, rate, 3, 100.0) == Approx (90.0)); // 3/4: 45 a bar, 90 for two
    // Even one bar faster than the range: that one, if it's a tempo at all
    CHECK (tempo::tempoFromLoop (57600, rate, 4, 120.0) == Approx (200.0));
    CHECK (! tempo::tempoFromLoop (24000, rate, 4, 120.0).has_value()); // 480 BPM
    CHECK (! tempo::tempoFromLoop (0, rate, 4, 120.0).has_value());
}

TEST_CASE ("tempo: tap tempo", "[tempo]")
{
    tempo::TapTempo tap;
    CHECK (! tap.tap (1000.0).has_value());
    CHECK (*tap.tap (1500.0) == Approx (120.0));
    // The mean of the last five intervals
    tap.tap (2000.0);
    tap.tap (2510.0);
    tap.tap (3000.0);
    CHECK (*tap.bpm() == Approx (60000.0 / 500.0));
    for (double t = 3480.0; t < 6000.0; t += 480.0)
        tap.tap (t);
    CHECK (*tap.bpm() == Approx (125.0));
    // A missed tap (twice the interval) is dropped
    CHECK (*tap.tap (5880.0 + 960.0) == Approx (125.0));
    // Over two seconds: start again
    CHECK (! tap.tap (9000.0).has_value());
    CHECK (*tap.tap (9400.0) == Approx (150.0));
    // Agreeing outliers: a new tempo
    tap.tap (9800.0);
    tap.tap (10800.0);
    CHECK (*tap.tap (11800.0) == Approx (60.0));
    // Clamped
    tempo::TapTempo fast;
    fast.tap (0.0);
    CHECK (*fast.tap (100.0) == Approx (tempo::maxBpm));
    tempo::TapTempo slow;
    slow.tap (0.0);
    CHECK (*slow.tap (1900.0) == Approx (tempo::minBpm));
}

TEST_CASE ("tempo: bars and beats in the editor's arithmetic", "[tempo][ui]")
{
    using namespace loopedit;
    CHECK (framesPerBar (120.0, rate, 4) == 96000.0);
    CHECK (framesPerBar (120.0, rate, 3) == 72000.0);
    CHECK (framesPerBar (120.0, rate, 7) == 168000.0);
    CHECK (barBeat (0, rate, 120.0, 3) == BarBeat { 1, 1 });
    CHECK (barBeat (72000 + 24000, rate, 120.0, 3) == BarBeat { 2, 2 });
    CHECK (barBeat (168000 - 1, rate, 120.0, 7) == BarBeat { 1, 7 });
    CHECK (barBeat (168000, rate, 120.0, 7) == BarBeat { 2, 1 });
    // Snapping an end to whole bars of 3/4
    CHECK (snapEnd (150000, 0, framesPerBar (120.0, rate, 3)) == 144000);
}

// ======================================================================================
// Parameters

TEST_CASE ("tempo: the master output, tempo and metronome live in the session, not presets", "[tempo][params]")
{
    for (PID p : { PID::MasterOutput, PID::Tempo, PID::TempoFollowHost, PID::LooperQuantise, PID::MetronomeOn, PID::MetronomeLevel,
                   PID::MetronomeAccent, PID::BeatsPerBar, PID::CountIn, PID::MetronomeRecOnly })
        CHECK (! inPresets (p));
    CHECK (inPresets (PID::OutputVolume));
    CHECK (paramInfo (PID::MasterOutput).id == "master_out");
    CHECK (paramInfo (PID::Master).id == "master");
    CHECK (paramInfo (PID::Tempo).def == 120.0f);
    CHECK (beatsPerBar ((int) paramInfo (PID::BeatsPerBar).def) == 4);

    Patch p = Patch::defaults();
    p[PID::MasterOutput] = 6.0f;
    p[PID::Tempo] = 90.0f;
    p[PID::MetronomeOn] = 1.0f;
    p[PID::BeatsPerBar] = 1.0f;
    p[PID::OutputVolume] = -5.0f;
    const auto text = serialisePreset (p, "Test");
    for (const char* id : { "master_out", "tempo", "metronome", "beats_per_bar", "count_in", "looper_quantise" })
        CHECK (text.find (id) == std::string::npos);
    const auto back = parsePreset (text).patch;
    CHECK (back[PID::MasterOutput] == 0.0f);
    CHECK (back[PID::Tempo] == 120.0f);
    CHECK (back[PID::OutputVolume] == -5.0f);
}

// ======================================================================================
// Metronome

namespace
{
std::vector<float> clickBlock (Metronome& m, std::vector<tempo::Click> clicks, size_t n = 4800)
{
    std::vector<float> x (n, 0.0f);
    m.render (x.data(), nullptr, (int) n, clicks.data(), (int) clicks.size());
    return x;
}
} // namespace

TEST_CASE ("metronome: deterministic ticks at the level, accents louder", "[tempo]")
{
    Metronome a, b;
    for (auto* m : { &a, &b })
    {
        m->prepare (rate);
        m->setPatch (-12.0f, true);
        m->reset();
    }
    const std::vector<tempo::Click> clicks { { 100, true }, { 2500, false } };
    const auto x = clickBlock (a, clicks), y = clickBlock (b, clicks);
    CHECK (x == y);

    // Each click restarts the voice from phase 0
    CHECK (x[100] == 0.0f);
    const float accent = peak (x, 100, 2500), plain = peak (x, 2500);
    CHECK (db (plain) == Approx (-12.0).margin (0.5));
    CHECK (db (accent) == Approx (-8.0).margin (0.5));
    CHECK (peak (x, 100 + 1920 + 1, 2500) == 0.0f); // cut at 40 ms
    CHECK (peak (x, 0, 100) == 0.0f);

    // Accent off: every click the same
    a.setPatch (-12.0f, false);
    a.reset();
    const auto z = clickBlock (a, { { 0, true }, { 2400, false } });
    for (size_t i = 0; i < 2400; ++i)
        REQUIRE (z[i] == z[i + 2400]);

    // No events: nothing added, stereo alike
    std::vector<float> l (1000, 0.25f), r (1000, -0.25f);
    a.render (l.data(), r.data(), 1000, nullptr, 0);
    CHECK (l == std::vector<float> (1000, 0.25f));
    CHECK (r == std::vector<float> (1000, -0.25f));
    a.setPatch (-48.0f, true);
    a.reset();
    CHECK (peak (clickBlock (a, { { 0, true } })) == 0.0f);
}

// ======================================================================================
// The rig: level before the looper, master after the clicks

namespace
{
struct LoopRig
{
    Rig rig;
    Patch patch = Patch::defaults();
    dustify::ConvolutionKernel passThrough { std::vector<float> { 1.0f }.data(), 1 };

    LoopRig()
    {
        patch[PID::Model] = (float) AmpModel::Clean;
        patch[PID::Gain] = 3.0f;
        patch[PID::GateOn] = 0.0f;
        patch[PID::Cab] = (float) CabType::Off;
        patch[PID::LooperOn] = 1.0f;
        rig.setPatch (patch);
        rig.prepare (rate, 1);
        rig.setCabKernelNow (&passThrough);
        rig.looper().setIdleTimeout (0.0);
        rig.looper().createPool (rate, 10.0);
        for (int t = 0; t < Looper::numTracks; ++t)
            rig.looper().setTrackCrossfade (t, 0.0f);
    }

    // n frames of `in` (repeated; silence if empty), mono
    std::vector<float> run (const std::vector<float>& in, size_t n)
    {
        std::vector<float> x (n, 0.0f);
        for (size_t i = 0; ! in.empty() && i < n; ++i)
            x[i] = in[i % in.size()];
        for (size_t pos = 0; pos < n; pos += 256)
        {
            const int m = (int) std::min<size_t> (256, n - pos);
            rig.setPatch (patch);
            rig.process (x.data() + pos, nullptr, m, true);
        }
        return x;
    }

    void record (const std::vector<float>& in, size_t n)
    {
        rig.looper().press (Looper::Action::Rec, 0);
        run (in, n);
        rig.looper().press (Looper::Action::Rec, 0);
        run ({}, 256);
    }
};

std::vector<float> tone (size_t n = 4800)
{
    std::vector<float> x (n);
    for (size_t i = 0; i < n; ++i)
        x[i] = 0.1f * (float) std::sin (2.0 * std::numbers::pi * 200.0 * (double) i / rate);
    return x;
}
} // namespace

TEST_CASE ("rig: the clicks are never recorded", "[tempo][rig]")
{
    LoopRig r;
    r.patch[PID::MetronomeOn] = 1.0f;
    r.patch[PID::MetronomeLevel] = 0.0f;
    r.rig.looper().press (Looper::Action::Rec, 0);
    const auto heard = r.run ({}, 48000);
    CHECK (peak (heard) > 0.5f);
    r.rig.looper().press (Looper::Action::Rec, 0);
    r.patch[PID::MetronomeOn] = 0.0f;
    const auto played = r.run ({}, 96000);
    CHECK (r.rig.looper().trackStatus (0).state == Looper::State::Playing);
    CHECK (peak (played) < 1.0e-6f);
    std::vector<float> l, rr;
    REQUIRE (r.rig.looper().copyTrackAudio (0, l, rr));
    CHECK (peak (l) < 1.0e-6f);
}

TEST_CASE ("rig: the master output scales the loops as heard, not as recorded; the level is recorded", "[tempo][rig]")
{
    LoopRig r;
    r.patch[PID::OutputVolume] = -9.0f;
    r.record (tone(), 48000);
    std::vector<float> before, unused;
    REQUIRE (r.rig.looper().copyTrackAudio (0, before, unused));

    auto loopRms = [&]
    {
        r.run ({}, 9600); // the smoothers settle
        const auto y = r.run ({}, 48000);
        return rms (y, 0, y.size());
    };
    const double base = loopRms();
    CHECK (base > 0.005);

    // Level: the loop as it was; the live sound follows
    r.patch[PID::OutputVolume] = 3.0f;
    CHECK (db (loopRms() / base) == Approx (0.0).margin (0.01));

    // Master: everything, by its gain; the recording untouched
    r.patch[PID::MasterOutput] = -12.0f;
    CHECK (db (loopRms() / base) == Approx (-12.0).margin (0.05));
    std::vector<float> after;
    REQUIRE (r.rig.looper().copyTrackAudio (0, after, unused));
    CHECK (after == before);
    r.patch[PID::MasterOutput] = -48.0f;
    CHECK (loopRms() < 1.0e-6); // (the smoother still settling)

    // Live: the level and master both apply
    LoopRig live;
    live.patch[PID::LooperOn] = 0.0f;
    auto liveRms = [&]
    {
        live.run (tone(), 9600);
        const auto y = live.run (tone(), 48000);
        return rms (y, 0, y.size());
    };
    live.patch[PID::OutputVolume] = -9.0f;
    const double quiet = liveRms();
    live.patch[PID::OutputVolume] = 3.0f;
    CHECK (db (liveRms() / quiet) == Approx (12.0).margin (0.05));
    live.patch[PID::MasterOutput] = 6.0f;
    CHECK (db (liveRms() / quiet) == Approx (18.0).margin (0.05));
}
