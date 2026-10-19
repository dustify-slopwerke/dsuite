#include "Delay.h"

#include "common/dsp/FastMath.h"

#include <bit>
#include <climits>
#include <cmath>

namespace damp::pedals
{
namespace
{
constexpr double knobGlide = 0.03;      // seconds
constexpr double switchSeconds = 0.01;  // switching on or off: the input feed and the repeats ramp over this
constexpr double crossfadeSeconds = 0.04; // digital: a time change crossfades between read positions
constexpr double offsetGlide = 0.005;   // smooths the modulation's offset across a change of mode
constexpr float audible = 3.2e-5f;      // -90 dBFS: anything quieter in the line counts as silence
constexpr int forever = INT_MAX / 2;
constexpr double maxModulation = 0.004; // seconds either side of the time, any mode (and its glide)

// Feedback at 10, small-signal. Digital stays clear of runaway; analog and tape sit at the edge
// of self-oscillation (their loops' peak gain just over 1), held back by the loop's saturation.
// Tape's head bump lifts its loop by about 1.1 at its peak, which its figure allows for.
constexpr float digitalTop = 0.95f, analogTop = 1.03f, tapeTop = 0.93f;

// Repeats at Mix 10 come out at about the level of the dry signal; these make up what each mode's
// band limit takes from a guitar's level
constexpr float digitalWet = 1.1f, analogWet = 1.85f, tapeWet = 1.3f;

inline float sine (double phase) { return (float) dustify::fastSin2Pi (phase); }

inline double wrap (double phase) { return phase - std::floor (phase); }

// Linear up to 1.5, then easing into 2: a digital loop's safety net
inline float softLimit (float v)
{
    const float a = std::abs (v);
    if (a <= 1.5f)
        return v;
    const float over = a - 1.5f;
    return std::copysign (1.5f + over / (1.0f + 2.0f * over), v);
}

// Unity gain for small signals, saturating at 1 / drive
inline float saturate (float v, float drive) { return dustify::fastTanh (drive * v) / drive; }

// A BBD's clock runs slower for a longer delay, and its filters are set for the clock: about
// 4 kHz at the shortest times, 2.5 kHz at the longest
inline double bbdCorner (double seconds)
{
    const double t = std::clamp (std::log (seconds / 0.02) / std::log (100.0), 0.0, 1.0);
    return 4000.0 * std::pow (2500.0 / 4000.0, t);
}

// Coefficients from `from`, keeping the state of `to`
inline void copyCoefficients (Biquad& to, const Biquad& from)
{
    to.b0 = from.b0;
    to.b1 = from.b1;
    to.b2 = from.b2;
    to.a1 = from.a1;
    to.a2 = from.a2;
}

inline float toward (float v, float to, float step) { return v < to ? std::min (to, v + step) : std::max (to, v - step); }
} // namespace

void EchoLine::allocate (int maxDelaySamples)
{
    const auto size = std::bit_ceil ((unsigned) std::max (maxDelaySamples + 4, 8));
    buffer.assign (size, 0.0f);
    mask = (int) size - 1;
    write = 0;
    valid = (int) size;
}

void EchoLine::ensureClear (int samples)
{
    samples = std::min (samples, (int) buffer.size());
    for (int k = valid + 1; k <= samples; ++k) // k samples before the write position
        buffer[(size_t) ((write - k) & mask)] = 0.0f;
    valid = std::max (valid, samples);
}

double Delay::targetSeconds (const Patch& p, double bpm)
{
    // Note values in quarter notes, in DelaySync's order
    static constexpr double quarters[] { 0.0, 2.0, 1.5, 1.0, 2.0 / 3.0, 0.75, 0.5, 1.0 / 3.0, 0.25 };
    const int sync = std::clamp ((int) p.choice<DelaySync> (PID::DelaySync), 0, (int) std::size (quarters) - 1);
    const double s = sync == 0 ? p[PID::DelayTime] / 1000.0 : quarters[sync] * 60.0 / std::clamp (bpm, 20.0, 999.0);
    return std::clamp (s, 0.02, maxSeconds);
}

void Delay::prepare (double maxRate)
{
    // The longest time, the modulation either side of it and the cubic read's spare samples
    for (auto& s : state)
        s.line.allocate ((int) std::ceil ((maxSeconds + 2.0 * maxModulation) * maxRate) + 8);
    reset();
}

void Delay::reset()
{
    for (auto& s : state)
    {
        s.line.forget();
        s.pre.reset();
        s.tone.reset();
        s.low.reset();
        s.band.reset();
        s.bump.reset();
        s.quiet = forever;
    }
    feedLevel = wetLevel = wetBefore = 0.0f;
    seconds = {};
    weight = weightTarget = 0.0f;
    phase = flutterPhase = driftPhase = 0.0;
    driftFrom = driftTo = 0.0f;
    offset.value = 0.0f;
    rng = dustify::Random (0xdea1u);
    fresh = true;
}

void Delay::glideTime (double target, double s)
{
    const auto moveWeight = [this, s] { weight = toward (weight, weightTarget, (float) (s / crossfadeSeconds)); };
    if (mode == DelayMode::Digital)
    {
        // A crossfade under way finishes first. Once it has, the silent read position jumps to the
        // new time (its weight was 0 at this point and the last, so the jump isn't heard) and the
        // next crossfade starts from the next point.
        if (weight != weightTarget)
        {
            moveWeight();
            return;
        }
        const int active = weightTarget > 0.5f ? 1 : 0;
        if (std::abs (seconds[(size_t) active] - target) * rate > 0.25)
        {
            seconds[(size_t) (1 - active)] = target;
            weightTarget = 1.0f - weightTarget;
        }
        return;
    }

    // Analog and tape: the time glides, bending the pitch of what's in the line. The read point
    // moves at most half as fast as the signal does, keeping the bend within an octave.
    const double tau = mode == DelayMode::Analog ? 0.15 : 0.3;
    const double k = 1.0 - std::exp (-s / tau), most = 0.5 * s;
    for (auto& t : seconds)
        t += std::clamp ((target - t) * k, -most, most);
    moveWeight();
}

double Delay::modulation (double s, float amount)
{
    double raw = 0.0;
    switch (mode)
    {
        case DelayMode::Digital: // a barely-there drift that keeps long repeats from sounding static
            phase = wrap (phase + s * 0.8);
            raw = 0.4e-3 * amount * sine (phase);
            break;
        case DelayMode::Analog: // a chorus-like wobble, quicker as it deepens
            phase = wrap (phase + s * (0.35 + 0.9 * amount));
            raw = 2.2e-3 * amount * sine (phase);
            break;
        case DelayMode::Tape:
        {
            // Wow: a slow capstan wobble plus a smooth random drift. Flutter: two quick components.
            phase = wrap (phase + s * 0.6);
            flutterPhase = wrap (flutterPhase + s * 6.3);
            driftPhase += s * 0.9;
            if (driftPhase >= 1.0)
            {
                driftPhase -= std::floor (driftPhase);
                driftFrom = driftTo;
                driftTo = rng.bipolar();
            }
            const float drift = driftFrom + (driftTo - driftFrom) * (0.5f - 0.5f * sine (0.5 * driftPhase + 0.25));
            raw = amount * (1.0e-3 * sine (phase) + 0.8e-3 * drift + 0.05e-3 * sine (flutterPhase)
                            + 0.025e-3 * sine (wrap (1.7 * flutterPhase + 0.3)));
            break;
        }
    }
    return raw;
}

void Delay::designFilters (const Patch& p)
{
    if (mode != filtersFor)
    {
        // These hold nothing of use from the last mode
        for (auto& s : state)
        {
            s.pre.reset();
            s.band.reset();
            s.bump.reset();
        }
        filtersFor = mode;
    }
    const double t = std::clamp (p[PID::DelayTone] / 10.0, 0.0, 1.0);
    Biquad band, bump;
    double lowCut = 30.0, preCorner = 20000.0, hiCut = 20000.0;
    switch (mode)
    {
        case DelayMode::Digital:
            hiCut = 1500.0 * std::pow (16000.0 / 1500.0, t); // ~4.9 kHz at noon
            break;
        case DelayMode::Analog:
        {
            const double time = seconds[0] + (double) weight * (seconds[1] - seconds[0]);
            const double corner = bbdCorner (time) * std::exp2 (t - 0.5); // Tone: half an octave either way
            band.setLowpass (corner, 0.7071, rate);
            preCorner = 1.3 * corner;
            lowCut = 90.0;
            break;
        }
        case DelayMode::Tape:
            band.setLowpass (4500.0 * std::exp2 (1.6 * (t - 0.5)), 0.6, rate); // 2.6 .. 7.8 kHz
            bump.setPeak (110.0, 0.9, 2.5, rate);
            lowCut = 45.0;
            break;
    }
    for (auto& s : state)
    {
        copyCoefficients (s.band, band);
        copyCoefficients (s.bump, bump);
        s.pre.setCutoff (preCorner, rate);
        s.tone.setCutoff (hiCut, rate);
        s.low.setCutoff (lowCut, rate);
    }
}

void Delay::setPatch (const Patch& p, double sampleRate, int samples)
{
    beginBlock (sampleRate, samples);
    mode = (DelayMode) std::clamp ((int) p.choice<DelayMode> (PID::DelayMode), 0, 2);
    const double target = targetSeconds (p, tempo);
    const bool trails = p.on (PID::DelayTrails);
    const float k10 = std::clamp (p[PID::DelayFeedback] / 10.0f, 0.0f, 1.0f);
    const float top = mode == DelayMode::Digital ? digitalTop : mode == DelayMode::Analog ? analogTop : tapeTop;
    const float fbTarget = top * k10 * (1.15f - 0.15f * k10); // a little more travel low down
    const float makeup = mode == DelayMode::Digital ? digitalWet : mode == DelayMode::Analog ? analogWet : tapeWet;
    const float mixTarget = makeup * std::clamp (p[PID::DelayMix] / 10.0f, 0.0f, 1.0f);
    const float modTarget = std::clamp (p[PID::DelayMod] / 10.0f, 0.0f, 1.0f);
    const float pingTarget = p.on (PID::DelayPingPong) ? 1.0f : 0.0f;
    const auto maxDelay = (float) state[0].line.maxDelay();
    const auto toSamples = [this, maxDelay] (double s) { return std::clamp ((float) (s * rate), 2.0f, maxDelay); };
    wetBefore = wetLevel;

    for (int j = 0; j <= segments; ++j)
    {
        if (j == 0 && ! fresh)
        {
            for (auto* lane : { &tap0, &tap1, &tapWeight, &feed, &gain, &wet, &pong })
                carry (*lane);
            continue;
        }
        const double s = segmentSeconds (j);
        const float k = glide (j, knobGlide);
        const float fb = feedback.follow (fbTarget, k);
        const float m = mix.follow (mixTarget, k);
        const float depth = mod.follow (modTarget, k);
        const float pp = ping.follow (pingTarget, k);
        const auto ramp = (float) (s / switchSeconds);
        feedLevel = toward (feedLevel, engaged ? 1.0f : 0.0f, ramp);
        wetLevel = toward (wetLevel, engaged || trails ? 1.0f : 0.0f, ramp);
        if (j == 0)
        {
            seconds = { target, target }; // a fresh start
            weight = weightTarget = 0.0f;
        }
        else
        {
            glideTime (target, s);
        }
        const double off = offset.follow ((float) modulation (s, depth), glide (j, offsetGlide));
        const auto at = (size_t) j;
        tap0[at] = toSamples (seconds[0] + off);
        tap1[at] = toSamples (seconds[1] + off);
        tapWeight[at] = weight;
        feed[at] = feedLevel;
        gain[at] = fb;
        wet[at] = m * wetLevel;
        pong[at] = pp;
    }
    designFilters (p);

    // What the reads can reach in this block holds silence or what has been written since a reset
    const int reach = (int) ((std::max ({ seconds[0], seconds[1], target }) + maxModulation) * rate) + 8;
    for (auto& s : state)
        s.line.ensureClear (reach);

    // The line is empty once nothing audible has gone into it, or come out, for its whole length
    // (and a little more for the filters to settle)
    quietLimit = (int) ((std::max ({ seconds[0], seconds[1], target }) + 0.05) * rate);
    for (auto& s : state)
        s.quiet = std::min (s.quiet + std::max (samples, 0), forever);
    fresh = false;
}

bool Delay::ringing() const
{
    // (Board asks after setPatch too: the block whose fade has just been laid out still has to run)
    return engaged || (std::max (wetBefore, wetLevel) > 0.0f && std::min (state[0].quiet, state[1].quiet) < quietLimit);
}

template <DelayMode M> float Delay::repeat (State& s, float tap) const
{
    if constexpr (M == DelayMode::Digital)
        return s.low.highpass (s.tone.lowpass (tap));
    else if constexpr (M == DelayMode::Analog)
        return s.low.highpass (s.band.process (tap));
    else
        return s.low.highpass (s.bump.process (s.band.process (tap)));
}

template <DelayMode M> float Delay::record (State& s, float in) const
{
    if constexpr (M == DelayMode::Analog)
        return s.pre.lowpass (in); // the BBD's input filter
    else
        return in;
}

template <DelayMode M> float Delay::loop (float v) const
{
    if constexpr (M == DelayMode::Digital)
        return softLimit (v);
    else if constexpr (M == DelayMode::Analog)
        return saturate (v, 1.25f);
    else
        return saturate (v, 1.6f);
}

template <DelayMode M> void Delay::run (State& s, float* x, int n)
{
    int lastLoud = -1;
    forSegments (n, [&] (int j, int from, int to, float inv)
    {
        const auto at = (size_t) j;
        float d0 = tap0[at], d1 = tap1[at], w = tapWeight[at], f = feed[at], g = gain[at], m = wet[at];
        const float dd0 = (tap0[at + 1] - d0) * inv, dd1 = (tap1[at + 1] - d1) * inv, dw = (tapWeight[at + 1] - w) * inv;
        const float df = (feed[at + 1] - f) * inv, dg = (gain[at + 1] - g) * inv, dm = (wet[at + 1] - m) * inv;
        const bool first = w < 1.0f || tapWeight[at + 1] < 1.0f, second = w > 0.0f || tapWeight[at + 1] > 0.0f;
        for (int i = from; i < to; ++i)
        {
            float tap;
            if (first && second)
            {
                const float a = s.line.read (d0, cubic);
                tap = a + w * (s.line.read (d1, cubic) - a);
            }
            else
            {
                tap = s.line.read (second ? d1 : d0, cubic);
            }
            const float y = repeat<M> (s, tap);
            const float v = loop<M> (f * record<M> (s, x[i]) + g * y);
            s.line.push (v);
            if (std::abs (v) > audible || std::abs (y) > audible)
                lastLoud = i;
            x[i] += m * y;
            d0 += dd0;
            d1 += dd1;
            w += dw;
            f += df;
            g += dg;
            m += dm;
        }
    });
    s.line.advanced (n);
    if (lastLoud >= 0)
        s.quiet = n - 1 - lastLoud;
}

template <DelayMode M> void Delay::runStereo (float* left, float* right, int n)
{
    auto& l = state[0];
    auto& r = state[1];
    int lastLoud = -1;
    forSegments (n, [&] (int j, int from, int to, float inv)
    {
        const auto at = (size_t) j;
        float d0 = tap0[at], d1 = tap1[at], w = tapWeight[at], f = feed[at], g = gain[at], m = wet[at], p = pong[at];
        const float dd0 = (tap0[at + 1] - d0) * inv, dd1 = (tap1[at + 1] - d1) * inv, dw = (tapWeight[at + 1] - w) * inv;
        const float df = (feed[at + 1] - f) * inv, dg = (gain[at + 1] - g) * inv, dm = (wet[at + 1] - m) * inv;
        const float dp = (pong[at + 1] - p) * inv;
        const bool first = w < 1.0f || tapWeight[at + 1] < 1.0f, second = w > 0.0f || tapWeight[at + 1] > 0.0f;
        const auto read = [&] (const EchoLine& line)
        {
            if (first && second)
            {
                const float a = line.read (d0, cubic);
                return a + w * (line.read (d1, cubic) - a);
            }
            return line.read (second ? d1 : d0, cubic);
        };
        for (int i = from; i < to; ++i)
        {
            const float yl = repeat<M> (l, read (l.line)), yr = repeat<M> (r, read (r.line));
            // Ping-pong: the mono sum goes into the left line only, and each line feeds the other
            const float inL = left[i], inR = right[i];
            const float intoL = inL + p * (0.5f * (inR - inL)), intoR = (1.0f - p) * inR;
            const float backL = yl + p * (yr - yl), backR = yr + p * (yl - yr);
            const float vl = loop<M> (f * record<M> (l, intoL) + g * backL);
            const float vr = loop<M> (f * record<M> (r, intoR) + g * backR);
            l.line.push (vl);
            r.line.push (vr);
            if (std::max ({ std::abs (vl), std::abs (vr), std::abs (yl), std::abs (yr) }) > audible)
                lastLoud = i;
            left[i] = inL + m * yl;
            right[i] = inR + m * yr;
            d0 += dd0;
            d1 += dd1;
            w += dw;
            f += df;
            g += dg;
            m += dm;
            p += dp;
        }
    });
    l.line.advanced (n);
    r.line.advanced (n);
    if (lastLoud >= 0)
        l.quiet = r.quiet = n - 1 - lastLoud;
}

void Delay::process (int channel, float* x, int n)
{
    auto& s = state[(size_t) channel];
    switch (mode)
    {
        case DelayMode::Digital: run<DelayMode::Digital> (s, x, n); break;
        case DelayMode::Analog: run<DelayMode::Analog> (s, x, n); break;
        case DelayMode::Tape: run<DelayMode::Tape> (s, x, n); break;
    }
}

void Delay::processStereo (float* left, float* right, int n)
{
    switch (mode)
    {
        case DelayMode::Digital: runStereo<DelayMode::Digital> (left, right, n); break;
        case DelayMode::Analog: runStereo<DelayMode::Analog> (left, right, n); break;
        case DelayMode::Tape: runStereo<DelayMode::Tape> (left, right, n); break;
    }
}

} // namespace damp::pedals
