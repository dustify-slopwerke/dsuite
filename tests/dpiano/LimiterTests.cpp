#include "dsp/Piano.h"
#include "dsp/Preset.h"

#include "common/dsp/Noise.h"
#include "common/dsp/PeakLimiter.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using dustify::PeakLimiter;

namespace
{
const float ceiling = PeakLimiter().ceilingLevel(); // -1 dBFS by default

struct Stereo
{
    std::vector<float> left, right;
};

// Runs a copy through a fresh limiter in uneven blocks (1 .. 700 samples)
Stereo limit (const Stereo& in, double rate, PeakLimiter* keep = nullptr)
{
    PeakLimiter local;
    auto& limiter = keep != nullptr ? *keep : local;
    limiter.prepare (rate);
    auto out = in;
    dustify::Random rng (7);
    for (size_t pos = 0; pos < out.left.size();)
    {
        const auto n = std::min<size_t> (1 + rng.nextInt() % 700, out.left.size() - pos);
        limiter.process (out.left.data() + pos, out.right.data() + pos, (int) n);
        pos += n;
    }
    return out;
}

float peakOf (const Stereo& x)
{
    float peak = 0.0f;
    for (size_t i = 0; i < x.left.size(); ++i)
        peak = std::max ({ peak, std::abs (x.left[i]), std::abs (x.right[i]) });
    return peak;
}

// Below the ceiling nothing touches the signal, and above it the gain does the limiting (the same
// on both channels), not the guard clamp
void checkLimited (const Stereo& in, const Stereo& out, int latency)
{
    REQUIRE (peakOf (out) <= ceiling);
    int checked = 0;
    for (size_t i = (size_t) latency; i < out.left.size(); ++i)
    {
        const float l = in.left[i - (size_t) latency], r = in.right[i - (size_t) latency];
        if (std::abs (l) > 1.0e-2f && std::abs (r) > 1.0e-2f)
        {
            const float gainLeft = out.left[i] / l, gainRight = out.right[i] / r;
            if (std::abs (gainLeft - gainRight) > 1.0e-5f)
            {
                INFO ("sample " << i << ": gain " << gainLeft << " left, " << gainRight << " right");
                REQUIRE (std::abs (gainLeft - gainRight) <= 1.0e-5f);
            }
            REQUIRE (gainLeft > 0.0f);
            REQUIRE (gainLeft <= 1.0f);
            ++checked;
        }
    }
    CHECK (checked > 0);
}
} // namespace

TEST_CASE ("peak limiter: about 1 ms of look-ahead, reported as its latency", "[limiter]")
{
    const auto [rate, expected] = GENERATE (std::pair { 44100.0, 44 }, std::pair { 48000.0, 48 }, std::pair { 96000.0, 96 });
    PeakLimiter limiter;
    limiter.prepare (rate);
    CHECK (limiter.latencySamples() == expected);

    // An impulse comes out exactly that late, untouched
    Stereo in { std::vector<float> (1000, 0.0f), std::vector<float> (1000, 0.0f) };
    in.left[100] = 0.5f;
    in.right[100] = -0.25f;
    const auto out = limit (in, rate);
    for (size_t i = 0; i < out.left.size(); ++i)
    {
        CHECK (out.left[i] == (i == 100 + (size_t) expected ? 0.5f : 0.0f));
        CHECK (out.right[i] == (i == 100 + (size_t) expected ? -0.25f : 0.0f));
    }

    dpiano::Piano piano;
    piano.prepare (rate);
    CHECK (piano.latencySamples() == expected);
}

TEST_CASE ("peak limiter: below the ceiling the signal passes bit for bit, only delayed", "[limiter]")
{
    const double rate = GENERATE (44100.0, 48000.0, 96000.0);
    dustify::Random rng (11);
    Stereo in;
    for (int i = 0; i < (int) rate; ++i)
    {
        in.left.push_back (0.99f * ceiling * rng.bipolar());
        in.right.push_back (0.99f * ceiling * rng.bipolar());
    }
    in.left[5000] = ceiling; // touching the ceiling isn't over it
    PeakLimiter limiter;
    const auto out = limit (in, rate, &limiter);
    const auto latency = (size_t) limiter.latencySamples();
    for (size_t i = 0; i < out.left.size(); ++i)
    {
        REQUIRE (out.left[i] == (i < latency ? 0.0f : in.left[i - latency]));
        REQUIRE (out.right[i] == (i < latency ? 0.0f : in.right[i - latency]));
    }
    CHECK (limiter.currentGain() == 1.0f);
    CHECK (limiter.takeLowestGain() == 1.0f);
}

TEST_CASE ("peak limiter: never over the ceiling, on random bursts and full-scale squares", "[limiter]")
{
    const double rate = GENERATE (44100.0, 48000.0, 96000.0);
    const int latency = (int) std::lround (0.001 * rate);
    dustify::Random rng (23);

    SECTION ("random fuzz with level jumps")
    {
        Stereo in;
        float level = 1.0f;
        for (int i = 0; i < 4 * (int) rate; ++i)
        {
            if (rng.nextInt() % 500 == 0)
                level = std::pow (10.0f, 1.5f * rng.bipolar()); // -30 .. +30 dB
            const float spike = rng.nextInt() % 3000 == 0 ? 20.0f : 1.0f;
            in.left.push_back (level * spike * rng.bipolar());
            in.right.push_back (level * rng.bipolar());
        }
        checkLimited (in, limit (in, rate), latency);
    }

    SECTION ("full-scale and hotter square bursts")
    {
        for (const float amplitude : { 1.0f, 4.0f })
        {
            Stereo in;
            for (int burst = 0; burst < 40; ++burst)
            {
                const int on = 1 + (int) (rng.nextInt() % 2000), off = (int) (rng.nextInt() % 6000);
                const int period = 2 + (int) (rng.nextInt() % 200);
                for (int i = 0; i < on; ++i)
                {
                    const float s = (i % period) < period / 2 ? amplitude : -amplitude;
                    in.left.push_back (s);
                    in.right.push_back (burst % 3 == 0 ? 0.5f * s : s);
                }
                for (int i = 0; i < off; ++i)
                {
                    in.left.push_back (0.0f);
                    in.right.push_back (0.0f);
                }
            }
            checkLimited (in, limit (in, rate), latency);
        }
    }
}

TEST_CASE ("peak limiter: ramps down ahead of a peak and releases back to exactly 1", "[limiter]")
{
    constexpr double rate = 48000.0;
    // A quiet tone, a 4x burst for 10 ms, then the quiet tone again
    Stereo in;
    for (int i = 0; i < (int) (2.0 * rate); ++i)
    {
        const float level = i >= 24000 && i < 24480 ? 4.0f : 0.1f;
        const float s = level * std::sin (2.0f * 3.14159265f * 440.0f * (float) i / (float) rate);
        in.left.push_back (s);
        in.right.push_back (s);
    }
    PeakLimiter limiter;
    limiter.prepare (rate);
    auto out = in;
    std::vector<float> gain;
    for (size_t i = 0; i < out.left.size(); ++i)
    {
        limiter.process (out.left.data() + i, out.right.data() + i, 1);
        gain.push_back (limiter.currentGain());
    }
    CHECK (peakOf (out) <= ceiling);
    const auto latency = (size_t) limiter.latencySamples();
    const size_t burstOut = 24000 + latency, endOut = 24480 + latency;

    // The attack is a ramp over the look-ahead, not a step
    CHECK (gain[24000 - 1] == 1.0f);
    for (size_t i = 24000; i < burstOut; ++i)
        CHECK (gain[i - 1] - gain[i] < 1.5f / (float) latency);
    const auto loudest = (size_t) std::distance (in.left.begin(), std::max_element (in.left.begin(), in.left.end()));
    CHECK (gain[loudest + latency] <= ceiling / in.left[loudest]);
    CHECK (gain[burstOut + latency] < 0.25f);

    // ~80 ms release: about two thirds back after 80 ms, not there yet after 20 ms, exactly 1 later
    const float deepest = *std::min_element (gain.begin(), gain.end());
    CHECK (gain[endOut + 960] < 0.9f);
    CHECK (gain[endOut + 3840] > 1.0f - 0.45f * (1.0f - deepest));
    CHECK (gain[endOut + 3840] < 1.0f);
    CHECK (gain[(size_t) (1.5 * rate)] == 1.0f);
    for (size_t i = (size_t) (1.5 * rate); i < out.left.size(); ++i)
        REQUIRE (out.left[i] == in.left[i - latency]);
}

TEST_CASE ("the piano's limiter holds fortissimo chords at -1 dBFS, even at full volume", "[piano][limiter]")
{
    const double rate = GENERATE (44100.0, 48000.0, 96000.0);
    int count = 0;
    for (const auto& entry : std::filesystem::directory_iterator (DPIANO_PRESET_DIR))
    {
        std::ifstream file (entry.path());
        const std::string text { std::istreambuf_iterator<char> (file), {} };
        auto patch = dpiano::parsePreset (text).patch;
        patch[dpiano::PID::MasterVolume] = 12.0f;
        INFO (entry.path().filename().string() << " at " << rate << " Hz");

        dpiano::Piano piano;
        piano.setPatch (patch);
        piano.prepare (rate);
        piano.sustainPedal (true);
        std::vector<float> left ((size_t) (0.6 * rate)), right (left.size());
        float peak = 0.0f;
        for (int chord = 0; chord < 3; ++chord)
        {
            for (int note : { 21, 33, 36, 48, 55, 60, 64, 67, 72, 84, 96 })
                piano.noteOn (note + chord, 1.0f);
            piano.process (left.data(), right.data(), (int) left.size());
            for (size_t i = 0; i < left.size(); ++i)
                peak = std::max ({ peak, std::abs (left[i]), std::abs (right[i]) });
        }
        CHECK (peak <= ceiling);
        CHECK (peak > 0.8f * ceiling); // and it is limiting, not silent
        ++count;
    }
    CHECK (count >= 10);
}
