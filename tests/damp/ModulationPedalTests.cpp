#include "common/Analysis.h"
#include "damp/PedalHelpers.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <random>
#include <utility>

using namespace damp;
using namespace damp::pedaltest;

namespace
{
using Settings = std::initializer_list<std::pair<PID, float>>;
constexpr double hostRate = 48000.0;

Patch patchWith (Settings settings)
{
    auto patch = Patch::defaults();
    for (auto [pid, v] : settings)
        patch[pid] = v;
    return patch;
}

std::vector<float> noise (size_t n, float amplitude, unsigned seed = 1)
{
    std::mt19937 rng (seed);
    std::uniform_real_distribution<float> u (-amplitude, amplitude);
    std::vector<float> x (n);
    for (auto& v : x)
        v = u (rng);
    return x;
}

// Both channels through processStereo, as after the amp with a stereo output
std::pair<std::vector<float>, std::vector<float>> runStereo (PedalId id, std::vector<float> left, std::vector<float> right,
                                                             Settings settings, double rate = hostRate)
{
    const auto patch = patchWith (settings);
    auto pedal = makePedal (id);
    pedal->prepare (rate);
    for (size_t pos = 0; pos < left.size(); pos += 256)
    {
        const int n = (int) std::min<size_t> (256, left.size() - pos);
        pedal->setPatch (patch, rate, n);
        pedal->processStereo (left.data() + pos, right.data() + pos, n);
    }
    return { std::move (left), std::move (right) };
}

// Magnitude response (dB) of the pedal around `at` seconds in: an impulse then, in the middle of
// a window of n samples (the analysis window would otherwise hide it). Bin k is k * rate / n Hz.
std::vector<double> responseDb (PedalId id, Settings settings, double at, size_t n = 16384)
{
    const auto start = std::max ((size_t) (at * hostRate), n / 2);
    std::vector<float> x (start + n / 2, 0.0f);
    x[start] = 1.0f;
    const auto y = runPedal (id, x, settings, hostRate);
    const auto mag = test::magnitudeSpectrum (y, start - n / 2, n);
    std::vector<double> out (mag.size());
    for (size_t i = 0; i < mag.size(); ++i)
        out[i] = test::db (mag[i]);
    return out;
}

double binHz (const std::vector<double>& response) { return hostRate / (2.0 * (double) response.size()); }

double medianDb (const std::vector<double>& response, double lo, double hi)
{
    std::vector<double> v (response.begin() + (long) (lo / binHz (response)), response.begin() + (long) (hi / binHz (response)));
    std::nth_element (v.begin(), v.begin() + (long) v.size() / 2, v.end());
    return v[v.size() / 2];
}

// Frequencies of the local minima between lo and hi Hz at least `belowDb` under the median there
std::vector<double> notches (const std::vector<double>& response, double lo, double hi, double belowDb = 10.0)
{
    const double floor = medianDb (response, lo, hi) - belowDb;
    std::vector<double> found;
    for (auto i = (size_t) (lo / binHz (response)); i < (size_t) (hi / binHz (response)); ++i)
        if (response[i] < floor && response[i] <= response[i - 1] && response[i] < response[i + 1])
            found.push_back ((double) i * binHz (response));
    return found;
}

// RMS in consecutive windows
std::vector<double> envelope (const std::vector<float>& y, size_t window, size_t from = 0)
{
    std::vector<double> e;
    for (size_t pos = from; pos + window <= y.size(); pos += window)
        e.push_back (test::rms (y, pos, pos + window));
    return e;
}

double correlation (const std::vector<double>& a, const std::vector<double>& b)
{
    const size_t n = std::min (a.size(), b.size());
    double ma = 0, mb = 0;
    for (size_t i = 0; i < n; ++i)
    {
        ma += a[i] / (double) n;
        mb += b[i] / (double) n;
    }
    double ab = 0, aa = 0, bb = 0;
    for (size_t i = 0; i < n; ++i)
    {
        ab += (a[i] - ma) * (b[i] - mb);
        aa += (a[i] - ma) * (a[i] - ma);
        bb += (b[i] - mb) * (b[i] - mb);
    }
    return ab / std::sqrt (aa * bb);
}

// Average spacing in seconds between upward crossings of a track's mean (with some hysteresis)
double crossingPeriod (const std::vector<double>& track, double secondsPerPoint)
{
    const auto [lo, hi] = std::minmax_element (track.begin(), track.end());
    const double mid = 0.5 * (*lo + *hi), band = 0.1 * (*hi - *lo);
    std::vector<size_t> ups;
    bool below = track[0] < mid;
    for (size_t i = 1; i < track.size(); ++i)
    {
        if (below && track[i] > mid + band)
        {
            ups.push_back (i);
            below = false;
        }
        else if (! below && track[i] < mid - band)
            below = true;
    }
    if (ups.size() < 2)
        return 0.0;
    return (double) (ups.back() - ups.front()) / (double) (ups.size() - 1) * secondsPerPoint;
}

// Pitch in cents (against `reference`) cycle by cycle from upward zero crossings, and the time
// between track points
std::vector<double> pitchTrack (const std::vector<float>& y, double reference, size_t from)
{
    std::vector<double> track;
    double last = -1.0;
    for (size_t i = from + 1; i < y.size(); ++i)
        if (y[i - 1] < 0.0f && y[i] >= 0.0f)
        {
            const double t = (double) (i - 1) + y[i - 1] / (double) (y[i - 1] - y[i]);
            if (last >= 0.0)
                track.push_back (test::cents (hostRate / (t - last), reference));
            last = t;
        }
    return track;
}

double swingCents (const std::vector<double>& track)
{
    const auto [lo, hi] = std::minmax_element (track.begin(), track.end());
    return 0.5 * (*hi - *lo);
}
} // namespace

TEST_CASE ("chorus and flanger combs move with the sweep", "[pedals][modulation]")
{
    // At the two ends of the sweep the delay is longest and shortest: many more notches at one
    // than the other (the LFO starts at the middle, rising)
    struct Case
    {
        PedalId id;
        Settings settings;
        double lo, hi;
    };
    const Case cases[] {
        { PedalId::Chorus, { { PID::ChorusRate, 0.5f }, { PID::ChorusDepth, 10.0f } }, 200.0, 3000.0 },
        { PedalId::Flanger, { { PID::FlangerRate, 0.5f }, { PID::FlangerRegen, 0.0f } }, 200.0, 10000.0 },
    };
    for (const auto& c : cases)
    {
        const auto longest = notches (responseDb (c.id, c.settings, 0.5), c.lo, c.hi);
        const auto shortest = notches (responseDb (c.id, c.settings, 1.5), c.lo, c.hi);
        CAPTURE ((int) c.id, longest.size(), shortest.size());
        CHECK (shortest.size() >= 3);
        CHECK (longest.size() > 2 * shortest.size());
    }

    // Chorus notches at the right spacing: 4 and 12 ms delays (1 / delay apart)
    const auto chorusShort = notches (responseDb (PedalId::Chorus, cases[0].settings, 1.5), 200.0, 3000.0);
    const double spacing = (chorusShort.back() - chorusShort.front()) / (double) (chorusShort.size() - 1);
    CHECK (std::abs (spacing - 250.0) < 25.0);
}

TEST_CASE ("flanger regeneration sharpens the comb and stays bounded", "[pedals][modulation]")
{
    // Static (no sweep) at 1.55 ms: peak to median grows with regeneration, notches stay deep
    auto peakOverMedian = [] (float regen)
    {
        const auto r = responseDb (PedalId::Flanger, { { PID::FlangerDepth, 0.0f }, { PID::FlangerRegen, regen } }, 0.2);
        const double peak = *std::max_element (r.begin() + (long) (200.0 / binHz (r)), r.begin() + (long) (5000.0 / binHz (r)));
        return peak - medianDb (r, 200.0, 5000.0);
    };
    const double none = peakOverMedian (0.0f), half = peakOverMedian (5.0f), full = peakOverMedian (10.0f);
    CAPTURE (none, half, full);
    CHECK (none < 5.0);
    CHECK (half > none + 3.0);
    CHECK (full > half + 6.0);
    const auto deep = notches (responseDb (PedalId::Flanger, { { PID::FlangerDepth, 0.0f }, { PID::FlangerRegen, 10.0f } }, 0.2),
                               200.0, 5000.0, 20.0);
    CHECK (deep.size() >= 6); // 1 / 1.55 ms apart

    // Hot input at full regeneration, a deep fast sweep and the shortest delays
    for (double rate : { hostRate, 1536000.0 })
    {
        const auto y = runPedal (PedalId::Flanger, noise ((size_t) (0.3 * rate), 1.0f),
                                 { { PID::FlangerRegen, 10.0f }, { PID::FlangerDepth, 10.0f }, { PID::FlangerManual, 0.0f },
                                   { PID::FlangerRate, 5.0f } },
                                 rate);
        float peak = 0.0f;
        for (float v : y)
            peak = std::max (peak, std::abs (v));
        CAPTURE (rate);
        CHECK (std::isfinite (peak));
        CHECK (peak < 2.0f);
    }
}

TEST_CASE ("phaser notches: half as many as stages, moving with the sweep", "[pedals][modulation]")
{
    for (int stages = 0; stages < 4; ++stages)
    {
        const auto r = responseDb (PedalId::Phaser, { { PID::PhaserDepth, 0.0f }, { PID::PhaserFeedback, 0.0f },
                                                      { PID::PhaserStages, (float) stages } },
                                   0.2);
        static constexpr int counts[] { 4, 6, 8, 12 };
        CAPTURE (counts[stages]);
        CHECK ((int) notches (r, 30.0, 20000.0, 20.0).size() == counts[stages] / 2);
    }

    // The sweep starts at the middle, rising: the top of the sweep at 0.5 s, the bottom at 1.5 s
    const Settings sweeping { { PID::PhaserRate, 0.5f }, { PID::PhaserFeedback, 0.0f } };
    const auto high = notches (responseDb (PedalId::Phaser, sweeping, 0.5), 30.0, 20000.0, 20.0);
    const auto low = notches (responseDb (PedalId::Phaser, sweeping, 1.5), 30.0, 20000.0, 20.0);
    REQUIRE (! high.empty());
    REQUIRE (! low.empty());
    CAPTURE (high.front(), low.front());
    CHECK (high.front() > 8.0 * low.front());

    // Feedback raises a resonance between the notches
    auto peakOverMedian = [] (float feedback)
    {
        const auto r = responseDb (PedalId::Phaser, { { PID::PhaserDepth, 0.0f }, { PID::PhaserFeedback, feedback } }, 0.2);
        return *std::max_element (r.begin() + 2, r.end()) - medianDb (r, 30.0, 20000.0);
    };
    CHECK (peakOverMedian (10.0f) > peakOverMedian (0.0f) + 6.0);
}

TEST_CASE ("tremolo modulates at its rate and depth", "[pedals][modulation]")
{
    const auto x = sine (1000.0, 2.0, 0.1, hostRate);
    const size_t window = 48, settle = 14400; // 1 ms windows (one cycle), after 0.3 s
    const double secondsPerPoint = (double) window / hostRate;

    SECTION ("sine: depth and rate")
    {
        for (float rate : { 2.0f, 5.0f, 11.0f })
        {
            const auto y = runPedal (PedalId::Tremolo, x, { { PID::TremoloShape, 0.0f }, { PID::TremoloRate, rate } }, hostRate);
            const auto e = envelope (y, window, settle);
            const auto [lo, hi] = std::minmax_element (e.begin(), e.end());
            CAPTURE (rate, *lo / *hi);
            CHECK (std::abs (*lo / *hi - 0.4) < 0.03); // depth 6: down to 40 %
            CHECK (std::abs (crossingPeriod (e, secondsPerPoint) * rate - 1.0) < 0.02);
        }
    }

    SECTION ("every shape modulates and keeps the loudness")
    {
        for (int shape = 0; shape < 5; ++shape)
        {
            const auto y = runPedal (PedalId::Tremolo, x, { { PID::TremoloShape, (float) shape } }, hostRate);
            const auto e = envelope (y, window, settle);
            const auto [lo, hi] = std::minmax_element (e.begin(), e.end());
            CAPTURE (shape, *lo / *hi);
            CHECK (*lo / *hi < 0.6);
            CHECK (std::abs (test::db (test::rms (y, settle)) - test::db (test::rms (x, settle))) < 1.0);
            CHECK (std::abs (crossingPeriod (e, secondsPerPoint) * 5.0 - 1.0) < 0.03);
        }
    }

    SECTION ("opto: the dips fall faster than they recover")
    {
        const auto y = runPedal (PedalId::Tremolo, x, { { PID::TremoloShape, 3.0f }, { PID::TremoloRate, 3.0f } }, hostRate);
        const auto e = envelope (y, window, settle);
        const auto [lo, hi] = std::minmax_element (e.begin(), e.end());
        // Count the windows spent falling and rising through the middle 80 % of the swing
        int falling = 0, rising = 0;
        for (size_t i = 1; i < e.size(); ++i)
        {
            const double v = (e[i] - *lo) / (*hi - *lo);
            if (v > 0.1 && v < 0.9)
                (e[i] < e[i - 1] ? falling : rising)++;
        }
        CAPTURE (falling, rising);
        CHECK (rising > 2 * falling);
    }

    SECTION ("square: edges slewed, no clicks")
    {
        const std::vector<float> dc (48000, 0.5f);
        const auto y = runPedal (PedalId::Tremolo, dc, { { PID::TremoloShape, 2.0f }, { PID::TremoloRate, 15.0f }, { PID::TremoloDepth, 10.0f } },
                                 hostRate);
        float biggestStep = 0.0f, lo = 1.0f, hi = 0.0f;
        for (size_t i = 4800; i < y.size(); ++i)
        {
            biggestStep = std::max (biggestStep, std::abs (y[i] - y[i - 1]));
            lo = std::min (lo, y[i]);
            hi = std::max (hi, y[i]);
        }
        CAPTURE (biggestStep, lo, hi);
        CHECK (lo < 0.05f * hi);                     // full depth
        CHECK (biggestStep < (hi - lo) / 100.0f); // each edge spread over more than 2 ms
    }

    SECTION ("harmonic: lows and highs in opposite phase")
    {
        const size_t slow = 960; // 20 ms windows, for the 150 Hz tone
        const auto lows = runPedal (PedalId::Tremolo, sine (150.0, 2.0, 0.1, hostRate), { { PID::TremoloShape, 4.0f } }, hostRate);
        const auto highs = runPedal (PedalId::Tremolo, sine (6000.0, 2.0, 0.1, hostRate), { { PID::TremoloShape, 4.0f } }, hostRate);
        const double r = correlation (envelope (lows, slow, settle), envelope (highs, slow, settle));
        CAPTURE (r);
        CHECK (r < -0.8);
    }

    SECTION ("pan: left and right in opposite phase in stereo, ordinary tremolo in mono")
    {
        const auto [left, right] = runStereo (PedalId::Tremolo, x, x, { { PID::TremoloShape, 0.0f }, { PID::TremoloPan, 1.0f } });
        const double r = correlation (envelope (left, window, settle), envelope (right, window, settle));
        CAPTURE (r);
        CHECK (r < -0.95);
        CHECK (runPedal (PedalId::Tremolo, x, { { PID::TremoloPan, 1.0f } }, hostRate) == runPedal (PedalId::Tremolo, x, {}, hostRate));
        const auto [l2, r2] = runStereo (PedalId::Tremolo, x, x, { { PID::TremoloPan, 0.0f } });
        CHECK (l2 == r2);
    }
}

TEST_CASE ("vibrato bends the pitch by its depth at its rate", "[pedals][modulation]")
{
    const auto x = sine (1000.0, 1.5, 0.1, hostRate);
    const size_t settle = 14400;
    auto run = [&] (float depth, float rate, float mode = 0.0f)
    {
        const auto y = runPedal (PedalId::Vibrato, x, { { PID::VibratoDepth, depth }, { PID::VibratoRate, rate }, { PID::VibratoMode, mode } },
                                 hostRate);
        return pitchTrack (y, 1000.0, settle);
    };

    const auto full = run (10.0f, 5.0f), half = run (5.0f, 5.0f);
    const double fullCents = swingCents (full), halfCents = swingCents (half);
    CAPTURE (fullCents, halfCents);
    CHECK (fullCents > 40.0);
    CHECK (fullCents < 60.0);
    CHECK (std::abs (halfCents / fullCents - 0.5) < 0.05);
    CHECK (swingCents (run (0.0f, 5.0f)) < 0.5);

    // One swing per LFO cycle (the track has a point per 1 kHz cycle)
    for (float rate : { 2.0f, 5.0f, 9.0f })
    {
        const double period = crossingPeriod (run (6.0f, rate), 1.0e-3);
        CAPTURE (rate, period);
        CHECK (std::abs (period * rate - 1.0) < 0.03);
    }

    // Vibe bends the pitch too, at the same rate, and is wet only (no comb against the dry)
    const auto vibe = run (6.0f, 4.0f, 1.0f);
    CHECK (swingCents (vibe) > 5.0);
    CHECK (std::abs (crossingPeriod (vibe, 1.0e-3) * 4.0 - 1.0) < 0.03);
    const auto y = runPedal (PedalId::Vibrato, x, { { PID::VibratoMode, 1.0f } }, hostRate);
    const auto e = envelope (y, 48, settle);
    const auto [lo, hi] = std::minmax_element (e.begin(), e.end());
    CHECK (test::db (*hi / *lo) < 2.0);
}

TEST_CASE ("modulation pedals spread a mono input in stereo only as set", "[pedals][modulation]")
{
    const auto x = noise (24000, 0.2f);
    auto spread = [&] (PedalId id, Settings settings)
    {
        const auto [left, right] = runStereo (id, x, x, settings);
        std::vector<float> difference (left.size());
        for (size_t i = 0; i < left.size(); ++i)
            difference[i] = left[i] - right[i];
        return test::rms (difference, 4800) / test::rms (left, 4800);
    };
    CHECK (spread (PedalId::Chorus, {}) > 0.1);
    CHECK (spread (PedalId::Chorus, { { PID::ChorusWidth, 0.0f } }) == 0.0);
    CHECK (spread (PedalId::Flanger, {}) > 0.1);
    CHECK (spread (PedalId::Phaser, {}) > 0.1);
    CHECK (spread (PedalId::Tremolo, { { PID::TremoloPan, 1.0f } }) > 0.1);
    CHECK (spread (PedalId::Tremolo, {}) == 0.0);
    CHECK (spread (PedalId::Vibrato, {}) == 0.0);
}

TEST_CASE ("modulation LFOs keep time across blocks and channels", "[pedals][modulation]")
{
    const auto x = noise (48000, 0.2f);
    const Settings cases[] { {}, { { PID::TremoloShape, 2.0f } }, { { PID::TremoloShape, 4.0f }, { PID::VibratoMode, 1.0f } } };
    for (int id = (int) PedalId::Chorus; id <= (int) PedalId::Vibrato; ++id)
        for (const auto& settings : cases)
        {
            const auto reference = runPedal ((PedalId) id, x, settings, hostRate); // blocks of 256

            // Blocks of all sizes (some long enough to need a coarser control schedule), each
            // channel processed on its own
            const auto patch = patchWith (settings);
            auto pedal = makePedal ((PedalId) id);
            pedal->prepare (hostRate);
            auto left = x, right = x;
            std::mt19937 rng (7);
            for (size_t pos = 0; pos < x.size();)
            {
                const int n = (int) std::min<size_t> (rng() % 8 == 0 ? 3000 + rng() % 2000 : 1 + rng() % 600, x.size() - pos);
                pedal->setPatch (patch, hostRate, n);
                pedal->process (0, left.data() + pos, n);
                pedal->process (1, right.data() + pos, n);
                pos += (size_t) n;
            }
            CAPTURE (id);
            CHECK (left == right);
            std::vector<float> difference (x.size());
            for (size_t i = 0; i < x.size(); ++i)
                difference[i] = left[i] - reference[i];
            // Not bit-exact: the control points fall elsewhere, and the photocells (opto, Vibe)
            // follow their lamps from point to point
            CHECK (test::db (test::rms (difference) / test::rms (reference)) < -55.0);
        }
}

TEST_CASE ("modulation pedals stay finite and DC-free under parameter abuse", "[pedals][modulation]")
{
    for (double rate : { hostRate, 1536000.0 })
        for (int id = (int) PedalId::Chorus; id <= (int) PedalId::Vibrato; ++id)
        {
            const auto& info = pedalInfo ((PedalId) id);
            auto pedal = makePedal ((PedalId) id);
            pedal->prepare (rate);
            std::mt19937 rng ((unsigned) id);
            auto x = noise ((size_t) (0.4 * rate), 0.5f, (unsigned) id);
            auto right = x;
            auto patch = Patch::defaults();
            for (size_t pos = 0; pos < x.size();)
            {
                if (rng() % 4 == 0)
                    for (PID pid : info.params)
                    {
                        const auto& p = paramInfo (pid);
                        patch[pid] = p.min + (p.max - p.min) * (float) (rng() % 1001) / 1000.0f;
                        if (p.type != dustify::ParamType::Float)
                            patch[pid] = std::round (patch[pid]);
                    }
                const int n = (int) std::min<size_t> (1 + rng() % 2048, x.size() - pos);
                pedal->setPatch (patch, rate, n);
                if (rng() % 2 == 0)
                    pedal->processStereo (x.data() + pos, right.data() + pos, n);
                else
                {
                    pedal->process (0, x.data() + pos, n);
                    pedal->process (1, right.data() + pos, n);
                }
                pos += (size_t) n;
            }
            double sum = 0.0;
            float peak = 0.0f;
            bool finite = true;
            for (size_t i = 0; i < x.size(); ++i)
            {
                finite = finite && std::isfinite (x[i]) && std::isfinite (right[i]);
                sum += (double) x[i] + right[i];
                peak = std::max ({ peak, std::abs (x[i]), std::abs (right[i]) });
            }
            CAPTURE (id, rate, peak);
            CHECK (finite);
            CHECK (peak < 4.0f);
            CHECK (std::abs (sum / (2.0 * (double) x.size())) < 0.005);
        }
}
