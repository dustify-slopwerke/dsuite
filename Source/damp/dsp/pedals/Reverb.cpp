#include "Reverb.h"

#include "common/dsp/FastMath.h"

#include <bit>
#include <cmath>
#include <numbers>

namespace damp::pedals
{
namespace
{
constexpr float silence = 3.16e-5f; // -90 dBFS: below this the tail counts as gone

inline float sine (double phase) { return (float) dustify::fastSin2Pi (phase); }

// Linear up to 1, then easing towards 2: keeps a feedback loop bounded
inline float softLimit (float v)
{
    const float a = std::abs (v);
    if (a <= 1.0f)
        return v;
    const float over = a - 1.0f;
    return std::copysign (1.0f + over / (1.0f + over), v);
}

inline int samplesFor (double seconds, double rate) { return (int) std::lround (seconds * rate); }

// Gain per pass for a loop of `seconds` to fall 60 dB in rt60
inline float loopGain (double seconds, double rt60) { return (float) std::pow (10.0, -3.0 * seconds / rt60); }

// The knob's decay, 0 .. 1, as RT60 seconds for each tank
double rt60For (ReverbMode mode, double decay)
{
    switch (mode)
    {
        case ReverbMode::Spring: return 1.0 * std::pow (4.0, decay);  // 1 .. 4 s, 2 s at noon
        case ReverbMode::Plate: return 0.5 * std::pow (12.0, decay);  // 0.5 .. 6 s, 1.7 s at noon
        case ReverbMode::Hall: return 1.0 * std::pow (10.0, decay);   // 1 .. 10 s, 3.2 s at noon
    }
    return 2.0;
}

// The RT60 to design each tank for, so it measures (broadband, Schroeder T30) as `rt60`: the
// loops' filters take a little more out on each pass than the gain alone, and the spring's lows
// take longer round the loop than the midrange it's designed for
double designRt60 (ReverbMode mode, double rt60)
{
    switch (mode)
    {
        case ReverbMode::Spring: return std::pow (rt60 / 0.89, 1.0 / 0.887);
        case ReverbMode::Plate: return std::pow (rt60 / 1.088, 1.0 / 0.907);
        case ReverbMode::Hall: return rt60 / 0.85;
    }
    return rt60;
}

// The wet level at full mix, per tank: each comes out about as loud as the dry signal at noon
// decay (measured through the clean amp on a strummed DI), getting a little louder as it lengthens
float wetNorm (ReverbMode mode, double rt60)
{
    switch (mode)
    {
        case ReverbMode::Spring: return 1.05f * (float) std::pow (2.0 / rt60, 0.3);
        case ReverbMode::Plate: return 0.88f * (float) std::pow (1.73 / rt60, 0.3);
        case ReverbMode::Hall: return 1.45f * (float) std::pow (3.16 / rt60, 0.3);
    }
    return 1.0f;
}
} // namespace

// ---- Line ----

void Reverb::Line::allocate (int samples)
{
    const auto size = std::bit_ceil ((unsigned) std::max (samples + 4, 8));
    buffer.assign (size, 0.0f);
    mask = (int) size - 1;
    write = 0;
}

void Reverb::Line::clear()
{
    std::fill (buffer.begin(), buffer.end(), 0.0f);
    write = 0;
}

// ---- Spring: per spring, input + loop -> allpass cascade -> loop filters -> delay -> back. The
// cascade's stages are (a + z^-K) / (1 + a z^-K) with K about rate / 8.8 kHz: the group delay
// is large at DC and falls towards 4.4 kHz, so each pass smears an echo into a falling chirp, and
// every round trip smears it further. (Its images above 4.4 kHz are filtered out.) ----

namespace
{
constexpr double springLoopMs[] { 26.3, 29.7, 33.1 };
constexpr double springStageMs[] { 0.43, 0.46, 0.49 }; // each stage's group delay at DC
constexpr double springWobbleHz[] { 0.71, 1.13, 0.53 };

int stretchFor (double rate) { return std::max (1, (int) std::lround (rate / 8800.0)); }
} // namespace

void Reverb::Spring::allocate (double maxRate)
{
    stretchCapacity = stretchFor (maxRate);
    for (auto& u : units)
    {
        u.cascade.assign ((size_t) (stages * stretchCapacity), 0.0f);
        u.loop.allocate (samplesFor (0.036, maxRate) + 4 * chunk);
    }
    rate = 0.0; // configure() works out the rest
}

void Reverb::Spring::clear()
{
    for (auto& u : units)
    {
        std::fill (u.cascade.begin(), u.cascade.end(), 0.0f);
        u.loop.clear();
        u.damp.reset();
        u.block.reset();
        u.phase = 0.0;
    }
    position = 0;
    for (auto* f : { &inputCut, &outLowL1, &outLowL2, &outLowR1, &outLowR2, &outCutL, &outCutR })
        f->reset();
    inputDark.reset();
}

void Reverb::Spring::configure (const Settings& s)
{
    if (s.rate != rate)
    {
        rate = s.rate;
        stretch = std::min (stretchFor (rate), stretchCapacity);
        clear();
        for (size_t i = 0; i < units.size(); ++i)
        {
            auto& u = units[i];
            // Stage group delay at DC is K (1 - a) / (1 + a) samples
            const double r = springStageMs[i] * 1.0e-3 * rate / stretch;
            u.coefficient = (float) ((1.0 - r) / (1.0 + r));
            u.length = (float) (springLoopMs[i] * 1.0e-3 * rate);
            u.damp.setCutoff (5000.0, rate);
            u.block.setCutoff (70.0, rate);
        }
        wobble = (float) (0.12e-3 * rate);
        inputCut.setHighpass (110.0, 0.7071, rate);
        inputDark.setCutoff (5000.0, rate);
        outCutL.setHighpass (100.0, 0.7071, rate);
        outCutR.setHighpass (100.0, 0.7071, rate);
    }

    for (size_t i = 0; i < units.size(); ++i)
    {
        auto& u = units[i];
        // The loop's length at ~600 Hz, where the drip has most of its energy: the line plus the
        // cascade's group delay there
        const double a = u.coefficient;
        const double theta = 2.0 * std::numbers::pi * 600.0 * stretch / rate;
        const double stage = stretch * (1.0 - a * a) / (1.0 + 2.0 * a * std::cos (theta) + a * a);
        const double loopSeconds = (u.length + stages * stage) / rate;
        u.gain = std::min (0.98f, loopGain (loopSeconds, s.rt60));
    }

    // Tone: the output's top end, 3 kHz .. 6.75 kHz (4.5 kHz at noon)
    const double top = 4500.0 * std::pow (1.5, 2.0 * s.tone - 1.0);
    outLowL1.setLowpass (top, 0.5412, rate);
    outLowL2.setLowpass (top, 1.3066, rate);
    outLowR1.setLowpass (top, 0.5412, rate);
    outLowR2.setLowpass (top, 1.3066, rate);
}

void Reverb::Spring::runCascade (Unit& u, float* x, int n) const
{
    const float a = u.coefficient;
    const int k = stretch;
    for (int m = 0; m < stages; ++m)
    {
        float* s = u.cascade.data() + m * k;
        int j = position;
        for (int i = 0; i < n; ++i)
        {
            const float old = s[j];
            const float w = x[i] - a * old;
            x[i] = a * w + old;
            s[j] = w;
            if (++j == k)
                j = 0;
        }
    }
}

void Reverb::Spring::process (const float* in, float* left, float* right, int n)
{
    std::array<float, chunk> drive {}, u {};
    std::array<std::array<float, chunk>, springs> out {};
    for (int from = 0; from < n; from += chunk)
    {
        const int m = std::min (chunk, n - from);
        // The driver: band-limited, and soft-clipped so loud picking splashes
        for (int i = 0; i < m; ++i)
        {
            const float x = inputDark.lowpass (inputCut.process (in[from + i]));
            drive[(size_t) i] = 0.35f * dustify::fastTanh (x * (1.0f / 0.35f));
        }

        for (size_t s = 0; s < units.size(); ++s)
        {
            auto& unit = units[s];
            // The loop length wobbles a little (the springs swing), interpolated across the chunk
            const float d0 = unit.length + wobble * sine (unit.phase);
            unit.phase += springWobbleHz[s] * m / rate;
            unit.phase -= std::floor (unit.phase);
            const float d1 = unit.length + wobble * sine (unit.phase);
            const float dd = (d1 - d0) / (float) m;
            for (int i = 0; i < m; ++i)
            {
                // Not yet pushed this chunk: the newest sample in the line is i + 1 ago
                const float z = unit.loop.tapLinear (d0 + dd * (float) i - 1.0f - (float) i);
                out[s][(size_t) i] = z;
                u[(size_t) i] = drive[(size_t) i] + unit.gain * z;
            }
            runCascade (unit, u.data(), m);
            for (int i = 0; i < m; ++i)
                unit.loop.push (softLimit (unit.block.highpass (unit.damp.lowpass (u[(size_t) i]))));
        }
        position = (position + m) % stretch;

        for (int i = 0; i < m; ++i)
        {
            const auto k = (size_t) i;
            const float l = out[0][k] + 0.5f * out[1][k], r = out[2][k] + 0.5f * out[1][k];
            left[from + i] = outCutL.process (outLowL2.process (outLowL1.process (l)));
            right[from + i] = outCutR.process (outLowR2.process (outLowR1.process (r)));
        }
    }
}

// ---- Plate: Dattorro's tank ("Effect Design, Part 1", 1997), its lengths given at 29761 Hz ----

namespace
{
constexpr double plateRate = 29761.0;
// Dattorro's four input diffusers and two more, which take the edge off the tank's first echoes
constexpr int plateDiffuser[] { 142, 107, 379, 277, 211, 461 };
constexpr float plateDiffusion[] { 0.75f, 0.75f, 0.625f, 0.625f, 0.6f, 0.6f };
constexpr int plateModulated[] { 672, 908 };
constexpr int plateDecayDiffuser[] { 1800, 2656 };
constexpr int plateDelay[] { 4453, 3720, 4217, 3163 }; // left half's two, right half's two
// The output taps: left from the right half's lines (and some of the left's, inverted), right
// the other way round
constexpr int plateTap[] { 266, 2974, 1913, 1996, 1990, 187, 1066, 353, 3627, 1228, 2673, 2111, 335, 121 };
} // namespace

void Reverb::Plate::allocate (double maxRate)
{
    const double k = maxRate / plateRate;
    const int slack = 8 + (int) std::ceil (0.0005 * maxRate);
    for (size_t i = 0; i < diffusers.size(); ++i)
        diffusers[i].line.allocate ((int) std::ceil (plateDiffuser[i] * k) + 8);
    for (size_t i = 0; i < 2; ++i)
    {
        modulated[i].line.allocate ((int) std::ceil (plateModulated[i] * k) + slack);
        decayDiffusers[i].line.allocate ((int) std::ceil (plateDecayDiffuser[i] * k) + 8);
    }
    for (size_t i = 0; i < delays.size(); ++i)
        delays[i].allocate ((int) std::ceil (plateDelay[i] * k) + 8);
    rate = 0.0;
}

void Reverb::Plate::clear()
{
    for (auto& a : diffusers)
        a.line.clear();
    for (auto* a : { &modulated[0], &modulated[1], &decayDiffusers[0], &decayDiffusers[1] })
        a->line.clear();
    for (auto& d : delays)
        d.clear();
    for (auto& d : damping)
        d.reset();
    bandwidth.reset();
    inputCut.reset();
    cutL.reset();
    cutR.reset();
    carry = {};
    phase = 0.0;
}

void Reverb::Plate::configure (const Settings& s)
{
    if (s.rate != rate)
    {
        rate = s.rate;
        const double scale = rate / plateRate;
        const auto at = [scale] (int samples) { return std::max (1, (int) std::lround (samples * scale)); };
        clear();
        for (size_t i = 0; i < diffusers.size(); ++i)
            diffuserLength[i] = at (plateDiffuser[i]);
        for (size_t i = 0; i < delays.size(); ++i)
            delayLength[i] = at (plateDelay[i]);
        for (size_t i = 0; i < 2; ++i)
        {
            decayDiffuserLength[i] = at (plateDecayDiffuser[i]);
            modulatedLength[i] = (float) (plateModulated[i] * scale);
        }
        for (size_t i = 0; i < outputTap.size(); ++i)
            outputTap[i] = at (plateTap[i]);
        excursion = (float) (0.00025 * rate); // gentle: a quarter of a millisecond
        phaseStep = 0.6 / rate;
        inputCut.setCutoff (50.0, rate); // and no DC building up in the tank
        cutL.setCutoff (60.0, rate);
        cutR.setCutoff (60.0, rate);
    }
    // Between the decay multipliers the loop averages a quarter of the whole tank
    double total = 0.0;
    for (int i = 0; i < 2; ++i)
        total += plateModulated[i] + plateDecayDiffuser[i];
    for (int d : plateDelay)
        total += d;
    decay = std::min (0.95f, loopGain (total / 4.0 / plateRate, s.rt60));
    // Tone: the damping, 2.5 kHz .. 15 kHz (6 kHz at noon); the input bandwidth above it
    const double hz = 2500.0 * std::pow (6.0, s.tone);
    for (auto& d : damping)
        d.setCutoff (hz, rate);
    bandwidth.setCutoff (std::min (2.0 * hz, 18000.0), rate);
}

void Reverb::Plate::process (const float* in, float* left, float* right, int n)
{
    const auto& t = outputTap;
    const float decayDiffusion = std::clamp (decay + 0.15f, 0.25f, 0.5f);

    for (int i = 0; i < n; ++i)
    {
        float x = bandwidth.lowpass (inputCut.highpass (in[i]));
        for (size_t d = 0; d < diffusers.size(); ++d)
            x = diffusers[d].process (x, diffuserLength[d], plateDiffusion[d]);

        phase += phaseStep;
        phase -= std::floor (phase);
        const float wob0 = excursion * sine (phase), wob1 = excursion * sine (phase + 0.25);

        // Left half, fed by the right half's end
        float a = modulated[0].processModulated (x + carry[1], modulatedLength[0] + wob0, -0.7f);
        delays[0].push (a);
        a = damping[0].lowpass (delays[0].tap (delayLength[0])) * decay;
        a = decayDiffusers[0].process (a, decayDiffuserLength[0], decayDiffusion);
        delays[1].push (a);
        const float endLeft = delays[1].tap (delayLength[1]) * decay;

        // Right half, fed by the left half's end
        float b = modulated[1].processModulated (x + carry[0], modulatedLength[1] + wob1, -0.7f);
        delays[2].push (b);
        b = damping[1].lowpass (delays[2].tap (delayLength[2])) * decay;
        b = decayDiffusers[1].process (b, decayDiffuserLength[1], decayDiffusion);
        delays[3].push (b);
        const float endRight = delays[3].tap (delayLength[3]) * decay;

        carry = { endLeft, endRight };

        const float l = delays[2].tap (t[0]) + delays[2].tap (t[1]) - decayDiffusers[1].line.tap (t[2]) + delays[3].tap (t[3])
                        - delays[0].tap (t[4]) - decayDiffusers[0].line.tap (t[5]) - delays[1].tap (t[6]);
        const float r = delays[0].tap (t[7]) + delays[0].tap (t[8]) - decayDiffusers[0].line.tap (t[9]) + delays[1].tap (t[10])
                        - delays[2].tap (t[11]) - decayDiffusers[1].line.tap (t[12]) - delays[3].tap (t[13]);
        left[i] = cutL.highpass (0.6f * l);
        right[i] = cutR.highpass (0.6f * r);
    }
}

// ---- Hall: early reflections off a tapped line, then a diffused feed into eight delay lines
// mixed through a Hadamard matrix. Each line has an allpass (density), a damping low-pass and a
// slowly wandering length (no fixed modes to ring). ----

namespace
{
constexpr double hallLineMs[] { 53.7, 61.3, 67.9, 76.1, 83.3, 91.7, 101.9, 113.3 };
constexpr double hallInnerMs[] { 7.1, 8.9, 10.3, 11.9, 6.3, 9.7, 12.7, 5.3 };
constexpr double hallWanderHz[] { 0.11, 0.17, 0.13, 0.23, 0.19, 0.29, 0.31, 0.37 };
constexpr double hallDiffuserMs[] { 1.7, 2.9, 4.3, 6.1, 8.9, 12.7 };
constexpr float hallDiffusion = 0.65f;
constexpr double hallReflectionMs[] { 5.3, 8.9, 12.7, 16.3, 20.9, 24.7, 29.9, 33.7, 39.7, 45.7, 52.3, 59.9 };
// Each reflection's level on the left and right (alternating sides, falling with time)
constexpr float hallReflectionL[] { 0.80f, 0.25f, 0.62f, -0.30f, 0.48f, 0.28f, -0.40f, 0.22f, 0.30f, -0.16f, 0.15f, 0.08f };
constexpr float hallReflectionR[] { 0.30f, 0.72f, -0.22f, 0.55f, 0.26f, -0.44f, 0.30f, 0.38f, -0.16f, 0.24f, 0.08f, 0.14f };
constexpr float hallInSign[] { 1, -1, 1, 1, -1, 1, -1, -1 };
constexpr float hallOutL[] { 1, 1, -1, 1, -1, 1, 1, -1 };
constexpr float hallOutR[] { 1, -1, 1, 1, 1, -1, 1, -1 };

// In place, unnormalised
inline void hadamard8 (std::array<float, 8>& v)
{
    for (size_t h = 1; h < 8; h *= 2)
        for (size_t i = 0; i < 8; i += 2 * h)
            for (size_t j = i; j < i + h; ++j)
            {
                const float a = v[j], b = v[j + h];
                v[j] = a + b;
                v[j + h] = a - b;
            }
}
} // namespace

void Reverb::Hall::allocate (double maxRate)
{
    early.allocate (samplesFor (0.062, maxRate));
    for (size_t i = 0; i < diffusers.size(); ++i)
        diffusers[i].line.allocate (samplesFor (hallDiffuserMs[i] * 1.0e-3, maxRate) + 4);
    for (size_t i = 0; i < (size_t) lines; ++i)
    {
        delay[i].allocate (samplesFor (hallLineMs[i] * 1.0e-3 + 0.001, maxRate) + 8);
        inner[i].line.allocate (samplesFor (hallInnerMs[i] * 1.0e-3, maxRate) + 4);
    }
    rate = 0.0;
}

void Reverb::Hall::clear()
{
    early.clear();
    for (auto& d : diffusers)
        d.line.clear();
    for (size_t i = 0; i < (size_t) lines; ++i)
    {
        delay[i].clear();
        inner[i].line.clear();
        damping[i].reset();
        phase[i] = (double) i / lines;
    }
    erDark.reset();
    cutL.reset();
    cutR.reset();
}

void Reverb::Hall::configure (const Settings& s)
{
    if (s.rate != rate)
    {
        rate = s.rate;
        clear();
        for (size_t i = 0; i < (size_t) reflections; ++i)
            reflectionTap[i] = samplesFor (hallReflectionMs[i] * 1.0e-3, rate);
        for (size_t i = 0; i < diffusers.size(); ++i)
            diffuserLength[i] = samplesFor (hallDiffuserMs[i] * 1.0e-3, rate);
        for (size_t i = 0; i < (size_t) lines; ++i)
        {
            length[i] = (float) (hallLineMs[i] * 1.0e-3 * rate);
            innerLength[i] = samplesFor (hallInnerMs[i] * 1.0e-3, rate);
        }
        excursion = (float) (0.0004 * rate);
        cutL.setCutoff (40.0, rate);
        cutR.setCutoff (40.0, rate);
    }
    for (size_t i = 0; i < (size_t) lines; ++i)
        gain[i] = std::min (0.97f, loopGain ((hallLineMs[i] + hallInnerMs[i]) * 1.0e-3, s.rt60));
    // Tone: the damping, 1.5 kHz .. 12 kHz (4.2 kHz at noon)
    const double hz = 1500.0 * std::pow (8.0, s.tone);
    for (auto& d : damping)
        d.setCutoff (hz, rate);
    erDark.setCutoff (std::min (1.5 * hz, 16000.0), rate);
}

void Reverb::Hall::process (const float* in, float* left, float* right, int n)
{
    // The lengths' wander, interpolated across the call (n is at most a chunk)
    std::array<float, lines> d0 {}, dd {};
    for (size_t k = 0; k < (size_t) lines; ++k)
    {
        d0[k] = length[k] + excursion * sine (phase[k]);
        phase[k] += hallWanderHz[k] * n / rate;
        phase[k] -= std::floor (phase[k]);
        dd[k] = (length[k] + excursion * sine (phase[k]) - d0[k]) / (float) std::max (n, 1);
    }

    constexpr float norm = 0.35355339f; // 1 / sqrt (8)
    for (int i = 0; i < n; ++i)
    {
        // The input smeared by the diffusers: the reflections take it from halfway down the chain
        // (each one a short cluster rather than a click), the network from the end
        float x = in[i];
        for (size_t d = 0; d < diffusers.size(); ++d)
        {
            x = diffusers[d].process (x, diffuserLength[d], hallDiffusion);
            if (d == 2)
                early.push (erDark.lowpass (x));
        }
        float earlyL = 0.0f, earlyR = 0.0f;
        for (size_t t = 0; t < (size_t) reflections; ++t)
        {
            const float v = early.tap (reflectionTap[t]);
            earlyL += hallReflectionL[t] * v;
            earlyR += hallReflectionR[t] * v;
        }

        std::array<float, lines> v {};
        for (size_t k = 0; k < (size_t) lines; ++k)
            v[k] = delay[k].tapCubic (d0[k] + dd[k] * (float) i - 1.0f);
        float l = 0.0f, r = 0.0f;
        for (size_t k = 0; k < (size_t) lines; ++k)
        {
            l += hallOutL[k] * v[k];
            r += hallOutR[k] * v[k];
        }
        hadamard8 (v);
        for (size_t k = 0; k < (size_t) lines; ++k)
        {
            float y = norm * v[k] + hallInSign[k] * 0.5f * x;
            y = inner[k].process (y, innerLength[k], 0.5f);
            y = damping[k].lowpass (y) * gain[k];
            delay[k].push (softLimit (y));
        }

        left[i] = cutL.highpass (norm * l + 0.5f * earlyL);
        right[i] = cutR.highpass (norm * r + 0.5f * earlyR);
    }
}

// ---- The pedal ----

void Reverb::prepare (double maxRate)
{
    for (auto& c : channels)
    {
        c.predelay.allocate (samplesFor (0.21, maxRate) + 8);
        c.spring.allocate (maxRate);
        c.plate.allocate (maxRate);
        c.hall.allocate (maxRate);
    }
    rate = maxRate;
    settings.rate = 0.0;
    reset();
}

void Reverb::reset()
{
    for (auto& c : channels)
    {
        c.predelay.clear();
        c.spring.clear();
        c.plate.clear();
        c.hall.clear();
    }
    fade = fadeFrom = 1.0f;
    fadeStep = 0.0f;
    gate = gateFrom = 0.0f;
    gateStep = 0.0f;
    quiet = 0;
    live = engaged;
    fresh = true;
}

void Reverb::setEngaged (bool on)
{
    engaged = on;
    if (on)
        live = true;
}

void Reverb::clearTank (ReverbMode m)
{
    for (auto& c : channels)
    {
        switch (m)
        {
            case ReverbMode::Spring: c.spring.clear(); break;
            case ReverbMode::Plate: c.plate.clear(); break;
            case ReverbMode::Hall: c.hall.clear(); break;
        }
    }
}

void Reverb::setPatch (const Patch& p, double sampleRate, int samples)
{
    rate = sampleRate;
    const int n = std::max (samples, 0);
    blockSamples = n;

    wantMode = (ReverbMode) std::clamp (p.index (PID::ReverbMode), 0, 2);
    if (fresh)
        mode = wantMode;
    else if (mode != wantMode && fade <= 0.0f)
    {
        mode = wantMode; // faded out: switch over to a clean tank
        clearTank (mode);
    }
    trails = p.on (PID::ReverbTrails);

    const double decay = std::clamp (p[PID::ReverbDecay] / 10.0, 0.0, 1.0);
    const double rt60 = rt60For (mode, decay);
    settings.rt60 = designRt60 (mode, rt60);
    settings.tone = std::clamp (p[PID::ReverbTone] / 10.0f, 0.0f, 1.0f);
    settings.rate = rate;
    for (auto& c : channels)
    {
        switch (mode)
        {
            case ReverbMode::Spring: c.spring.configure (settings); break;
            case ReverbMode::Plate: c.plate.configure (settings); break;
            case ReverbMode::Hall: c.hall.configure (settings); break;
        }
    }

    // The wet fades over 10 ms (trails off, mode change), the tank's input over 5 ms
    const float fadeTarget = (mode == wantMode && (engaged || trails)) ? 1.0f : 0.0f;
    const auto perSample = [this] (double seconds) { return (float) (1.0 / (seconds * rate)); };
    fadeFrom = fade;
    fadeStep = fadeTarget > fade ? perSample (0.01) : (fadeTarget < fade ? -perSample (0.01) : 0.0f);
    fade = std::clamp (fade + fadeStep * (float) n, 0.0f, 1.0f);
    const float gateTarget = engaged ? 1.0f : 0.0f;
    gateFrom = gate;
    gateStep = gateTarget > gate ? perSample (0.005) : (gateTarget < gate ? -perSample (0.005) : 0.0f);
    gate = std::clamp (gate + gateStep * (float) n, 0.0f, 1.0f);

    // Mix: the dry stays at unity; the wet rises to about the dry's loudness at 10
    const float mix = std::clamp (p[PID::ReverbMix] / 10.0f, 0.0f, 1.0f);
    const float wet = mix * wetNorm (mode, rt60);
    wetFrom = fresh ? wet : wetTo;
    wetTo = wet;

    // Pre-delay, gliding (a big jump would sweep the pitch a long way)
    const float maxDelay = (float) (0.2 * rate);
    const float target = std::clamp (p[PID::ReverbPredelay] * 1.0e-3f * (float) rate, 0.0f, maxDelay);
    delayFrom = fresh ? target : delayTo;
    const float limit = 0.25f * (float) n;
    delayTo = delayFrom + std::clamp (target - delayFrom, -limit, limit);

    quietNeeded = (int) (delayTo + 0.3 * rate);
    fresh = false;
}

void Reverb::run (Channel& c, const float* in, float* left, float* right, int n)
{
    switch (mode)
    {
        case ReverbMode::Spring: c.spring.process (in, left, right, n); break;
        case ReverbMode::Plate: c.plate.process (in, left, right, n); break;
        case ReverbMode::Hall: c.hall.process (in, left, right, n); break;
    }
}

void Reverb::finish (float peak, int n, bool countQuiet)
{
    if (peak > silence)
        quiet = 0;
    else if (countQuiet)
        quiet = std::min (quiet + n, 1 << 30);
    live = engaged || (fade > 0.0f && quiet < quietNeeded);
}

void Reverb::process (int channel, float* x, int n)
{
    auto& c = channels[(size_t) std::clamp (channel, 0, 1)];
    const float inv = 1.0f / (float) std::max (blockSamples, 1);
    float peak = 0.0f;
    for (int from = 0; from < n; from += chunk)
    {
        const int m = std::min (chunk, n - from);
        for (int i = 0; i < m; ++i)
        {
            const auto k = (float) std::min (from + i + 1, blockSamples);
            const float g = std::clamp (gateFrom + gateStep * k, 0.0f, 1.0f);
            c.predelay.push (g * x[from + i]);
            feed[(size_t) i] = c.predelay.tapLinear (delayFrom + (delayTo - delayFrom) * k * inv);
        }
        run (c, feed.data(), wetL.data(), wetR.data(), m);
        for (int i = 0; i < m; ++i)
        {
            const auto k = (float) std::min (from + i + 1, blockSamples);
            const float f = std::clamp (fadeFrom + fadeStep * k, 0.0f, 1.0f);
            const float w = (wetFrom + (wetTo - wetFrom) * k * inv) * f * 0.70710678f
                            * (wetL[(size_t) i] + wetR[(size_t) i]);
            x[from + i] += w;
            peak = std::max (peak, std::abs (w));
        }
    }
    finish (peak, n, channel == 0);
}

void Reverb::processStereo (float* left, float* right, int n)
{
    auto& c = channels[0];
    const float inv = 1.0f / (float) std::max (blockSamples, 1);
    float peak = 0.0f;
    for (int from = 0; from < n; from += chunk)
    {
        const int m = std::min (chunk, n - from);
        for (int i = 0; i < m; ++i)
        {
            const auto k = (float) std::min (from + i + 1, blockSamples);
            const float g = std::clamp (gateFrom + gateStep * k, 0.0f, 1.0f);
            c.predelay.push (0.5f * g * (left[from + i] + right[from + i]));
            feed[(size_t) i] = c.predelay.tapLinear (delayFrom + (delayTo - delayFrom) * k * inv);
        }
        run (c, feed.data(), wetL.data(), wetR.data(), m);
        for (int i = 0; i < m; ++i)
        {
            const auto k = (float) std::min (from + i + 1, blockSamples);
            const float f = std::clamp (fadeFrom + fadeStep * k, 0.0f, 1.0f);
            const float g = (wetFrom + (wetTo - wetFrom) * k * inv) * f;
            const float wl = g * wetL[(size_t) i], wr = g * wetR[(size_t) i];
            left[from + i] += wl;
            right[from + i] += wr;
            peak = std::max ({ peak, std::abs (wl), std::abs (wr) });
        }
    }
    finish (peak, n, true);
}

} // namespace damp::pedals
