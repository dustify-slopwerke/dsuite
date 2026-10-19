#include "AnalysisHelpers.h"

#include "common/dsp/BbdChorus.h"
#include "common/dsp/Decimator.h"
#include "common/dsp/Delay.h"
#include "common/dsp/Reverb.h"
#include "common/dsp/Envelope.h"
#include "common/dsp/FastMath.h"
#include "dsp/LadderFilter.h"
#include "common/dsp/Lfo.h"
#include "common/dsp/Noise.h"
#include "dsp/Oscillator.h"
#include "dsp/OtaFilter.h"
#include "dsp/SemFilter.h"
#include "common/dsp/Svf.h"
#include "dsp/JunoHpf.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

using namespace dsynth;
using Catch::Matchers::WithinAbs;

namespace
{
constexpr double internalRate = 96000.0;
constexpr double hostRate = 48000.0;

// Oscillator at the internal rate, decimated to the host rate, like the engine does it
std::vector<float> renderOscillator (Wave wave, double hz, float pw, double seconds, double syncRatio = 0.0)
{
    Oscillator osc, master;
    HalfbandDecimator decimator;
    std::vector<float> out;
    const auto n = (size_t) (seconds * hostRate);
    out.reserve (n);

    float pair[2];
    for (size_t i = 0; i < n; ++i)
    {
        for (float& s : pair)
        {
            double sync = -1.0;
            if (syncRatio > 0.0)
            {
                master.process (hz / internalRate, Wave::Saw, 0.5f);
                sync = master.didWrap() ? master.wrapDistanceSamples() : -1.0;
            }
            s = osc.process (hz * (syncRatio > 0.0 ? syncRatio : 1.0) / internalRate, wave, pw, sync);
        }
        out.push_back (decimator.process (pair[0], pair[1]));
    }
    return out;
}
} // namespace

TEST_CASE ("Oscillator runs at the requested frequency")
{
    const double hz = GENERATE (55.0, 440.0, 3520.0);
    // (combined waves cross zero several times per cycle; the aliasing test covers their pitch)
    const auto wave = GENERATE (Wave::Sine, Wave::Saw, Wave::Square, Wave::Triangle);
    const auto out = renderOscillator (wave, hz, 0.5f, 1.0);
    CHECK_THAT (test::cents (test::estimateFrequency (out, hostRate, 1000), hz), WithinAbs (0.0, 0.1));
}

TEST_CASE ("Band-limited waveforms keep aliasing low at 2x oversampling")
{
    // A high, inharmonic-to-the-sample-rate note is the worst case for aliasing
    const double hz = 2637.02; // E7

    // polyBLEP's residual aliasing piles up just below Nyquist, so the full band and the more
    // audible band below 10 kHz get separate limits
    struct Case { Wave wave; float pw; double maxFullBandDb, maxBelow10kDb; };
    const auto c = GENERATE (Case { Wave::Saw, 0.5f, -52.0, -68.0 }, Case { Wave::Ramp, 0.5f, -52.0, -68.0 },
                             Case { Wave::Square, 0.5f, -52.0, -68.0 }, Case { Wave::Pulse, 0.1f, -52.0, -60.0 },
                             Case { Wave::Triangle, 0.5f, -80.0, -90.0 },
                             Case { Wave::SawPulse, 0.3f, -52.0, -64.0 }, Case { Wave::TriPulse, 0.3f, -52.0, -60.0 });

    const auto out = renderOscillator (c.wave, hz, c.pw, 1.0);
    double where = 0.0;
    const double fullBand = test::worstAliasDb (out, hostRate, hz, 20000.0, &where);
    const double below10k = test::worstAliasDb (out, hostRate, hz, 10000.0);
    INFO ("wave " << (int) c.wave << " worst alias " << fullBand << " dB at " << where << " Hz, " << below10k << " dB below 10 kHz");
    CHECK (fullBand < c.maxFullBandDb);
    CHECK (below10k < c.maxBelow10kDb);
}

TEST_CASE ("Hard sync stays band-limited")
{
    const double hz = 880.0;
    const auto out = renderOscillator (Wave::Saw, hz, 0.5f, 1.0, 2.37);
    for (float s : out)
        REQUIRE (std::isfinite (s));

    // The synced waveform repeats at the master frequency
    const double alias = test::worstAliasDb (out, hostRate, hz, 20000.0);
    INFO ("worst alias " << alias << " dB");
    CHECK (alias < -50.0);
}

TEST_CASE ("Ladder self-oscillates at its cutoff")
{
    const double cutoff = GENERATE (110.0, 440.0, 1000.0, 4000.0);

    LadderFilter filter;
    filter.setSampleRate (internalRate);
    Random rng (7);
    std::vector<float> out;
    for (int i = 0; i < (int) internalRate * 2; ++i)
        out.push_back (filter.process (i == 0 ? 0.1f : 1.0e-5f * rng.bipolar(), cutoff, 1.0f, 0.0f));

    const auto half = out.size() / 2;
    const double f = test::estimateFrequency (out, internalRate, half);
    INFO ("cutoff " << cutoff << " oscillates at " << f << " Hz (" << test::cents (f, cutoff) << " cents)");
    CHECK (std::abs (test::cents (f, cutoff)) < 1.0);
    CHECK (test::rms (out, half) > 0.1);
}

TEST_CASE ("Ladder rings down below the self-oscillation point")
{
    LadderFilter filter;
    filter.setSampleRate (internalRate);
    std::vector<float> out;
    for (int i = 0; i < (int) internalRate; ++i)
        out.push_back (filter.process (i == 0 ? 0.5f : 0.0f, 1000.0, 0.85f, 0.0f));
    CHECK (test::rms (out, out.size() - 4800) < 1.0e-4);
}

TEST_CASE ("Ladder passes the passband and rolls off at 24 dB/octave")
{
    auto gainAt = [] (double hz)
    {
        LadderFilter filter;
        filter.setSampleRate (internalRate);
        std::vector<float> out;
        for (int i = 0; i < (int) internalRate; ++i)
            out.push_back (filter.process (0.01f * (float) std::sin (2.0 * std::numbers::pi * hz * i / internalRate), 1000.0, 0.0f, 0.0f));
        return test::db (test::rms (out, out.size() / 2) * std::sqrt (2.0) / 0.01);
    };

    CHECK_THAT (gainAt (100.0), WithinAbs (-0.17, 0.02)); // 4 x -0.043 dB a decade below cutoff
    CHECK_THAT (gainAt (1000.0), WithinAbs (-12.0, 0.5)); // four poles at -3 dB each
    CHECK_THAT (gainAt (8000.0), WithinAbs (-72.0, 4.0)); // 3 octaves above
}

TEST_CASE ("Analog envelope segments take their set times")
{
    constexpr double rate = internalRate;
    Envelope env;
    env.setSampleRate (rate);
    env.setParameters (0.1f, 0.2f, 0.5f, 0.3f, false);
    env.gateOn();

    int attackSamples = 0;
    while (env.getStage() == Envelope::Stage::Attack)
    {
        env.next();
        ++attackSamples;
    }
    CHECK_THAT (attackSamples / rate, WithinAbs (0.1, 0.001));

    for (int i = 0; i < (int) (0.2 * rate); ++i)
        env.next();
    CHECK_THAT (env.getLevel(), WithinAbs (0.5, 0.0051)); // 99% of the way from 1.0 to 0.5

    for (int i = 0; i < (int) rate; ++i)
        env.next();
    env.gateOff();
    for (int i = 0; i < (int) (0.3 * rate); ++i)
        env.next();
    CHECK (env.getLevel() < 0.5f * 0.0101f);
}

TEST_CASE ("Linear envelope ramps at constant rate")
{
    constexpr double rate = internalRate;
    Envelope env;
    env.setSampleRate (rate);
    env.setParameters (0.05f, 0.1f, 0.25f, 0.1f, true);
    env.gateOn();
    for (int i = 0; i < (int) (0.025 * rate); ++i)
        env.next();
    CHECK_THAT (env.getLevel(), WithinAbs (0.5, 0.001));
}

TEST_CASE ("Envelope retrigger continues from the current level")
{
    Envelope env;
    env.setSampleRate (internalRate);
    env.setParameters (0.01f, 0.1f, 0.5f, 0.5f, false);
    env.gateOn();
    for (int i = 0; i < 20000; ++i)
        env.next();
    env.gateOff();
    for (int i = 0; i < 5000; ++i)
        env.next();
    const float before = env.getLevel();
    env.gateOn();
    CHECK_THAT (env.next(), WithinAbs (before, 0.01));
}

TEST_CASE ("Halfband decimator passes the audio band and rejects images")
{
    auto gain = [] (double hz)
    {
        HalfbandDecimator dec;
        std::vector<float> out;
        for (int i = 0; i < 48000; ++i)
        {
            auto s = [&] (int k) { return (float) std::sin (2.0 * std::numbers::pi * hz * k / internalRate); };
            out.push_back (dec.process (s (2 * i), s (2 * i + 1)));
        }
        return test::db (test::rms (out, 1000) * std::sqrt (2.0));
    };

    CHECK_THAT (gain (1000.0), WithinAbs (0.0, 0.01));
    CHECK_THAT (gain (18000.0), WithinAbs (0.0, 0.05));
    CHECK (gain (30000.0) < -80.0); // would alias to 18 kHz
    CHECK (gain (40000.0) < -80.0);
}

TEST_CASE ("4x cascade (short first stage + halfband) passes the audio band and rejects images")
{
    const double inputRate = 4.0 * internalRate / 2.0; // 4x the host rate
    auto gain = [&] (double hz)
    {
        HalfbandDecimatorT<11> first;
        HalfbandDecimator last;
        std::vector<float> out;
        for (int i = 0; i < 48000; ++i)
        {
            auto s = [&] (int k) { return (float) std::sin (2.0 * std::numbers::pi * hz * k / inputRate); };
            const float a = first.process (s (4 * i), s (4 * i + 1));
            const float b = first.process (s (4 * i + 2), s (4 * i + 3));
            out.push_back (last.process (a, b));
        }
        return test::db (test::rms (out, 1000) * std::sqrt (2.0));
    };

    CHECK_THAT (gain (1000.0), WithinAbs (0.0, 0.01));
    CHECK_THAT (gain (18000.0), WithinAbs (0.0, 0.05));
    for (double hz : { 30000.0, 50000.0, 70000.0, 78000.0, 90000.0, 95000.0 })
    {
        INFO (hz << " Hz");
        CHECK (gain (hz) < -80.0);
    }
}

TEST_CASE ("CEM filter self-oscillates close to its cutoff")
{
    const double cutoff = GENERATE (110.0, 440.0, 1000.0, 4000.0);

    OtaFilter filter;
    filter.setSampleRate (internalRate);
    Random rng (7);
    std::vector<float> out;
    for (int i = 0; i < (int) internalRate * 2; ++i)
        out.push_back (filter.process (i == 0 ? 0.1f : 1.0e-5f * rng.bipolar(), cutoff, 1.0f, 0.0f));

    const auto half = out.size() / 2;
    const double f = test::estimateFrequency (out, internalRate, half);
    INFO ("cutoff " << cutoff << " oscillates at " << f << " Hz (" << test::cents (f, cutoff) << " cents)");
    CHECK (std::abs (test::cents (f, cutoff)) < 10.0); // analog-like, not tuned to the cent
    CHECK (test::rms (out, half) > 0.1);
}

TEST_CASE ("CEM filter response and ring-down")
{
    auto gainAt = [] (double hz)
    {
        OtaFilter filter;
        filter.setSampleRate (internalRate);
        std::vector<float> out;
        for (int i = 0; i < (int) internalRate; ++i)
            out.push_back (filter.process (0.01f * (float) std::sin (2.0 * std::numbers::pi * hz * i / internalRate), 1000.0, 0.0f, 0.0f));
        return test::db (test::rms (out, out.size() / 2) * std::sqrt (2.0) / 0.01);
    };
    CHECK_THAT (gainAt (100.0), WithinAbs (-0.17, 0.02));
    CHECK_THAT (gainAt (1000.0), WithinAbs (-12.0, 0.5));
    CHECK_THAT (gainAt (8000.0), WithinAbs (-72.0, 4.0));

    OtaFilter filter;
    filter.setSampleRate (internalRate);
    std::vector<float> out;
    for (int i = 0; i < (int) internalRate; ++i)
        out.push_back (filter.process (i == 0 ? 0.5f : 0.0f, 1000.0, 0.85f, 0.0f));
    CHECK (test::rms (out, out.size() - 4800) < 1.0e-4);
}

TEST_CASE ("LFO runs at its rate; S&H changes once per cycle")
{
    const double rate = 1000.0;
    Lfo lfo;
    lfo.setSampleRate (rate);

    std::vector<float> sine;
    for (int i = 0; i < 10000; ++i)
        sine.push_back (lfo.next (4.0, LfoWave::Sine));
    CHECK_THAT (test::estimateFrequency (sine, rate), WithinAbs (4.0, 0.01));

    lfo.reset();
    int changes = 0;
    float last = lfo.next (5.0, LfoWave::SampleHold);
    for (int i = 0; i < 1000; ++i)
    {
        const float v = lfo.next (5.0, LfoWave::SampleHold);
        changes += v != last;
        last = v;
    }
    CHECK (changes == 5);
}

TEST_CASE ("IR3109 filter self-oscillates close to its cutoff in both modes")
{
    const double cutoff = GENERATE (110.0, 440.0, 1000.0, 4000.0);
    const bool twoPole = GENERATE (false, true);

    OtaFilter filter;
    filter.setSampleRate (internalRate);
    filter.setCharacter (OtaFilter::ir3109);
    Random rng (7);
    std::vector<float> out;
    for (int i = 0; i < (int) internalRate * 2; ++i)
        out.push_back (filter.process (i == 0 ? 0.1f : 1.0e-5f * rng.bipolar(), cutoff, 1.0f, 0.0f, twoPole));

    const auto half = out.size() / 2;
    const double f = test::estimateFrequency (out, internalRate, half);
    INFO ("cutoff " << cutoff << (twoPole ? " (12 dB)" : " (24 dB)") << " oscillates at " << f << " Hz (" << test::cents (f, cutoff) << " cents)");
    CHECK (std::abs (test::cents (f, cutoff)) < 12.0);
}

TEST_CASE ("IR3109 12 dB mode rolls off at 12 dB/octave")
{
    auto gainAt = [] (double hz)
    {
        OtaFilter filter;
        filter.setSampleRate (internalRate);
        filter.setCharacter (OtaFilter::ir3109);
        std::vector<float> out;
        for (int i = 0; i < (int) internalRate; ++i)
            out.push_back (filter.process (0.01f * (float) std::sin (2.0 * std::numbers::pi * hz * i / internalRate), 500.0, 0.0f, 0.0f, true));
        return test::db (test::rms (out, out.size() / 2) * std::sqrt (2.0) / 0.01);
    };
    CHECK_THAT (gainAt (50.0), WithinAbs (0.0, 0.2));
    CHECK_THAT (gainAt (8000.0) - gainAt (4000.0), WithinAbs (-12.0, 1.0));
}

TEST_CASE ("Juno HPF positions")
{
    auto gainAt = [] (HpfMode mode, double hz)
    {
        JunoHpf hpf;
        hpf.prepare (internalRate);
        hpf.setMode (mode);
        std::vector<float> out;
        for (int i = 0; i < (int) internalRate; ++i)
            out.push_back (hpf.process (0.1f * (float) std::sin (2.0 * std::numbers::pi * hz * i / internalRate)));
        return test::db (test::rms (out, out.size() / 2) * std::sqrt (2.0) / 0.1);
    };

    CHECK_THAT (gainAt (HpfMode::Off, 50.0), WithinAbs (0.0, 0.01));
    CHECK_THAT (gainAt (HpfMode::BassBoost, 30.0), WithinAbs (5.5, 0.6));
    CHECK_THAT (gainAt (HpfMode::BassBoost, 2000.0), WithinAbs (0.0, 0.1));
    CHECK_THAT (gainAt (HpfMode::One, 225.0), WithinAbs (-3.0, 0.1));
    CHECK_THAT (gainAt (HpfMode::Three, 1100.0), WithinAbs (-3.0, 0.1));
    CHECK (gainAt (HpfMode::Two, 100.0) < -20.0);
    CHECK_THAT (gainAt (HpfMode::Three, 8000.0), WithinAbs (0.0, 0.1));
}

TEST_CASE ("BBD chorus widens to stereo without mono cancellation")
{
    const auto mode = GENERATE (ChorusMode::I, ChorusMode::II, ChorusMode::Both);

    BbdChorus chorus;
    chorus.prepare (hostRate);
    chorus.setMode (mode);

    // Broadband input: any single frequency can land in a notch of the chorus comb
    const auto n = (size_t) (4.0 * hostRate);
    std::vector<float> left (n), right (n);
    Random rng (3);
    for (size_t i = 0; i < n; ++i)
        left[i] = right[i] = 0.3f * rng.bipolar();
    chorus.process (left.data(), right.data(), (int) n);

    double sumLR = 0.0, sumLL = 0.0, sumRR = 0.0;
    std::vector<float> mono (n);
    for (size_t i = n / 4; i < n; ++i)
    {
        sumLR += (double) left[i] * right[i];
        sumLL += (double) left[i] * left[i];
        sumRR += (double) right[i] * right[i];
        mono[i] = 0.5f * (left[i] + right[i]);
    }
    const double correlation = sumLR / std::sqrt (sumLL * sumRR);
    INFO ("mode " << (int) mode << " L/R correlation " << correlation);
    CHECK (correlation < 0.98);
    CHECK_THAT (test::db (test::rms (left, n / 4) / test::rms (right, n / 4)), WithinAbs (0.0, 0.5));
    // Mono sum stays within a few dB of the stereo level: no deep cancellation
    CHECK (test::db (test::rms (mono, n / 4) / test::rms (left, n / 4)) > -3.0);
    for (float s : left)
        REQUIRE (std::isfinite (s));
}

TEST_CASE ("BBD chorus delay sweeps the Juno range")
{
    BbdChorus chorus;
    chorus.prepare (hostRate);
    chorus.setMode (ChorusMode::I);

    // A click every 0.1 s; the wet copy follows a few milliseconds later
    const auto n = (size_t) (2.0 * hostRate);
    std::vector<float> left (n), right (n);
    for (size_t i = 0; i < n; i += 4800)
        left[i] = right[i] = 1.0f;
    chorus.process (left.data(), right.data(), (int) n);

    double minDelay = 1.0e9, maxDelay = 0.0;
    for (size_t click = 0; click + 4800 < n; click += 4800)
    {
        size_t peak = click + 24;
        for (size_t i = click + 24; i < click + 4800; ++i)
            if (std::abs (left[i]) > std::abs (left[peak]))
                peak = i;
        const double ms = (double) (peak - click) / hostRate * 1000.0;
        minDelay = std::min (minDelay, ms);
        maxDelay = std::max (maxDelay, ms);
    }
    INFO ("delay " << minDelay << " .. " << maxDelay << " ms");
    CHECK (minDelay > 1.4);
    CHECK (minDelay < 2.5);
    CHECK (maxDelay > 4.5);
    CHECK (maxDelay < 5.8);
}

namespace
{
// Steady-state gain of a filter at one frequency, in dB, for a small sine
template <typename Process>
double gainDb (double hz, Process&& process, float amplitude = 0.01f)
{
    std::vector<float> out;
    for (int i = 0; i < (int) internalRate; ++i)
        out.push_back (process (amplitude * (float) std::sin (2.0 * std::numbers::pi * hz * i / internalRate)));
    return test::db (test::rms (out, out.size() / 2) * std::sqrt (2.0) / amplitude);
}
} // namespace

TEST_CASE ("SEM filter shapes: low-pass, notch, high-pass, bandpass")
{
    auto sem = [] (float shape, bool bp)
    {
        return [shape, bp, f = [] { SemFilter s; s.setSampleRate (internalRate); return s; }()] (float x) mutable
        { return f.process (x, 1000.0, 0.0f, shape, bp); };
    };

    // Low-pass: flat below, 12 dB/oct above, -6 dB at cutoff (Q = 0.5 with no resonance)
    CHECK_THAT (gainDb (50.0, sem (0.0f, false)), WithinAbs (0.0, 0.1));
    CHECK_THAT (gainDb (1000.0, sem (0.0f, false)), WithinAbs (-6.0, 0.3));
    CHECK_THAT (gainDb (8000.0, sem (0.0f, false)) - gainDb (4000.0, sem (0.0f, false)), WithinAbs (-12.0, 1.0));

    // Notch: deep cut at the cutoff, passband either side
    CHECK (gainDb (1000.0, sem (0.5f, false)) < -30.0);
    CHECK_THAT (gainDb (50.0, sem (0.5f, false)), WithinAbs (0.0, 0.5));
    CHECK_THAT (gainDb (20000.0, sem (0.5f, false)), WithinAbs (0.0, 0.5));

    // High-pass mirrors the low-pass
    CHECK_THAT (gainDb (20000.0, sem (1.0f, false)), WithinAbs (0.0, 0.2));
    CHECK (gainDb (100.0, sem (1.0f, false)) < -38.0);

    // Bandpass: unity at the centre; with Q = 0.5 it's broad: 2x / (1 + x^2) = -14 dB at fc / 10
    CHECK_THAT (gainDb (1000.0, sem (0.0f, true)), WithinAbs (0.0, 0.3));
    CHECK_THAT (gainDb (100.0, sem (0.0f, true)), WithinAbs (-14.0, 0.5));
}

TEST_CASE ("SEM filter resonates but never self-oscillates")
{
    SemFilter filter;
    filter.setSampleRate (internalRate);
    std::vector<float> out;
    for (int i = 0; i < (int) internalRate; ++i)
        out.push_back (filter.process (i == 0 ? 0.5f : 0.0f, 1000.0, 1.0f, 0.0f, false));
    CHECK (test::rms (out, 0, 960) > 0.01);                      // rings
    CHECK (test::rms (out, out.size() - 4800) < 1.0e-5);         // and dies away
}

TEST_CASE ("MS-20 filter: 12 dB/oct and self-oscillation at the cutoff")
{
    auto ms20 = [] (float reso)
    {
        return [reso, f = [] { Ms20Filter m; m.setSampleRate (internalRate); return m; }()] (float x) mutable
        { return f.process (x, 1000.0, reso); };
    };
    CHECK_THAT (gainDb (50.0, ms20 (0.0f)), WithinAbs (0.0, 0.2));
    CHECK_THAT (gainDb (8000.0, ms20 (0.0f)) - gainDb (4000.0, ms20 (0.0f)), WithinAbs (-12.0, 1.0));

    const double cutoff = GENERATE (220.0, 1000.0, 4000.0);
    Ms20Filter filter;
    filter.setSampleRate (internalRate);
    Random rng (5);
    std::vector<float> out;
    for (int i = 0; i < (int) internalRate * 2; ++i)
        out.push_back (filter.process (i == 0 ? 0.1f : 1.0e-5f * rng.bipolar(), cutoff, 1.0f));
    const double f = test::estimateFrequency (out, internalRate, out.size() / 2);
    INFO ("cutoff " << cutoff << " oscillates at " << f << " Hz (" << test::cents (f, cutoff) << " cents)");
    CHECK (std::abs (test::cents (f, cutoff)) < 2.0);
    CHECK (test::rms (out, out.size() / 2) > 0.1);
}

TEST_CASE ("ARP ladder keeps more low end than the Moog at high resonance")
{
    auto ladder = [] (LadderFilter::Character c)
    {
        return [f = [c] { LadderFilter l; l.setSampleRate (internalRate); l.setCharacter (c); return l; }()] (float x) mutable
        { return f.process (x, 1000.0, 0.8f, 0.0f); };
    };
    const double moog = gainDb (100.0, ladder (LadderFilter::moog));
    const double arp = gainDb (100.0, ladder (LadderFilter::arp));
    INFO ("100 Hz at resonance 0.8: Moog " << moog << " dB, ARP " << arp << " dB");
    CHECK (moog < -8.0);
    CHECK (arp > moog + 5.0);
}

TEST_CASE ("Voice high-pass: off by default, 12 dB/oct, resonant peak")
{
    auto hpf = [] (double cutoff, float reso)
    {
        return [f = [cutoff, reso] { VoiceHighpass h; h.setSampleRate (internalRate); h.setup (cutoff, reso); return h; }()] (float x) mutable
        { return f.process (x); };
    };
    CHECK_THAT (gainDb (20.0, hpf (10.0, 0.0f)), WithinAbs (0.0, 1.0e-6)); // bypassed
    CHECK_THAT (gainDb (1000.0, hpf (1000.0, 0.0f)), WithinAbs (-3.0, 0.2));
    CHECK_THAT (gainDb (250.0, hpf (1000.0, 0.0f)) - gainDb (500.0, hpf (1000.0, 0.0f)), WithinAbs (-12.0, 1.0));
    CHECK (gainDb (1000.0, hpf (1000.0, 1.0f)) > 15.0);
}

TEST_CASE ("Delay: echo timing, feedback decay, ping-pong alternation")
{
    const size_t n = (size_t) (2.0 * hostRate);

    SECTION ("Stereo: echoes every 250 ms, each about 'feedback' times the last")
    {
        StereoDelay delay;
        delay.prepare (hostRate);
        delay.setParameters (DelayMode::Stereo, 0.25, 0.5f, 20000.0f, 1.0f);
        std::vector<float> left (n), right (n);
        left[0] = right[0] = 0.1f;
        delay.process (left.data(), right.data(), (int) n);

        double previous = 0.0;
        for (int echo = 1; echo <= 3; ++echo)
        {
            const auto at = (size_t) (echo * 0.25 * hostRate);
            size_t peak = at - 50;
            for (size_t i = at - 50; i < at + 50; ++i)
                if (std::abs (left[i]) > std::abs (left[peak]))
                    peak = i;
            INFO ("echo " << echo);
            CHECK (std::abs ((long) peak - (long) at) <= 2);
            if (echo > 1)
                CHECK_THAT (std::abs (left[peak]) / previous, WithinAbs (0.5, 0.05));
            previous = std::abs (left[peak]);
        }
    }

    SECTION ("Ping-pong: odd echoes on the left, even echoes on the right")
    {
        StereoDelay delay;
        delay.prepare (hostRate);
        delay.setParameters (DelayMode::PingPong, 0.25, 0.6f, 20000.0f, 1.0f);
        std::vector<float> left (n), right (n);
        left[0] = right[0] = 0.1f;
        delay.process (left.data(), right.data(), (int) n);

        auto energyAt = [&] (const std::vector<float>& x, int echo)
        {
            const auto at = (size_t) (echo * 0.25 * hostRate);
            return test::rms (x, at - 100, at + 100);
        };
        CHECK (energyAt (left, 1) > 10.0 * energyAt (right, 1));
        CHECK (energyAt (right, 2) > 10.0 * energyAt (left, 2));
        CHECK (energyAt (left, 3) > 10.0 * energyAt (right, 3));
    }
}

TEST_CASE ("Reverb: decay time follows the setting, stereo tail, dies away")
{
    const float decay = GENERATE (1.0f, 3.0f);

    FdnReverb reverb;
    reverb.prepare (hostRate);
    reverb.setParameters (0.6f, decay, 0.0f, 0.0f);
    reverb.setMix (1.0f);

    const size_t n = (size_t) ((decay * 1.5 + 0.5) * hostRate);
    std::vector<float> left (n), right (n);
    left[0] = right[0] = 1.0f;
    reverb.process (left.data(), right.data(), (int) n);
    left[0] = right[0] = 0.0f;

    // RT60 from the slope of the energy in 50 ms windows, between -5 and -35 dB (T30 x 2)
    auto windowDb = [&] (size_t start) { return test::db (test::rms (left, start, start + 2400)); };
    const double reference = windowDb ((size_t) (0.1 * hostRate));
    double t5 = -1.0, t35 = -1.0;
    for (size_t start = (size_t) (0.1 * hostRate); start + 2400 < n; start += 480)
    {
        const double level = windowDb (start) - reference;
        if (t5 < 0.0 && level < -5.0) t5 = (double) start / hostRate;
        if (t35 < 0.0 && level < -35.0) t35 = (double) start / hostRate;
    }
    REQUIRE (t35 > 0.0);
    const double rt60 = (t35 - t5) * 2.0;
    INFO ("set " << decay << " s, measured RT60 " << rt60 << " s");
    CHECK_THAT (rt60, WithinAbs (decay, 0.25 * decay));

    double lr = 0.0, ll = 0.0, rr = 0.0;
    for (size_t i = 2400; i < n / 2; ++i)
    {
        lr += (double) left[i] * right[i];
        ll += (double) left[i] * left[i];
        rr += (double) right[i] * right[i];
    }
    CHECK (std::abs (lr / std::sqrt (ll * rr)) < 0.5);
    for (float s : left)
        REQUIRE (std::isfinite (s));
}

TEST_CASE ("Fast tanh approximation stays within 1e-4 of std::tanh")
{
    double worstTanh = 0.0, worstSlope = 0.0;
    for (double x = -12.0; x <= 12.0; x += 0.001)
    {
        worstTanh = std::max (worstTanh, std::abs (fastTanh (x) - std::tanh (x)));
        if (std::abs (x) > 1.0e-3)
            worstSlope = std::max (worstSlope, std::abs (tanhSlope (x) - std::tanh (x) / x));
    }
    INFO ("worst tanh error " << worstTanh << ", worst slope error " << worstSlope);
    CHECK (worstTanh < 1.0e-4);
    CHECK (worstSlope < 1.0e-4);
    CHECK (tanhSlope (0.0) == 1.0);
}

TEST_CASE ("Fast exp2, tan and sin match the standard library")
{
    double worstExp = 0.0, worstTan = 0.0, worstSin = 0.0;
    for (double x = -20.0; x <= 20.0; x += 0.001)
        worstExp = std::max (worstExp, std::abs (fastExp2 (x) / std::exp2 (x) - 1.0));
    for (double x = 0.0; x <= 0.45 * std::numbers::pi; x += 0.0001)
        worstTan = std::max (worstTan, std::abs (fastTan (x) / std::max (std::tan (x), 1.0e-12) - 1.0) * (x > 1.0e-6));
    for (double p = -3.0; p <= 3.0; p += 0.0001)
        worstSin = std::max (worstSin, std::abs (fastSin2Pi (p) - std::sin (2.0 * std::numbers::pi * p)));
    INFO ("exp2 " << worstExp << ", tan " << worstTan << ", sin " << worstSin);
    CHECK (worstExp < 1.0e-8);  // 0.00002 cents as a pitch
    CHECK (worstTan < 1.0e-7);
    CHECK (worstSin < 3.0e-8);
}
