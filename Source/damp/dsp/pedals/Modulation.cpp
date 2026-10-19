#include "Modulation.h"

#include "common/dsp/FastMath.h"

#include <bit>
#include <cmath>

namespace damp::pedals
{
namespace
{
constexpr double knobGlide = 0.03; // seconds

// Levels: every pedal at its default knobs comes out within a few tenths of a dB of the
// bypassed loudness (measured on a plucked-string DI peaking at -12 dBFS). The mixes are scaled
// for constant power across frequency, which lands there by itself except for the phaser: its
// resonant peaks sit in the low mids, where a guitar has most of its energy.
constexpr float phaserTrim = 0.91f;

inline float sine (double phase) { return (float) dustify::fastSin2Pi (phase); }

// Triangle from -1 to 1 (0 rising at phase 0), its corners rounded off a little: the slope at
// the turns is half that in the middle, like a BBD pedal's integrator-and-comparator LFO
inline float roundedTriangle (double phase)
{
    const double p = phase + 0.25;
    const auto t = (float) (1.0 - 4.0 * std::abs (p - std::floor (p) - 0.5));
    return t * (1.25f - 0.25f * t * t);
}

// Linear up to 1.5, then easing into 2: keeps a feedback loop bounded without colouring
// ordinary levels
inline float softLimit (float v)
{
    const float a = std::abs (v);
    if (a <= 1.5f)
        return v;
    const float over = a - 1.5f;
    return std::copysign (1.5f + over / (1.0f + 2.0f * over), v);
}

// TPT one-pole coefficient for a corner at hz
inline float onePoleCoefficient (double hz, double rate)
{
    const double w = std::tan (std::numbers::pi * std::min (hz, 0.45 * rate) / rate);
    return (float) (w / (1.0 + w));
}

// First-order allpass with a TPT one-pole's corner: 2 lowpass - input, rearranged so the input
// passes through one multiply-add (stages in series are latency-bound). a = 2g - 1, from
// allpassCoefficient().
inline float allpass (float& s, float x, float a)
{
    const float y = a * x + (1.0f - a) * s;
    s = (1.0f + a) * x - a * s;
    return y;
}

inline float allpassCoefficient (double hz, double rate) { return 2.0f * onePoleCoefficient (hz, rate) - 1.0f; }

// A lamp or photocell following `target`, quicker one way than the other
inline void lampStep (float& state, float target, double seconds, double rise, double fall)
{
    const double tau = target > state ? rise : fall;
    state += (target - state) * (float) (1.0 - std::exp (-seconds / tau));
}

inline int delayBufferSamples (double seconds, double maxRate) { return (int) std::ceil (seconds * maxRate) + 8; }
} // namespace

// ---- Shared ----

void DelayLine::allocate (int maxDelaySamples)
{
    const auto size = std::bit_ceil ((unsigned) std::max (maxDelaySamples + 4, 8));
    buffer.assign (size, 0.0f);
    mask = (int) size - 1;
    write = 0;
}

void DelayLine::clear()
{
    std::fill (buffer.begin(), buffer.end(), 0.0f);
    write = 0;
}

void ModulationPedal::beginBlock (double sampleRate, int samples, double controlRate)
{
    if (sampleRate != rate)
        fresh = true; // delays are in samples: start the lanes over at a new rate
    rate = sampleRate;
    cubic = rate < 150000.0;
    blockSamples = std::max (samples, 0);
    const int base = std::max (1, (int) std::lround (rate / controlRate));
    step = std::max (base, (blockSamples + maxSegments - 1) / maxSegments);
    previousSegments = segments;
    segments = (blockSamples + step - 1) / step;
}

double ModulationPedal::segmentSeconds (int j) const
{
    if (j <= 0)
        return 0.0;
    return (double) (std::min (j * step, blockSamples) - (j - 1) * step) / rate;
}

float ModulationPedal::glide (int j, double seconds) const
{
    if (j == 0)
        return 1.0f; // only reached on a fresh start: the others carry
    return (float) (1.0 - std::exp (-segmentSeconds (j) / seconds));
}

double ModulationPedal::advance (int j, double hz)
{
    phase += segmentSeconds (j) * hz;
    phase -= std::floor (phase);
    return phase;
}

// ---- Chorus: a BBD's 4-12 ms delay under a triangle LFO; the wet path loses its lows below
// ~120 Hz and its highs above ~5 kHz (the BBD's anti-alias and reconstruction filters) ----

void Chorus::prepare (double maxRate)
{
    for (auto& s : state)
        s.line.allocate (delayBufferSamples (0.013, maxRate));
    reset();
}

void Chorus::reset()
{
    for (auto& s : state)
    {
        s.line.clear();
        s.thin.reset();
        s.dark1.reset();
        s.dark2.reset();
    }
    phase = 0.0;
    fresh = true;
}

void Chorus::setPatch (const Patch& p, double sampleRate, int samples)
{
    beginBlock (sampleRate, samples);
    const auto maxDelay = (float) state[0].line.maxDelay();
    const auto toSamples = [this, maxDelay] (double seconds) { return std::clamp ((float) (seconds * rate), 2.0f, maxDelay); };
    for (int j = 0; j <= segments; ++j)
    {
        if (j == 0 && ! fresh)
        {
            for (auto* lane : { &delayLeft, &delayRight, &dry, &wet })
                carry (*lane);
            continue;
        }
        const float k = glide (j, knobGlide);
        const double hz = rateHz.follow (p[PID::ChorusRate], k);
        const float sweep = 4.0e-3f * depth.follow (p[PID::ChorusDepth] / 10.0f, k);
        const float m = mix.follow (p[PID::ChorusMix] / 10.0f, k);
        const float spread = 0.5f * width.follow (p[PID::ChorusWidth] / 10.0f, k);
        const double ph = advance (j, hz);
        delayLeft[(size_t) j] = toSamples (8.0e-3 + sweep * roundedTriangle (ph));
        delayRight[(size_t) j] = toSamples (8.0e-3 + sweep * roundedTriangle (ph + spread));
        // Constant power across the mix: at noon dry and wet are equal, each at -3 dB
        const float norm = 1.0f / std::sqrt ((1.0f - m) * (1.0f - m) + m * m);
        dry[(size_t) j] = (1.0f - m) * norm;
        wet[(size_t) j] = 1.25f * m * norm; // making up what the BBD filters take
    }
    for (auto& s : state)
    {
        s.thin.setCutoff (120.0, rate);
        s.dark1.setCutoff (5000.0, rate);
        s.dark2.setCutoff (5000.0, rate);
    }
    fresh = false;
}

void Chorus::run (State& s, const Lane& delay, float* x, int n)
{
    forSegments (n, [&] (int j, int from, int to, float inv)
    {
        const auto at = (size_t) j;
        float d = delay[at], g0 = dry[at], g1 = wet[at];
        const float dd = (delay[at + 1] - d) * inv, dg0 = (dry[at + 1] - g0) * inv, dg1 = (wet[at + 1] - g1) * inv;
        for (int i = from; i < to; ++i)
        {
            const float in = x[i];
            const float delayed = s.dark2.lowpass (s.line.read (d, cubic));
            s.line.push (s.dark1.lowpass (s.thin.highpass (in)));
            x[i] = g0 * in + g1 * delayed;
            d += dd;
            g0 += dg0;
            g1 += dg1;
        }
    });
}

void Chorus::process (int channel, float* x, int n)
{
    run (state[(size_t) channel], delayLeft, x, n);
}

void Chorus::processStereo (float* left, float* right, int n)
{
    run (state[0], delayLeft, left, n);
    run (state[1], delayRight, right, n);
}

// ---- Flanger: the delay swept on a log curve (so the notches move evenly up and down in
// pitch), regeneration fed back through a gentle low cut. The output mixes what goes into the
// delay with what comes out, which keeps the notches full depth at any regeneration while the
// peaks between them sharpen. ----

void Flanger::prepare (double maxRate)
{
    for (auto& s : state)
        s.line.allocate (delayBufferSamples (0.015, maxRate));
    reset();
}

void Flanger::reset()
{
    for (auto& s : state)
    {
        s.line.clear();
        s.dark.reset();
        s.block.reset();
    }
    phase = 0.0;
    fresh = true;
}

void Flanger::setPatch (const Patch& p, double sampleRate, int samples)
{
    beginBlock (sampleRate, samples);
    const auto maxDelay = (float) state[0].line.maxDelay();
    const auto toSamples = [this, maxDelay] (double seconds) { return std::clamp ((float) (seconds * rate), 2.0f, maxDelay); };
    for (int j = 0; j <= segments; ++j)
    {
        if (j == 0 && ! fresh)
        {
            for (auto* lane : { &delayLeft, &delayRight, &feedback, &gain })
                carry (*lane);
            continue;
        }
        const float k = glide (j, knobGlide);
        const double hz = rateHz.follow (p[PID::FlangerRate], k);
        const float octaves = 2.0f * depth.follow (p[PID::FlangerDepth] / 10.0f, k); // each way
        const double middle = 0.3e-3 * std::pow (8.0 / 0.3, centre.follow (p[PID::FlangerManual], k) / 10.0); // 0.3 .. 8 ms
        const float fb = 0.9f * regen.follow (p[PID::FlangerRegen] / 10.0f, k);
        const double ph = advance (j, hz);
        delayLeft[(size_t) j] = toSamples (middle * dustify::fastExp2 (octaves * roundedTriangle (ph)));
        delayRight[(size_t) j] = toSamples (middle * dustify::fastExp2 (octaves * roundedTriangle (ph + 0.25)));
        feedback[(size_t) j] = fb;
        // (1 + z^-D) / (1 - fb z^-D) has a mean power of 2 / (1 - fb) across frequency
        gain[(size_t) j] = std::sqrt ((1.0f - fb) / 2.0f);
    }
    for (auto& s : state)
    {
        s.dark.setCutoff (12000.0, rate);
        s.block.setCutoff (30.0, rate);
    }
    fresh = false;
}

void Flanger::run (State& s, const Lane& delay, float* x, int n)
{
    forSegments (n, [&] (int j, int from, int to, float inv)
    {
        const auto at = (size_t) j;
        float d = delay[at], fb = feedback[at], g = gain[at];
        const float dd = (delay[at + 1] - d) * inv, dfb = (feedback[at + 1] - fb) * inv, dg = (gain[at + 1] - g) * inv;
        for (int i = from; i < to; ++i)
        {
            const float tap = s.dark.lowpass (s.line.read (d, cubic));
            const float v = softLimit (x[i] + fb * s.block.highpass (tap));
            s.line.push (v);
            x[i] = g * (v + tap);
            d += dd;
            fb += dfb;
            g += dg;
        }
    });
}

void Flanger::process (int channel, float* x, int n)
{
    run (state[(size_t) channel], delayLeft, x, n);
}

void Flanger::processStereo (float* left, float* right, int n)
{
    run (state[0], delayLeft, left, n);
    run (state[1], delayRight, right, n);
}

// ---- Phaser: the stages' corner swept from 100 Hz to 4 kHz on a log curve by a sine; half as
// many notches as stages. As with the flanger the dry side of the mix is taken after the
// feedback joins, so the notches stay deep and the feedback raises peaks between them. ----

void Phaser::prepare (double) { reset(); }

void Phaser::reset()
{
    for (auto& s : state)
        s = {};
    phase = 0.0;
    fresh = true;
}

void Phaser::setPatch (const Patch& p, double sampleRate, int samples)
{
    beginBlock (sampleRate, samples);
    static constexpr int counts[] { 4, 6, 8, 12 };
    const int stages = counts[std::clamp ((int) p.choice<PhaserStages> (PID::PhaserStages), 0, 3)];
    bool switched = false;
    if (fresh)
    {
        stagesFrom = stagesTo = stages;
        blend.value = 1.0f;
    }
    else if (stages != stagesTo)
    {
        // Stages that weren't running start from rest
        for (auto& s : state)
            std::fill (s.s.begin() + std::max (stagesFrom, stagesTo), s.s.end(), 0.0f);
        stagesFrom = blend.value > 0.5f ? stagesTo : stagesFrom;
        stagesTo = stages;
        blend.value = 0.0f;
        switched = true;
    }

    for (int j = 0; j <= segments; ++j)
    {
        if (j == 0 && ! fresh)
        {
            for (auto* lane : { &coefficientLeft, &coefficientRight, &feedback, &gain, &mixTo })
                carry (*lane);
            if (switched)
                mixTo[0] = 0.0f;
            continue;
        }
        const float k = glide (j, knobGlide);
        const double hz = rateHz.follow (p[PID::PhaserRate], k);
        const float swing = depth.follow (p[PID::PhaserDepth] / 10.0f, k);
        const float fb = 0.8f * resonance.follow (p[PID::PhaserFeedback] / 10.0f, k);
        mixTo[(size_t) j] = blend.follow (1.0f, glide (j, 0.02));
        const double ph = advance (j, hz);
        const auto corner = [&] (double at) { return allpassCoefficient (100.0 * std::pow (40.0, 0.5 + 0.5 * swing * sine (at)), rate); };
        coefficientLeft[(size_t) j] = corner (ph);
        coefficientRight[(size_t) j] = corner (ph + 0.25);
        feedback[(size_t) j] = fb;
        // 1 / sqrt (2) for the 50/50 mix; feedback's peaks taken down a little more than their
        // broadband power, sqrt (1 - fb), as a guitar's energy sits under them
        gain[(size_t) j] = phaserTrim * std::sqrt (0.5f) * std::pow (1.0f - fb, 0.65f);
    }
    if (mixTo[0] > 0.9999f)
        stagesFrom = stagesTo; // crossfade done
    fresh = false;
}

void Phaser::run (State& s, const Lane& coefficient, float* x, int n)
{
    if (stagesFrom == stagesTo)
    {
        switch (stagesTo)
        {
            case 4: runStages<4> (s, coefficient, x, n); return;
            case 6: runStages<6> (s, coefficient, x, n); return;
            case 8: runStages<8> (s, coefficient, x, n); return;
            default: runStages<12> (s, coefficient, x, n); return;
        }
    }

    // Changing the number of stages: run enough for both and crossfade their outputs
    const int top = std::max (stagesFrom, stagesTo);
    const auto from = (size_t) stagesFrom, to = (size_t) stagesTo;
    forSegments (n, [&] (int j, int start, int end, float inv)
    {
        const auto at = (size_t) j;
        float c = coefficient[at], fb = feedback[at], g = gain[at], b = mixTo[at];
        const float dc = (coefficient[at + 1] - c) * inv, dfb = (feedback[at + 1] - fb) * inv, dg = (gain[at + 1] - g) * inv,
                    db = (mixTo[at + 1] - b) * inv;
        std::array<float, maxStages + 1> taps {};
        for (int i = start; i < end; ++i)
        {
            const float v = softLimit (x[i] + fb * s.last);
            float t = v;
            for (int k = 0; k < top; ++k)
                taps[(size_t) k + 1] = t = allpass (s.s[(size_t) k], t, c);
            const float y = taps[from] + b * (taps[to] - taps[from]);
            s.last = y;
            x[i] = g * (v + y);
            c += dc;
            fb += dfb;
            g += dg;
            b += db;
        }
    });
}

template <int stages> void Phaser::runStages (State& s, const Lane& coefficient, float* x, int n)
{
    forSegments (n, [&] (int j, int start, int end, float inv)
    {
        const auto at = (size_t) j;
        float c = coefficient[at], fb = feedback[at], g = gain[at];
        const float dc = (coefficient[at + 1] - c) * inv, dfb = (feedback[at + 1] - fb) * inv, dg = (gain[at + 1] - g) * inv;
        for (int i = start; i < end; ++i)
        {
            const float v = softLimit (x[i] + fb * s.last);
            float t = v;
            for (size_t k = 0; k < (size_t) stages; ++k)
                t = allpass (s.s[k], t, c);
            s.last = t;
            x[i] = g * (v + t);
            c += dc;
            fb += dfb;
            g += dg;
        }
    });
}

void Phaser::process (int channel, float* x, int n)
{
    run (state[(size_t) channel], coefficientLeft, x, n);
}

void Phaser::processStereo (float* left, float* right, int n)
{
    run (state[0], coefficientLeft, left, n);
    run (state[1], coefficientRight, right, n);
}

// ---- Tremolo: gain = 1 - depth + depth x shape, made up to the bypassed RMS; the low and high
// bands always get gains of their own, equal except for Harmonic ----

namespace
{
// Opto: a neon lamp that strikes above a threshold, its photocell darkening quickly and recovering
// more slowly; the cell shunts the signal, so the volume falls off steeply as the light comes up
inline float optoDrive (double phase) { return std::max (0.0f, (sine (phase) - 0.2f) / 0.8f); }
// Over `seconds` up to `phase`, driven by the LFO at the middle of the step (so the result hardly
// depends on how the time is cut up)
inline void optoStep (float& cell, double phase, double seconds, double hz)
{
    lampStep (cell, optoDrive (phase - 0.5 * seconds * hz), seconds, 0.003, 0.018);
}
inline float optoShape (float cell) { return (1.0f / (1.0f + 4.0f * cell) - 0.2f) / 0.8f; }
} // namespace

void Tremolo::prepare (double) { reset(); }

void Tremolo::reset()
{
    for (auto& s : state)
        s = {};
    photocell = {};
    phase = 0.0;
    fresh = true;
}

// 0 (quietest) .. 1 for the stateless shapes
float Tremolo::shapeAt (TremoloShape shape, double at, float hz) const
{
    switch (shape)
    {
        case TremoloShape::Triangle: return 0.5f + 0.5f * (float) (1.0 - 4.0 * std::abs (at + 0.25 - std::floor (at + 0.25) - 0.5));
        case TremoloShape::Square:
        {
            // A steep sine, clipped, with S-curve corners: each edge takes 3 ms
            const float steep = std::max (1.0f, 1.0f / (2.0f * std::numbers::pi_v<float> * hz * 0.0015f));
            const float v = std::clamp (steep * sine (at), -1.0f, 1.0f);
            return 0.5f + 0.5f * v * (1.5f - 0.5f * v * v);
        }
        case TremoloShape::Sine:
        case TremoloShape::Opto:
        case TremoloShape::Harmonic: break;
    }
    return 0.5f + 0.5f * sine (at);
}

// The shape's mean and mean square over a cycle at hz, for the make-up gain
void Tremolo::measureShape (TremoloShape shape, float hz)
{
    constexpr int points = 96;
    const int cycles = shape == TremoloShape::Opto ? 4 : 1; // the photocell settles first
    double sum = 0.0, squares = 0.0;
    float cell = 0.0f;
    for (int i = 0; i < points * cycles; ++i)
    {
        const double at = (double) i / points;
        float u;
        if (shape == TremoloShape::Opto)
        {
            optoStep (cell, at, 1.0 / (hz * points), hz);
            u = optoShape (cell);
        }
        else
            u = shapeAt (shape, at, hz);
        if (i >= points * (cycles - 1))
        {
            sum += u;
            squares += (double) u * u;
        }
    }
    shapeMean = (float) (sum / points);
    shapeSquare = (float) (squares / points);
    measuredShape = shape;
    measuredHz = hz;
}

void Tremolo::setPatch (const Patch& p, double sampleRate, int samples)
{
    beginBlock (sampleRate, samples, 8000.0);
    const auto shape = p.choice<TremoloShape> (PID::TremoloShape);
    const bool pan = p.on (PID::TremoloPan);
    const float targetHz = p[PID::TremoloRate];
    if (shape != measuredShape || std::abs (targetHz - measuredHz) > 0.005f * targetHz)
        measureShape (shape, targetHz);

    for (int j = 0; j <= segments; ++j)
    {
        if (j == 0 && ! fresh)
        {
            for (auto* lane : { &lowLeft, &highLeft, &lowRight, &highRight })
                carry (*lane);
            continue;
        }
        const float k = glide (j, knobGlide);
        const auto hz = (float) rateHz.follow (targetHz, k);
        const float d = depth.follow (p[PID::TremoloDepth] / 10.0f, k);
        const double ph = advance (j, hz);
        const double seconds = segmentSeconds (j);
        const float makeup = 1.0f / std::sqrt ((1.0f - d) * (1.0f - d) + 2.0f * (1.0f - d) * d * shapeMean + d * d * shapeSquare);
        const auto gainOf = [&] (float u) { return makeup * (1.0f - d + d * u); };

        std::array<float, 4> target {};
        for (int side = 0; side < 2; ++side)
        {
            const double at = side == 1 && pan ? ph + 0.5 : ph;
            float low, high;
            if (shape == TremoloShape::Harmonic)
            {
                low = shapeAt (TremoloShape::Sine, at, hz);
                high = shapeAt (TremoloShape::Sine, at + 0.5, hz);
            }
            else if (shape == TremoloShape::Opto)
            {
                optoStep (photocell[(size_t) side], at, seconds, hz);
                low = high = optoShape (photocell[(size_t) side]);
            }
            else
                low = high = shapeAt (shape, at, hz);
            target[(size_t) side * 2] = gainOf (low);
            target[(size_t) side * 2 + 1] = gainOf (high);
        }
        if (shape != TremoloShape::Opto) // keep the photocell following, for a switch to Opto
            for (int side = 0; side < 2; ++side)
                optoStep (photocell[(size_t) side], side == 1 && pan ? ph + 0.5 : ph, seconds, hz);

        // A last 1.5 ms smoothing: no clicks from a shape change or the square's corners. Exact
        // for a target moving in a straight line between the points, so where the points fall
        // hardly matters.
        constexpr double tau = 0.0015;
        const auto decay = (float) std::exp (-seconds / tau);
        for (size_t g = 0; g < 4; ++g)
        {
            if (j == 0)
                smoothed[g] = target[g];
            else
            {
                const auto drift = (float) ((target[g] - lastTarget[g]) * tau / seconds);
                smoothed[g] = target[g] - drift + (smoothed[g] - lastTarget[g] + drift) * decay;
            }
        }
        lastTarget = target;
        lowLeft[(size_t) j] = smoothed[0];
        highLeft[(size_t) j] = smoothed[1];
        lowRight[(size_t) j] = smoothed[2];
        highRight[(size_t) j] = smoothed[3];
    }
    for (auto& s : state)
    {
        s.split1.setCutoff (1200.0, rate);
        s.split2.setCutoff (1200.0, rate);
    }
    fresh = false;
}

void Tremolo::run (State& s, const Lane& low, const Lane& high, float* x, int n)
{
    forSegments (n, [&] (int j, int from, int to, float inv)
    {
        const auto at = (size_t) j;
        float gl = low[at], gh = high[at];
        const float dgl = (low[at + 1] - gl) * inv, dgh = (high[at + 1] - gh) * inv;
        for (int i = from; i < to; ++i)
        {
            const float lows = s.split2.lowpass (s.split1.lowpass (x[i]));
            x[i] = gl * lows + gh * (x[i] - lows);
            gl += dgl;
            gh += dgh;
        }
    });
}

void Tremolo::process (int channel, float* x, int n)
{
    run (state[(size_t) channel], lowLeft, highLeft, x, n);
}

void Tremolo::processStereo (float* left, float* right, int n)
{
    run (state[0], lowLeft, highLeft, left, n);
    run (state[1], lowRight, highRight, right, n);
}

// ---- Vibrato: a 3.5 ms delay swept by a sine; Depth sets the pitch swing (+-50 cents at 10 and
// 5 Hz), growing more slowly than the rate so slow settings still move. Vibe: the Uni-Vibe's
// four stages (15 nF, 220 nF, 470 pF, 4.7 nF against one photocell each), swept up to 2 octaves
// either way. The two modes crossfade when switched. ----

namespace
{
constexpr std::array<double, 4> vibeCorners { 330.0, 22.5, 10500.0, 1050.0 }; // at the middle of the sweep
}

void Vibrato::prepare (double maxRate)
{
    for (auto& s : state)
        s.line.allocate (delayBufferSamples (0.0068, maxRate));
    reset();
}

void Vibrato::reset()
{
    for (auto& s : state)
    {
        s.line.clear();
        s.s = {};
    }
    lamp = 0.5f;
    phase = 0.0;
    fresh = true;
}

void Vibrato::setPatch (const Patch& p, double sampleRate, int samples)
{
    beginBlock (sampleRate, samples);
    const auto maxDelay = (float) state[0].line.maxDelay();
    const float vibeTarget = p.choice<VibratoMode> (PID::VibratoMode) == VibratoMode::Vibe ? 1.0f : 0.0f;
    for (int j = 0; j <= segments; ++j)
    {
        if (j == 0 && ! fresh)
        {
            for (auto* lane : { &delaySamples, &mode, &level, &coefficient[0], &coefficient[1], &coefficient[2], &coefficient[3] })
                carry (*lane);
            continue;
        }
        const float k = glide (j, knobGlide);
        const double hz = rateHz.follow (p[PID::VibratoRate], k);
        const float d = depth.follow (p[PID::VibratoDepth] / 10.0f, k);
        mode[(size_t) j] = vibe.follow (vibeTarget, glide (j, 0.015));
        const double ph = advance (j, hz);

        // Vibrato: a sine sweep of amplitude A gives a pitch swing of 2 pi f A
        const double swing = std::min (3.0e-3, 0.93e-3 * d * std::sqrt (5.0 / hz));
        delaySamples[(size_t) j] = std::clamp ((float) ((3.5e-3 + swing * sine (ph)) * rate), 2.0f, maxDelay);

        // Vibe: the lamp brightens faster than it dims (driven from the middle of the step, as Opto)
        const double seconds = segmentSeconds (j);
        lampStep (lamp, 0.5f + 0.5f * sine (ph - 0.5 * seconds * hz), seconds, 0.006, 0.025);
        const float octaves = 2.0f * d * (2.0f * lamp - 1.0f);
        const double shift = dustify::fastExp2 (octaves);
        for (size_t c = 0; c < 4; ++c)
            coefficient[c][(size_t) j] = allpassCoefficient (vibeCorners[c] * shift, rate);
        level[(size_t) j] = (1.0f - 0.1f * d * lamp) / (1.0f - 0.05f * d); // a little of its throb is level
    }

    // Only the mode(s) heard this block run (the delay line is always written)
    constexpr float settled = 1.0e-5f;
    const bool vibeWas = vibeRuns;
    vibeRuns = mode[0] > settled || mode[(size_t) segments] > settled;
    delayRuns = mode[0] < 1.0f - settled || mode[(size_t) segments] < 1.0f - settled;
    if (vibeRuns && ! vibeWas)
        for (auto& s : state)
            s.s = {};
    fresh = false;
}

void Vibrato::process (int channel, float* x, int n)
{
    auto& s = state[(size_t) channel];
    forSegments (n, [&] (int j, int from, int to, float inv)
    {
        const auto at = (size_t) j;
        float d = delaySamples[at], m = mode[at], g = level[at];
        const float dd = (delaySamples[at + 1] - d) * inv, dm = (mode[at + 1] - m) * inv, dg = (level[at + 1] - g) * inv;
        std::array<float, 4> c {}, dc {};
        for (size_t k = 0; k < 4; ++k)
        {
            c[k] = coefficient[k][at];
            dc[k] = (coefficient[k][at + 1] - c[k]) * inv;
        }
        for (int i = from; i < to; ++i)
        {
            const float in = x[i];
            const float delayed = delayRuns ? s.line.read (d, cubic) : 0.0f;
            s.line.push (in);
            float t = 0.0f;
            if (vibeRuns)
            {
                t = in;
                for (size_t k = 0; k < 4; ++k)
                {
                    t = allpass (s.s[k], t, c[k]);
                    c[k] += dc[k];
                }
            }
            x[i] = delayed + m * (g * t - delayed);
            d += dd;
            m += dm;
            g += dg;
        }
    });
}

} // namespace damp::pedals
