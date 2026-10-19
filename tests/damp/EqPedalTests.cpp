#include "common/Analysis.h"
#include "damp/PedalHelpers.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <random>
#include <utility>

using namespace damp;
using namespace damp::pedaltest;

namespace
{
using Settings = std::initializer_list<std::pair<PID, float>>;
constexpr double hostRate = 44100.0;
constexpr PID bands[] { PID::Eq100, PID::Eq200, PID::Eq400, PID::Eq800, PID::Eq1k6, PID::Eq3k2, PID::Eq6k4 };
constexpr double bandHz[] { 100.0, 200.0, 400.0, 800.0, 1600.0, 3200.0, 6400.0 };

// The pedal's steady-state gain for a sine at f, dB
double gainDb (double f, Settings settings, double rate = highRate)
{
    const auto x = sine (f, 0.5, 0.1, rate);
    const auto y = runPedal (PedalId::Eq, x, settings, rate);
    return test::db (test::rms (y, y.size() / 2) / test::rms (x, x.size() / 2));
}
} // namespace

TEST_CASE ("Graphic EQ: flat is transparent", "[damp][pedal][eq]")
{
    for (double rate : { hostRate, highRate })
        for (double f : { 50.0, 100.0, 300.0, 1000.0, 5000.0, 12000.0 })
        {
            INFO (rate << " Hz rate, " << f << " Hz");
            CHECK (std::abs (gainDb (f, {}, rate)) < 0.01);
        }
}

TEST_CASE ("Graphic EQ: each band boosts and cuts at its centre, little two octaves off", "[damp][pedal][eq]")
{
    for (double rate : { hostRate, highRate })
        for (size_t b = 0; b < std::size (bands); ++b)
        {
            INFO (rate << " Hz rate, band " << bandHz[b] << " Hz");
            const Settings boost { { bands[b], 12.0f } }, cut { { bands[b], -12.0f } };
            CHECK (std::abs (gainDb (bandHz[b], boost, rate) - 12.0) < 0.5);
            CHECK (std::abs (gainDb (bandHz[b], cut, rate) + 12.0) < 0.5);
            CHECK (std::abs (gainDb (bandHz[b] / 4.0, boost, rate)) < 3.0);
            if (bandHz[b] * 4.0 < 0.4 * rate)
                CHECK (std::abs (gainDb (bandHz[b] * 4.0, boost, rate)) < 3.0);
        }
}

TEST_CASE ("Graphic EQ: neighbouring bands combine smoothly", "[damp][pedal][eq]")
{
    // Two neighbours at +12: no dip between them, no big bump at their centres
    const Settings pair { { PID::Eq800, 12.0f }, { PID::Eq1k6, 12.0f } };
    const double between = gainDb (800.0 * std::sqrt (2.0), pair);
    CHECK (between > 10.5);
    CHECK (gainDb (800.0, pair) < between + 4.0);
}

TEST_CASE ("Graphic EQ: level", "[damp][pedal][eq]")
{
    CHECK (std::abs (gainDb (1000.0, { { PID::EqLevel, 6.0f } }) - 6.0) < 0.05);
    CHECK (std::abs (gainDb (1000.0, { { PID::EqLevel, -15.0f } }) + 15.0) < 0.05);
}

TEST_CASE ("Graphic EQ: stable and finite at the extremes", "[damp][pedal][eq]")
{
    std::mt19937 rng (7);
    std::normal_distribution<float> noise (0.0f, 0.3f);
    for (double rate : { hostRate, highRate, 1536000.0 })
        for (float db : { 15.0f, -15.0f })
        {
            std::vector<float> x ((size_t) (0.3 * rate));
            for (auto& v : x)
                v = noise (rng);
            x[x.size() / 3] = 1.0e4f; // a huge spike
            auto patch = Patch::defaults();
            for (auto pid : bands)
                patch[pid] = db;
            patch[PID::EqLevel] = 15.0f;
            pedals::Eq eq;
            eq.prepare (rate);
            // Every slider thrown end to end each block
            for (size_t pos = 0; pos < x.size(); pos += 256)
            {
                const int n = (int) std::min<size_t> (256, x.size() - pos);
                eq.setPatch (patch, rate, n);
                eq.process (0, x.data() + pos, n);
                for (auto pid : bands)
                    patch[pid] = -patch[pid];
            }
            bool finite = true;
            float peak = 0.0f;
            for (float v : x)
            {
                finite = finite && std::isfinite (v);
                peak = std::max (peak, std::abs (v));
            }
            INFO (rate << " Hz, " << db << " dB");
            CHECK (finite);
            CHECK (peak < 1.0e6f);
            // And it settles: silence after
            std::vector<float> tail ((size_t) (0.5 * rate), 0.0f);
            for (size_t pos = 0; pos < tail.size(); pos += 256)
            {
                const int n = (int) std::min<size_t> (256, tail.size() - pos);
                eq.setPatch (patch, rate, n);
                eq.process (0, tail.data() + pos, n);
            }
            CHECK (std::abs (tail.back()) < 1.0e-6f);
        }
}
