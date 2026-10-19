#include "Pitch.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace damp::pedals
{
namespace
{
constexpr double controlRate = 12000.0; // the decimated copy and the control ticks, about
constexpr double minJumpSeconds = 0.006; // the shortest grain
constexpr double jumpSpanSeconds = 0.014; // the search beyond it: a period of the lowest notes (~70 Hz)
constexpr double maxFadeSeconds = 0.006;
constexpr double windowSeconds = 0.010;   // of the correlation
constexpr double glideSeconds = 0.004;    // time constant of the shift's glide (settles in ~15 ms)
constexpr double recentreSeconds = 0.02;  // between tries at closing up the delay at unison
constexpr double retrySeconds = 0.001;    // between tries at a better match shifting down
constexpr double longGrainSeconds = 0.032; // the most a poor match shifting down waits for
constexpr double longFadeSeconds = 0.010;
constexpr double lineSeconds = 0.06;
constexpr int coarseSize = 4096;          // > (line + search + window) at the highest control rate
constexpr float poorMatch = 0.5f;         // below this a splice point is no better than any other
constexpr float recentreMatch = 0.95f;
constexpr float goodMatch = 0.9f;
constexpr float tieBreak = 0.02f;         // of correlation, across the search range
constexpr int maxWindow = 256;
constexpr double highCutHz = 16000.0; // an octave up

// The shifted signal has the input's level, but before the amp, moved up it falls into the cab's
// roll-off and down below the amp's lows (and the ear's): about 3 dB quieter an octave up, 2.5-8 dB
// an octave down depending on the voicing. After the amp (and cab) it loses less. Made up for in
// between, so a shift (and a sweep of the pedal) stays within about 2 dB of the bypassed loudness
// (integrated, the demo guitar through clean and crunch presets) either side of the amp; only the
// cleanest, thinnest voicings lose more on big down-shifts.
float loudness (double semis)
{
    const double db = semis > 0.0 ? 2.5 * semis / 12.0 : -3.0 * semis / 12.0;
    return (float) std::pow (10.0, db / 20.0);
}
} // namespace

void Pitch::prepare (double maxRate)
{
    for (auto& s : state)
    {
        s.line.allocate ((int) std::ceil (lineSeconds * maxRate));
        s.coarse.assign ((size_t) coarseSize, 0.0f);
    }
    coarseMask = coarseSize - 1;
    rate = 0.0; // configure on the first setPatch
    reset();
}

void Pitch::reset()
{
    for (auto& s : state)
    {
        s.line.clear();
        std::fill (s.coarse.begin(), s.coarse.end(), 0.0f);
        s.coarseWrite = 0;
        s.antiAlias1.reset();
        s.antiAlias2.reset();
        s.highCut.reset();
        s.highCut.g = 1.0f;
        s.sum = 0.0f;
        s.count = 0;
        s.delay = s.fading = floorDelay;
        s.fade = 1.0f;
        s.fadeStep = 0.0f;
        s.rho = 1.0f;
        s.wait = 0;
    }
    fresh = true;
}

void Pitch::configure (double sampleRate)
{
    rate = sampleRate;
    decimation = std::max (1, (int) std::lround (rate / controlRate));
    const double coarseRate = rate / decimation;
    cubic = rate < 150000.0;
    floorDelay = 3.0;
    minJump = minJumpSeconds * rate;
    shortestJump = 0.001 * rate; // a period at 1 kHz
    jumpSpan = jumpSpanSeconds * rate;
    maxFade = maxFadeSeconds * rate;
    longFade = longFadeSeconds * rate;
    longGrain = longGrainSeconds * rate;
    maxDelay = state[0].line.maxDelay() - 2.0;
    window = std::clamp ((int) std::lround (windowSeconds * coarseRate), 8, maxWindow);
    recentreTicks = (int) std::lround (recentreSeconds * coarseRate);
    retryTicks = (int) std::lround (retrySeconds * coarseRate);
    glide = (float) (1.0 - std::exp (-1.0 / (glideSeconds * coarseRate)));
    for (auto& s : state)
    {
        // Ahead of the decimation: two poles near a quarter of the control rate (the boxcar sum
        // that follows nulls what would alias to the low end)
        s.antiAlias1.setCutoff (0.22 * coarseRate, rate);
        s.antiAlias2.setCutoff (0.22 * coarseRate, rate);
    }
}

void Pitch::setPatch (const Patch& p, double sampleRate, int)
{
    if (sampleRate != rate)
    {
        configure (sampleRate);
        reset(); // delays are in samples
    }

    const double shift = p[PID::PitchShift] * std::clamp (p[PID::PitchPosition], 0.0f, 10.0f) / 10.0f;
    const bool steps = p.choice<PitchMode> (PID::PitchMode) == PitchMode::Steps;
    semisTarget = std::clamp (steps ? std::round (shift) : shift, -24.0, 24.0);

    // Constant power: 5 is a harmony with both at -3 dB
    const double m = std::clamp (p[PID::PitchMix], 0.0f, 10.0f) / 10.0;
    dryTarget = m >= 1.0 ? 0.0f : (float) std::cos (0.5 * std::numbers::pi * m);
    wetTarget = m <= 0.0 ? 0.0f : (float) std::sin (0.5 * std::numbers::pi * m) * loudness (semisTarget);

    if (fresh)
    {
        for (auto& s : state)
        {
            s.semis = semisTarget;
            s.ratio = std::exp2 (semisTarget / 12.0);
            s.ratioStep = 0.0;
            s.dry.snap (dryTarget);
            s.wet.snap (wetTarget);
        }
        fresh = false;
    }
}

void Pitch::splice (State& s, double jump, float match, double fadeSamples)
{
    s.fading = s.delay;
    s.delay = std::clamp (s.delay + jump, floorDelay, maxDelay);
    s.fade = 0.0f;
    s.fadeStep = (float) (1.0 / std::max (fadeSamples, 1.0));
    s.rho = match;
    s.wait = 0;
}

// Once a control tick, after a coarse sample: moves the glide on and splices when the tap needs it
void Pitch::control (State& s)
{
    if (std::abs (semisTarget - s.semis) < 1.0e-4)
        s.semis = semisTarget;
    else
        s.semis += glide * (semisTarget - s.semis);
    const double next = std::exp2 (s.semis / 12.0);
    s.ratioStep = (next - s.ratio) / decimation;
    // A gentle high-cut on up-shifts, for the fizz of highs moved up (and the splices' edges):
    // off at unison (g = 1 passes the input as it is), 16 kHz an octave up, 9 kHz two
    if (next > 1.0 + 1.0e-6)
        s.highCut.setCutoffUnwarped (std::min (highCutHz / std::sqrt (next - 1.0), 0.45 * rate), rate);
    else
        s.highCut.g = 1.0f;

    if (s.fade < 1.0f)
        return; // one splice at a time

    // Whether a splice that can wait may be tried this tick
    const auto waited = [&s]
    {
        if (s.wait <= 0)
            return true;
        --s.wait;
        return false;
    };
    const double r = s.ratio;
    const double speed = std::max (std::abs (1.0 - r), std::abs (1.0 - next)); // of the tap, samples a sample
    // A splice must be over before the next is due (a grain of at least minJump lasts
    // minJump / speed): the fade takes under half of that
    const double fadeLength = speed > 1.0e-9 ? std::min (maxFade, 0.45 * minJump / speed) : maxFade;
    if (r > 1.0 + 1.0e-9)
    {
        // Closing on the input: jump back while the old tap can still run out its fade
        const double slack = 2.0 * speed * decimation + 2.0;
        if (s.delay < floorDelay + (r - 1.0) * fadeLength + slack)
        {
            const auto [jump, match] = findSplice (s, s.delay, 1, minJump, minJump + jumpSpan, false);
            splice (s, jump, match, fadeLength);
        }
    }
    else if (r < 1.0 - 1.0e-9)
    {
        // Falling behind: jump forward, as close to the input as a good match allows. The old
        // tap moves away from the input, so the fade can take anything under half the grain.
        // A chord or noise matches poorly at any jump: then the tap falls further behind, for a
        // longer grain (fewer splices) with a longer fade, retrying in case a match turns up.
        if (s.delay > floorDelay + jumpSpan && waited())
        {
            const auto [jump, match] = findSplice (s, s.delay, -1, shortestJump, s.delay - floorDelay, true);
            const bool good = match >= goodMatch;
            if (good || s.delay > floorDelay + longGrain)
                splice (s, -jump, match, std::min (good ? maxFade : longFade, 0.45 * jump / speed));
            else
                s.wait = retryTicks;
        }
    }
    else if (s.delay > floorDelay + 0.002 * rate && waited())
    {
        // At unison the tap stands still wherever a shift left it: close up the delay where a
        // splice won't show (on a steady note, or silence)
        s.wait = recentreTicks;
        const double room = s.delay - floorDelay;
        const auto [jump, match] = findSplice (s, s.delay, -1, shortestJump, room, true);
        // A match at either end of the range is a peak outside it, not a period
        if (match >= recentreMatch && jump > shortestJump + decimation && jump < room - decimation)
            splice (s, -jump, match, maxFade);
    }
}

std::pair<double, float> Pitch::findSplice (const State& s, double tapDelay, int dir, double jumpLo, double jumpHi,
                                            bool preferLong)
{
    // Coarse samples by age: 0 is the latest
    const auto at = [&s, this] (int age) { return s.coarse[(size_t) ((s.coarseWrite - 1 - std::max (age, 0)) & coarseMask)]; };
    const int tap = (int) std::lround (tapDelay / decimation);
    const int kLo = std::max (1, (int) std::ceil (jumpLo / decimation));
    int kHi = std::clamp ((int) std::floor (jumpHi / decimation), kLo, kLo + (int) scores.size() - 1);
    kHi = std::min (kHi, (int) run.size() - window - 1);

    // The window reaches up to half of it past the tap towards the present, as far as both taps'
    // windows have been heard
    int ahead = window / 2;
    if (dir > 0)
        ahead = std::min (ahead, tap);
    else
    {
        ahead = std::clamp (tap - kHi, 0, ahead);
        kHi = std::max (kLo, std::min (kHi, tap - ahead));
    }
    const int newest = tap - ahead;

    // Everything both windows cover, oldest last, into one run: the window at the tap starts at
    // `a`, the one k coarse samples away at a + dir * k
    const int base = dir > 0 ? newest : std::max (newest - kHi, 0);
    const int length = kHi + window + 1;
    for (int j = 0; j < length; ++j)
        run[(size_t) j] = at (base + j);
    const float* a = run.data() + (newest - base);

    const auto dot = [this] (const float* u, const float* v)
    {
        // Four partial sums, which the compiler can keep in one vector register
        float s0 = 0.0f, s1 = 0.0f, s2 = 0.0f, s3 = 0.0f;
        int i = 0;
        for (; i + 4 <= window; i += 4)
        {
            s0 += u[i] * v[i];
            s1 += u[i + 1] * v[i + 1];
            s2 += u[i + 2] * v[i + 2];
            s3 += u[i + 3] * v[i + 3];
        }
        for (; i < window; ++i)
            s0 += u[i] * v[i];
        return (double) ((s0 + s1) + (s2 + s3));
    };
    const double energy = dot (a, a);
    if (energy < 1.0e-14 * window) // silence: any splice is as good
        return { (preferLong ? kHi : kLo) * (double) decimation, 1.0f };

    const float* b = a + dir * kLo;
    double bb = dot (b, b); // kept up as the window slides
    for (int k = kLo; k <= kHi; ++k, b += dir)
    {
        if (k > kLo) // the window moved one on: drop the sample it left, add the one it reached
        {
            const float gone = dir > 0 ? b[-1] : b[window], added = dir > 0 ? b[window - 1] : b[0];
            bb = std::max (bb + (double) added * added - (double) gone * gone, 0.0);
        }
        scores[(size_t) (k - kLo)] = bb > 1.0e-30 ? (float) (dot (a, b) / std::sqrt (energy * bb)) : 0.0f;
    }

    // Each peak's top, from a parabola through it and its neighbours: {lag, height}
    const auto peak = [&] (int k)
    {
        const float c = scores[(size_t) (k - kLo)];
        if (k == kLo || k == kHi)
            return std::pair { (double) k, c };
        const double l = scores[(size_t) (k - kLo - 1)], r = scores[(size_t) (k - kLo + 1)];
        const double d = l - 2.0 * c + r;
        if (d > -1.0e-9)
            return std::pair { (double) k, c };
        const double offset = std::clamp (0.5 * (l - r) / d, -0.5, 0.5);
        return std::pair { k + offset, (float) (c - 0.25 * (l - r) * offset) };
    };
    // The best match, near-ties going to the shorter jump (less delay) or the longer (preferLong)
    const auto pick = [&] (int lo)
    {
        std::pair<double, float> best { (double) lo, -10.0f };
        float bestScore = -10.0f;
        const float span = (float) std::max (kHi - kLo, 1);
        for (int k = lo; k <= kHi; ++k)
        {
            const float here = scores[(size_t) (k - kLo)];
            if ((k > lo && scores[(size_t) (k - kLo - 1)] > here) || (k < kHi && scores[(size_t) (k - kLo + 1)] > here))
                continue; // not a peak
            const auto top = peak (k);
            const float t = (float) (top.first - kLo) / span;
            const float score = top.second + tieBreak * (preferLong ? t : -t);
            if (score > bestScore)
            {
                bestScore = score;
                best = top;
            }
        }
        return best;
    };
    auto best = pick (kLo);
    // Nothing periodic to lock to (a chord, noise): a longer grain splices less often
    if (! preferLong && best.second < poorMatch)
        best = pick ((kLo + kHi) / 2);
    return { best.first * decimation, std::clamp (best.second, 0.0f, 1.0f) };
}

void Pitch::process (int channel, float* x, int n)
{
    auto& s = state[(size_t) channel];
    s.dry.toward (dryTarget, n);
    s.wet.toward (wetTarget, n);
    const float toCoarse = 1.0f / (float) decimation;
    for (int i = 0; i < n; ++i)
    {
        const float in = x[i];
        float shifted = s.line.read ((float) s.delay, cubic);
        if (s.fade < 1.0f)
        {
            // Crossfading taps: gains for constant level given how alike they are (rho 1: they
            // add in phase, a straight crossfade; 0: unrelated, constant power)
            const float f = s.fade, g = 1.0f - f;
            const float old = s.line.read ((float) s.fading, cubic);
            const float norm = 1.0f / std::sqrt (std::max (f * f + g * g + 2.0f * s.rho * f * g, 0.25f));
            shifted = norm * (f * shifted + g * old);
            s.fade += s.fadeStep;
        }
        s.line.push (in);

        s.sum += s.antiAlias2.lowpass (s.antiAlias1.lowpass (in));
        if (++s.count == decimation)
        {
            s.coarse[(size_t) s.coarseWrite] = s.sum * toCoarse;
            s.coarseWrite = (s.coarseWrite + 1) & coarseMask;
            s.sum = 0.0f;
            s.count = 0;
            control (s);
        }

        s.ratio += s.ratioStep;
        const double drift = 1.0 - s.ratio;
        s.delay = std::clamp (s.delay + drift, floorDelay, maxDelay);
        s.fading = std::clamp (s.fading + drift, floorDelay, maxDelay);

        x[i] = s.dry.next() * in + s.wet.next() * s.highCut.lowpass (shifted);
    }
}

} // namespace damp::pedals
