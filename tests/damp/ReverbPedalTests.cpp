#include "damp/PedalHelpers.h"

#include "dsp/pedals/Board.h"

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
constexpr ReverbMode modes[] { ReverbMode::Spring, ReverbMode::Plate, ReverbMode::Hall };
const char* modeName (ReverbMode m) { return m == ReverbMode::Spring ? "spring" : m == ReverbMode::Plate ? "plate" : "hall"; }

Patch reverbPatch (ReverbMode mode, Settings settings = {})
{
    auto patch = Patch::defaults();
    patch[PID::ReverbOn] = 1.0f;
    patch[PID::ReverbMode] = (float) mode;
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

// The pedal on its own, engaged, in blocks of 256: mono through process(), or both channels
// through processStereo()
void runReverb (pedals::Reverb& reverb, const Patch& patch, std::vector<float>& left, std::vector<float>* right, double rate,
                size_t from = 0)
{
    for (size_t pos = from; pos < left.size(); pos += 256)
    {
        const int n = (int) std::min<size_t> (256, left.size() - pos);
        reverb.setPatch (patch, rate, n);
        if (right != nullptr)
            reverb.processStereo (left.data() + pos, right->data() + pos, n);
        else
            reverb.process (0, left.data() + pos, n);
    }
}

struct Response
{
    std::vector<float> left, right;
};

// The wet response to an impulse (the dry impulse taken out), from the impulse on. The impulse
// comes 20 ms in, once the tank's input has faded in.
Response impulseResponse (const Patch& patch, double seconds, bool stereo = true, double rate = hostRate)
{
    const auto at = (size_t) (0.02 * rate);
    const auto n = at + (size_t) (seconds * rate);
    std::vector<float> left (n, 0.0f), right (n, 0.0f);
    left[at] = right[at] = 1.0f;
    pedals::Reverb reverb;
    reverb.prepare (rate);
    reverb.setEngaged (true);
    reverb.reset();
    runReverb (reverb, patch, left, stereo ? &right : nullptr, rate);
    left[at] -= 1.0f;
    right[at] -= 1.0f;
    left.erase (left.begin(), left.begin() + (long) at);
    right.erase (right.begin(), right.begin() + (long) at);
    if (! stereo)
        right = left;
    return { std::move (left), std::move (right) };
}

double energy (const std::vector<float>& x, size_t from = 0, size_t to = 0)
{
    if (to == 0)
        to = x.size();
    double e = 0.0;
    for (size_t i = from; i < to; ++i)
        e += (double) x[i] * x[i];
    return e;
}

// RT60 from the Schroeder backward integral: the -5 .. -35 dB slope, extended to 60 dB
double rt60 (const Response& r, double rate = hostRate)
{
    std::vector<double> tail (r.left.size() + 1, 0.0);
    for (size_t i = r.left.size(); i-- > 0;)
        tail[i] = tail[i + 1] + (double) r.left[i] * r.left[i] + (double) r.right[i] * r.right[i];
    size_t a = 0;
    for (size_t i = 0; i < r.left.size(); ++i)
    {
        const double down = 10.0 * std::log10 (tail[i] / tail[0] + 1.0e-30);
        if (a == 0 && down < -5.0)
            a = i;
        if (down < -35.0)
            return 2.0 * (double) (i - a) / rate;
    }
    return 0.0;
}

// Through one filter applied twice
std::vector<float> filtered (const std::vector<float>& x, Biquad f)
{
    Biquad g = f;
    std::vector<float> y (x);
    for (auto& v : y)
        v = g.process (f.process (v));
    return y;
}

// Sample-to-sample change, the largest in [from, to)
float largestStep (const std::vector<float>& x, size_t from, size_t to)
{
    float worst = 0.0f;
    for (size_t i = std::max<size_t> (from, 1); i < to; ++i)
        worst = std::max (worst, std::abs (x[i] - x[i - 1]));
    return worst;
}

double correlation (const std::vector<float>& a, const std::vector<float>& b, size_t from, size_t to)
{
    double ab = 0.0, aa = 0.0, bb = 0.0;
    for (size_t i = from; i < to; ++i)
    {
        ab += (double) a[i] * b[i];
        aa += (double) a[i] * a[i];
        bb += (double) b[i] * b[i];
    }
    return ab / std::sqrt (aa * bb + 1.0e-30);
}

// The whole after-amp board, stereo, with the patch switched per block by `patchAt` (seconds)
template <typename F>
std::pair<std::vector<float>, std::vector<float>> runBoard (std::vector<float> left, F&& patchAt)
{
    auto right = left;
    Board board;
    board.prepare (hostRate, highRate);
    board.setPatch (patchAt (0.0));
    board.reset();
    for (size_t pos = 0; pos < left.size(); pos += 256)
    {
        const int n = (int) std::min<size_t> (256, left.size() - pos);
        board.setPatch (patchAt ((double) pos / hostRate));
        board.processAfterAmp (left.data() + pos, right.data() + pos, n, hostRate);
    }
    return { std::move (left), std::move (right) };
}
} // namespace

TEST_CASE ("Reverb: RT60 follows the decay knob in every mode", "[pedals][reverb]")
{
    struct Expect
    {
        ReverbMode mode;
        float decay;
        double seconds;
    };
    const Expect cases[] {
        { ReverbMode::Spring, 0.0f, 1.0 }, { ReverbMode::Spring, 5.0f, 2.0 }, { ReverbMode::Spring, 10.0f, 4.0 },
        { ReverbMode::Plate, 0.0f, 0.5 },  { ReverbMode::Plate, 5.0f, 1.73 }, { ReverbMode::Plate, 10.0f, 6.0 },
        { ReverbMode::Hall, 0.0f, 1.0 },   { ReverbMode::Hall, 5.0f, 3.16 },  { ReverbMode::Hall, 10.0f, 10.0 },
    };
    for (const auto& c : cases)
    {
        const auto r = impulseResponse (reverbPatch (c.mode, { { PID::ReverbDecay, c.decay }, { PID::ReverbPredelay, 0.0f } }),
                                        0.75 * c.seconds + 0.5);
        const double measured = rt60 (r);
        INFO (modeName (c.mode) << " decay " << c.decay << ": RT60 " << measured << " s, expected " << c.seconds);
        CHECK (std::abs (measured / c.seconds - 1.0) < 0.15);
    }
}

TEST_CASE ("Reverb: the spring's echoes are dispersive, lows arriving after highs", "[pedals][reverb]")
{
    const auto r = impulseResponse (reverbPatch (ReverbMode::Spring, { { PID::ReverbPredelay, 0.0f } }), 0.1, false);
    // When each band's share of the first echo arrives: the energy centroid over the first 52 ms
    // (the highs' second pass starts at ~55 ms)
    const auto arrival = [&] (double hz)
    {
        Biquad band;
        band.setBandpass (hz, 2.0, hostRate);
        const auto y = filtered (r.left, band);
        double weighted = 0.0, total = 0.0;
        for (size_t i = 0; i < (size_t) (0.052 * hostRate); ++i)
        {
            const double e = (double) y[i] * y[i];
            weighted += e * (double) i;
            total += e;
        }
        return weighted / total / hostRate;
    };
    const double low = arrival (250.0), mid = arrival (800.0), high = arrival (2500.0);
    INFO ("first echo at 250 Hz " << low * 1e3 << " ms, 800 Hz " << mid * 1e3 << " ms, 2.5 kHz " << high * 1e3 << " ms");
    CHECK (high > 0.024);       // the spring's transit time
    CHECK (mid > high + 0.003); // a falling chirp
    CHECK (low > mid + 0.004);
}

TEST_CASE ("Reverb: the spring is band-limited", "[pedals][reverb]")
{
    for (float tone : { 0.0f, 5.0f, 10.0f })
    {
        const auto r = impulseResponse (reverbPatch (ReverbMode::Spring, { { PID::ReverbTone, tone } }), 0.5, false);
        const double total = energy (r.left);
        Biquad high, low;
        high.setHighpass (9000.0, 0.7071, hostRate);
        low.setLowpass (45.0, 0.7071, hostRate);
        const double highDb = 10.0 * std::log10 (energy (filtered (r.left, high)) / total);
        const double lowDb = 10.0 * std::log10 (energy (filtered (r.left, low)) / total);
        INFO ("tone " << tone << ": above 9 kHz " << highDb << " dB, below 45 Hz " << lowDb << " dB");
        CHECK (highDb < (tone > 5.0f ? -25.0 : -30.0)); // Tone opens it up a little
        CHECK (lowDb < -20.0);
    }
}

TEST_CASE ("Reverb: plate and hall build up dense, with no isolated echoes", "[pedals][reverb]")
{
    for (auto mode : { ReverbMode::Plate, ReverbMode::Hall })
    {
        const auto r = impulseResponse (reverbPatch (mode, { { PID::ReverbPredelay, 0.0f } }), 0.5, false);
        // From 50 ms on, in 5 ms windows: an isolated echo stands far above its window's RMS,
        // a dense tail looks like noise (a crest factor of about 3)
        const auto window = (size_t) (0.005 * hostRate);
        double worst = 0.0;
        for (size_t from = (size_t) (0.05 * hostRate); from + window <= r.left.size(); from += window)
        {
            float peak = 0.0f;
            for (size_t i = from; i < from + window; ++i)
                peak = std::max (peak, std::abs (r.left[i]));
            worst = std::max (worst, peak / std::sqrt (energy (r.left, from, from + window) / (double) window));
        }
        INFO (modeName (mode) << ": worst crest factor after 50 ms " << worst);
        CHECK (worst < 5.0);
    }
}

TEST_CASE ("Reverb: pre-delay delays the onset", "[pedals][reverb]")
{
    for (auto mode : modes)
    {
        const auto onset = [mode] (float ms)
        {
            const auto r = impulseResponse (reverbPatch (mode, { { PID::ReverbPredelay, ms } }), 0.4, false);
            float peak = 0.0f;
            for (auto v : r.left)
                peak = std::max (peak, std::abs (v));
            for (size_t i = 0; i < r.left.size(); ++i)
                if (std::abs (r.left[i]) > 1.0e-3f * peak)
                    return (double) i / hostRate;
            return 1.0;
        };
        const double early = onset (0.0f), late = onset (120.0f);
        INFO (modeName (mode) << ": onset " << early * 1e3 << " ms, with 120 ms pre-delay " << late * 1e3 << " ms");
        CHECK (std::abs (late - early - 0.12) < 0.002);
    }
}

TEST_CASE ("Reverb: mix 0 is the dry signal", "[pedals][reverb]")
{
    for (auto mode : modes)
    {
        const auto x = noise ((size_t) (0.5 * hostRate), 0.5f);
        auto y = x;
        pedals::Reverb reverb;
        reverb.prepare (hostRate);
        reverb.setEngaged (true);
        reverb.reset();
        runReverb (reverb, reverbPatch (mode, { { PID::ReverbMix, 0.0f } }), y, nullptr, hostRate);
        float worst = 0.0f;
        for (size_t i = 0; i < x.size(); ++i)
            worst = std::max (worst, std::abs (y[i] - x[i]));
        INFO (modeName (mode));
        CHECK (worst == 0.0f);
    }
}

TEST_CASE ("Reverb: a mono input comes out as a wide stereo tail, and the mono path rings too", "[pedals][reverb]")
{
    for (auto mode : modes)
    {
        const auto stereo = impulseResponse (reverbPatch (mode), 1.0);
        const double c = correlation (stereo.left, stereo.right, (size_t) (0.1 * hostRate), stereo.left.size());
        const auto mono = impulseResponse (reverbPatch (mode), 1.0, false);
        const double monoTail = energy (mono.left, (size_t) (0.1 * hostRate));
        const double stereoTail = 0.5 * (energy (stereo.left, (size_t) (0.1 * hostRate)) + energy (stereo.right, (size_t) (0.1 * hostRate)));
        INFO (modeName (mode) << ": L/R correlation " << c << ", mono tail " << 10.0 * std::log10 (monoTail / stereoTail)
                              << " dB against each stereo side");
        CHECK (std::abs (c) < 0.5);
        CHECK (std::abs (10.0 * std::log10 (monoTail / stereoTail)) < 4.0);
    }
}

TEST_CASE ("Reverb: bounded under a loud input at any decay", "[pedals][reverb]")
{
    for (auto mode : modes)
        for (float decay : { 0.0f, 10.0f })
        {
            auto x = noise ((size_t) (1.5 * hostRate), 4.0f, 7);
            std::fill (x.begin() + (long) hostRate, x.end(), 0.0f); // then silence: the tail alone
            auto y = x, right = x;
            pedals::Reverb reverb;
            reverb.prepare (hostRate);
            reverb.setEngaged (true);
            reverb.reset();
            runReverb (reverb, reverbPatch (mode, { { PID::ReverbDecay, decay }, { PID::ReverbMix, 10.0f } }), y, &right, hostRate);
            float peak = 0.0f;
            bool finite = true;
            for (size_t i = 0; i < y.size(); ++i)
            {
                finite = finite && std::isfinite (y[i]) && std::isfinite (right[i]);
                peak = std::max ({ peak, std::abs (y[i] - x[i]), std::abs (right[i] - x[i]) });
            }
            INFO (modeName (mode) << " decay " << decay << ": wet peak " << peak << " from a +12 dBFS input");
            CHECK (finite);
            CHECK (peak < 12.0f);
        }
}

TEST_CASE ("Reverb: finite and DC-free under random abuse", "[pedals][reverb]")
{
    for (double rate : { 44100.0, 192000.0 })
    {
        std::mt19937 rng (3);
        std::uniform_real_distribution<float> u (0.0f, 1.0f);
        pedals::Reverb reverb;
        reverb.prepare (rate);
        reverb.setEngaged (true);
        reverb.reset();
        auto patch = reverbPatch (ReverbMode::Spring);
        const auto total = (size_t) (3.0 * rate);
        std::vector<float> left (512), right (512);
        bool finite = true;
        float peak = 0.0f;
        for (size_t pos = 0; pos < total;)
        {
            const int n = 1 + (int) (u (rng) * 511.0f);
            if (u (rng) < 0.2f)
            {
                patch[PID::ReverbMode] = std::floor (u (rng) * 2.99f);
                patch[PID::ReverbDecay] = 10.0f * u (rng);
                patch[PID::ReverbPredelay] = 200.0f * u (rng);
                patch[PID::ReverbTone] = 10.0f * u (rng);
                patch[PID::ReverbMix] = 10.0f * u (rng);
                patch[PID::ReverbTrails] = u (rng) < 0.5f ? 1.0f : 0.0f;
                reverb.setEngaged (u (rng) < 0.8f);
            }
            const float level = u (rng) < 0.3f ? 3.0f : 0.3f;
            for (int i = 0; i < n; ++i)
                left[(size_t) i] = right[(size_t) i] = level * (2.0f * u (rng) - 1.0f);
            reverb.setPatch (patch, rate, n);
            if (u (rng) < 0.5f)
                reverb.processStereo (left.data(), right.data(), n);
            else
                reverb.process (0, left.data(), n);
            for (int i = 0; i < n; ++i)
            {
                finite = finite && std::isfinite (left[(size_t) i]) && std::isfinite (right[(size_t) i]);
                peak = std::max (peak, std::abs (left[(size_t) i]));
            }
            pos += (size_t) n;
        }
        INFO ("rate " << rate << ": peak " << peak);
        CHECK (finite);
        CHECK (peak < 20.0f);

        // Then a DC offset (with a little noise) in every mode: none of it gets into the wet
        for (auto mode : modes)
        {
            reverb.setEngaged (true);
            const auto settled = reverbPatch (mode, { { PID::ReverbMix, 10.0f }, { PID::ReverbDecay, 10.0f } });
            const auto n = (size_t) (2.0 * rate);
            auto x = noise (n, 0.05f, 5);
            for (auto& v : x)
                v += 0.25f;
            const auto in = x;
            runReverb (reverb, settled, x, nullptr, rate);
            double wet = 0.0;
            for (size_t i = n / 2; i < n; ++i)
                wet += x[i] - in[i];
            INFO (modeName (mode) << " at " << rate << ": wet mean over the last second " << wet / (double) (n / 2));
            CHECK (std::abs (wet / (double) (n / 2)) < 2.5e-4); // 0.1 % of the offset
        }
    }
}

TEST_CASE ("Reverb: tails through the board", "[pedals][reverb]")
{
    for (auto mode : modes)
    {
        INFO (modeName (mode));
        const auto offAt = 1.0, burst = 0.5;
        auto input = noise ((size_t) (2.0 * hostRate), 0.3f, 11);
        std::fill (input.begin() + (long) (burst * hostRate), input.end(), 0.0f);
        const auto patchWith = [mode] (bool on, bool trails)
        {
            auto p = reverbPatch (mode, { { PID::ReverbTrails, trails ? 1.0f : 0.0f } });
            p[PID::ReverbOn] = on ? 1.0f : 0.0f;
            return p;
        };
        const auto [reference, referenceRight] = runBoard (input, [&] (double) { return patchWith (true, true); });

        SECTION ("trails on: the tail rings out as if it were still on")
        {
            const auto [y, right] = runBoard (input, [&] (double t) { return patchWith (t < offAt, true); });
            float worst = 0.0f;
            for (size_t i = 0; i < y.size(); ++i)
                worst = std::max (worst, std::abs (y[i] - reference[i]));
            CHECK (worst < 1.0e-6f);
            CHECK (energy (y, (size_t) (1.2 * hostRate), (size_t) (1.3 * hostRate)) > 1.0e-4);
        }

        SECTION ("trails off: the tail fades out within 20 ms, smoothly")
        {
            const auto [y, right] = runBoard (input, [&] (double t) { return patchWith (t < offAt, false); });
            const auto off = (size_t) (offAt * hostRate);
            const double before = energy (reference, off, off + 960), after = energy (y, off + 960, off + 1920) + 1.0e-30;
            const float steady = largestStep (reference, off - 4800, off + 4800);
            INFO ("20-40 ms after: " << 10.0 * std::log10 (after / before) << " dB; largest step " << largestStep (y, off - 256, off + 1920)
                                     << " against " << steady);
            CHECK (10.0 * std::log10 (after / before) < -80.0);
            CHECK (largestStep (y, off - 256, off + 1920) <= 1.05f * steady);
            for (size_t i = off + 1920; i < y.size(); ++i)
                REQUIRE (y[i] == 0.0f); // fully off: nothing but the (silent) dry signal
        }

        SECTION ("switched on and off over a playing signal: the dry stays at unity, no click")
        {
            const auto tone = sine (220.0, 2.0, 0.3, hostRate);
            const auto [y, right] = runBoard (tone, [&] (double t) { return patchWith (t >= 0.3 && t < 1.0, false); });
            const auto [on, onRight] = runBoard (tone, [&] (double) { return patchWith (true, false); });
            const float steady = std::max (largestStep (tone, 0, tone.size()), largestStep (on, 0, on.size()));
            INFO ("largest step " << largestStep (y, 0, y.size()) << " against " << steady);
            CHECK (largestStep (y, 0, y.size()) < 1.1f * steady);
            float worst = 0.0f;
            for (size_t i = 0; i < (size_t) (0.3 * hostRate); ++i)
                worst = std::max (worst, std::abs (y[i] - tone[i]));
            for (size_t i = (size_t) (1.05 * hostRate); i < y.size(); ++i)
                worst = std::max (worst, std::abs (y[i] - tone[i]));
            CHECK (worst == 0.0f);
        }

        SECTION ("re-engaged mid-tail: smooth")
        {
            for (bool trails : { true, false })
            {
                const auto tone = sine (220.0, 2.0, 0.3, hostRate);
                const auto [y, right] = runBoard (tone, [&] (double t) { return patchWith (t < 0.6 || t >= 0.605, trails); });
                const auto [on, onRight] = runBoard (tone, [&] (double) { return patchWith (true, trails); });
                const float steady = largestStep (on, 0, on.size());
                INFO ("trails " << trails << ": largest step " << largestStep (y, 0, y.size()) << " against " << steady);
                CHECK (largestStep (y, 0, y.size()) < 1.1f * steady);
            }
        }
    }
}

TEST_CASE ("Reverb: stops ringing once the tail has died away", "[pedals][reverb]")
{
    for (auto mode : modes)
        for (bool trails : { true, false })
        {
            pedals::Reverb reverb;
            reverb.prepare (hostRate);
            reverb.setEngaged (true);
            reverb.reset();
            const auto patch = reverbPatch (mode, { { PID::ReverbTrails, trails ? 1.0f : 0.0f }, { PID::ReverbDecay, 0.0f } });
            auto x = noise ((size_t) (0.5 * hostRate), 0.5f);
            runReverb (reverb, patch, x, nullptr, hostRate);
            reverb.setEngaged (false);
            std::vector<float> block (256, 0.0f);
            int blocks = 0;
            while (reverb.ringing() && blocks < 2000)
            {
                std::fill (block.begin(), block.end(), 0.0f);
                reverb.setPatch (patch, hostRate, 256);
                reverb.process (0, block.data(), 256);
                ++blocks;
            }
            const double seconds = blocks * 256.0 / hostRate;
            INFO (modeName (mode) << (trails ? " trails" : " no trails") << ": rang for " << seconds << " s");
            CHECK_FALSE (reverb.ringing());
            if (trails)
                CHECK ((seconds > 0.5 && seconds < 3.0)); // about 1.5 x RT60 (1 s) for 90 dB, plus the hold
            else
                CHECK (seconds < 0.02);
        }
}
