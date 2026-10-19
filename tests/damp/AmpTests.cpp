#include "common/Analysis.h"
#include "dsp/Amp.h"
#include "dsp/AmpModels.h"
#include "dsp/Cabinet.h"
#include "dsp/PitchDetector.h"
#include "dsp/ToneStack.h"
#include "dsp/Triode.h"
#include "dsp/Preset.h"
#include "dsp/Rig.h"

#include "common/dsp/Convolver.h"
#include "common/dsp/Noise.h"
#include "common/dsp/Oversampler.h"
#include "common/dsp/Upsampler.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>

#include <algorithm>
#include <cmath>
#include <complex>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <numbers>

using namespace damp;

namespace
{
constexpr double rate = 48000.0;

std::vector<float> sine (double f, double seconds, double amplitude, double sampleRate = rate)
{
    std::vector<float> x ((size_t) (seconds * sampleRate));
    for (size_t i = 0; i < x.size(); ++i)
        x[i] = (float) (amplitude * std::sin (2.0 * std::numbers::pi * f * (double) i / sampleRate));
    return x;
}

Amp makeAmp (std::initializer_list<std::pair<PID, float>> settings = {})
{
    auto patch = Patch::defaults();
    for (auto [pid, value] : settings)
        patch[pid] = value;
    Amp amp;
    amp.setPatch (patch);
    amp.prepare (rate);
    return amp;
}

// Mono, in odd-sized blocks
std::vector<float> run (Amp& amp, std::vector<float> x)
{
    for (size_t pos = 0; pos < x.size(); pos += 173)
        amp.process (x.data() + pos, nullptr, (int) std::min<size_t> (173, x.size() - pos));
    return x;
}

// Through an oversampler with nothing in between
std::vector<float> roundTrip (int stages, const std::vector<float>& x)
{
    dustify::Oversampler os (stages);
    std::vector<float> y (x.size());
    for (size_t pos = 0; pos < x.size(); pos += dustify::Oversampler::maxBlock)
    {
        const int n = (int) std::min<size_t> (dustify::Oversampler::maxBlock, x.size() - pos);
        os.upsample (x.data() + pos, n);
        os.downsample (y.data() + pos, n);
    }
    return y;
}
} // namespace

TEST_CASE ("parameter table is consistent", "[amp][params]")
{
    REQUIRE (allParams().size() == (size_t) numParams);
    for (const auto& info : allParams())
    {
        CHECK (info.min < info.max);
        CHECK (info.def >= info.min);
        CHECK (info.def <= info.max);
        CHECK (findParam (info.id) == &info);
    }
}

TEST_CASE ("factory presets parse cleanly and round-trip", "[amp][presets]")
{
    int count = 0;
    for (const auto& entry : std::filesystem::directory_iterator (DAMP_PRESET_DIR))
    {
        std::ifstream f (entry.path());
        const std::string text { std::istreambuf_iterator<char> (f), {} };
        const auto parsed = parsePreset (text);
        INFO (entry.path().filename().string());
        CHECK (parsed.warnings.empty());
        CHECK (! parsed.name.empty());

        const auto again = parsePreset (serialisePreset (parsed.patch, parsed.name));
        CHECK (again.warnings.empty());
        CHECK (again.patch.v == parsed.patch.v);
        ++count;
    }
    CHECK (count > 0);
}

TEST_CASE ("halfband upsampler passes the band and rejects the image", "[oversampling]")
{
    dustify::HalfbandUpsampler up;
    const double f = 9000.0;
    const auto x = sine (f, 0.5, 0.5);
    std::vector<float> y (2 * x.size());
    for (size_t i = 0; i < x.size(); ++i)
        up.process (x[i], y[2 * i], y[2 * i + 1]);

    const size_t n = 1 << 14;
    CHECK (std::abs (test::toneLevelDb (y, 2 * rate, f, y.size() - n, n) - test::db (0.5)) < 0.05);
    // Zero-stuffing leaves an image at rate - f, as loud as the tone until the filter removes it
    CHECK (test::toneLevelDb (y, 2 * rate, rate - f, y.size() - n, n) < test::db (0.5) - 75.0);
}

TEST_CASE ("oversampler round trip is flat and delays by exactly its latency", "[oversampling]")
{
    const int stages = GENERATE (1, 2, 3);
    INFO ((1 << stages) << "x");

    // An impulse comes back centred on the latency
    std::vector<float> impulse (512, 0.0f);
    impulse[10] = 1.0f;
    const auto response = roundTrip (stages, impulse);
    size_t peak = 0;
    for (size_t i = 0; i < response.size(); ++i)
        if (std::abs (response[i]) > std::abs (response[peak]))
            peak = i;
    CHECK (peak == 10 + (size_t) dustify::Oversampler::latency);
    CHECK (std::abs (response[peak] - 1.0f) < 0.3f);

    // Sines: unchanged level, and matching the input delayed by the latency sample for sample
    for (double f : { 100.0, 1000.0, 5000.0, 15000.0, 19000.0 })
    {
        INFO (f << " Hz");
        const auto x = sine (f, 0.3, 0.5);
        const auto y = roundTrip (stages, x);
        const size_t from = 4000, n = 8192;
        CHECK (std::abs (test::toneLevelDb (y, rate, f, from, n) - test::db (0.5)) < 0.02);

        double error = 0.0;
        for (size_t i = from; i < from + n; ++i)
            error = std::max (error, (double) std::abs (y[i] - x[i - (size_t) dustify::Oversampler::latency]));
        CHECK (test::db (error / 0.5) < -60.0);
    }
}

TEST_CASE ("noise gate shuts out hiss and lets playing through", "[amp][gate]")
{
    dustify::Random rng (3);
    auto hiss = [&] (size_t n, double level)
    {
        std::vector<float> x (n);
        for (auto& s : x)
            s = (float) level * rng.bipolar();
        return x;
    };

    SECTION ("hiss below the threshold is silenced")
    {
        auto amp = makeAmp ({ { PID::GateThreshold, -60.0f } });
        const auto y = run (amp, hiss (48000, std::pow (10.0, -75.0 / 20.0)));
        CHECK (test::db (test::rms (y, 24000)) < -120.0);
    }

    SECTION ("with the gate off the same hiss passes")
    {
        auto amp = makeAmp ({ { PID::GateThreshold, -60.0f }, { PID::GateOn, 0.0f } });
        const auto y = run (amp, hiss (48000, std::pow (10.0, -75.0 / 20.0)));
        CHECK (test::db (test::rms (y, 24000)) > -90.0);
    }

    SECTION ("a note opens it, and it closes over the release once the note has gone")
    {
        auto amp = makeAmp ({ { PID::GateThreshold, -50.0f }, { PID::GateRelease, 50.0f } });
        auto x = sine (220.0, 1.0, 0.1);
        x.resize (96000, 0.0f);
        const auto y = run (amp, x);

        // Open within a couple of milliseconds of the note
        CHECK (test::db (test::rms (y, 200, 300)) > -40.0);
        // Steady while the note plays
        CHECK (std::abs (test::db (test::rms (y, 20000, 24000)) - test::db (test::rms (y, 40000, 44000))) < 0.1);
        // Closed by detector release + hold + gate release (well under 200 ms)
        CHECK (test::db (test::rms (y, 48000 + 9600, 48000 + 14400)) < -100.0);
    }
}

TEST_CASE ("oversampling keeps aliasing down in every model", "[amp][aliasing]")
{
    // A high note, full gain and a hot master: the worst case for harmonics folding back
    const int model = GENERATE (range (0, numModels));
    const double f = 2489.0;
    double worst[3] {};
    for (int factor = 0; factor < 3; ++factor)
    {
        auto amp = makeAmp ({ { PID::Model, (float) model }, { PID::Gain, 10.0f }, { PID::Master, 8.0f },
                              { PID::GateOn, 0.0f }, { PID::Oversampling, (float) factor } });
        const auto y = run (amp, sine (f, 1.0, 0.25));
        worst[factor] = test::worstAliasDb (y, rate, f, 20000.0);
    }
    INFO ("model " << model << ": 2x " << worst[0] << " dB, 4x " << worst[1] << " dB, 8x " << worst[2] << " dB");
    CHECK (worst[1] < worst[0] - 5.0);
    CHECK ((worst[2] < worst[1] - 5.0 || worst[1] < -90.0)); // unless 4x is already clean
    CHECK (worst[1] < -35.0);
    CHECK (worst[2] < -48.0);
}

TEST_CASE ("stereo with identical channels matches mono", "[amp]")
{
    auto mono = makeAmp ({ { PID::Gain, 7.0f } });
    auto stereo = makeAmp ({ { PID::Gain, 7.0f } });
    const auto x = sine (330.0, 0.2, 0.2);
    const auto m = run (mono, x);
    auto l = x, r = x;
    stereo.process (l.data(), r.data(), (int) l.size());
    for (size_t i = 0; i < x.size(); ++i)
    {
        REQUIRE (l[i] == m[i]);
        REQUIRE (r[i] == m[i]);
    }
}

TEST_CASE ("no DC, NaN or runaway under abuse", "[amp][fuzz]")
{
    dustify::Random rng (11);
    auto unit = [&] { return 0.5f * (rng.bipolar() + 1.0f); };

    for (int trial = 0; trial < 20; ++trial)
    {
        auto patch = Patch::defaults();
        for (const auto& info : allParams())
            patch[info.pid] = info.min + unit() * (info.max - info.min);
        Amp amp;
        amp.setPatch (patch);
        amp.prepare (trial % 2 ? 44100.0 : 96000.0);

        std::vector<float> l (8192), r (8192);
        double peak = 0.0;
        for (int block = 0; block < 40; ++block)
        {
            // Mixed material: loud noise, full-scale square, silence; parameters jump around
            for (size_t i = 0; i < l.size(); ++i)
            {
                const float kind = (float) (block % 3);
                l[i] = kind == 0 ? 4.0f * rng.bipolar() : kind == 1 ? ((i / 50) % 2 ? 1.0f : -1.0f) : 0.0f;
                r[i] = -l[i];
            }
            patch[PID::Oversampling] = (float) (block % 3);
            patch[PID::Gain] = 10.0f * unit();
            amp.setPatch (patch);
            amp.process (l.data(), block % 4 ? r.data() : nullptr, (int) l.size());
            for (size_t i = 0; i < l.size(); ++i)
            {
                REQUIRE (std::isfinite (l[i]));
                REQUIRE (std::isfinite (r[i]));
                peak = std::max (peak, (double) std::abs (l[i]));
            }
        }
        CHECK (peak < 20.0); // clipper output (<= 1) times at most +12 dB volume, plus filter overshoot
    }

    SECTION ("a heavily driven asymmetric clip leaves no DC")
    {
        auto amp = makeAmp ({ { PID::Gain, 10.0f }, { PID::GateOn, 0.0f } });
        const auto y = run (amp, sine (110.0, 2.0, 0.5));
        double mean = 0.0;
        for (size_t i = 48000; i < y.size(); ++i)
            mean += y[i];
        mean /= (double) (y.size() - 48000);
        CHECK (std::abs (mean) < 1.0e-3);
    }
}

namespace
{
// The tone stack circuit solved directly by nodal analysis, independently of Yeh's formulas: an
// ideal source drives C1 to the treble pot and R4 to the junction of C2 (to the treble pot's
// lower end and the bass pot) and C3 (to the mid pot's wiper); the bass pot runs from there to
// the top of the mid pot. Output at the treble wiper, unloaded.
std::complex<double> circuitResponse (const ToneStackParts& p, double t, double m, double bassPot, double hz)
{
    using C = std::complex<double>;
    const double l = std::exp ((bassPot - 1.0) * 3.4);
    const C s (0.0, 2.0 * std::numbers::pi * hz);
    constexpr double tiny = 1.0e-3; // ohms, for a pot at its end stop
    // Nodes: 0 input (driven), 1 treble top, 2 output, 3 treble bottom / bass top, 4 slope junction,
    // 5 mid top, 6 mid wiper; ground is implicit
    struct Element { int a, b; C y; };
    const Element elements[] {
        { 0, 1, s * p.c1 }, { 1, 2, 1.0 / std::max ((1.0 - t) * p.r1, tiny) }, { 2, 3, 1.0 / std::max (t * p.r1, tiny) },
        { 3, 5, 1.0 / std::max (l * p.r2, tiny) }, { 5, 6, 1.0 / std::max ((1.0 - m) * p.r3, tiny) },
        { 6, -1, 1.0 / std::max (m * p.r3, tiny) }, { 0, 4, 1.0 / p.r4 }, { 4, 3, s * p.c2 }, { 4, 6, s * p.c3 },
    };
    constexpr int n = 6; // unknown nodes 1..6
    C a[n][n + 1] {};
    for (const auto& e : elements)
    {
        auto add = [&] (int row, int col, C v)
        {
            if (row <= 0)
                return;
            if (col == 0)
                a[row - 1][n] -= v; // the driven input (1 V) moves to the right-hand side
            else if (col > 0)
                a[row - 1][col - 1] += v;
        };
        add (e.a, e.a, e.y);
        add (e.b, e.b, e.y);
        add (e.a, e.b, -e.y);
        add (e.b, e.a, -e.y);
    }
    for (int i = 0; i < n; ++i) // Gaussian elimination with partial pivoting
    {
        int pivot = i;
        for (int r = i + 1; r < n; ++r)
            if (std::abs (a[r][i]) > std::abs (a[pivot][i]))
                pivot = r;
        std::swap (a[i], a[pivot]);
        for (int r = 0; r < n; ++r)
            if (r != i)
            {
                const C f = a[r][i] / a[i][i];
                for (int c = i; c <= n; ++c)
                    a[r][c] -= f * a[i][c];
            }
    }
    return a[1][n] / a[1][1]; // node 2
}
} // namespace

TEST_CASE ("tone stack matches the circuit", "[amp][tonestack]")
{
    const int model = GENERATE (range (0, numModels));
    const auto& parts = modelSpec ((AmpModel) model).toneStack;
    const double highRate = 4.0 * rate;

    for (auto [t, m, b] : { std::array { 0.5, 0.5, 0.5 }, { 0.0, 1.0, 0.2 }, { 1.0, 0.0, 1.0 }, { 0.8, 0.3, 0.0 } })
    {
        INFO ("model " << model << " treble " << t << " middle " << m << " bass " << b);
        ToneStack stack;
        stack.prepare (highRate);
        stack.set (parts, t, m, b);
        std::vector<float> ir (1 << 16);
        for (size_t i = 0; i < ir.size(); ++i)
            ir[i] = stack.process (i == 0 ? 1.0f : 0.0f);

        const auto analog = ToneStack::analog (parts, t, m, b);
        for (double hz : { 40.0, 120.0, 400.0, 1000.0, 3000.0, 8000.0, 15000.0 })
        {
            INFO (hz << " Hz");
            std::complex<double> digital = 0.0;
            for (size_t i = 0; i < ir.size(); ++i)
                digital += (double) ir[i] * std::polar (1.0, -2.0 * std::numbers::pi * hz * (double) i / highRate);
            const auto circuit = circuitResponse (parts, t, m, b, hz);
            const double circuitDb = test::db (std::abs (circuit));
            // Yeh's closed form is the circuit exactly
            CHECK (std::abs (test::db (std::abs (ToneStack::response (analog, hz))) - circuitDb) < 0.001);
            // The bilinear transform at 4x only warps the very top
            if (circuitDb > -50.0)
                CHECK (std::abs (test::db (std::abs (digital)) - circuitDb) < (hz > 10000.0 ? 0.3 : 0.05));
        }
    }
}

TEST_CASE ("triode curves sit where a 12AX7 stage does", "[amp][triode]")
{
    TriodeCurve curve;
    curve.build ({ 100.0e3, 1.5e3, 300.0, true });
    CHECK (curve.platePoint() > 150.0);
    CHECK (curve.platePoint() < 250.0);
    CHECK (curve.bias() < -0.8);
    CHECK (curve.bias() > -2.0);
    CHECK (curve.gain() < -50.0); // mu 100 into 100k: about 60
    CHECK (curve.gain() > -70.0);

    // Inverting and monotonic; it runs out of swing at cutoff (towards the supply) and when the
    // plate bottoms out
    float previous = curve (-12.0f);
    for (float v = -11.9f; v < 6.0f; v += 0.1f)
    {
        REQUIRE (curve (v) <= previous);
        previous = curve (v);
    }
    CHECK (curve (-12.0f) < 300.0f - (float) curve.platePoint() + 0.5f);
    CHECK (curve (-12.0f) - curve (-8.0f) < 1.0f);
    CHECK (curve (0.0f) == 0.0f);

    // Asymmetric: more swing upwards (towards cutoff) than down (towards saturation) for the same drive
    CHECK (curve (-3.0f) > -curve (3.0f) * 0.5f);

    TriodeCurve cold;
    cold.build ({ 100.0e3, 10.0e3, 300.0, false });
    CHECK (cold.gain() > -12.0); // cathode feedback
    CHECK (cold.bias() < curve.bias());
}

TEST_CASE ("tone, presence and resonance controls do what they say", "[amp][controls]")
{
    const int model = GENERATE (range (0, numModels));
    INFO ("model " << model);
    // Low gain and master keep everything linear enough to read levels
    auto level = [&] (std::initializer_list<std::pair<PID, float>> extra, double hz)
    {
        auto patch = Patch::defaults();
        patch[PID::Model] = (float) model;
        patch[PID::Gain] = 1.0f;
        patch[PID::Master] = 2.0f;
        patch[PID::GateOn] = 0.0f;
        for (auto [pid, v] : extra)
            patch[pid] = v;
        Amp amp;
        amp.setPatch (patch);
        amp.prepare (rate);
        const auto y = run (amp, sine (hz, 0.5, 0.02));
        return test::toneLevelDb (y, rate, hz, 12000, 8192);
    };

    CHECK (level ({ { PID::Bass, 10.0f } }, 80.0) > level ({ { PID::Bass, 0.0f } }, 80.0) + 4.0);
    CHECK (level ({ { PID::Treble, 10.0f } }, 3000.0) > level ({ { PID::Treble, 0.0f } }, 3000.0) + 4.0);
    if (model != (int) AmpModel::Chime) // its network has no real mid control
        CHECK (level ({ { PID::Middle, 10.0f } }, 600.0) > level ({ { PID::Middle, 0.0f } }, 600.0) + 3.0);
    CHECK (level ({ { PID::Presence, 10.0f } }, 5000.0) > level ({ { PID::Presence, 0.0f } }, 5000.0) + 2.0);
    if (modelSpec ((AmpModel) model).power.feedback > 0.0)
        CHECK (level ({ { PID::Resonance, 10.0f } }, 70.0) > level ({ { PID::Resonance, 0.0f } }, 70.0) + 2.0);
    if (modelSpec ((AmpModel) model).brightHz > 0.0)
        CHECK (level ({ { PID::Bright, 1.0f } }, 4000.0) > level ({ { PID::Bright, 0.0f } }, 4000.0) + 2.0);
}

TEST_CASE ("gain and master raise the distortion", "[amp][gain]")
{
    const int model = GENERATE (range (0, numModels));
    INFO ("model " << model);
    auto thd = [&] (float gain, float master)
    {
        auto amp = makeAmp ({ { PID::Model, (float) model }, { PID::Gain, gain }, { PID::Master, master }, { PID::GateOn, 0.0f } });
        const auto y = run (amp, sine (220.0, 0.6, 0.1));
        const double fundamental = std::pow (10.0, test::toneLevelDb (y, rate, 220.0, 12000, 16384) / 20.0);
        double harmonics = 0.0;
        for (int h = 2; h < 40; ++h)
            harmonics += std::pow (10.0, test::toneLevelDb (y, rate, 220.0 * h, 12000, 16384) / 10.0);
        return std::sqrt (harmonics) / fundamental;
    };
    const double low = thd (1.0f, 3.0f), high = thd (10.0f, 3.0f), cranked = thd (10.0f, 10.0f);
    INFO ("THD gain 1: " << low << ", gain 10: " << high << ", gain and master 10: " << cranked);
    CHECK (high > 2.0 * low);
    CHECK (high > 0.02); // soft picking: even the cleanest models break up a little flat out
    CHECK (cranked > 2.0 * low); // the power amp's feedback and sag compress rather than add much more
}

TEST_CASE ("the modern high gain model is fatter and more saturated than the high gain one", "[amp][models]")
{
    auto render = [] (AmpModel model, const std::vector<float>& x)
    {
        auto amp = makeAmp ({ { PID::Model, (float) model }, { PID::GateOn, 0.0f } }); // gain 5, master 5
        return run (amp, x);
    };
    auto thd = [] (const std::vector<float>& y, double f)
    {
        const double fundamental = std::pow (10.0, test::toneLevelDb (y, rate, f, 12000, 16384) / 20.0);
        double harmonics = 0.0;
        for (int h = 2; h * f < 20000.0; ++h)
            harmonics += std::pow (10.0, test::toneLevelDb (y, rate, f * h, 12000, 16384) / 10.0);
        return std::sqrt (harmonics) / fundamental;
    };

    // A soft note: the modern preamp is already well into saturation
    const auto soft = sine (220.0, 0.7, std::pow (10.0, -36.0 / 20.0));
    const double hgThd = thd (render (AmpModel::HighGain, soft), 220.0), modernThd = thd (render (AmpModel::ModernHighGain, soft), 220.0);
    INFO ("THD at -36 dBFS: high gain " << hgThd << ", modern " << modernThd);
    CHECK (modernThd > 2.0 * hgThd);

    // Played hard, a low E with an upper note: more weight under 200 Hz against the 1-3 kHz bite,
    // at about the same loudness
    auto chord = sine (82.4, 1.0, 0.25);
    const auto upper = sine (329.6, 1.0, 0.15);
    for (size_t i = 0; i < chord.size(); ++i)
        chord[i] += upper[i];
    auto lowToBite = [] (const std::vector<float>& y)
    {
        double low = 0.0, bite = 0.0;
        for (double f = 82.4; f < 200.0; f += 82.4)
            low += std::pow (10.0, test::toneLevelDb (y, rate, f, 12000, 32768) / 10.0);
        for (double f = 82.4 * 13; f < 3000.0; f += 82.4) // harmonics and intermodulation all land on 82.4 Hz multiples
            bite += std::pow (10.0, test::toneLevelDb (y, rate, f, 12000, 32768) / 10.0);
        return 10.0 * std::log10 (low / bite);
    };
    const auto hg = render (AmpModel::HighGain, chord), modern = render (AmpModel::ModernHighGain, chord);
    INFO ("low / bite: high gain " << lowToBite (hg) << " dB, modern " << lowToBite (modern) << " dB; levels "
                                   << test::db (test::rms (hg, 12000)) << ", " << test::db (test::rms (modern, 12000)) << " dB");
    CHECK (lowToBite (modern) > lowToBite (hg) + 2.0);
    CHECK (std::abs (test::db (test::rms (modern, 12000)) - test::db (test::rms (hg, 12000))) < 3.0);
}

TEST_CASE ("silence stays silent and cheap", "[amp][denormals]")
{
    const int model = GENERATE ((int) AmpModel::HighGain, (int) AmpModel::ModernHighGain);
    auto amp = makeAmp ({ { PID::Model, (float) model }, { PID::Gain, 10.0f }, { PID::GateOn, 0.0f } });
    run (amp, sine (100.0, 0.2, 0.5));
    const auto y = run (amp, std::vector<float> ((size_t) rate * 2, 0.0f));
    CHECK (test::db (test::rms (y, (size_t) rate)) < -120.0);
    for (float s : y)
        REQUIRE ((s == 0.0f || std::abs (s) >= std::numeric_limits<float>::min())); // no denormals
}

namespace
{
std::vector<float> directConvolution (const std::vector<float>& x, const std::vector<float>& h)
{
    std::vector<float> y (x.size(), 0.0f);
    for (size_t n = 0; n < x.size(); ++n)
    {
        double acc = 0.0;
        for (size_t k = 0; k < h.size() && k <= n; ++k)
            acc += (double) h[k] * x[n - k];
        y[n] = (float) acc;
    }
    return y;
}

std::vector<float> noise (size_t n, uint32_t seed, float decay = 0.0f)
{
    dustify::Random rng (seed);
    std::vector<float> x (n);
    for (size_t i = 0; i < n; ++i)
        x[i] = rng.bipolar() * (decay > 0.0f ? std::exp (-(float) i / decay) : 1.0f);
    return x;
}

// In blocks of varying size, as hosts do
void convolve (dustify::Convolver& c, std::vector<float>& x, size_t from = 0, size_t to = 0)
{
    to = to == 0 ? x.size() : to;
    const int sizes[] { 1, 17, 64, 100, 3, 256, 63, 65 };
    for (size_t pos = from, i = 0; pos < to; ++i)
    {
        const auto n = std::min<size_t> ((size_t) sizes[i % 8], to - pos);
        c.process (x.data() + pos, (int) n);
        pos += n;
    }
}
} // namespace

TEST_CASE ("convolver matches direct convolution with no latency", "[cab][convolver]")
{
    const int length = GENERATE (1, 63, 64, 65, 200, 3001);
    INFO ("IR length " << length);
    const auto h = noise ((size_t) length, 9, (float) length / 4.0f);
    const auto x = noise (6000, 4);
    const dustify::ConvolutionKernel kernel (h.data(), length);
    dustify::Convolver c;
    c.prepare (4000, 256);
    c.setKernelNow (&kernel);
    auto y = x;
    convolve (c, y);
    const auto expected = directConvolution (x, h);
    double worst = 0.0;
    for (size_t i = 0; i < y.size(); ++i)
        worst = std::max (worst, (double) std::abs (y[i] - expected[i]));
    CHECK (worst < 2.0e-4);

    // Without a kernel it passes straight through
    dustify::Convolver bypass;
    bypass.prepare (4000, 256);
    auto z = x;
    convolve (bypass, z);
    CHECK (z == x);
}

TEST_CASE ("convolver switches kernels with a crossfade and full history", "[cab][convolver]")
{
    const auto a = noise (1500, 1, 300.0f), b = noise (2500, 2, 500.0f);
    const dustify::ConvolutionKernel ka (a.data(), (int) a.size()), kb (b.data(), (int) b.size());
    const auto x = noise (12000, 3);
    dustify::Convolver c;
    c.prepare (4000, 512);
    c.setKernelNow (&ka);
    auto y = x;
    convolve (c, y, 0, 5000);
    c.setKernel (&kb);
    convolve (c, y, 5000, 0);

    const auto withA = directConvolution (x, a), withB = directConvolution (x, b);
    double before = 0.0, after = 0.0, jump = 0.0;
    for (size_t i = 0; i < 5000; ++i)
        before = std::max (before, (double) std::abs (y[i] - withA[i]));
    // The new kernel is complete from the moment it fades in: it has the whole input history
    for (size_t i = 5000 + 64 + 512; i < y.size(); ++i)
        after = std::max (after, (double) std::abs (y[i] - withB[i]));
    for (size_t i = 4990; i < 5000 + 64 + 512 + 10; ++i)
        jump = std::max (jump, (double) std::abs (y[i] - y[i - 1]));
    double typicalJump = 0.0;
    for (size_t i = 1; i < 4990; ++i)
        typicalJump = std::max (typicalJump, (double) std::abs (withA[i] - withA[i - 1]));
    CHECK (before < 2.0e-4);
    CHECK (after < 2.0e-4);
    CHECK (jump < 1.5 * std::max (typicalJump, 1.0e-3)); // no click at the switch
}

// The plugin builds cab kernels on the message thread and hands them to the audio thread one at a
// time, freeing all but the last two it took (see DampAudioProcessor::rebuildCab and
// processBlock). The cab switches only at its next 64-sample boundary and then crossfades, so with
// short host blocks a kernel freed that way could still be playing (pluginval caught a
// use-after-free with 32-sample blocks). Here "freeing" poisons a kernel with NaN, so any read of
// one shows in the output.
TEST_CASE ("the rig never reads a cab kernel the plugin has freed", "[cab][convolver]")
{
    const int block = GENERATE (1, 16, 32, 63, 100);
    INFO ("block size " << block);
    const auto poison = std::vector<float> (2000, std::numeric_limits<float>::quiet_NaN());
    const dustify::ConvolutionKernel poisoned (poison.data(), (int) poison.size());
    std::vector<std::unique_ptr<dustify::ConvolutionKernel>> made;
    for (uint32_t i = 0; i < 150; ++i)
    {
        const auto h = noise (2000, 100 + i, 200.0f);
        made.push_back (std::make_unique<dustify::ConvolutionKernel> (h.data(), (int) h.size()));
    }

    Rig rig;
    auto patch = Patch::defaults();
    patch[PID::Model] = (float) AmpModel::Clean;
    patch[PID::GateOn] = 0.0f;
    rig.setPatch (patch);
    rig.prepare (rate, 2000);
    rig.setCabKernelNow (made[0].get());

    std::vector<dustify::ConvolutionKernel*> handedOver { made[0].get() }; // in order (the plugin's `kernels`)
    dustify::ConvolutionKernel* pending = nullptr;
    const dustify::ConvolutionKernel* taken = made[0].get();
    size_t next = 1;
    int takes = 0;
    auto in = noise (48000, 7);
    std::vector<float> left ((size_t) block), right ((size_t) block);
    for (size_t pos = 0; pos + (size_t) block <= in.size(); pos += (size_t) block)
    {
        // Message thread: new kernels in bursts (each replacing one not yet taken); everything
        // before the last one taken but one is freed
        if (next < made.size() && pos % 1000 < 3 * (size_t) block)
        {
            if (pending != nullptr)
                handedOver.pop_back();
            pending = made[next++].get();
            handedOver.push_back (pending);
        }
        const auto it = std::find (handedOver.begin(), handedOver.end(), taken);
        if (it != handedOver.end() && it != handedOver.begin())
        {
            for (auto at = handedOver.begin(); at != it - 1; ++at)
                **at = poisoned;
            handedOver.erase (handedOver.begin(), it - 1);
        }

        // Audio thread, as processBlock
        if (! rig.cabSwitching() && pending != nullptr)
        {
            rig.setCabKernel (pending);
            taken = pending;
            pending = nullptr;
            ++takes;
        }
        std::copy_n (in.begin() + (std::ptrdiff_t) pos, block, left.begin());
        std::copy_n (in.begin() + (std::ptrdiff_t) pos, block, right.begin());
        const bool monoInput = (pos / 4800) % 3 == 1; // the right cab sits out now and then
        rig.process (left.data(), right.data(), block, monoInput);
        for (int i = 0; i < block; ++i)
            if (! std::isfinite (left[(size_t) i]) || ! std::isfinite (right[(size_t) i]))
                FAIL ("a freed kernel was read at sample " << pos + (size_t) i);
    }
    CHECK (takes >= 15); // kernels kept coming through
}

TEST_CASE ("modelled cabinets sound like their descriptions", "[cab]")
{
    const double sampleRate = GENERATE (44100.0, 48000.0, 96000.0);
    INFO (sampleRate << " Hz");
    auto response = [&] (const std::vector<float>& h, double hz)
    {
        std::complex<double> acc = 0.0;
        for (size_t n = 0; n < h.size(); ++n)
            acc += (double) h[n] * std::polar (1.0, -2.0 * std::numbers::pi * hz * (double) n / sampleRate);
        return test::db (std::abs (acc));
    };
    auto cab = [&] (CabType type, MicType mic, float position, float distance)
    {
        return modelledCabinet ({ type, mic, position, distance }, sampleRate);
    };

    const auto closed = cab (CabType::Closed4x12, MicType::Dynamic, 0.3f, 0.2f);
    const auto open = cab (CabType::Open1x12, MicType::Dynamic, 0.3f, 0.2f);
    for (const auto* h : { &closed, &open })
    {
        for (float s : *h)
            REQUIRE (std::isfinite (s));
        // Minimum phase: the energy comes first
        double total = 0.0, early = 0.0;
        for (size_t i = 0; i < h->size(); ++i)
        {
            total += (double) (*h)[i] * (*h)[i];
            if ((double) i < 0.005 * sampleRate)
                early += (double) (*h)[i] * (*h)[i];
        }
        CHECK (early > 0.9 * total);
        // Speakers stop well short of the top octave
        CHECK (response (*h, 12000.0) < response (*h, 1000.0) - 25.0);
    }
    // A closed 4x12 keeps its low end; an open 1x12 loses it round the back
    CHECK (response (closed, 60.0) - response (closed, 1000.0) > response (open, 60.0) - response (open, 1000.0) + 4.0);

    // Towards the edge of the cone it gets darker; a ribbon is darker than a condenser
    const auto centre = cab (CabType::Closed4x12, MicType::Dynamic, 0.0f, 0.0f), edge = cab (CabType::Closed4x12, MicType::Dynamic, 1.0f, 0.0f);
    CHECK (response (edge, 3000.0) < response (centre, 3000.0) - 6.0);
    const auto ribbon = cab (CabType::Closed4x12, MicType::Ribbon, 0.3f, 0.2f), condenser = cab (CabType::Closed4x12, MicType::Condenser, 0.3f, 0.2f);
    CHECK (response (ribbon, 6000.0) < response (condenser, 6000.0) - 3.0);
    // Close up, the proximity effect adds bass
    const auto close = cab (CabType::Closed4x12, MicType::Dynamic, 0.3f, 0.0f), far = cab (CabType::Closed4x12, MicType::Dynamic, 0.3f, 1.0f);
    CHECK (response (close, 150.0) - response (close, 1000.0) > response (far, 150.0) - response (far, 1000.0) + 3.0);

    // Off and User (with nothing loaded) pass the amp straight through
    CHECK (cab (CabType::Off, MicType::Dynamic, 0.3f, 0.2f) == std::vector<float> { 1.0f });
}

TEST_CASE ("user impulses are resampled faithfully and trimmed", "[cab]")
{
    auto response = [] (const std::vector<float>& x, double rateHz, double hz)
    {
        std::complex<double> acc = 0.0;
        for (size_t n = 0; n < x.size(); ++n)
            acc += (double) x[n] * std::polar (1.0, -2.0 * std::numbers::pi * hz * (double) n / rateHz);
        return test::db (std::abs (acc));
    };
    // A known, broadband response at 44.1 kHz (a heavily damped resonance at 2 kHz)
    auto known = [] (size_t preDelay)
    {
        std::vector<float> h (4410, 0.0f);
        for (size_t i = preDelay; i < h.size(); ++i)
        {
            const double t = (double) (i - preDelay);
            h[i] = (float) (std::exp (-t / 6.0) * std::sin (2.0 * std::numbers::pi * 2000.0 * t / 44100.0 + 0.5));
        }
        return h;
    };
    const double from = 44100.0, to = 96000.0;

    // With a little silence first (as most IRs have) the shape survives exactly; levelling only
    // moves the whole response by one constant
    for (size_t preDelay : { (size_t) 0, (size_t) 40 })
    {
        const auto h = known (preDelay);
        const auto r = prepareUserImpulse (h, from, to, maxImpulseSeconds);
        CHECK (std::abs ((double) r.size() - (double) h.size() * to / from) < 2.0);
        const double offset = response (r, to, 2000.0) - response (h, from, 2000.0);
        for (double hz : { 300.0, 1000.0, 3000.0, 8000.0, 15000.0 })
        {
            INFO ("pre-delay " << preDelay << ", " << hz << " Hz");
            // An IR that starts at its very first sample loses the interpolator's leading half
            // there: a little top end, far above where any speaker radiates
            CHECK (std::abs (response (r, to, hz) - response (h, from, hz) - offset) < (preDelay > 0 ? 0.05 : hz > 5000.0 ? 1.0 : 0.2));
        }
    }

    // Longer than the limit: cut short
    const std::vector<float> longOne ((size_t) (2.0 * from), 0.001f);
    CHECK (prepareUserImpulse (longOne, from, from, maxImpulseSeconds).size() == (size_t) (maxImpulseSeconds * from));
}

TEST_CASE ("overdriven stages don't block", "[amp][blocking]")
{
    // Grid current charges an overdriven stage's coupling cap and shifts its bias colder. A little
    // is the feel of a real amp; driving a grid with tens of volts (no attenuation between stages)
    // holds it cut off for most of every cycle, which chokes and spits. Bound the shift while a
    // hard note sustains with everything turned up (where a real amp does block a little).
    const int model = GENERATE (range (0, numModels));
    const auto& spec = modelSpec ((AmpModel) model);
    const auto& curves = modelCurves ((AmpModel) model);
    const double highRate = 4.0 * rate;
    std::array<PreampStage, 4> stages;
    for (int s = 0; s < spec.numStages; ++s)
        stages[(size_t) s].prepare (spec.stages[(size_t) s], curves.stage[(size_t) s], curves.unbypassed[(size_t) s], highRate);
    ToneStack stack;
    stack.prepare (highRate);
    stack.set (spec.toneStack, 1.0, 1.0, 1.0);

    std::array<float, 4> worst {};
    for (int i = 0; i < (int) (0.5 * highRate); ++i)
    {
        const double t = i / highRate;
        float v = (float) (0.5 * spec.inputVolts * std::sin (2.0 * std::numbers::pi * 110.0 * t)); // hard picking, full gain
        for (int s = 0; s < spec.numStages; ++s)
        {
            v = stages[(size_t) s].process (v);
            worst[(size_t) s] = std::max (worst[(size_t) s], std::abs (stages[(size_t) s].biasShift()));
            if (s == spec.toneStackAfter)
                v = stack.process (v);
        }
    }
    for (int s = 0; s < spec.numStages; ++s)
    {
        INFO (spec.name << " stage " << s << ": bias shift " << worst[(size_t) s] << " V, bias " << curves.stage[(size_t) s].bias() << " V");
        CHECK (worst[(size_t) s] < 5.0f * (float) -curves.stage[(size_t) s].bias());
    }
}

TEST_CASE ("tuner reads guitar notes to a fraction of a cent", "[tuner]")
{
    // Plucked-string-like tones (decaying harmonics, the octave strong) across the neck, slightly
    // off pitch, with a little noise
    const double detectRate = 48000.0 / 4.0;
    PitchDetector detector (detectRate, 2048);
    dustify::Random rng (21);
    for (int note : { 35, 40, 45, 50, 55, 59, 64, 69, 76, 84 }) // B1 (7-string / drop) .. C6
    {
        for (double offCents : { -23.0, 0.0, 7.5 })
        {
            const double f = 440.0 * std::pow (2.0, (note - 69 + offCents / 100.0) / 12.0);
            std::vector<float> x (2048);
            for (size_t i = 0; i < x.size(); ++i)
            {
                const double t = (double) i / detectRate;
                double v = 0.0;
                for (int h = 1; h <= 8; ++h)
                    if (h * f < 0.45 * detectRate)
                        v += (h == 2 ? 0.9 : 1.0 / h) * std::sin (2.0 * std::numbers::pi * h * f * t + h);
                x[i] = (float) (v * std::exp (-t * 2.0) + 0.01 * rng.bipolar());
            }
            const auto result = detector.detect (x.data());
            const auto reading = nearestNote (result.hz);
            INFO ("note " << note << " " << offCents << " cents: read " << result.hz << " Hz, note " << reading.midiNote << " " << reading.cents);
            CHECK (reading.midiNote == note);
            CHECK (std::abs (reading.cents - offCents) < 0.5);
            CHECK (result.clarity > 0.8);
        }
    }

    // Noise and silence give no reading
    std::vector<float> hiss (2048), silence (2048, 0.0f);
    for (auto& s : hiss)
        s = 0.1f * rng.bipolar();
    CHECK (detector.detect (hiss.data()).hz == 0.0);
    CHECK (detector.detect (silence.data()).hz == 0.0);

    // A different reference pitch
    CHECK (nearestNote (432.0, 432.0).midiNote == 69);
    CHECK (std::abs (nearestNote (440.0, 432.0).cents - 31.77) < 0.05);
}
